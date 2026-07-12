#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <ArduinoJson.h>

// --- NETWORK CONFIGURATION ---
const char* ssid = "MaryiPhone";
const char* password = "maryaraujooo";

// --- QNX BASE STATION GATEWAY ---
const char* serverName = "http://172.20.10.9:8080/sensor-data";

// --- DHT11 SENSOR CONFIGURATION ---
#define DHTPIN 4          // Data pin connected to GPIO 4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// --- HC-SR04 PROXIMITY SENSOR CONFIGURATION ---
// PLAN B PIVOT: this used to be wired straight to the Pi's own GPIO
// (TRIG=23/ECHO=24 on the Pi), polled from a QNX thread. The Pi's VFS
// added too much latency for reliable microsecond-scale echo timing, so
// the sensor moved here, alongside the DHT11 on the same ESP32 — same
// pins as the ORIGINAL hackathon plan (TRIG=GPIO5, ECHO=GPIO18), just no
// longer routed through ESP-NOW/a gateway board, straight WiFi like the
// DHT11 already was.
#define TRIG_PIN 5
#define ECHO_PIN 18
#define HCSR04_TIMEOUT_US 30000UL  // ~30ms round-trip ceiling = "nothing in range"

// --- NODE IDENTITY ---
// Must be "node_1" — hub/main.c only knows Arctic coordinates for
// node_1..node_5. Any other node_id gets seeded with lat/lon 0,0.
const char* nodeId = "node_1";
const char* nodeLocation = "Inuvik";

// --- STORE-AND-FORWARD BUFFER ---
// Readings that fail to send (WiFi down or hub unreachable) are queued
// here and flushed oldest-first once the link is back, instead of lost.
struct BufferedReading {
  float temperature;
  float humidity;
  float distanceCm;
  bool hasDistance;  // false when the HC-SR04 timed out (nothing in range)
};
#define BUFFER_CAPACITY 40
BufferedReading buffer[BUFFER_CAPACITY];
int bufferCount = 0;

// Pulses TRIG and times the ECHO response. Returns distance in cm, or -1
// on timeout (nothing in range, or the sensor isn't responding) — caller
// must check for -1 rather than send it as a real reading, since a
// negative "distance" would look like an extreme proximity breach to the
// hub instead of "no echo".
float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, HCSR04_TIMEOUT_US);
  if (duration == 0) {
    return -1.0;  // pulseIn timed out
  }
  return (duration * 0.0343) / 2.0;  // speed of sound, round trip
}

// Pushes a reading into the buffer, dropping the oldest if full.
void bufferPush(float temperature, float humidity, float distanceCm, bool hasDistance) {
  if (bufferCount >= BUFFER_CAPACITY) {
    for (int i = 1; i < BUFFER_CAPACITY; i++) buffer[i - 1] = buffer[i];
    bufferCount--;
    Serial.println("Buffer full — dropped oldest reading.");
  }
  buffer[bufferCount++] = { temperature, humidity, distanceCm, hasDistance };
}

// POSTs one reading to the hub. Returns true on HTTP 2xx. distance_cm is
// only included in the JSON when hasDistance is true — omitting it (vs.
// sending a bogus negative number) keeps a sensor timeout from looking
// like a proximity breach to hub/main.c's threshold check.
bool sendReading(float temperature, float humidity, float distanceCm, bool hasDistance) {
  StaticJsonDocument<200> doc;
  doc["node_id"] = nodeId;
  doc["location"] = nodeLocation;
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;
  if (hasDistance) {
    doc["distance_cm"] = distanceCm;
  }

  String jsonRaw;
  serializeJson(doc, jsonRaw);

  HTTPClient http;
  http.begin(serverName);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  int httpResponseCode = http.POST(jsonRaw);
  bool ok = (httpResponseCode >= 200 && httpResponseCode < 300);

  if (ok) {
    Serial.print("Sent OK (");
    Serial.print(httpResponseCode);
    Serial.print("): ");
    Serial.println(jsonRaw);
  } else {
    Serial.print("POST failed, code ");
    Serial.print(httpResponseCode);
    Serial.print(" for: ");
    Serial.println(jsonRaw);
  }

  http.end();
  return ok;
}

// Flushes buffered readings oldest-first; stops at the first failure so
// the rest stay queued for the next attempt.
void flushBuffer() {
  int sent = 0;
  while (sent < bufferCount) {
    BufferedReading r = buffer[sent];
    if (!sendReading(r.temperature, r.humidity, r.distanceCm, r.hasDistance)) break;
    sent++;
  }
  if (sent > 0) {
    Serial.print("Flushed ");
    Serial.print(sent);
    Serial.println(" buffered reading(s).");
    for (int j = sent; j < bufferCount; j++) buffer[j - sent] = buffer[j];
    bufferCount -= sent;
  }
}

void setup() {
  Serial.begin(115200);
  dht.begin();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  WiFi.mode(WIFI_STA);
  Serial.print("Connecting to SSID: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.print("ESP32 Local IP: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("Failed to read from DHT11 sensor. Retrying...");
    delay(2000);
    return;
  }

  float distance = readDistanceCm();
  bool hasDistance = (distance >= 0);
  if (!hasDistance) {
    Serial.println("HC-SR04: no echo (out of range) — sending temp/humidity only this cycle.");
  }

  if (WiFi.status() != WL_CONNECTED) {
    bufferPush(temperature, humidity, distance, hasDistance);
    Serial.print("WiFi down — reading buffered (");
    Serial.print(bufferCount);
    Serial.println(" pending). Reconnecting...");
    WiFi.begin(ssid, password);
    delay(5000);
    return;
  }

  flushBuffer();

  if (!sendReading(temperature, humidity, distance, hasDistance)) {
    bufferPush(temperature, humidity, distance, hasDistance);
    Serial.print("Reading buffered (");
    Serial.print(bufferCount);
    Serial.println(" pending).");
  }

  // 1s cadence — hub/main.c's radar classifier wants a window of the last
  // 10 readings to mean something time-wise, and this sits right at
  // DHT11's own minimum ~1s sampling interval (don't go faster than this
  // or the DHT11 reads start failing).
  delay(1000);
}
