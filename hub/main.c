/*
 * Sentinel Hub — hub/main.c
 * ---------------------------------------------------------------------------
 * Runs on the Raspberry Pi 5 under QNX 8.0 as the central base station for
 * the Sentinel Arctic sensor network (cuHacking).
 *
 * Responsibilities (per Phase 1 MVP):
 *   1. Listens for HTTP POST from ESP32 Node 1 (DHT11 temp/humidity) on
 *      port 8080 at /sensor-data (alias: /data).
 *   2. Polls the HC-SR04 ultrasonic sensor via GPIO every 500ms.
 *   3. When distance < 30cm, triggers the Pi Camera + an external OpenCV
 *      classifier process (person / object / none) and attaches the result
 *      to the resulting alert.
 *   4. Writes all telemetry to InfluxDB (port 8086) using HTTP line
 *      protocol.
 *   5. Exposes GET /nodes and GET /alerts for Julia's Flask backend /
 *      dashboard to poll.
 *
 * Build (on the Pi, via SSH):
 *      clang -Wall -Wextra -O2 -std=c11 -o hub main.c -lpthread
 *   (see hub/Makefile)
 *
 * Run:
 *      INFLUX_TOKEN=xxxx ./hub
 *
 * Useful env vars (all optional, see CONFIG section below):
 *      SENTINEL_HTTP_PORT     default 8080
 *      INFLUX_HOST            default 127.0.0.1
 *      INFLUX_PORT            default 8086
 *      INFLUX_ORG             default "sentinel"
 *      INFLUX_BUCKET          default "sentinel"
 *      INFLUX_TOKEN           default "" (required for a real InfluxDB 2.x
 *                              instance — get it from `influx auth list`)
 *      SENTINEL_GPIO_SIM      default 0 — real HC-SR04 GPIO is used by
 *                              default (see GPIO section). Set to 1 to force
 *                              simulated distance readings (e.g. testing the
 *                              HTTP/InfluxDB/alerts pipeline off-hardware).
 *      SENTINEL_OPENCV_CMD    default "./opencv_classify.py"
 *
 * IMPORTANT — read before demo day:
 *   The GPIO section below talks to the real HC-SR04 over
 *   /dev/gpio23 (TRIG) and /dev/gpio24 (ECHO), confirmed present via
 *   `ls /dev/gpio*` on this Pi. The exact wire protocol of this
 *   QNX GPIO character-device driver isn't documented anywhere we have
 *   access to, so the implementation uses the most common convention for a
 *   one-device-per-pin GPIO driver: open() with O_WRONLY/O_RDONLY selects
 *   direction, and a single-byte read()/write() gets/sets the pin level
 *   (ASCII '0'/'1', with a raw binary 0/1 fallback on write). Sanity-check
 *   this against the real driver before relying on it for the demo:
 *
 *     echo 1 > /dev/gpio23 ; echo 0 > /dev/gpio23   # TRIG should toggle
 *     cat /dev/gpio24                                # ECHO should read back
 *
 *   If the driver behaves differently, adjust gpio_pin_write()/
 *   gpio_pin_read() in the GPIO section below. Either way,
 *   gpio_hw_init() falls back to SENTINEL_GPIO_SIM automatically if
 *   opening either device fails, so a wiring/driver problem degrades to a
 *   working simulated demo instead of a crash.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/select.h>

/* =========================================================================
 * CONFIG
 * ========================================================================= */

#define DEFAULT_HTTP_PORT        8080
#define DEFAULT_INFLUX_HOST      "127.0.0.1"
#define DEFAULT_INFLUX_PORT      8086
#define DEFAULT_INFLUX_ORG       "sentinel"
#define DEFAULT_INFLUX_BUCKET    "sentinel"
#define DEFAULT_OPENCV_CMD       "./opencv_classify.py"

#define MAX_NODES                8
#define MAX_ALERTS               200
#define REQ_BUF_SIZE             8192
#define LINE_BUF_SIZE            512

#define PROXIMITY_THRESHOLD_CM   30.0
#define TEMP_HIGH_C              35.0
#define TEMP_LOW_C               (-10.0)
#define HUMIDITY_HIGH_PCT        80.0
#define NODE_OFFLINE_SEC         10
#define GPIO_POLL_INTERVAL_MS    500
#define ALERT_COOLDOWN_SEC       15   /* dedupe: don't refire same alert type
                                         for a node more often than this */
#define CAMERA_TIMEOUT_SEC       5

static int   g_http_port      = DEFAULT_HTTP_PORT;
static char  g_influx_host[128];
static int   g_influx_port    = DEFAULT_INFLUX_PORT;
static char  g_influx_org[64];
static char  g_influx_bucket[64];
static char  g_influx_token[256];
static char  g_opencv_cmd[256];
static int   g_gpio_sim       = 1;

static volatile sig_atomic_t g_shutdown = 0;

/* =========================================================================
 * LOGGING
 * ========================================================================= */

static void log_line(const char *level, const char *fmt, ...) {
    char timebuf[32];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", &tm_now);

    fprintf(stderr, "[%s] %-5s ", timebuf, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

#define LOG_INFO(...)  log_line("INFO", __VA_ARGS__)
#define LOG_WARN(...)  log_line("WARN", __VA_ARGS__)
#define LOG_ERR(...)   log_line("ERROR", __VA_ARGS__)

/* =========================================================================
 * SHARED STATE: NODES
 * ========================================================================= */

typedef struct {
    char   node_id[32];
    char   location[48];
    double lat, lon;
    double temperature_c;
    double humidity_pct;
    double distance_cm;
    int    has_temperature;
    int    has_distance;
    int    proximity_alert;
    time_t last_seen;
    int    in_use;
} node_state_t;

static node_state_t   g_nodes[MAX_NODES];
static pthread_mutex_t g_nodes_lock = PTHREAD_MUTEX_INITIALIZER;

/* Known field coordinates from the hackathon plan (used only as defaults
 * when a node reports without explicit lat/lon). */
static void seed_known_location(const char *node_id, char *loc_out, size_t loc_sz,
                                 double *lat_out, double *lon_out) {
    if (strcmp(node_id, "node_1") == 0) {
        snprintf(loc_out, loc_sz, "Inuvik"); *lat_out = 68.3607; *lon_out = -133.7230;
    } else if (strcmp(node_id, "node_2") == 0) {
        snprintf(loc_out, loc_sz, "Iqaluit"); *lat_out = 63.7467; *lon_out = -68.5170;
    } else if (strcmp(node_id, "node_3") == 0) {
        snprintf(loc_out, loc_sz, "Whitehorse"); *lat_out = 60.7212; *lon_out = -135.0568;
    } else if (strcmp(node_id, "node_4") == 0) {
        snprintf(loc_out, loc_sz, "Resolute"); *lat_out = 74.7069; *lon_out = -94.8288;
    } else if (strcmp(node_id, "node_5") == 0) {
        snprintf(loc_out, loc_sz, "Churchill"); *lat_out = 58.7667; *lon_out = -94.1667;
    } else {
        snprintf(loc_out, loc_sz, "Unknown"); *lat_out = 0.0; *lon_out = 0.0;
    }
}

/* Finds a node by id, or allocates a new slot. Returns NULL if the table
 * is full. Caller must hold g_nodes_lock. */
static node_state_t *find_or_create_node_locked(const char *node_id) {
    int free_slot = -1;
    for (int i = 0; i < MAX_NODES; i++) {
        if (g_nodes[i].in_use && strcmp(g_nodes[i].node_id, node_id) == 0) {
            return &g_nodes[i];
        }
        if (!g_nodes[i].in_use && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return NULL;

    node_state_t *n = &g_nodes[free_slot];
    memset(n, 0, sizeof(*n));
    snprintf(n->node_id, sizeof(n->node_id), "%s", node_id);
    seed_known_location(node_id, n->location, sizeof(n->location), &n->lat, &n->lon);
    n->in_use = 1;
    return n;
}

/* =========================================================================
 * SHARED STATE: ALERTS  (simple ring buffer)
 * ========================================================================= */

typedef struct {
    long   id;
    char   node_id[32];
    char   type[32];        /* proximity_alert | temp_high | temp_low |
                                humidity_high | node_offline */
    char   message[256];
    char   classification[16]; /* person | object | none | "" */
    time_t timestamp;
} alert_t;

static alert_t          g_alerts[MAX_ALERTS];
static int               g_alert_count = 0;   /* number of valid entries */
static int               g_alert_head  = 0;   /* next write index (ring) */
static long               g_alert_next_id = 1;
static pthread_mutex_t   g_alerts_lock = PTHREAD_MUTEX_INITIALIZER;

/* Last time each (node_id, type) pair fired, for cooldown dedupe. */
typedef struct {
    char   node_id[32];
    char   type[32];
    time_t last_fired;
} alert_cooldown_t;
static alert_cooldown_t g_cooldowns[MAX_NODES * 6];
static int g_cooldown_count = 0;
static pthread_mutex_t g_cooldown_lock = PTHREAD_MUTEX_INITIALIZER;

static int cooldown_ready(const char *node_id, const char *type) {
    time_t now = time(NULL);
    int ready = 1;
    pthread_mutex_lock(&g_cooldown_lock);
    for (int i = 0; i < g_cooldown_count; i++) {
        if (strcmp(g_cooldowns[i].node_id, node_id) == 0 &&
            strcmp(g_cooldowns[i].type, type) == 0) {
            if (now - g_cooldowns[i].last_fired < ALERT_COOLDOWN_SEC) ready = 0;
            if (ready) g_cooldowns[i].last_fired = now;
            pthread_mutex_unlock(&g_cooldown_lock);
            return ready;
        }
    }
    if (g_cooldown_count < (int)(sizeof(g_cooldowns) / sizeof(g_cooldowns[0]))) {
        alert_cooldown_t *c = &g_cooldowns[g_cooldown_count++];
        snprintf(c->node_id, sizeof(c->node_id), "%s", node_id);
        snprintf(c->type, sizeof(c->type), "%s", type);
        c->last_fired = now;
    }
    pthread_mutex_unlock(&g_cooldown_lock);
    return ready;
}

/* Forward decl */
static void influx_write_alert_async(const alert_t *a);

static void push_alert(const char *node_id, const char *type,
                        const char *classification, const char *fmt, ...) {
    if (!cooldown_ready(node_id, type)) return;

    alert_t a;
    memset(&a, 0, sizeof(a));
    snprintf(a.node_id, sizeof(a.node_id), "%s", node_id);
    snprintf(a.type, sizeof(a.type), "%s", type);
    if (classification) snprintf(a.classification, sizeof(a.classification), "%s", classification);
    a.timestamp = time(NULL);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a.message, sizeof(a.message), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_alerts_lock);
    a.id = g_alert_next_id++;
    g_alerts[g_alert_head] = a;
    g_alert_head = (g_alert_head + 1) % MAX_ALERTS;
    if (g_alert_count < MAX_ALERTS) g_alert_count++;
    pthread_mutex_unlock(&g_alerts_lock);

    LOG_WARN("ALERT [%s] %s: %s", type, node_id, a.message);
    influx_write_alert_async(&a);
}

/* =========================================================================
 * TINY JSON HELPERS
 * ---------------------------------------------------------------------------
 * The ESP32 firmware sends small, flat JSON objects. Rather than pull in a
 * JSON library (apk availability during a hackathon is unpredictable), we
 * do minimal, defensive string scanning for known keys. This is NOT a
 * general-purpose JSON parser — it only needs to survive well-formed,
 * flat sensor payloads.
 * ========================================================================= */

/* Finds "key" in a flat JSON object and returns a pointer to the character
 * right after the following colon, skipping whitespace. Returns NULL if
 * not found. */
static const char *json_find_value(const char *json, const char *key) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    p = strchr(p, ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int json_get_double(const char *json, const char *key, double *out) {
    const char *v = json_find_value(json, key);
    if (!v) return 0;
    char *end = NULL;
    double val = strtod(v, &end);
    if (end == v) return 0;
    *out = val;
    return 1;
}

static int json_get_bool(const char *json, const char *key, int *out) {
    const char *v = json_find_value(json, key);
    if (!v) return 0;
    if (strncmp(v, "true", 4) == 0)  { *out = 1; return 1; }
    if (strncmp(v, "false", 5) == 0) { *out = 0; return 1; }
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_sz) {
    const char *v = json_find_value(json, key);
    if (!v || *v != '"') return 0;
    v++;
    size_t i = 0;
    while (*v && *v != '"' && i + 1 < out_sz) {
        out[i++] = *v++;
    }
    out[i] = '\0';
    return 1;
}

/* =========================================================================
 * MINIMAL HTTP CLIENT  (used to talk to InfluxDB)
 * ========================================================================= */

/* Connects, sends a raw HTTP/1.1 request, reads the response into `resp`
 * (best-effort, truncates at resp_sz). Returns 0 on success (request sent
 * and at least a status line read back), -1 on failure. */
static int http_post_raw(const char *host, int port, const char *path,
                          const char *extra_headers, const char *body,
                          char *resp, size_t resp_sz) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { LOG_ERR("http_post_raw: socket() failed: %s", strerror(errno)); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        LOG_ERR("http_post_raw: invalid host '%s'", host);
        close(fd);
        return -1;
    }

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_WARN("http_post_raw: connect to %s:%d failed: %s", host, port, strerror(errno));
        close(fd);
        return -1;
    }

    size_t body_len = body ? strlen(body) : 0;
    char req[REQ_BUF_SIZE];
    int n = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "%s"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, port, extra_headers ? extra_headers : "", body_len);

    if (n < 0 || (size_t)n >= sizeof(req)) {
        LOG_ERR("http_post_raw: request too large");
        close(fd);
        return -1;
    }

    if (send(fd, req, (size_t)n, 0) < 0 ||
        (body_len > 0 && send(fd, body, body_len, 0) < 0)) {
        LOG_WARN("http_post_raw: send() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    size_t total = 0;
    ssize_t r;
    while (resp && total + 1 < resp_sz &&
           (r = recv(fd, resp + total, resp_sz - 1 - total, 0)) > 0) {
        total += (size_t)r;
    }
    if (resp) resp[total] = '\0';

    close(fd);
    return 0;
}

/* =========================================================================
 * INFLUXDB WRITER  (HTTP line protocol, InfluxDB 2.x /api/v2/write style)
 * ========================================================================= */

static void influx_write_line(const char *line) {
    if (g_influx_token[0] == '\0') {
        LOG_WARN("INFLUX_TOKEN not set — skipping InfluxDB write (line: %s)", line);
        return;
    }

    char path[256];
    snprintf(path, sizeof(path), "/api/v2/write?org=%s&bucket=%s&precision=s",
             g_influx_org, g_influx_bucket);

    char headers[512];
    snprintf(headers, sizeof(headers),
             "Authorization: Token %s\r\n"
             "Content-Type: text/plain; charset=utf-8\r\n",
             g_influx_token);

    char resp[512];
    if (http_post_raw(g_influx_host, g_influx_port, path, headers, line, resp, sizeof(resp)) != 0) {
        LOG_WARN("InfluxDB write failed (network) — data kept in memory only, will retry on next reading");
        return;
    }
    if (strstr(resp, "204") == NULL && strstr(resp, "200") == NULL) {
        LOG_WARN("InfluxDB write may have failed, response: %.200s", resp);
    }
}

/* Escapes spaces/commas in InfluxDB tag values (rudimentary — good enough
 * for our known node_id/location strings). */
static void influx_escape_tag(const char *in, char *out, size_t out_sz) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < out_sz; i++) {
        if (in[i] == ' ' || in[i] == ',' || in[i] == '=') out[j++] = '\\';
        out[j++] = in[i];
    }
    out[j] = '\0';
}

static void influx_write_sensor_reading(const node_state_t *n) {
    char loc_esc[96];
    influx_escape_tag(n->location, loc_esc, sizeof(loc_esc));

    char fields[256];
    size_t off = 0;
    fields[0] = '\0';
    if (n->has_temperature) {
        off += (size_t)snprintf(fields + off, sizeof(fields) - off,
                                 "temperature=%.2f,humidity=%.2f,", n->temperature_c, n->humidity_pct);
    }
    if (n->has_distance) {
        off += (size_t)snprintf(fields + off, sizeof(fields) - off,
                                 "distance_cm=%.1f,proximity_alert=%s,",
                                 n->distance_cm, n->proximity_alert ? "true" : "false");
    }
    if (off == 0) return; /* nothing to write yet */
    if (off > 0 && fields[off - 1] == ',') fields[off - 1] = '\0'; /* trim trailing comma */

    char line[LINE_BUF_SIZE];
    snprintf(line, sizeof(line), "sensor_data,node_id=%s,location=%s %s %ld",
             n->node_id, loc_esc, fields, (long)time(NULL));

    influx_write_line(line);
}

static void influx_write_alert_async(const alert_t *a) {
    /* Kept synchronous-but-fast (single short-lived socket) for simplicity;
       InfluxDB runs locally on the Pi so latency should be sub-millisecond.
       If this ever needs to be non-blocking, move the call into its own
       pthread here. */
    char msg_esc[256];
    influx_escape_tag(a->message, msg_esc, sizeof(msg_esc));

    char line[LINE_BUF_SIZE];
    snprintf(line, sizeof(line),
             "alerts,node_id=%s,type=%s message=\"%s\",classification=\"%s\" %ld",
             a->node_id, a->type, msg_esc, a->classification, (long)a->timestamp);
    influx_write_line(line);
}

/* =========================================================================
 * GPIO / HC-SR04
 * ---------------------------------------------------------------------------
 * Real backend, confirmed against this Pi's device layout
 * (`ls /dev/gpio*` -> /dev/gpio0 .. /dev/gpio27, plus /dev/gpio/msg):
 * TRIG is /dev/gpio23, ECHO is /dev/gpio24, one character device per pin.
 *
 * NOTE: HC-SR04's ECHO line is 5V logic — you need a voltage divider (or
 * logic-level shifter) down to 3.3V before it touches a Pi GPIO pin, or you
 * risk damaging the Pi's GPIO input.
 *
 * The exact wire protocol of this driver isn't documented anywhere we have
 * access to, so gpio_pin_write()/gpio_pin_read() below use the most common
 * convention for a one-device-per-pin GPIO character driver: open() with
 * O_WRONLY vs. O_RDONLY selects direction, and a single-byte read()/write()
 * gets/sets the pin level. Verify with the shell before fully trusting it:
 *
 *   echo 1 > /dev/gpio23 ; echo 0 > /dev/gpio23   # TRIG should toggle
 *   cat /dev/gpio24                                # ECHO should read back
 *
 * If that doesn't match reality, this is the only place to fix — the rest
 * of the pipeline (thresholds, alerts, InfluxDB, HTTP API) doesn't care how
 * gpio_read_distance_cm() gets its number. SENTINEL_GPIO_SIM=1 remains a
 * one-env-var fallback regardless.
 * ========================================================================= */

#define GPIO_DEV_PATH_FMT       "/dev/gpio%d"
#define GPIO_TRIG_PIN            23
#define GPIO_ECHO_PIN            24

#define HCSR04_TRIG_PULSE_US     10     /* datasheet minimum */
#define HCSR04_ECHO_TIMEOUT_US   30000  /* ~30ms, comfortably covers ~500cm
                                            round trip; also our "nothing in
                                            range" bailout */

static int g_gpio_trig_fd = -1;
static int g_gpio_echo_fd = -1;

static int gpio_pin_open(int pin, int flags) {
    char path[32];
    snprintf(path, sizeof(path), GPIO_DEV_PATH_FMT, pin);
    int fd = open(path, flags);
    if (fd < 0) {
        LOG_ERR("gpio_pin_open: open(%s) failed: %s", path, strerror(errno));
    }
    return fd;
}

/* Drives an output pin to logical 0/1. Tries ASCII '0'/'1' first (the
 * sysfs-style convention), falls back to a raw binary byte if that write
 * is rejected. Returns 0 on success, -1 on failure. */
static int gpio_pin_write(int fd, int value) {
    char ascii = value ? '1' : '0';
    if (write(fd, &ascii, 1) == 1) return 0;

    char raw = (char)(value ? 1 : 0);
    if (write(fd, &raw, 1) == 1) return 0;

    return -1;
}

/* Samples an input pin's current logical level (0/1), or -1 on error.
 * lseek back to the start before each read — a standard idiom for polling
 * single-value character/sysfs-style device files repeatedly without
 * reopening; harmless if this particular driver doesn't need it. */
static int gpio_pin_read(int fd) {
    lseek(fd, 0, SEEK_SET);
    unsigned char b;
    ssize_t n = read(fd, &b, 1);
    if (n != 1) return -1;
    if (b == '0') return 0;
    if (b == '1') return 1;
    return b ? 1 : 0;
}

static int gpio_hw_init(void) {
    g_gpio_trig_fd = gpio_pin_open(GPIO_TRIG_PIN, O_WRONLY);
    if (g_gpio_trig_fd < 0) return -1;

    g_gpio_echo_fd = gpio_pin_open(GPIO_ECHO_PIN, O_RDONLY);
    if (g_gpio_echo_fd < 0) {
        close(g_gpio_trig_fd);
        g_gpio_trig_fd = -1;
        return -1;
    }

    gpio_pin_write(g_gpio_trig_fd, 0); /* ensure TRIG idles low */

    LOG_INFO("GPIO initialized: TRIG=/dev/gpio%d (fd=%d), ECHO=/dev/gpio%d (fd=%d)",
              GPIO_TRIG_PIN, g_gpio_trig_fd, GPIO_ECHO_PIN, g_gpio_echo_fd);
    return 0;
}

static void gpio_hw_cleanup(void) {
    if (g_gpio_trig_fd >= 0) { close(g_gpio_trig_fd); g_gpio_trig_fd = -1; }
    if (g_gpio_echo_fd >= 0) { close(g_gpio_echo_fd); g_gpio_echo_fd = -1; }
}

static double timespec_diff_us(const struct timespec *start, const struct timespec *end) {
    return (double)(end->tv_sec - start->tv_sec) * 1e6 +
           (double)(end->tv_nsec - start->tv_nsec) / 1e3;
}

/* Pulses TRIG high for 10us, then times how long ECHO stays high
 * (microseconds), which is proportional to round-trip distance. Returns -1
 * on failure/timeout (e.g. nothing in range, or a wiring problem). */
static double gpio_hw_read_echo_pulse_us(void) {
    if (g_gpio_trig_fd < 0 || g_gpio_echo_fd < 0) return -1.0;

    gpio_pin_write(g_gpio_trig_fd, 1);
    struct timespec pulse = { .tv_sec = 0, .tv_nsec = HCSR04_TRIG_PULSE_US * 1000L };
    nanosleep(&pulse, NULL);
    gpio_pin_write(g_gpio_trig_fd, 0);

    struct timespec t0, t_now, echo_start;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Wait for ECHO to go high (start of the return pulse). */
    int level;
    do {
        level = gpio_pin_read(g_gpio_echo_fd);
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        if (timespec_diff_us(&t0, &t_now) > HCSR04_ECHO_TIMEOUT_US) return -1.0;
    } while (level != 1);
    echo_start = t_now;

    /* Wait for ECHO to go low again (end of the return pulse). */
    do {
        level = gpio_pin_read(g_gpio_echo_fd);
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        if (timespec_diff_us(&echo_start, &t_now) > HCSR04_ECHO_TIMEOUT_US) return -1.0;
    } while (level != 0);

    return timespec_diff_us(&echo_start, &t_now);
}

static int gpio_sim_seeded = 0;
static double gpio_sim_next_distance_cm(void) {
    if (!gpio_sim_seeded) { srand((unsigned int)time(NULL)); gpio_sim_seeded = 1; }
    /* Mostly far away (no intruder), occasionally dips under 30cm so the
       alert + camera pipeline can be exercised end-to-end in simulation. */
    int roll = rand() % 100;
    if (roll < 8) {
        return 5.0 + (rand() % 25); /* 5–29 cm: triggers proximity alert */
    }
    return 60.0 + (rand() % 200); /* 60–259 cm: normal */
}

/* Returns distance in cm, or -1.0 on read failure. */
static double gpio_read_distance_cm(void) {
    if (g_gpio_sim) {
        return gpio_sim_next_distance_cm();
    }
    double pulse_us = gpio_hw_read_echo_pulse_us();
    if (pulse_us < 0) return -1.0;
    /* Speed of sound ~343 m/s => 0.0343 cm/us; divide by 2 for round trip. */
    return (pulse_us * 0.0343) / 2.0;
}

/* =========================================================================
 * CAMERA + OPENCV TRIGGER
 * ---------------------------------------------------------------------------
 * Satisfies the QNX hard requirement to use open-source AI from
 * oss.qnx.com — we use OpenCV. Rather than link OpenCV's C++ API directly
 * into this hub (adds significant build complexity mid-hackathon), we
 * shell out to a small helper (hub/opencv_classify.py) that captures a
 * frame and prints one of: person | object | none. See that file for the
 * actual OpenCV usage.
 * ========================================================================= */

/* Runs the classifier command with a timeout, returns malloc'd string
 * (caller frees) with the trimmed stdout, or NULL on failure/timeout. */
static char *run_classifier_with_timeout(const char *cmd, int timeout_sec) {
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        LOG_ERR("run_classifier_with_timeout: popen('%s') failed: %s", cmd, strerror(errno));
        return NULL;
    }

    int fd = fileno(fp);
    fd_set rfds;
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    char buf[128] = {0};
    int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (sel > 0 && FD_ISSET(fd, &rfds)) {
        if (fgets(buf, sizeof(buf), fp) == NULL) buf[0] = '\0';
    } else {
        LOG_WARN("OpenCV classifier timed out after %ds", timeout_sec);
    }
    pclose(fp);

    /* trim trailing whitespace/newline */
    size_t len = strlen(buf);
    while (len > 0 && isspace((unsigned char)buf[len - 1])) buf[--len] = '\0';
    if (len == 0) return NULL;

    char *result = malloc(len + 1);
    if (result) memcpy(result, buf, len + 1);
    return result;
}

static void trigger_camera_and_classify(const char *node_id, double distance_cm) {
    LOG_INFO("Perimeter breach on %s (%.1fcm) — triggering camera + OpenCV", node_id, distance_cm);

    char *classification = run_classifier_with_timeout(g_opencv_cmd, CAMERA_TIMEOUT_SEC);
    const char *cls = classification ? classification : "unknown";

    push_alert(node_id, "proximity_alert", cls,
               "Perimeter breach detected — distance %.1fcm, classified as %s",
               distance_cm, cls);

    free(classification);
}

/* =========================================================================
 * THRESHOLD / ANOMALY DETECTION
 * ========================================================================= */

static void check_thresholds_and_alert(node_state_t *n) {
    if (n->has_temperature) {
        if (n->temperature_c > TEMP_HIGH_C) {
            push_alert(n->node_id, "temp_high", NULL,
                       "Heat anomaly detected at %s, %s (%.1f\xC2\xB0" "C)",
                       n->node_id, n->location, n->temperature_c);
        } else if (n->temperature_c < TEMP_LOW_C) {
            push_alert(n->node_id, "temp_low", NULL,
                       "Extreme cold warning at %s, %s (%.1f\xC2\xB0" "C)",
                       n->node_id, n->location, n->temperature_c);
        }
        if (n->humidity_pct > HUMIDITY_HIGH_PCT) {
            push_alert(n->node_id, "humidity_high", NULL,
                       "High humidity alert at %s, %s — possible moisture intrusion (%.0f%%)",
                       n->node_id, n->location, n->humidity_pct);
        }
    }

    if (n->has_distance) {
        n->proximity_alert = (n->distance_cm < PROXIMITY_THRESHOLD_CM);
        if (n->proximity_alert) {
            trigger_camera_and_classify(n->node_id, n->distance_cm);
        }
    }
}

/* =========================================================================
 * HTTP SERVER
 * ========================================================================= */

typedef struct {
    char method[8];
    char path[256];
    char body[REQ_BUF_SIZE];
    size_t content_length;
} http_request_t;

/* strcasestr isn't guaranteed available on every libc (including some QNX
 * configurations) — small local implementation used below. */
static char *strcasestr_local(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) return (char *)p;
    }
    return NULL;
}

/* Very small HTTP/1.1 request reader: reads until it has the header block,
 * parses Content-Length, then reads that many more body bytes. Good enough
 * for small JSON POSTs and header-only GETs from Flask/ESP32. */
static int http_read_request(int fd, http_request_t *req) {
    char buf[REQ_BUF_SIZE];
    size_t total = 0;
    ssize_t r;
    char *header_end = NULL;

    while (total + 1 < sizeof(buf)) {
        r = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (r <= 0) break;
        total += (size_t)r;
        buf[total] = '\0';
        header_end = strstr(buf, "\r\n\r\n");
        if (header_end) break;
    }
    if (!header_end) return -1;

    memset(req, 0, sizeof(*req));
    if (sscanf(buf, "%7s %255s", req->method, req->path) != 2) return -1;

    size_t content_length = 0;
    const char *cl = strcasestr_local(buf, "Content-Length:");
    if (cl) content_length = (size_t)strtoul(cl + strlen("Content-Length:"), NULL, 10);
    req->content_length = content_length;

    size_t header_len = (size_t)(header_end - buf) + 4;
    size_t body_have = total - header_len;
    size_t body_need = content_length > body_have ? content_length - body_have : 0;

    size_t copy_len = body_have < sizeof(req->body) - 1 ? body_have : sizeof(req->body) - 1;
    memcpy(req->body, buf + header_len, copy_len);
    size_t body_total = copy_len;

    while (body_need > 0 && body_total + 1 < sizeof(req->body)) {
        r = recv(fd, req->body + body_total, sizeof(req->body) - 1 - body_total, 0);
        if (r <= 0) break;
        body_total += (size_t)r;
        body_need = (content_length > body_total) ? content_length - body_total : 0;
    }
    req->body[body_total] = '\0';
    return 0;
}

static void http_send_response(int fd, int status, const char *status_text,
                                const char *content_type, const char *body) {
    char header[256];
    size_t body_len = body ? strlen(body) : 0;
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body_len);
    if (n > 0) send(fd, header, (size_t)n, 0);
    if (body_len > 0) send(fd, body, body_len, 0);
}

static void http_send_json(int fd, int status, const char *status_text, const char *json_body) {
    http_send_response(fd, status, status_text, "application/json", json_body);
}

/* -------------------------------------------------------------------------
 * Route: POST /sensor-data  (alias: /data)
 * Expects a flat JSON body from ESP32 Node 1, e.g.:
 *   {"node_id":"node_1","temperature":22.4,"humidity":65,"distance_cm":120}
 * "location" is optional; known node_ids fall back to their documented
 * Arctic coordinates.
 * ---------------------------------------------------------------------- */
static void handle_post_sensor_data(int fd, const http_request_t *req) {
    char node_id[32];
    if (!json_get_string(req->body, "node_id", node_id, sizeof(node_id))) {
        snprintf(node_id, sizeof(node_id), "node_1"); /* MVP default: only real node */
    }

    double temperature = 0, humidity = 0, distance = 0;
    int has_temp = json_get_double(req->body, "temperature", &temperature);
    int has_hum  = json_get_double(req->body, "humidity", &humidity);
    int has_dist = json_get_double(req->body, "distance_cm", &distance);

    /* Some ESP32 firmware revisions compute their own proximity flag
     * client-side; we don't strictly need it since the hub re-evaluates the
     * 30cm threshold itself below, but we log a mismatch if the node
     * disagrees with us — useful for catching clock/threshold drift during
     * debug. */
    int node_reported_proximity = 0;
    if (json_get_bool(req->body, "proximity_alert", &node_reported_proximity)) {
        if (has_dist && node_reported_proximity != (distance < PROXIMITY_THRESHOLD_CM)) {
            LOG_WARN("Node %s proximity flag (%d) disagrees with hub's own threshold check for distance %.1fcm",
                      node_id, node_reported_proximity, distance);
        }
    }

    if (!has_temp && !has_hum && !has_dist) {
        http_send_json(fd, 400, "Bad Request", "{\"error\":\"no recognized sensor fields\"}");
        return;
    }

    pthread_mutex_lock(&g_nodes_lock);
    node_state_t *n = find_or_create_node_locked(node_id);
    if (!n) {
        pthread_mutex_unlock(&g_nodes_lock);
        LOG_ERR("Node table full — dropping reading from %s", node_id);
        http_send_json(fd, 503, "Service Unavailable", "{\"error\":\"node table full\"}");
        return;
    }

    char loc[48];
    if (json_get_string(req->body, "location", loc, sizeof(loc))) {
        snprintf(n->location, sizeof(n->location), "%s", loc);
    }
    if (has_temp && has_hum) {
        n->temperature_c = temperature;
        n->humidity_pct = humidity;
        n->has_temperature = 1;
    }
    if (has_dist) {
        n->distance_cm = distance;
        n->has_distance = 1;
    }
    n->last_seen = time(NULL);

    node_state_t snapshot = *n;
    pthread_mutex_unlock(&g_nodes_lock);

    LOG_INFO("POST /sensor-data node=%s temp=%.1f hum=%.1f dist=%.1f",
              node_id, snapshot.temperature_c, snapshot.humidity_pct, snapshot.distance_cm);

    check_thresholds_and_alert(&snapshot);
    influx_write_sensor_reading(&snapshot);

    /* reflect proximity_alert flag possibly set inside check_thresholds */
    pthread_mutex_lock(&g_nodes_lock);
    n = find_or_create_node_locked(node_id);
    if (n) n->proximity_alert = snapshot.proximity_alert;
    pthread_mutex_unlock(&g_nodes_lock);

    http_send_json(fd, 200, "OK", "{\"status\":\"ok\"}");
}

/* -------------------------------------------------------------------------
 * Route: GET /nodes
 * ---------------------------------------------------------------------- */
static void handle_get_nodes(int fd) {
    char json[4096];
    size_t off = 0;
    off += (size_t)snprintf(json + off, sizeof(json) - off, "[");

    pthread_mutex_lock(&g_nodes_lock);
    time_t now = time(NULL);
    int first = 1;
    for (int i = 0; i < MAX_NODES; i++) {
        if (!g_nodes[i].in_use) continue;
        node_state_t *n = &g_nodes[i];
        int online = (now - n->last_seen) < NODE_OFFLINE_SEC;

        if (!online && n->last_seen != 0) {
            /* Fire (deduped/cooled-down) offline alert lazily on read. */
            pthread_mutex_unlock(&g_nodes_lock);
            push_alert(n->node_id, "node_offline", NULL,
                       "Node %s offline — no data for over %ds, store-and-forward assumed active",
                       n->node_id, NODE_OFFLINE_SEC);
            pthread_mutex_lock(&g_nodes_lock);
        }

        if (!first) off += (size_t)snprintf(json + off, sizeof(json) - off, ",");
        first = 0;
        off += (size_t)snprintf(json + off, sizeof(json) - off,
            "{\"node_id\":\"%s\",\"location\":\"%s\",\"lat\":%.4f,\"lon\":%.4f,"
            "\"temperature_c\":%.1f,\"humidity_pct\":%.1f,\"distance_cm\":%.1f,"
            "\"proximity_alert\":%s,\"online\":%s,\"last_seen\":%ld}",
            n->node_id, n->location, n->lat, n->lon,
            n->temperature_c, n->humidity_pct, n->distance_cm,
            n->proximity_alert ? "true" : "false",
            online ? "true" : "false",
            (long)n->last_seen);

        if (off > sizeof(json) - 256) break; /* leave room, avoid overflow on many nodes */
    }
    pthread_mutex_unlock(&g_nodes_lock);

    off += (size_t)snprintf(json + off, sizeof(json) - off, "]");
    http_send_json(fd, 200, "OK", json);
}

/* -------------------------------------------------------------------------
 * Route: GET /alerts  — most recent first, capped to avoid huge payloads.
 * ---------------------------------------------------------------------- */
#define ALERTS_RESPONSE_LIMIT 50

static void handle_get_alerts(int fd) {
    char json[8192];
    size_t off = 0;
    off += (size_t)snprintf(json + off, sizeof(json) - off, "[");

    pthread_mutex_lock(&g_alerts_lock);
    int count = g_alert_count;
    int returned = 0;
    for (int k = 0; k < count && returned < ALERTS_RESPONSE_LIMIT; k++) {
        int idx = (g_alert_head - 1 - k + MAX_ALERTS) % MAX_ALERTS;
        alert_t *a = &g_alerts[idx];
        if (returned > 0) off += (size_t)snprintf(json + off, sizeof(json) - off, ",");
        off += (size_t)snprintf(json + off, sizeof(json) - off,
            "{\"id\":%ld,\"node_id\":\"%s\",\"type\":\"%s\",\"message\":\"%s\","
            "\"classification\":\"%s\",\"timestamp\":%ld}",
            a->id, a->node_id, a->type, a->message, a->classification, (long)a->timestamp);
        returned++;
        if (off > sizeof(json) - 512) break;
    }
    pthread_mutex_unlock(&g_alerts_lock);

    off += (size_t)snprintf(json + off, sizeof(json) - off, "]");
    http_send_json(fd, 200, "OK", json);
}

/* -------------------------------------------------------------------------
 * Connection dispatcher (runs in its own thread per connection).
 * ---------------------------------------------------------------------- */
static void *handle_client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;

    http_request_t req;
    if (http_read_request(fd, &req) != 0) {
        http_send_json(fd, 400, "Bad Request", "{\"error\":\"malformed request\"}");
        close(fd);
        return NULL;
    }

    if (strcmp(req.method, "POST") == 0 &&
        (strcmp(req.path, "/sensor-data") == 0 || strcmp(req.path, "/data") == 0)) {
        handle_post_sensor_data(fd, &req);
    } else if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/nodes") == 0) {
        handle_get_nodes(fd);
    } else if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/alerts") == 0) {
        handle_get_alerts(fd);
    } else if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/health") == 0) {
        http_send_json(fd, 200, "OK", "{\"status\":\"sentinel-hub-up\"}");
    } else {
        http_send_json(fd, 404, "Not Found", "{\"error\":\"unknown route\"}");
    }

    close(fd);
    return NULL;
}

static int g_listen_fd = -1;

static void run_http_server(void) {
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        LOG_ERR("socket() failed: %s", strerror(errno));
        exit(1);
    }

    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)g_http_port);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_ERR("bind() to port %d failed: %s", g_http_port, strerror(errno));
        exit(1);
    }
    if (listen(g_listen_fd, 16) != 0) {
        LOG_ERR("listen() failed: %s", strerror(errno));
        exit(1);
    }

    LOG_INFO("Sentinel hub HTTP server listening on :%d "
              "(POST /sensor-data, GET /nodes, GET /alerts, GET /health)", g_http_port);

    while (!g_shutdown) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(g_listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (g_shutdown) break;
            LOG_WARN("accept() failed: %s", strerror(errno));
            continue;
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client_thread, (void *)(intptr_t)client_fd) != 0) {
            LOG_ERR("pthread_create failed, handling inline");
            handle_client_thread((void *)(intptr_t)client_fd);
        } else {
            pthread_detach(tid);
        }
    }
}

/* =========================================================================
 * BACKGROUND: HC-SR04 POLLING THREAD
 * ========================================================================= */

static void *gpio_poll_thread(void *arg) {
    (void)arg;
    LOG_INFO("HC-SR04 polling thread started (interval=%dms, sim=%s)",
              GPIO_POLL_INTERVAL_MS, g_gpio_sim ? "yes" : "no");

    while (!g_shutdown) {
        double distance = gpio_read_distance_cm();
        if (distance >= 0) {
            pthread_mutex_lock(&g_nodes_lock);
            node_state_t *n = find_or_create_node_locked("node_1");
            node_state_t snapshot;
            if (n) {
                n->distance_cm = distance;
                n->has_distance = 1;
                n->last_seen = time(NULL);
                snapshot = *n;
            }
            pthread_mutex_unlock(&g_nodes_lock);

            if (n) {
                check_thresholds_and_alert(&snapshot);
                influx_write_sensor_reading(&snapshot);

                pthread_mutex_lock(&g_nodes_lock);
                node_state_t *n2 = find_or_create_node_locked("node_1");
                if (n2) n2->proximity_alert = snapshot.proximity_alert;
                pthread_mutex_unlock(&g_nodes_lock);
            }
        }

        usleep(GPIO_POLL_INTERVAL_MS * 1000);
    }
    return NULL;
}

/* =========================================================================
 * SIGNAL HANDLING / SHUTDOWN
 * ========================================================================= */

static void handle_sigint(int sig) {
    (void)sig;
    g_shutdown = 1;
    if (g_listen_fd >= 0) close(g_listen_fd);
}

/* =========================================================================
 * CONFIG LOADING
 * ========================================================================= */

static void load_config(void) {
    const char *v;

    v = getenv("SENTINEL_HTTP_PORT");
    g_http_port = v ? atoi(v) : DEFAULT_HTTP_PORT;

    v = getenv("INFLUX_HOST");
    snprintf(g_influx_host, sizeof(g_influx_host), "%s", v ? v : DEFAULT_INFLUX_HOST);

    v = getenv("INFLUX_PORT");
    g_influx_port = v ? atoi(v) : DEFAULT_INFLUX_PORT;

    v = getenv("INFLUX_ORG");
    snprintf(g_influx_org, sizeof(g_influx_org), "%s", v ? v : DEFAULT_INFLUX_ORG);

    v = getenv("INFLUX_BUCKET");
    snprintf(g_influx_bucket, sizeof(g_influx_bucket), "%s", v ? v : DEFAULT_INFLUX_BUCKET);

    v = getenv("INFLUX_TOKEN");
    snprintf(g_influx_token, sizeof(g_influx_token), "%s", v ? v : "");

    v = getenv("SENTINEL_OPENCV_CMD");
    snprintf(g_opencv_cmd, sizeof(g_opencv_cmd), "%s", v ? v : DEFAULT_OPENCV_CMD);

    v = getenv("SENTINEL_GPIO_SIM");
    g_gpio_sim = v ? atoi(v) : 0; /* real HC-SR04 GPIO by default; set
                                     SENTINEL_GPIO_SIM=1 to force simulation */

    if (!g_gpio_sim) {
        if (gpio_hw_init() != 0) {
            LOG_WARN("Real GPIO init failed — forcing simulation mode so the "
                      "rest of the pipeline still runs.");
            g_gpio_sim = 1;
        }
    }

    if (g_influx_token[0] == '\0') {
        LOG_WARN("INFLUX_TOKEN is not set. InfluxDB writes will be skipped "
                  "(logged only) until you export INFLUX_TOKEN=<your token>.");
    }
}

/* =========================================================================
 * MAIN
 * ========================================================================= */

int main(void) {
    load_config();

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    LOG_INFO("Sentinel hub starting — HTTP :%d, InfluxDB %s:%d (bucket=%s), GPIO sim=%s",
              g_http_port, g_influx_host, g_influx_port, g_influx_bucket,
              g_gpio_sim ? "on" : "off");

    pthread_t gpio_tid;
    if (pthread_create(&gpio_tid, NULL, gpio_poll_thread, NULL) != 0) {
        LOG_ERR("Failed to start GPIO polling thread: %s", strerror(errno));
        return 1;
    }
    pthread_detach(gpio_tid);

    run_http_server(); /* blocks until g_shutdown */

    gpio_hw_cleanup();
    LOG_INFO("Shutting down.");
    return 0;
}
