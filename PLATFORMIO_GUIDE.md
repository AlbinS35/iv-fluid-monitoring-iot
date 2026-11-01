# PlatformIO IoT Integration Guide
## Smart IoT IV Fluid Infusion Monitoring and Drip-Rate Telematics System
**Amal Jyothi College of Engineering — Master of Computer Applications (MCA)**  
**Course:** 24MCAT213 – Internet of Things

---

## 1. Overview & Embedded Architecture

This project uses **PlatformIO** (the modern, cross-platform embedded development ecosystem for VS Code and CLI) to develop, build, test, and flash the ESP32 edge firmware for the IV Fluid Infusion Monitoring Node.

The firmware performs:
1. **Interrupt-Driven Drop Counting**: Senses every falling drop using the TCRT5000 optical infrared sensor connected to **GPIO 19** with a 60 ms hardware debounce lockout (`IRAM_ATTR`).
2. **Edge Flow Mathematics**: Calculates DPM (5-drop moving average), flow rate ($mL/h = DPM \times 60 / DF$), infused volume, remaining volume, and estimated completion time (ETA) directly at the bedside.
3. **Offline Safety Alerts**: Controls the active buzzer on **GPIO 18** and blue LED on **GPIO 2** with distinct audible patterns for *Low Volume*, *No Flow (Occlusion)*, *Free Flow (>150 DPM)*, and *Rate Deviation (±20%)*.
4. **Bi-Directional Telemetry**:
   - **Data Publishing (POST)**: Transmits a JSON telemetry record every 5 seconds to `/api/telemetry`.
   - **Data Fetching (GET)**: Fetches latest prescription/target rate and bed status from `/api/latest/<device_id>` every 15 seconds.
   - **Offline Ring Buffer**: Automatically retains the last 60 readings in RAM during network drops and uploads them in a burst once connection is restored.

---

## 2. Hardware Wiring Table (Report Table 8.1)

All peripherals use 3.3V logic levels directly compatible with the ESP32:

| Component Pin | Pin Type | ESP32 Pin | Voltage | Purpose / Clinical Function |
| :--- | :--- | :--- | :--- | :--- |
| **TCRT5000 VCC** | Power Input | `3V3` | 3.3 V | IR LED and LM393 comparator power |
| **TCRT5000 GND** | Ground | `GND` | 0 V | Common ground |
| **TCRT5000 DO** | Digital Output | `GPIO 19` | 3.3 V | Digital drop pulse to external interrupt (`FALLING`) |
| **TCRT5000 AO** | Analog Output | *Unconnected* | 0 – 3.3 V | Left open in standard optical drop counter mode |
| **Active Buzzer (+)** | Digital Output | `GPIO 18` | 3.3 V / 5 V | Audible alarm (direct or via BC547 NPN driver) |
| **Active Buzzer (-)** | Ground | `GND` | 0 V | Return path |
| **Onboard Blue LED** | Internal | `GPIO 2` | 3.3 V | Optical drop flash & diagnostic sync indicator |
| **ESP32 VIN / USB** | Power Input | `VIN / USB` | 5 V DC | Power from bedside adapter or ward laptop |

---

## 3. PlatformIO Project Structure

```
IV fluid monitoring system IoT/
├── platformio.ini               <-- Workspace PlatformIO configuration
├── firmware/
│   ├── platformio.ini           <-- Dedicated node configuration
│   ├── include/
│   │   ├── config.h             <-- Wi-Fi, Gateway IP, Pinout, Thresholds
│   │   └── telematics.h         <-- Structs, Status enums, prototypes
│   ├── src/
│   │   └── main.cpp             <-- Edge firmware (ISR, Math, Wi-Fi REST)
│   └── esp32_iv_telematics.ino  <-- Arduino IDE legacy file (preserved)
├── app.py                       <-- Flask REST API & Gateway
├── templates/
│   └── index.html               <-- Obsidian Clinical Telematics Dashboard
└── test_suite.py                <-- Compliance test suite
```

---

## 4. Step-by-Step Setup in VS Code with PlatformIO

### Step 1: Install PlatformIO IDE Extension
1. Open **Visual Studio Code**.
2. Go to the Extensions Marketplace (`Ctrl+Shift+X`).
3. Search for **PlatformIO IDE** (by PlatformIO) and click **Install**.
4. Restart VS Code if prompted.

### Step 2: Open the Project
1. In VS Code, click **File** -> **Open Folder...**
2. Select `IV fluid monitoring system IoT` (or open the `firmware/` subfolder).
3. PlatformIO will automatically recognize `platformio.ini` and download the `espressif32` toolchain and `ArduinoJson` library.

### Step 3: Configure Network and Gateway IP
Open `firmware/include/config.h` and update your Wi-Fi credentials and Gateway IP:

```cpp
// 1. Wi-Fi credentials (your Wi-Fi router or laptop mobile hotspot)
#define WIFI_SSID             "Hospital_Ward_WiFi"
#define WIFI_PASSWORD         "ClinicalSecure2026"

// 2. Ward Gateway Host IP (IP address of your laptop running app.py)
// Example: If your laptop Wi-Fi IP is 192.168.1.100:
#define GATEWAY_HOST          "http://192.168.1.100:5000"

// 3. Bed Identifier
#define DEVICE_ID             "WARD-A-BED-07"
```

> **Tip to find your laptop IP:**
> Open PowerShell and run `ipconfig`. Find your IPv4 Address under *Wireless LAN adapter Wi-Fi* (e.g. `192.168.1.100` or `192.168.137.1` for Windows Hotspot).

---

## 5. Building, Flashing, and Monitoring

You can use either the **PlatformIO toolbar** (bottom status bar in VS Code) or the **PlatformIO CLI**:

### Using PlatformIO Status Bar Icons:
- **Build**: Click the **✓** (Checkmark) icon.
- **Upload**: Connect your ESP32 via USB and click the **→** (Right Arrow) icon.
- **Serial Monitor**: Click the **🔌** (Plug) icon to open the 115200 baud monitor.

### Using CLI Terminal Commands:
```bash
# Navigate to firmware directory
cd firmware

# Compile firmware
pio run

# Flash to connected ESP32
pio run --target upload

# Open Serial Monitor at 115200 baud
pio device monitor --baud 115200
```

---

## 6. How the Live Telematics Data Flow Works

```
  [ TCRT5000 IR Sensor ]
            │  (Falling drop optical refraction)
            ▼
  [ GPIO 19 Hardware ISR ] (60ms Debounce lockout)
            │
            ▼
  [ ESP32 Edge Math Core ]
    - 5-interval Moving Average DPM
    - Flow Rate: Flow = DPM * 60 / DF (mL/h)
    - Volume Remaining = 500 mL - Infused
    - ETA = Volume Remaining / Flow * 60 min
            │
            ├─────────────────────────────────────────┐
            ▼                                         ▼
   [ Local Offline Actuators ]              [ Wi-Fi HTTP Client ]
    - GPIO 18 Active Buzzer                  - POST /api/telemetry (Every 5s)
    - GPIO 2 Diagnostic Blue LED             - GET /api/latest/<id> (Every 15s)
    (Operates even when network fails!)      - Offline 60-packet Ring Buffer
                                                      │
                                                      ▼
                                            [ Flask REST API Gateway ]
                                                      │
                                                      ▼
                                    [ Obsidian Clinical Dashboard ]
                                    (templates/index.html)
                                    - Live DPM Semicircular Gauge
                                    - Animated Wave IV Bag
                                    - Rolling Canvas Flow Graph
                                    - Ward-A Bed Grid Matrix
                                    - Auditory Nurse Alerts
```

---

## 7. Testing and Demonstration

1. **Start the Backend Gateway**:
   ```powershell
   python app.py
   ```
   Open your browser to `http://localhost:5000` (or `http://<laptop_ip>:5000`).

2. **Power the ESP32 Node**:
   - The ESP32 will beep twice on boot.
   - It connects to Wi-Fi and begins streaming packets every 5 seconds.
   - The dashboard updates in real time without refreshing the page!

3. **Demonstrating Table 7.1 Alert Conditions**:
   - **Normal Flow**: Adjust roller clamp to 40 DPM -> Flow is 120 mL/h, Green NORMAL status.
   - **Occlusion / Blockage**: Close the clamp completely for 30 seconds -> Local continuous buzzer sounds, dashboard trips to Crimson `NO_FLOW` alert.
   - **Free-Flow**: Open clamp fully (>150 DPM) -> Rapid urgent buzzer sounds, dashboard trips to `FREE_FLOW` alert.
   - **Low Volume**: When bag volume falls $\le 50$ mL -> Intermittent beep sounds, dashboard trips to Amber `LOW_VOLUME`.
   - **Wi-Fi Loss**: Turn off router/hotspot -> Sensor drop counting and local buzzer continue running offline; buffered packets upload automatically upon reconnection.
