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
};
#define BUFFER_CAPACITY 40
BufferedReading buffer[BUFFER_CAPACITY];
int bufferCount = 0;

// Pushes a reading into the buffer, dropping the oldest if full.
void bufferPush(float temperature, float humidity) {
  if (bufferCount >= BUFFER_CAPACITY) {
    for (int i = 1; i < BUFFER_CAPACITY; i++) buffer[i - 1] = buffer[i];
    bufferCount--;
    Serial.println("Buffer full — dropped oldest reading.");
  }
  buffer[bufferCount++] = { temperature, humidity };
}

// POSTs one reading to the hub. Returns true on HTTP 2xx.
bool sendReading(float temperature, float humidity) {
  StaticJsonDocument<200> doc;
  doc["node_id"] = nodeId;
  doc["location"] = nodeLocation;
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;

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
    if (!sendReading(buffer[sent].temperature, buffer[sent].humidity)) break;
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

  if (WiFi.status() != WL_CONNECTED) {
    bufferPush(temperature, humidity);
    Serial.print("WiFi down — reading buffered (");
    Serial.print(bufferCount);
    Serial.println(" pending). Reconnecting...");
    WiFi.begin(ssid, password);
    delay(5000);
    return;
  }

  flushBuffer();

  if (!sendReading(temperature, humidity)) {
    bufferPush(temperature, humidity);
    Serial.print("Reading buffered (");
    Serial.print(bufferCount);
    Serial.println(" pending).");
  }

  delay(5000);
}
