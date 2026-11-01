# Smart IoT IV Fluid Infusion Monitoring and Drip-Rate Telematics System

**Amal Jyothi College of Engineering, Kanjirappally**  
*Master of Computer Applications (MCA) — 24MCAT213: Internet of Things*

[![Docker](https://img.shields.io/badge/Docker-Ready-blue.svg)](https://www.docker.com/)
[![ESP32](https://img.shields.io/badge/Hardware-ESP32%20DevKit%20V1-red.svg)](https://espressif.com/)
[![PlatformIO](https://img.shields.io/badge/Embedded-PlatformIO%20IDE-orange.svg)](https://platformio.org/)
[![Stitch](https://img.shields.io/badge/UI%20Design-Stitch%20Clinical%20Obsidian-06b6d4.svg)](#)
[![Flask](https://img.shields.io/badge/Backend-Python%20Flask%20Gateway-green.svg)](https://flask.palletsprojects.com/)
[![PostgreSQL](https://img.shields.io/badge/Database-PostgreSQL%2015-336791.svg)](https://www.postgresql.org/)

A retrofit, edge-intelligent telematics system that continuously monitors gravity IV drip infusions, detects occlusions and runaway free-flow, provides network-independent offline buzzer alerts, and streams telemetry to a ward-level fog gateway. Designed with **Stitch (Obsidian Clinical Telematics)** and powered by **PlatformIO** for seamless embedded IoT integration.


---

## 1. Prototype Circuit Wiring (Report Table 8.1)

![Prototype Wiring](circuit_connection.png)

### Pin Connection Details
| Peripheral Pin | ESP32 DevKit V1 Pin | Operating Logic | Purpose / Description |
| :--- | :--- | :--- | :--- |
| **TCRT5000 VCC** | **3V3 (Pin 1)** | 3.3 V DC | Regulated power supply for IR LED & comparator |
| **TCRT5000 GND** | **GND (Pin 14)** | 0 V (Ground) | Common circuit ground |
| **TCRT5000 DO** | **GPIO 19 (Pin 28)** | 3.3 V (active-LOW) | Digital drop pulse to external interrupt / polling |
| **TCRT5000 AO** | *Not Connected* | 0 – 3.3 V | Analog output (left unconnected in prototype) |
| **Active Buzzer (+)** | **GPIO 18 (Pin 29)** | 3.3 V / 5 V | Audible alert output (direct or via BC547 driver) |
| **Active Buzzer (-)** | **GND (Pin 17)** | 0 V (Ground) | Buzzer return path to common ground |
| **Onboard Blue LED** | **GPIO 2 (Internal)** | 3.3 V | 5ms visual flash per detected drop & sync |
| **ESP32 Micro-USB** | **VIN / USB (5V)** | 5 V DC | Powers the whole node from laptop or 5V adapter |

### Circuit Schematic & Driver Notes
```
                  +-----------------------------------+
                  |         ESP32 DevKit V1           |
                  |                                   |
[3V3 (Pin 1)] ----+ 3V3                               |
[GND (Pin 14)] ---+ GND                               |
                  |                                   |
                  |       GPIO 19 (Pin 28) <----------+--- [TCRT5000 DO]
                  |                                   |    (Drop Pulse)
                  |       GPIO 18 (Pin 29) ---------->+--- [Buzzer (+) / Transistor Base]
                  |                                   |    (Acoustic Alert)
[GND (Pin 17)] ---+ GND <-----------------------------+--- [Buzzer (-)]
                  +-----------------------------------+
```

> [!IMPORTANT]
> **Technical Reconciliation: Transistor Driver for 5V Buzzer**
> The ESP32 GPIO pins are rated for a maximum source current of **12–20 mA**. A small 3.3V piezo buzzer can be driven directly from GPIO 18. However, for higher-current 5V active buzzers, it is recommended to drive the buzzer through an **NPN transistor (BC547)** with a **1 kΩ resistor** on the base connected to GPIO 18, emitter to GND, and the buzzer between 5V/VIN and the collector.

---

## 2. Technical Reconciliation: Hardware Interrupts vs. Polling

Your project report and firmware provide full support for both detection methodologies:

1. **Hardware Interrupt Mode (`attachInterrupt` on FALLING edge)**:
   - *Working Principle*: The TCRT5000 LM393 comparator pulls GPIO 19 LOW when a droplet cuts the IR beam. The ESP32 triggers `IRAM_ATTR isrDropDetected()` within microseconds, reads `micros()`, enforces a 60 ms software debounce lock, and increments a volatile drop counter.
   - *Advantage*: Zero drops are lost even when the ESP32 Wi-Fi stack or JSON serialization is busy.
2. **Polling Mode (`digitalRead` with state change detection)**:
   - *Working Principle*: The main loop continuously polls GPIO 19. When a transition from `HIGH` to `LOW` is observed and elapsed time is $\ge 60\text{ ms}$, the drop is logged.
   - *Configurable*: Set `#define USE_HARDWARE_INTERRUPTS 1` (default, matches Report Section 7.2) or `0` in `firmware/esp32_iv_telematics.ino`.

---

## 3. Mathematical Infusion Model (Macro Set: 20 gtt/mL)

- **Drop Factor ($DF$)**: Standard Romsons macro drip set: $20\text{ drops} = 1\text{ mL}$.
- **DPM Calculation**: Computed from moving average of last 5 inter-drop intervals:
  $$\text{DPM} = \frac{60,000,000}{\text{mean interval in } \mu\text{s}}$$
- **Flow Rate in mL/h**:
  $$\text{Flow Rate (mL/h)} = \frac{\text{DPM} \times 60}{DF} = \text{DPM} \times 3$$
- **Volume Infused**: $\text{Volume Infused (mL)} = \frac{\text{Total Drops}}{20}$.
- **Volume Remaining**: $\text{Volume Remaining (mL)} = 500 - \text{Volume Infused}$.
- **Time to Empty (ETA)**:
  $$\text{Time Remaining (min)} = \frac{\text{Volume Remaining}}{\text{Flow Rate}} \times 60$$

---

## 4. Alert Conditions & Thresholds (Report Table 7.1)

| Alert State | Trigger Condition | Local Bedside Action | Gateway Web UI Indication |
| :--- | :--- | :--- | :--- |
| **`NORMAL`** | Nominal infusion (approx 40 DPM, 120 mL/h) | Buzzer silent, onboard LED flashes on drops | Green status badge, nominal flow trace |
| **`LOW_VOLUME`** | Bag volume $\le 10\%$ ($\le 50\text{ mL}$ of 500 mL) | Slow beep on buzzer + onboard LED | Amber badge, flashing card border |
| **`NO_FLOW`** | No drop for $>30\text{ s}$ (blockage / empty) | Continuous alarm on buzzer | Flashing red alarm badge, stalled ETA |
| **`FREE_FLOW`** | $\text{DPM} > 150$ (roller clamp opened / runaway) | Fast intermittent beep on buzzer | Red emergency badge, flashing alarm |
| **`RATE_ERR`** | Flow deviates by $>\pm 20\%$ for 60s | Double beep on buzzer | Purple deviation badge |
| **`OFFLINE`** | Gateway Wi-Fi disconnected | Offline alerts continue; buffers 60 packets | Red status indicator on dashboard |

---

## 5. PostgreSQL Schema (`infusion_readings` - Report Table 9.2)

```sql
CREATE TABLE IF NOT EXISTS infusion_readings (
    id BIGSERIAL PRIMARY KEY,
    device_id VARCHAR(32) NOT NULL,
    ts TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    dpm REAL NOT NULL,
    flow_ml_hr REAL NOT NULL,
    vol_infused_ml REAL NOT NULL,
    vol_remaining_ml REAL NOT NULL,
    status VARCHAR(20) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_infusion_dev_ts ON infusion_readings(device_id, ts DESC);
```

---

## 6. REST API Endpoints (Report Table 9.1)

- **`POST /api/telemetry`**: Ingests JSON payload from ESP32 every 5 seconds.
  ```json
  {
    "device_id": "WARD-A-BED-07",
    "dpm": 40.0,
    "flow_ml_hr": 120.0,
    "vol_infused_ml": 128.0,
    "vol_remaining_ml": 372.0,
    "status": "NORMAL"
  }
  ```
- **`GET /api/latest/<device_id>`**: Returns latest record for a bed (e.g. `WARD-A-BED-07`).
- **`GET /api/history/<device_id>?limit=30`**: Returns chronologically ordered readings for Chart.js / Canvas.
- **`GET /api/devices`**: Lists all active bed nodes and last-seen timestamps.
- **`GET /api/health`**: Health diagnostic for container & database connection.
- **`GET /`**: Renders self-contained clinical dashboard.

---

## 7. Quick Start: Live Demo & Testing

### 1. Launch Docker Containers (Ward Gateway)
```bash
docker compose up -d
```
Open **`http://localhost:5000`** in your browser.

### 2. Live Simulator (No Physical ESP32 Required)
In a terminal, run:
```bash
# Normal infusion (40 DPM, 120 mL/h)
python simulator.py --mode normal

# Dynamic test (cycles through Normal -> No Flow -> Free Flow -> Rate Error)
python simulator.py --mode dynamic

# Test blockage alarm
python simulator.py --mode no_flow
```

### 3. Flash Physical ESP32 with PlatformIO (Recommended)
1. Open this repository in **VS Code** with the **PlatformIO IDE** extension installed.
2. Edit [`firmware/include/config.h`](firmware/include/config.h) with your Wi-Fi credentials and laptop Gateway IP:
   ```cpp
   #define WIFI_SSID     "Your_WiFi_SSID"
   #define WIFI_PASSWORD "Your_WiFi_Password"
   #define GATEWAY_HOST  "http://<YOUR_LAPTOP_IP>:5000"
   ```
3. Connect your ESP32 DevKit V1 via USB.
4. Click the **PlatformIO Upload (→)** button on the bottom status bar, or run:
   ```bash
   cd firmware
   pio run --target upload
   pio device monitor --baud 115200
   ```
*(Detailed step-by-step instructions available in [PLATFORMIO_GUIDE.md](PLATFORMIO_GUIDE.md))*

### 4. Flash via Arduino IDE (Legacy Option)
1. Open [`firmware/esp32_iv_telematics.ino`](firmware/esp32_iv_telematics.ino) in Arduino IDE.
2. Set your Wi-Fi credentials and laptop Gateway IP in the configuration section.
3. Install the **ArduinoJson** library in Library Manager.
4. Select board **ESP32 Dev Module** and click **Upload**.

---

## 7. Stitch-Generated Obsidian Clinical Telematics Dashboard

The web dashboard is served directly by the Flask API gateway at `http://localhost:5000/`:
- **Obsidian Dark Aesthetic**: Surgical, high-contrast palette engineered for ICU nurse stations.
- **Semicircular DPM Arc Gauge**: Real-time needle and active arc showing live DPM from edge interrupt sampling.
- **Animated IV Fluid Bag**: Liquid level wave showing remaining volume and percentage.
- **Rolling Canvas Flow Graph**: Time-series curve of Flow Rate (mL/h) and DPM against tolerance baseline.
- **Ward-A 8-Bed Matrix**: Multi-channel monitoring (Bed-01 to Bed-08) with one-click channel switching.
- **Web Audio Buzzer Synthesizer**: Auditory emergency alarms directly in the browser with mute toggle.
- **Live Ingestion Log & CSV Export**: Real-time audit stream with search filter and CSV export.
- **Bedside Simulator Drawer**: Built-in test drawer to trigger clinical states without physical hardware.
