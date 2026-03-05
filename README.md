# SafetyHub — Campus Emergency & Multi-Hazard Alert System

## System Architecture

```
MPU6050 ──┐
MQ2    ──┼──► ESP32 DevKit V1
DHT11  ──┘        │
                  │  WiFi AP  (SSID: SafetyHub)
                  ▼
         http://192.168.4.1/api/data
                  │
                  │  HTTP poll every 2 s
                  ▼
         Python Gateway  (laptop)
         http://localhost:8000/sensor-data
                  │
                  │  Vite proxy
                  ▼
         React Dashboard
         http://localhost:5173
```

---

## Part 1 — ESP32 Setup

### Arduino IDE Libraries Required

Install via Library Manager (Sketch → Include Library → Manage Libraries):

| Library | Version |
|---|---|
| DHT sensor library (Adafruit) | ≥ 1.4.4 |
| Adafruit Unified Sensor | ≥ 1.1.9 |

> **MPU6050**: The new `.ino` uses raw I2C register reads — no external MPU library needed.

### Board Settings

- Board: **ESP32 Dev Module**
- Upload Speed: 115200
- CPU Frequency: 240 MHz
- Flash Frequency: 80 MHz
- Partition Scheme: Default 4MB with spiffs

### Wiring

```
ESP32 Pin   →  Sensor
─────────────────────────────────
GPIO 21     →  MPU6050 SDA
GPIO 22     →  MPU6050 SCL
3.3V        →  MPU6050 VCC
GND         →  MPU6050 GND

GPIO 4      →  DHT11 DATA
3.3V        →  DHT11 VCC
GND         →  DHT11 GND
(10kΩ pull-up between DATA and VCC)

GPIO 34     →  MQ2 AOUT (analog)
5V          →  MQ2 VCC  (MQ2 heater needs 5V)
GND         →  MQ2 GND
```

> ⚠ GPIO 34 is input-only ADC1 — required because ADC2 pins conflict with WiFi.

### Flash the ESP32

1. Open `esp32/SafetyHub.ino` in Arduino IDE
2. Connect ESP32 via USB
3. Select the correct COM/tty port
4. Click Upload
5. Open Serial Monitor at 115200 baud — you should see:

```
[SafetyHub] MPU6050 OK
[SafetyHub] DHT11 OK
[SafetyHub] MQ2 OK
[SafetyHub] AP started  SSID: SafetyHub  IP: 192.168.4.1
[SafetyHub] HTTP server running on port 80
[SafetyHub] Ready.
```

### Test ESP32 directly

Connect your laptop to WiFi: **SafetyHub** (password: `safetyhub123`)

```bash
curl http://192.168.4.1/api/data
```

Expected response:
```json
{
  "temperature": 31.5,
  "humidity": 67.2,
  "gasLevel": 410,
  "vibration": 0.72,
  "alert": "System Normal"
}
```

---

## Part 2 — Python Gateway

### Install dependencies

```bash
cd gateway
pip install -r requirements.txt
```

### Run the gateway

Make sure your laptop is connected to the **SafetyHub** WiFi, then:

```bash
python gateway.py
```

Output:
```
09:41:00  INFO     SafetyHub Gateway starting — polling http://192.168.4.1/api/data every 2.0s
INFO:     Uvicorn running on http://0.0.0.0:8000
```

### Gateway endpoints

| Endpoint | Description |
|---|---|
| `GET /sensor-data` | Latest sensor snapshot |
| `GET /health` | ESP32 + gateway status |
| `GET /history?limit=50` | Last N readings |

### Test gateway in browser

```
http://localhost:8000/sensor-data
http://localhost:8000/health
http://localhost:8000/history
```

Or via curl:
```bash
curl http://localhost:8000/sensor-data
```

---

## Part 3 — React Dashboard

```bash
cd ..              # root of campus-safety-dashboard
npm install
npm run dev
```

Open: **http://localhost:5173**

---

## What Caused WiFi Disconnections (and How Each Was Fixed)

### 1. WiFi Power-Save Mode (Root Cause #1)
The ESP32 WiFi stack enables power-save by default, which puts the radio to sleep between beacons. This causes delayed ACKs and dropped connections.

**Fix:**
```cpp
esp_wifi_set_ps(WIFI_PS_NONE);
```

### 2. `String` Object Heap Fragmentation (Root Cause #2)
Concatenating Arduino `String` objects in `handleClient()` causes repeated `malloc/free` cycles. Over hours this fragments the 320 KB heap until allocation fails and the WiFi stack crashes.

**Fix:** All JSON is built with `snprintf()` into a fixed `char buf[256]` — zero heap allocation per request.

### 3. `delay()` in the Main Loop (Root Cause #3)
Any `delay()` inside `loop()` blocks `server.handleClient()` and the WiFi stack's internal keep-alive. Clients time out and disconnect.

**Fix:** All sensor reads are gated on `millis()` intervals. `delay()` removed entirely from `loop()`.

### 4. No Watchdog Timer
If the MPU6050 I2C bus locked up, the ESP32 would hang forever.

**Fix:** Hardware watchdog set to 60 s. `esp_task_wdt_reset()` called every loop iteration.

### 5. ADC2 Pins Used with WiFi Active
ADC2 shares hardware with the WiFi radio. Reading any ADC2 pin while WiFi is active returns garbage and can destabilize the RF subsystem.

**Fix:** MQ2 wired to GPIO 34 (ADC1 channel 6) — no conflict with WiFi.

### 6. `yield()` Missing
On FreeRTOS-based ESP32, the Arduino `loop()` is a single task. Without `yield()`, it starves the WiFi/TCP tasks.

**Fix:** `yield()` at the end of every `loop()` iteration.

---

## Troubleshooting

| Symptom | Check |
|---|---|
| `ESP32 Offline` in dashboard | Confirm laptop is on "SafetyHub" WiFi |
| temperature shows 0 | Check DHT11 wiring + 10kΩ pull-up |
| gasLevel always 0 or 4095 | MQ2 needs 5V, GPIO 34 must be used |
| vibration always 0 | Check I2C wiring SDA=21 SCL=22 |
| Gateway 503 errors | Run `curl 192.168.4.1/api/data` to isolate |
| Serial garbage | Confirm baud rate is 115200 |
