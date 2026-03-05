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
 *  Architecture:
 *    ESP32 runs a WiFi Access Point + HTTP server.
 *    Clients connect to "SafetyHub" SSID and poll:
 *      GET http://192.168.4.1/api/data
 *
 *  Stability fixes applied:
 *    1. WiFi power-save disabled (WIFI_PS_NONE)
 *    2. AP-mode only — no STA, no association timeouts
 *    3. Sensor reads are non-blocking (timed intervals)
 *    4. WebServer.handleClient() called every loop tick
 *    5. Watchdog timer enabled (60 s)
 *    6. JSON built with char[] — no String heap fragmentation
 *    7. MPU6050 read uses raw registers — no heavy library malloc
 *    8. CORS header added so browser dashboards work directly
 * ============================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <DHT.h>
#include <math.h>
#include <esp_task_wdt.h>   // hardware watchdog
#include <esp_wifi.h>       // esp_wifi_set_ps() — required in core v3.x

// ── Pin / bus config ────────────────────────────────────────
#define DHT_PIN        4
#define DHT_TYPE       DHT11
#define MQ2_PIN        34     // ADC1 channel (must be ADC1 for WiFi coexistence)
#define SDA_PIN        21
#define SCL_PIN        22
#define MPU6050_ADDR   0x68

// ── WiFi AP credentials ─────────────────────────────────────
const char* AP_SSID     = "SafetyHub";
const char* AP_PASSWORD = "safetyhub123";   // min 8 chars for WPA2; use "" for open

// ── Thresholds ───────────────────────────────────────────────
const float  TEMP_DANGER      = 45.0f;
const int    GAS_DANGER       = 800;
const float  VIBRATION_DANGER = 2.5f;

// ── Timing (ms) ─────────────────────────────────────────────
const uint32_t SENSOR_INTERVAL = 500;   // read sensors every 500 ms
const uint32_t WDT_TIMEOUT_SEC = 60;    // watchdog resets ESP32 if loop stalls

// ── Global objects ───────────────────────────────────────────
DHT       dht(DHT_PIN, DHT_TYPE);
WebServer server(80);

// ── Sensor value cache (written by readSensors, read by handleAPI) ──
struct SensorData {
  float    temperature;
  float    humidity;
  int      gasLevel;
  float    vibration;       // resultant G-force (0-based baseline subtracted)
  char     alert[48];       // fixed-size, no heap allocation
  bool     valid;           // false until first successful read
} sensorCache = {0, 0, 0, 0.0f, "Initializing", false};

uint32_t lastSensorRead = 0;

// ============================================================
//  MPU6050 helpers — raw I2C, no library overhead
// ============================================================
bool mpuInit() {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x6B);   // PWR_MGMT_1
  Wire.write(0x00);   // wake up, internal 8 MHz oscillator
  if (Wire.endTransmission(true) != 0) return false;

  // Set accel full-scale to ±8 g  (0x10 → AFS_SEL=1)
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x1C);
  Wire.write(0x10);
  Wire.endTransmission(true);

  // DLPF bandwidth 44 Hz — reduces noise on seismic readings
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x1A);
  Wire.write(0x03);
  Wire.endTransmission(true);

  return true;
}

// Returns resultant acceleration magnitude minus 1 g (gravity removed)
float mpuReadVibration() {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x3B);   // ACCEL_XOUT_H
  if (Wire.endTransmission(false) != 0) return -1.0f;
  if (Wire.requestFrom(MPU6050_ADDR, 6, true) < 6) return -1.0f;

  int16_t ax = (Wire.read() << 8) | Wire.read();
  int16_t ay = (Wire.read() << 8) | Wire.read();
  int16_t az = (Wire.read() << 8) | Wire.read();

  // ±8 g scale factor: 32768 / 8 = 4096 LSB/g
  float gx = ax / 4096.0f;
  float gy = ay / 4096.0f;
  float gz = az / 4096.0f;

  // Resultant magnitude
  float mag = sqrtf(gx*gx + gy*gy + gz*gz);

  // Subtract 1 g (gravity baseline); clamp to 0
  float vib = mag - 1.0f;
  if (vib < 0.0f) vib = 0.0f;

  return vib;
}

// ============================================================
//  Sensor read (called on interval, non-blocking)
// ============================================================
void readSensors() {
  // ── DHT11 ────────────────────────────────────────────────
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) sensorCache.temperature = t;
  if (!isnan(h)) sensorCache.humidity    = h;

  // ── MQ2 (raw ADC → integer 0-4095) ───────────────────────
  // Take 4-sample average to reduce ADC noise
  int adcSum = 0;
  for (int i = 0; i < 4; i++) {
    adcSum += analogRead(MQ2_PIN);
    delayMicroseconds(200);
  }
  sensorCache.gasLevel = adcSum / 4;

  // ── MPU6050 ───────────────────────────────────────────────
  float vib = mpuReadVibration();
  if (vib >= 0.0f) sensorCache.vibration = vib;

  // ── Alert logic ──────────────────────────────────────────
  if (sensorCache.vibration >= VIBRATION_DANGER) {
    snprintf(sensorCache.alert, sizeof(sensorCache.alert),
             "Earthquake Detected! %.2fG", sensorCache.vibration);
  } else if (sensorCache.gasLevel >= GAS_DANGER) {
    snprintf(sensorCache.alert, sizeof(sensorCache.alert),
             "Gas Leak Detected! Level: %d", sensorCache.gasLevel);
  } else if (sensorCache.temperature >= TEMP_DANGER) {
    snprintf(sensorCache.alert, sizeof(sensorCache.alert),
             "High Temperature! %.1fC", sensorCache.temperature);
  } else {
    strncpy(sensorCache.alert, "System Normal", sizeof(sensorCache.alert));
  }

  sensorCache.valid = true;
}

// ============================================================
//  HTTP handlers
// ============================================================

// /api/data  — returns JSON sensor snapshot
void handleApiData() {
  // CORS — allows browser dashboards on any origin
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Cache-Control",                "no-cache");

  if (!sensorCache.valid) {
    server.send(503, "application/json",
                "{\"error\":\"Sensors initializing, retry in 1s\"}");
    return;
  }

  // Build JSON into a fixed char buffer — avoids String heap churn
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{"
      "\"temperature\":%.1f,"
      "\"humidity\":%.1f,"
      "\"gasLevel\":%d,"
      "\"vibration\":%.3f,"
      "\"alert\":\"%s\""
    "}",
    sensorCache.temperature,
    sensorCache.humidity,
    sensorCache.gasLevel,
    sensorCache.vibration,
    sensorCache.alert
  );

  server.send(200, "application/json", buf);
}

// OPTIONS pre-flight (CORS)
void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

// 404 fallback
void handleNotFound() {
  server.send(404, "application/json", "{\"error\":\"Not found\"}");
}

// ============================================================
//  setup()
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[SafetyHub] Booting...");

  // ── Watchdog (ESP32 Arduino core v3.x API) ───────────────
  // v3.x changed esp_task_wdt_init() to accept a config struct
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms     = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,    // don't watch idle tasks
    .trigger_panic  = true  // hard-reset on timeout
  };
  esp_task_wdt_reconfigure(&wdt_config);  // reconfigure if already init'd by IDF
  esp_task_wdt_add(NULL);   // watch the main Arduino task

  // ── I2C ──────────────────────────────────────────────────
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);    // 400 kHz fast mode

  if (mpuInit()) {
    Serial.println("[SafetyHub] MPU6050 OK");
  } else {
    Serial.println("[SafetyHub] MPU6050 FAIL — check wiring");
  }

  // ── DHT11 ────────────────────────────────────────────────
  dht.begin();
  Serial.println("[SafetyHub] DHT11 OK");

  // ── MQ2 ──────────────────────────────────────────────────
  // analogSetAttenuation() removed in core v3.x — set per-pin only
  analogSetPinAttenuation(MQ2_PIN, ADC_11db);   // 0–3.3 V range
  Serial.println("[SafetyHub] MQ2 OK");

  // ── WiFi Access Point ────────────────────────────────────
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD[0] ? AP_PASSWORD : nullptr);

  // CRITICAL: disable power-save — this is the #1 cause of AP disconnects
  // esp_wifi_set_ps() is in <esp_wifi.h> (added to includes above)
  esp_wifi_set_ps(WIFI_PS_NONE);

  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[SafetyHub] AP started  SSID: %s  IP: %s\n",
                AP_SSID, ip.toString().c_str());

  // ── HTTP routes ──────────────────────────────────────────
  server.on("/api/data", HTTP_GET,     handleApiData);
  server.on("/api/data", HTTP_OPTIONS, handleOptions);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[SafetyHub] HTTP server running on port 80");

  // ── Initial sensor read ──────────────────────────────────
  readSensors();
  lastSensorRead = millis();

  Serial.println("[SafetyHub] Ready.");
}

// ============================================================
//  loop()  — must be tight and non-blocking
// ============================================================
void loop() {
  // Feed watchdog every iteration
  esp_task_wdt_reset();

  // Handle pending HTTP requests immediately
  server.handleClient();

  // Read sensors on interval (non-blocking)
  uint32_t now = millis();
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensors();
  }

  // Yield to RTOS / WiFi stack — prevents task starvation
  yield();
}
