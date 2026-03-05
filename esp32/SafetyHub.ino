/*
 * ============================================================
 *  SafetyHub.ino  —  Campus Emergency & Multi-Hazard Alert
 *  ESP32 DevKit V1  |  v3 — STA mode, direct cloud push
 *
 *  Sensors: MPU6050 (I2C), MQ2 (ADC 34), DHT11 (GPIO 4)
 *
 *  Architecture:
 *    ESP32 connects to Mac hotspot (campussafety).
 *    POSTs sensor JSON to Render cloud relay every 2s.
 *    No laptop gateway required.
 * ============================================================
 */

#include <DHT.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <esp_wifi.h>
#include <math.h>

// ── Pin config ───────────────────────────────────────────────
#define DHT_PIN 4
#define DHT_TYPE DHT11
#define MQ2_PIN 34
#define SDA_PIN 21
#define SCL_PIN 22
#define MPU6050_ADDR 0x68

// ── WiFi (Mac hotspot) ───────────────────────────────────────
const char *STA_SSID = "campussafety";
const char *STA_PASSWORD = "campussafety";

// ── Cloud relay ──────────────────────────────────────────────
const char *RELAY_URL = "https://safetyhub-relay.onrender.com/ingest";

// ── Thresholds ───────────────────────────────────────────────
const float TEMP_DANGER = 45.0f;
const int GAS_DANGER = 800;
const float VIBRATION_DANGER = 2.5f;

// ── Timing ───────────────────────────────────────────────────
const uint32_t PUSH_INTERVAL = 2000;
const uint32_t SENSOR_INTERVAL = 500;

// ── Globals ──────────────────────────────────────────────────
DHT dht(DHT_PIN, DHT_TYPE);

struct SensorData {
  float temperature;
  float humidity;
  int gasLevel;
  float vibration;
  char alert[48];
  bool valid;
} sd = {0, 0, 0, 0.0f, "Initializing", false};

uint32_t lastSensorRead = 0;
uint32_t lastPush = 0;

// ── MPU6050 ──────────────────────────────────────────────────
bool mpuInit() {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  return Wire.endTransmission(true) == 0;
}

float mpuReadVibration() {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0)
    return 0.0f;
  Wire.requestFrom((uint8_t)MPU6050_ADDR, (uint8_t)6, true);
  if (Wire.available() < 6)
    return 0.0f;
  int16_t ax = (Wire.read() << 8) | Wire.read();
  int16_t ay = (Wire.read() << 8) | Wire.read();
  int16_t az = (Wire.read() << 8) | Wire.read();
  float mag =
      sqrtf((ax / 16384.f) * (ax / 16384.f) + (ay / 16384.f) * (ay / 16384.f) +
            (az / 16384.f) * (az / 16384.f));
  return fabsf(mag - 1.0f);
}

// ── Sensor read ──────────────────────────────────────────────
void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t))
    sd.temperature = t;
  if (!isnan(h))
    sd.humidity = h;
  sd.gasLevel = map(analogRead(MQ2_PIN), 0, 4095, 0, 1023);
  sd.vibration = mpuReadVibration();
  sd.valid = true;

  if (sd.temperature >= TEMP_DANGER)
    snprintf(sd.alert, 48, "FIRE ALERT");
  else if (sd.gasLevel >= GAS_DANGER)
    snprintf(sd.alert, 48, "GAS ALERT");
  else if (sd.vibration >= VIBRATION_DANGER)
    snprintf(sd.alert, 48, "SEISMIC ALERT");
  else
    snprintf(sd.alert, 48, "System Normal");
}

// ── Cloud push ───────────────────────────────────────────────
void pushToCloud() {
  if (WiFi.status() != WL_CONNECTED)
    return;

  char body[256];
  snprintf(body, sizeof(body),
           "{\"temperature\":%.1f,\"humidity\":%.1f,\"gasLevel\":%d,"
           "\"vibration\":%.3f,\"alert\":\"%s\"}",
           sd.temperature, sd.humidity, sd.gasLevel, sd.vibration, sd.alert);

  WiFiClientSecure client;
  client.setInsecure(); // skip cert verification
  HTTPClient http;
  http.begin(client, RELAY_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);
  int code = http.POST(body);
  Serial.printf("[Cloud] POST %d | temp=%.1f hum=%.1f gas=%d vib=%.3f\n", code,
                sd.temperature, sd.humidity, sd.gasLevel, sd.vibration);
  http.end();
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n[SafetyHub] Booting v3...");

  Wire.begin(SDA_PIN, SCL_PIN);
  Serial.println(mpuInit() ? "[MPU6050] OK" : "[MPU6050] Not found");

  dht.begin();
  Serial.println("[DHT11] Started");

  Serial.printf("[WiFi] Connecting to '%s'...\n", STA_SSID);
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.begin(STA_SSID, STA_PASSWORD);

  uint32_t start = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++dots % 20 == 0)
      Serial.printf("\n[WiFi] status=%d (6=wrong pw, 1=no AP)\n",
                    WiFi.status());
    if (millis() - start > 30000) {
      Serial.println("\n[WiFi] TIMEOUT — rebooting...");
      ESP.restart();
    }
  }

  Serial.printf("\n[WiFi] Connected! IP: %s\n",
                WiFi.localIP().toString().c_str());
  Serial.println("[SafetyHub] Ready — pushing every 2s");
}

// ── Loop ─────────────────────────────────────────────────────
void loop() {
  uint32_t now = millis();

  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensors();
  }

  if (sd.valid && now - lastPush >= PUSH_INTERVAL) {
    lastPush = now;
    pushToCloud();
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Lost — reconnecting...");
    WiFi.reconnect();
    delay(1000);
  }
}
