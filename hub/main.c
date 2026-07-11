/*
 * Sentinel Hub
 *
 * QNX 8.0 base station for the Sentinel Arctic sensor network (cuHacking).
 * Receives ESP32/DHT11 readings over HTTP, polls an HC-SR04 over GPIO,
 * triggers the camera + an OpenCV classifier on proximity alerts, persists
 * everything to SQLite, and serves GET /nodes and GET /alerts for Flask.
 *
 * Build:  clang -Wall -Wextra -O2 -std=c11 -o hub main.c -lpthread -lsqlite3
 *         (see hub/Makefile)
 * Run:    ./hub
 *
 * Env vars:
 *   SENTINEL_HTTP_PORT   HTTP port                (default 8080)
 *   SENTINEL_DB_PATH     SQLite DB path            (default $HOME/sentinel/sentinel.db)
 *   SENTINEL_GPIO_SIM    force simulated HC-SR04   (default 0, real GPIO)
 *   SENTINEL_OPENCV_CMD  camera classifier command (default ./opencv_classify.py)
 *
 * GPIO note: talks to HC-SR04 over /dev/gpio23 (TRIG) / /dev/gpio24 (ECHO)
 * using a best-effort read/write protocol (see gpio_pin_read/gpio_pin_write);
 * falls back to SENTINEL_GPIO_SIM automatically if the devices won't open.
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
#include <arpa/inet.h>
#include <sys/select.h>
#include <sqlite3.h>

#define DEFAULT_HTTP_PORT        8080
#define DEFAULT_OPENCV_CMD       "./opencv_classify.py"
#define DEFAULT_DB_SUBPATH       "/sentinel/sentinel.db" /* appended to $HOME */

#define MAX_NODES                8
#define REQ_BUF_SIZE             8192

#define PROXIMITY_THRESHOLD_CM   30.0
#define TEMP_HIGH_C              35.0
#define TEMP_LOW_C               (-10.0)
#define HUMIDITY_HIGH_PCT        80.0
#define NODE_OFFLINE_SEC         10
#define GPIO_POLL_INTERVAL_MS    500
#define ALERT_COOLDOWN_SEC       15 /* min seconds between repeats of the same alert */
#define CAMERA_TIMEOUT_SEC       5
#define ALERTS_RESPONSE_LIMIT    20

static int   g_http_port      = DEFAULT_HTTP_PORT;
static char  g_db_path[512];
static char  g_opencv_cmd[256];
static int   g_gpio_sim       = 1;

static sqlite3          *g_db = NULL;
static pthread_mutex_t   g_db_lock = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t g_shutdown = 0;

/* Timestamped log line to stderr. */
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

/* Known lat/lon per node_id, from the hackathon plan. */
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

/* Finds a node by id or allocates a free slot; caller holds g_nodes_lock. */
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

typedef struct {
    char   node_id[32];
    char   type[32];        /* proximity_alert | temp_high | temp_low |
                                humidity_high | node_offline */
    char   message[256];
    char   classification[16]; /* person | object | none | "" */
    time_t timestamp;
} alert_t;

typedef struct {
    char   node_id[32];
    char   type[32];
    time_t last_fired;
} alert_cooldown_t;
static alert_cooldown_t g_cooldowns[MAX_NODES * 6];
static int g_cooldown_count = 0;
static pthread_mutex_t g_cooldown_lock = PTHREAD_MUTEX_INITIALIZER;

/* Rate-limits repeat alerts for the same (node_id, type) pair. */
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

static void db_write_alert(const alert_t *a);

/* Logs and persists an alert, subject to cooldown_ready(). */
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

    LOG_WARN("ALERT [%s] %s: %s", type, node_id, a.message);
    db_write_alert(&a);
}

/* Returns a pointer to the value after "key": in a flat JSON object, or NULL. */
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

/* Reads a numeric JSON field. */
static int json_get_double(const char *json, const char *key, double *out) {
    const char *v = json_find_value(json, key);
    if (!v) return 0;
    char *end = NULL;
    double val = strtod(v, &end);
    if (end == v) return 0;
    *out = val;
    return 1;
}

/* Reads a boolean JSON field. */
static int json_get_bool(const char *json, const char *key, int *out) {
    const char *v = json_find_value(json, key);
    if (!v) return 0;
    if (strncmp(v, "true", 4) == 0)  { *out = 1; return 1; }
    if (strncmp(v, "false", 5) == 0) { *out = 0; return 1; }
    return 0;
}

/* Reads a string JSON field. */
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

/* Opens/creates the SQLite DB, schema, and WAL mode. */
static int db_init(void) {
    int rc = sqlite3_open(g_db_path, &g_db);
    if (rc != SQLITE_OK) {
        LOG_ERR("sqlite3_open(%s) failed: %s", g_db_path,
                 g_db ? sqlite3_errmsg(g_db) : "unknown error");
        return -1;
    }

    const char *schema =
        "CREATE TABLE IF NOT EXISTS sensor_data ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  node_id TEXT NOT NULL,"
        "  location TEXT,"
        "  temperature REAL,"
        "  humidity REAL,"
        "  distance_cm REAL,"
        "  proximity_alert INTEGER,"
        "  timestamp INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_sensor_data_node_ts "
        "  ON sensor_data(node_id, timestamp);"
        "CREATE TABLE IF NOT EXISTS alerts ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  node_id TEXT NOT NULL,"
        "  type TEXT NOT NULL,"
        "  message TEXT,"
        "  classification TEXT,"
        "  timestamp INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_alerts_ts ON alerts(timestamp DESC);";

    char *errmsg = NULL;
    rc = sqlite3_exec(g_db, schema, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        LOG_ERR("sqlite3_exec(schema) failed: %s", errmsg ? errmsg : "unknown error");
        sqlite3_free(errmsg);
        return -1;
    }

    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    LOG_INFO("SQLite database ready at %s", g_db_path);
    return 0;
}

/* Closes the SQLite DB. */
static void db_close(void) {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

/* Inserts one sensor_data row for a node's current snapshot. */
static void db_write_sensor_reading(const node_state_t *n) {
    if (!g_db) return;
    if (!n->has_temperature && !n->has_distance) return;

    static const char *sql =
        "INSERT INTO sensor_data "
        "(node_id, location, temperature, humidity, distance_cm, proximity_alert, timestamp) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";

    pthread_mutex_lock(&g_db_lock);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERR("db_write_sensor_reading: prepare failed: %s", sqlite3_errmsg(g_db));
        pthread_mutex_unlock(&g_db_lock);
        return;
    }

    sqlite3_bind_text(stmt, 1, n->node_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, n->location, -1, SQLITE_TRANSIENT);
    if (n->has_temperature) {
        sqlite3_bind_double(stmt, 3, n->temperature_c);
        sqlite3_bind_double(stmt, 4, n->humidity_pct);
    } else {
        sqlite3_bind_null(stmt, 3);
        sqlite3_bind_null(stmt, 4);
    }
    if (n->has_distance) {
        sqlite3_bind_double(stmt, 5, n->distance_cm);
        sqlite3_bind_int(stmt, 6, n->proximity_alert ? 1 : 0);
    } else {
        sqlite3_bind_null(stmt, 5);
        sqlite3_bind_null(stmt, 6);
    }
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)time(NULL));

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        LOG_ERR("db_write_sensor_reading: step failed: %s", sqlite3_errmsg(g_db));
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_lock);
}

/* Inserts one alerts row. */
static void db_write_alert(const alert_t *a) {
    if (!g_db) return;

    static const char *sql =
        "INSERT INTO alerts (node_id, type, message, classification, timestamp) "
        "VALUES (?, ?, ?, ?, ?);";

    pthread_mutex_lock(&g_db_lock);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERR("db_write_alert: prepare failed: %s", sqlite3_errmsg(g_db));
        pthread_mutex_unlock(&g_db_lock);
        return;
    }

    sqlite3_bind_text(stmt, 1, a->node_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, a->type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, a->message, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, a->classification, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)a->timestamp);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        LOG_ERR("db_write_alert: step failed: %s", sqlite3_errmsg(g_db));
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_lock);
}

#define GPIO_DEV_PATH_FMT       "/dev/gpio%d"
#define GPIO_TRIG_PIN            23
#define GPIO_ECHO_PIN            24

#define HCSR04_TRIG_PULSE_US     10    /* datasheet minimum */
#define HCSR04_ECHO_TIMEOUT_US   30000 /* ~30ms; also our "nothing in range" bailout */

static int g_gpio_trig_fd = -1;
static int g_gpio_echo_fd = -1;

/* Opens /dev/gpio<pin>. */
static int gpio_pin_open(int pin, int flags) {
    char path[32];
    snprintf(path, sizeof(path), GPIO_DEV_PATH_FMT, pin);
    int fd = open(path, flags);
    if (fd < 0) {
        LOG_ERR("gpio_pin_open: open(%s) failed: %s", path, strerror(errno));
    }
    return fd;
}

/* Drives an output pin to 0/1 (ASCII, falling back to a raw byte). */
static int gpio_pin_write(int fd, int value) {
    char ascii = value ? '1' : '0';
    if (write(fd, &ascii, 1) == 1) return 0;

    char raw = (char)(value ? 1 : 0);
    if (write(fd, &raw, 1) == 1) return 0;

    return -1;
}

/* Samples an input pin's current level (0/1), or -1 on error. */
static int gpio_pin_read(int fd) {
    lseek(fd, 0, SEEK_SET);
    unsigned char b;
    ssize_t n = read(fd, &b, 1);
    if (n != 1) return -1;
    if (b == '0') return 0;
    if (b == '1') return 1;
    return b ? 1 : 0;
}

/* Opens the TRIG/ECHO GPIO devices. */
static int gpio_hw_init(void) {
    g_gpio_trig_fd = gpio_pin_open(GPIO_TRIG_PIN, O_WRONLY);
    if (g_gpio_trig_fd < 0) return -1;

    g_gpio_echo_fd = gpio_pin_open(GPIO_ECHO_PIN, O_RDONLY);
    if (g_gpio_echo_fd < 0) {
        close(g_gpio_trig_fd);
        g_gpio_trig_fd = -1;
        return -1;
    }

    gpio_pin_write(g_gpio_trig_fd, 0);

    LOG_INFO("GPIO initialized: TRIG=/dev/gpio%d (fd=%d), ECHO=/dev/gpio%d (fd=%d)",
              GPIO_TRIG_PIN, g_gpio_trig_fd, GPIO_ECHO_PIN, g_gpio_echo_fd);
    return 0;
}

/* Closes the GPIO devices. */
static void gpio_hw_cleanup(void) {
    if (g_gpio_trig_fd >= 0) { close(g_gpio_trig_fd); g_gpio_trig_fd = -1; }
    if (g_gpio_echo_fd >= 0) { close(g_gpio_echo_fd); g_gpio_echo_fd = -1; }
}

/* Microsecond delta between two timespecs. */
static double timespec_diff_us(const struct timespec *start, const struct timespec *end) {
    return (double)(end->tv_sec - start->tv_sec) * 1e6 +
           (double)(end->tv_nsec - start->tv_nsec) / 1e3;
}

/* Pulses TRIG and times the ECHO response in microseconds, or -1 on timeout. */
static double gpio_hw_read_echo_pulse_us(void) {
    if (g_gpio_trig_fd < 0 || g_gpio_echo_fd < 0) return -1.0;

    gpio_pin_write(g_gpio_trig_fd, 1);
    struct timespec pulse = { .tv_sec = 0, .tv_nsec = HCSR04_TRIG_PULSE_US * 1000L };
    nanosleep(&pulse, NULL);
    gpio_pin_write(g_gpio_trig_fd, 0);

    struct timespec t0, t_now, echo_start;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int level;
    do {
        level = gpio_pin_read(g_gpio_echo_fd);
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        if (timespec_diff_us(&t0, &t_now) > HCSR04_ECHO_TIMEOUT_US) return -1.0;
    } while (level != 1);
    echo_start = t_now;

    do {
        level = gpio_pin_read(g_gpio_echo_fd);
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        if (timespec_diff_us(&echo_start, &t_now) > HCSR04_ECHO_TIMEOUT_US) return -1.0;
    } while (level != 0);

    return timespec_diff_us(&echo_start, &t_now);
}

static int gpio_sim_seeded = 0;

/* Simulated HC-SR04 reading, occasionally dipping under the alert threshold. */
static double gpio_sim_next_distance_cm(void) {
    if (!gpio_sim_seeded) { srand((unsigned int)time(NULL)); gpio_sim_seeded = 1; }
    int roll = rand() % 100;
    if (roll < 8) {
        return 5.0 + (rand() % 25);
    }
    return 60.0 + (rand() % 200);
}

/* Returns distance in cm (real or simulated), or -1.0 on read failure. */
static double gpio_read_distance_cm(void) {
    if (g_gpio_sim) {
        return gpio_sim_next_distance_cm();
    }
    double pulse_us = gpio_hw_read_echo_pulse_us();
    if (pulse_us < 0) return -1.0;
    return (pulse_us * 0.0343) / 2.0; /* speed of sound, round trip */
}

/* Runs the OpenCV classifier command with a timeout, returns malloc'd stdout or NULL. */
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

    size_t len = strlen(buf);
    while (len > 0 && isspace((unsigned char)buf[len - 1])) buf[--len] = '\0';
    if (len == 0) return NULL;

    char *result = malloc(len + 1);
    if (result) memcpy(result, buf, len + 1);
    return result;
}

/* Runs the camera/OpenCV classifier and pushes a proximity alert with the result. */
static void trigger_camera_and_classify(const char *node_id, double distance_cm) {
    LOG_INFO("Perimeter breach on %s (%.1fcm) — triggering camera + OpenCV", node_id, distance_cm);

    char *classification = run_classifier_with_timeout(g_opencv_cmd, CAMERA_TIMEOUT_SEC);
    const char *cls = classification ? classification : "unknown";

    push_alert(node_id, "proximity_alert", cls,
               "Perimeter breach detected — distance %.1fcm, classified as %s",
               distance_cm, cls);

    free(classification);
}

/* Checks a node snapshot against temp/humidity/distance thresholds and fires alerts. */
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

typedef struct {
    char method[8];
    char path[256];
    char body[REQ_BUF_SIZE];
    size_t content_length;
} http_request_t;

/* Case-insensitive strstr, since strcasestr isn't guaranteed on every libc. */
static char *strcasestr_local(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) return (char *)p;
    }
    return NULL;
}

/* Reads headers + body of one HTTP/1.1 request. */
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

/* Writes an HTTP response with headers + body. */
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

/* Writes an HTTP response with a JSON body. */
static void http_send_json(int fd, int status, const char *status_text, const char *json_body) {
    http_send_response(fd, status, status_text, "application/json", json_body);
}

/* POST /sensor-data (alias /data): ingests an ESP32 reading, checks thresholds, persists it. */
static void handle_post_sensor_data(int fd, const http_request_t *req) {
    char node_id[32];
    if (!json_get_string(req->body, "node_id", node_id, sizeof(node_id))) {
        snprintf(node_id, sizeof(node_id), "node_1");
    }

    double temperature = 0, humidity = 0, distance = 0;
    int has_temp = json_get_double(req->body, "temperature", &temperature);
    int has_hum  = json_get_double(req->body, "humidity", &humidity);
    int has_dist = json_get_double(req->body, "distance_cm", &distance);

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
    db_write_sensor_reading(&snapshot);

    pthread_mutex_lock(&g_nodes_lock);
    n = find_or_create_node_locked(node_id);
    if (n) n->proximity_alert = snapshot.proximity_alert;
    pthread_mutex_unlock(&g_nodes_lock);

    http_send_json(fd, 200, "OK", "{\"status\":\"ok\"}");
}

typedef struct {
    char   node_id[32];
    char   location[48];
    int    has_temp;
    double temperature;
    double humidity;
    int    has_dist;
    double distance;
    int    proximity_alert;
    time_t last_seen;
} node_row_t;

#define MAX_NODE_ROWS 32

/* GET /nodes: latest reading per node, read from SQLite via correlated subqueries. */
static void handle_get_nodes(int fd) {
    static const char *sql =
        "SELECT s1.node_id,"
        "       s1.location,"
        "       (SELECT temperature FROM sensor_data s2 WHERE s2.node_id = s1.node_id"
        "          AND s2.temperature IS NOT NULL ORDER BY s2.timestamp DESC LIMIT 1),"
        "       (SELECT humidity FROM sensor_data s2 WHERE s2.node_id = s1.node_id"
        "          AND s2.humidity IS NOT NULL ORDER BY s2.timestamp DESC LIMIT 1),"
        "       (SELECT distance_cm FROM sensor_data s2 WHERE s2.node_id = s1.node_id"
        "          AND s2.distance_cm IS NOT NULL ORDER BY s2.timestamp DESC LIMIT 1),"
        "       (SELECT proximity_alert FROM sensor_data s2 WHERE s2.node_id = s1.node_id"
        "          AND s2.distance_cm IS NOT NULL ORDER BY s2.timestamp DESC LIMIT 1),"
        "       MAX(s1.timestamp)"
        "  FROM sensor_data s1"
        " GROUP BY s1.node_id;";

    node_row_t rows[MAX_NODE_ROWS];
    int row_count = 0;

    pthread_mutex_lock(&g_db_lock);
    sqlite3_stmt *stmt = NULL;
    int rc = g_db ? sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) : SQLITE_ERROR;
    if (rc != SQLITE_OK) {
        LOG_ERR("handle_get_nodes: prepare failed: %s", g_db ? sqlite3_errmsg(g_db) : "db not open");
        pthread_mutex_unlock(&g_db_lock);
        http_send_json(fd, 200, "OK", "[]");
        return;
    }

    while (row_count < MAX_NODE_ROWS && sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *node_id = sqlite3_column_text(stmt, 0);
        if (!node_id) continue;
        const unsigned char *location_db = sqlite3_column_text(stmt, 1);

        node_row_t *r = &rows[row_count++];
        memset(r, 0, sizeof(*r));
        snprintf(r->node_id, sizeof(r->node_id), "%s", (const char *)node_id);
        snprintf(r->location, sizeof(r->location), "%s", location_db ? (const char *)location_db : "");
        r->has_temp = sqlite3_column_type(stmt, 2) != SQLITE_NULL;
        r->temperature = sqlite3_column_double(stmt, 2);
        r->humidity = sqlite3_column_double(stmt, 3);
        r->has_dist = sqlite3_column_type(stmt, 4) != SQLITE_NULL;
        r->distance = sqlite3_column_double(stmt, 4);
        r->proximity_alert = sqlite3_column_int(stmt, 5);
        r->last_seen = (time_t)sqlite3_column_int64(stmt, 6);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_lock);

    char json[4096];
    size_t off = 0;
    off += (size_t)snprintf(json + off, sizeof(json) - off, "[");

    time_t now = time(NULL);
    for (int i = 0; i < row_count; i++) {
        node_row_t *r = &rows[i];

        char seed_location[48]; double lat, lon;
        seed_known_location(r->node_id, seed_location, sizeof(seed_location), &lat, &lon);
        const char *loc_display = r->location[0] ? r->location : seed_location;

        int online = (now - r->last_seen) < NODE_OFFLINE_SEC;
        if (!online) {
            push_alert(r->node_id, "node_offline", NULL,
                       "Node %s offline — no data for over %ds, store-and-forward assumed active",
                       r->node_id, NODE_OFFLINE_SEC);
        }

        if (i > 0) off += (size_t)snprintf(json + off, sizeof(json) - off, ",");
        off += (size_t)snprintf(json + off, sizeof(json) - off,
            "{\"node_id\":\"%s\",\"location\":\"%s\",\"lat\":%.4f,\"lon\":%.4f,"
            "\"temperature_c\":%.1f,\"humidity_pct\":%.1f,\"distance_cm\":%.1f,"
            "\"proximity_alert\":%s,\"online\":%s,\"last_seen\":%ld}",
            r->node_id, loc_display, lat, lon,
            r->has_temp ? r->temperature : 0.0,
            r->has_temp ? r->humidity : 0.0,
            r->has_dist ? r->distance : 0.0,
            (r->has_dist && r->proximity_alert) ? "true" : "false",
            online ? "true" : "false",
            (long)r->last_seen);

        if (off > sizeof(json) - 256) break;
    }

    off += (size_t)snprintf(json + off, sizeof(json) - off, "]");
    http_send_json(fd, 200, "OK", json);
}

/* GET /alerts: last N alerts, most recent first, read from SQLite. */
static void handle_get_alerts(int fd) {
    static const char *sql =
        "SELECT id, node_id, type, message, classification, timestamp "
        "FROM alerts ORDER BY id DESC LIMIT ?;";

    char json[8192];
    size_t off = 0;
    off += (size_t)snprintf(json + off, sizeof(json) - off, "[");

    pthread_mutex_lock(&g_db_lock);
    sqlite3_stmt *stmt = NULL;
    int rc = g_db ? sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) : SQLITE_ERROR;
    if (rc != SQLITE_OK) {
        LOG_ERR("handle_get_alerts: prepare failed: %s", g_db ? sqlite3_errmsg(g_db) : "db not open");
        pthread_mutex_unlock(&g_db_lock);
        http_send_json(fd, 200, "OK", "[]");
        return;
    }
    sqlite3_bind_int(stmt, 1, ALERTS_RESPONSE_LIMIT);

    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
        const unsigned char *node_id = sqlite3_column_text(stmt, 1);
        const unsigned char *type = sqlite3_column_text(stmt, 2);
        const unsigned char *message = sqlite3_column_text(stmt, 3);
        const unsigned char *classification = sqlite3_column_text(stmt, 4);
        sqlite3_int64 ts = sqlite3_column_int64(stmt, 5);

        if (!first) off += (size_t)snprintf(json + off, sizeof(json) - off, ",");
        first = 0;
        off += (size_t)snprintf(json + off, sizeof(json) - off,
            "{\"id\":%lld,\"node_id\":\"%s\",\"type\":\"%s\",\"message\":\"%s\","
            "\"classification\":\"%s\",\"timestamp\":%lld}",
            (long long)id,
            node_id ? (const char *)node_id : "",
            type ? (const char *)type : "",
            message ? (const char *)message : "",
            classification ? (const char *)classification : "",
            (long long)ts);

        if (off > sizeof(json) - 512) break;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_lock);

    off += (size_t)snprintf(json + off, sizeof(json) - off, "]");
    http_send_json(fd, 200, "OK", json);
}

/* Per-connection request dispatcher, run in its own thread. */
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

/* Binds :g_http_port and accepts connections until g_shutdown. */
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

/* Polls HC-SR04 every GPIO_POLL_INTERVAL_MS, checks thresholds, persists readings. */
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
                db_write_sensor_reading(&snapshot);

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

/* SIGINT/SIGTERM handler: signals shutdown and unblocks accept(). */
static void handle_sigint(int sig) {
    (void)sig;
    g_shutdown = 1;
    if (g_listen_fd >= 0) close(g_listen_fd);
}

/* Loads config from env vars and initializes GPIO. */
static void load_config(void) {
    const char *v;

    v = getenv("SENTINEL_HTTP_PORT");
    g_http_port = v ? atoi(v) : DEFAULT_HTTP_PORT;

    v = getenv("SENTINEL_DB_PATH");
    if (v) {
        snprintf(g_db_path, sizeof(g_db_path), "%s", v);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0]) home = ".";
        snprintf(g_db_path, sizeof(g_db_path), "%s%s", home, DEFAULT_DB_SUBPATH);
    }

    v = getenv("SENTINEL_OPENCV_CMD");
    snprintf(g_opencv_cmd, sizeof(g_opencv_cmd), "%s", v ? v : DEFAULT_OPENCV_CMD);

    v = getenv("SENTINEL_GPIO_SIM");
    g_gpio_sim = v ? atoi(v) : 0;

    if (!g_gpio_sim) {
        if (gpio_hw_init() != 0) {
            LOG_WARN("Real GPIO init failed — forcing simulation mode so the "
                      "rest of the pipeline still runs.");
            g_gpio_sim = 1;
        }
    }
}

int main(void) {
    load_config();

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    if (db_init() != 0) {
        LOG_ERR("Could not open/create SQLite database at %s — exiting.", g_db_path);
        return 1;
    }

    LOG_INFO("Sentinel hub starting — HTTP :%d, DB %s, GPIO sim=%s",
              g_http_port, g_db_path, g_gpio_sim ? "on" : "off");

    pthread_t gpio_tid;
    if (pthread_create(&gpio_tid, NULL, gpio_poll_thread, NULL) != 0) {
        LOG_ERR("Failed to start GPIO polling thread: %s", strerror(errno));
        return 1;
    }
    pthread_detach(gpio_tid);

    run_http_server();

    gpio_hw_cleanup();
    db_close();
    LOG_INFO("Shutting down.");
    return 0;
}
