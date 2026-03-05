/*
 * ============================================================
 *  SafetyHub.ino  —  Campus Emergency & Multi-Hazard Alert
 *  ESP32 DevKit V1
 *
 *  Sensors:
 *    - MPU6050   (I2C, vibration / seismic)
 *    - MQ2       (ADC pin 34, gas level)
 *    - DHT11     (GPIO 4, temp + humidity)
 *
 *  Architecture (v2 - STA mode, direct cloud push):
 *    ESP32 connects to Mac hotspot WiFi.
 *    Every 2s, POSTs sensor JSON to cloud relay:
 *      POST https://safetyhub-relay.onrender.com/ingest
 *
 *  No laptop gateway needed. ESP32 → Cloud → Vercel directly.
 * ============================================================
 */

#include <DHT.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Wire.h>
// esp_task_wdt.h removed — causes TWDT errors on ESP32 core v3.x
#include <esp_wifi.h>
#include <math.h>

// ── Pin / bus config ────────────────────────────────────────
#define DHT_PIN 4
#define DHT_TYPE DHT11
#define MQ2_PIN 34
#define SDA_PIN 21
#define SCL_PIN 22
#define MPU6050_ADDR 0x68

// ── WiFi STA credentials (Mac hotspot) ─────────────────────
const char *STA_SSID = "campussafety";
const char *STA_PASSWORD = "campussafety";

// ── Cloud relay endpoint ─────────────────────────────────────
const char *RELAY_URL = "https://safetyhub-relay.onrender.com/ingest";

// ── Thresholds ───────────────────────────────────────────────
const float TEMP_DANGER = 45.0f;
const int GAS_DANGER = 800;
const float VIBRATION_DANGER = 2.5f;

// ── Timing (ms) ─────────────────────────────────────────────
const uint32_t PUSH_INTERVAL = 2000;  // push to cloud every 2s
const uint32_t SENSOR_INTERVAL = 500; // read sensors every 500ms
// WDT_TIMEOUT_SEC removed — watchdog disabled

// ── Global objects ───────────────────────────────────────────
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

// ============================================================
//  MPU6050 helpers
// ============================================================
bool mpuInit() {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  if (Wire.endTransmission(true) != 0)
    return false;
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x1C);
  Wire.write(0x00);
  Wire.endTransmission(true);
  return true;
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
  float gx = ax / 16384.0f;
  float gy = ay / 16384.0f;
  float gz = az / 16384.0f;
  float magnitude = sqrtf(gx * gx + gy * gy + gz * gz);
  float vibration = fabsf(magnitude - 1.0f);
  return vibration;
}

// ============================================================
//  Sensor reading
// ============================================================
void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t))
    sd.temperature = t;
  if (!isnan(h))
    sd.humidity = h;

  int raw = analogRead(MQ2_PIN);
  sd.gasLevel = map(raw, 0, 4095, 0, 1023);
  sd.vibration = mpuReadVibration();
  sd.valid = true;

  // Determine alert
  if (sd.temperature >= TEMP_DANGER) {
    snprintf(sd.alert, sizeof(sd.alert), "FIRE ALERT — High Temperature");
  } else if (sd.gasLevel >= GAS_DANGER) {
    snprintf(sd.alert, sizeof(sd.alert), "GAS ALERT — Dangerous Gas Level");
  } else if (sd.vibration >= VIBRATION_DANGER) {
    snprintf(sd.alert, sizeof(sd.alert), "SEISMIC ALERT — Vibration Detected");
  } else {
    snprintf(sd.alert, sizeof(sd.alert), "System Normal");
  }
}

// ============================================================
//  Push to cloud relay
// ============================================================
void pushToCloud() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Not connected — skipping push");
    return;
  }

  char body[256];
  snprintf(body, sizeof(body),
           "{\"temperature\":%.1f,\"humidity\":%.1f,\"gasLevel\":%d,"
           "\"vibration\":%.3f,\"alert\":\"%s\"}",
           sd.temperature, sd.humidity, sd.gasLevel, sd.vibration, sd.alert);

  HTTPClient http;
  http.begin(RELAY_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(4000);

  int code = http.POST(body);
  if (code > 0) {
    Serial.printf("[Cloud] POST %d — %s\n", code, sd.alert);
  } else {
    Serial.printf("[Cloud] POST failed: %s\n",
                  http.errorToString(code).c_str());
  }
  http.end();
}

// ============================================================
//  Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n[SafetyHub] Booting...");

  // Watchdog
  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = WDT_TIMEOUT_SEC * 1000,
      .trigger_panic = true,
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  // I2C for MPU6050
  Wire.begin(SDA_PIN, SCL_PIN);
  if (mpuInit()) {
    Serial.println("[SafetyHub] MPU6050 OK");
  } else {
    Serial.println("[SafetyHub] MPU6050 not found — vibration will read 0");
  }

  // DHT11
  dht.begin();
  Serial.println("[SafetyHub] DHT11 started");

  // Connect to Mac hotspot
  Serial.printf("[WiFi] Connecting to '%s'...\n", STA_SSID);
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.begin(STA_SSID, STA_PASSWORD);

  uint32_t wifiStart = millis();
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;
    if (attempts % 20 == 0) {
      Serial.printf("\n[WiFi] Still trying... status=%d\n", WiFi.status());
    }
    if (millis() - wifiStart > 30000) {
      Serial.println("\n[WiFi] TIMEOUT after 30s — restarting...");
      ESP.restart();
    }
  }

  Serial.printf("\n[WiFi] Connected! IP: %s\n",
                WiFi.localIP().toString().c_str());
  Serial.printf("[SafetyHub] Pushing to: %s\n", RELAY_URL);
  Serial.println("[SafetyHub] Ready.");
}

// ============================================================
//  Loop
// ============================================================
void loop() {
  esp_task_wdt_reset();

  uint32_t now = millis();

  // Read sensors every 500ms
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensors();
  }

  // Push to cloud every 2s
  if (sd.valid && now - lastPush >= PUSH_INTERVAL) {
    lastPush = now;
    pushToCloud();
  }

  // Reconnect if WiFi dropped
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Lost connection — reconnecting...");
    WiFi.reconnect();
    delay(1000);
  }
}
