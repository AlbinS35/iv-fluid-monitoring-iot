/*
 * =====================================================================================
 * Project: Smart IoT IV Fluid Infusion Monitoring and Drip-Rate Telematics System
 * Institution: Amal Jyothi College of Engineering, Kanjirappally
 * Course: Master of Computer Applications (MCA) | 24MCAT213 - Internet of Things
 * 
 * Hardware Prototype Specifications:
 *   - Microcontroller: ESP32 DevKit V1 (30-Pin / 38-Pin)
 *   - Optical Drop Sensor: TCRT5000 IR Sensor Module (Comparator DO on GPIO 19)
 *   - Acoustic Alert: Active Buzzer 5V/3.3V on GPIO 18 (Direct or via BC547 NPN Driver)
 *   - Visual Status: Onboard Blue LED on GPIO 2
 *   - Drip Set: Romsons Adult Macro Infusion Set (Drop Factor DF = 20 drops/mL)
 *   - Telemetry: Wi-Fi HTTP POST to Ward Fog Gateway (Flask API / PostgreSQL)
 * =====================================================================================
 * 
 * Circuit Wiring Details (Report Table 8.1 & Prototype Photograph):
 *   ESP32 DevKit V1 Pin   Hardware Component Pin   Description
 *   -------------------   ----------------------   -----------
 *   3V3 (Pin 1)     --->  TCRT5000 VCC             3.3V Regulated Power Rail
 *   GND (Pin 14)    --->  TCRT5000 GND             Sensor Ground
 *   GPIO 19 (Pin 28)<---  TCRT5000 DO              Digital Drop Pulse (active-LOW)
 *   [Not Connected]       TCRT5000 AO              Analog output left open
 *   GPIO 18 (Pin 29)--->  Active Buzzer (+)        Buzzer trigger (or Base of BC547)
 *   GND (Pin 17)    --->  Active Buzzer (-)        Buzzer Ground return
 *   GPIO 2 (Internal)---> Onboard Blue LED         Drop registration & sync flash
 * =====================================================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// -------------------------------------------------------------------------------------
// CONFIGURATION CONSTANTS
// -------------------------------------------------------------------------------------
const char* WIFI_SSID     = "Hospital_WiFi_ICU";       // Replace with your Wi-Fi SSID
const char* WIFI_PASSWORD = "ClinicalPassword123";      // Replace with your Wi-Fi Password

// Ward Gateway Endpoint (Report Section 9 & Table 9.1)
const char* SERVER_URL    = "http://192.168.1.100:5000/api/telemetry";
const char* DEVICE_ID     = "WARD-A-BED-07";           // Primary bed identifier

// Hardware Pin Assignments (Report Table 8.1)
const uint8_t PIN_IR_SENSOR = 19;  // TCRT5000 Digital Output (DO)
const uint8_t PIN_BUZZER    = 18;  // Active Buzzer (+)
const uint8_t PIN_LED       = 2;   // Onboard Blue Status LED

// Infusion Parameters (Report Section 7.4)
const float DRIP_FACTOR           = 20.0f;  // Macro Set: 20 drops = 1 mL
const float INITIAL_BOTTLE_VOL_ML = 500.0f; // Standard IV saline bag volume
const float PRESCRIBED_FLOW_ML_HR = 120.0f; // Prescribed rate for rate-deviation alert (40 DPM)

// Alert Timing Thresholds (Report Table 7.1)
const unsigned long NO_FLOW_TIMEOUT_MS  = 30000; // No drop for >30s -> Blockage / Empty bag
const unsigned long DEBOUNCE_MICROS     = 60000; // 60 ms debounce lock (Report Section 7.2)
const unsigned long TELEMETRY_PERIOD_MS = 5000;  // 5 seconds transmission period (Report 7.6)

// -------------------------------------------------------------------------------------
// DETECTION ARCHITECTURE SELECTION (INTERRUPT VS POLLING)
// Set to 1 for Hardware Interrupt (attachInterrupt - Report Section 7.2)
// Set to 0 for Continuous Polling (digitalRead)
// -------------------------------------------------------------------------------------
#define USE_HARDWARE_INTERRUPTS 1

// Volatile Interrupt Variables
volatile unsigned long volatileDropCount = 0;
volatile unsigned long lastDropMicros    = 0;
volatile unsigned long dropIntervalsUs[5] = {1500000, 1500000, 1500000, 1500000, 1500000};
volatile uint8_t intervalIndex          = 0;
volatile bool flagNewDrop               = false;

// Polling Fallback State (used when USE_HARDWARE_INTERRUPTS == 0)
int lastSensorState = HIGH;
unsigned long lastPollDropTime = 0;

// Runtime Infusion State
unsigned long totalDropCount = 0;
float volumeInfusedMl        = 0.0f;
float volumeRemainingMl      = INITIAL_BOTTLE_VOL_ML;
float currentDpm             = 0.0f;
float currentFlowMlHr        = 0.0f;
String currentStatus         = "NORMAL";
unsigned long lastDropMs     = 0;
unsigned long lastTelemetryMs= 0;

// Offline Ring Buffer (Report Section 7.6: Stores up to 60 records during Wi-Fi outages)
struct TelemetryRecord {
    float dpm;
    float flow_ml_hr;
    float vol_infused_ml;
    float vol_remaining_ml;
    char status[16];
};
const uint8_t BUFFER_SIZE = 60;
TelemetryRecord ringBuffer[BUFFER_SIZE];
uint8_t bufferHead = 0;
uint8_t bufferCount = 0;

// -------------------------------------------------------------------------------------
// HARDWARE INTERRUPT SERVICE ROUTINE (ISR) - REPORT SECTION 7.2
// -------------------------------------------------------------------------------------
void IRAM_ATTR isrDropDetected() {
    unsigned long nowUs = micros();
    unsigned long diff = nowUs - lastDropMicros;
    
    // 60 ms debounce filter (Report Section 7.2)
    if (diff >= DEBOUNCE_MICROS) {
        lastDropMicros = nowUs;
        volatileDropCount++;
        
        // Store interval for moving average
        dropIntervalsUs[intervalIndex] = diff;
        intervalIndex = (intervalIndex + 1) % 5;
        
        flagNewDrop = true;
    }
}

// -------------------------------------------------------------------------------------
// SETUP
// -------------------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n========================================================");
    Serial.println("  AMAL JYOTHI COLLEGE OF ENGINEERING - MCA 24MCAT213");
    Serial.println("  Smart IoT IV Fluid Infusion Monitoring Node");
    Serial.printf("  Device ID: %s | Drip Factor: %.0f gtt/mL\n", DEVICE_ID, DRIP_FACTOR);
    Serial.println("========================================================");

    // Pin Configurations
    pinMode(PIN_IR_SENSOR, INPUT_PULLUP);
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
    digitalWrite(PIN_LED, LOW);

    // Initial Diagnostic Chirp (2 short beeps)
    chirpBuzzer(2, 70);

#if USE_HARDWARE_INTERRUPTS
    Serial.println("[SENSOR] Mode: Hardware Interrupt attached on GPIO 19 (FALLING edge).");
    attachInterrupt(digitalPinToInterrupt(PIN_IR_SENSOR), isrDropDetected, FALLING);
#else
    Serial.println("[SENSOR] Mode: Polling loop on GPIO 19 with 60ms debounce window.");
#endif

    lastDropMs = millis();
    lastDropMicros = micros();

    // Connect to Ward Gateway Wi-Fi
    connectWiFi();
}

// -------------------------------------------------------------------------------------
// MAIN LOOP
// -------------------------------------------------------------------------------------
void loop() {
    unsigned long currentMillis = millis();

    // 1. Drop Detection Handling
#if USE_HARDWARE_INTERRUPTS
    if (flagNewDrop) {
        flagNewDrop = false;
        lastDropMs = currentMillis;
        onDropRegistered();
    }
#else
    // Polling mode alternative (digitalRead with debounce)
    int sensorState = digitalRead(PIN_IR_SENSOR);
    if (sensorState == LOW && lastSensorState == HIGH) {
        if (currentMillis - lastPollDropTime >= 60) {
            lastPollDropTime = currentMillis;
            lastDropMs = currentMillis;
            volatileDropCount++;
            onDropRegistered();
        }
    }
    lastSensorState = sensorState;
#endif

    // 2. Periodic Telemetry & Mathematics (Every 5 seconds - Report Section 7.6)
    if (currentMillis - lastTelemetryMs >= TELEMETRY_PERIOD_MS) {
        lastTelemetryMs = currentMillis;

        // Atomically snapshot and compute DPM using moving-average of 5 intervals (Report 7.3)
        noInterrupts();
        unsigned long snapshotDrops = volatileDropCount;
        volatileDropCount = 0;
        unsigned long sumUs = 0;
        for (int i = 0; i < 5; i++) {
            sumUs += dropIntervalsUs[i];
        }
        interrupts();

        totalDropCount += snapshotDrops;

        // DPM & Flow Rate Mathematics (Report Section 7.3 & 7.4)
        unsigned long timeSinceLastDrop = currentMillis - lastDropMs;
        if (timeSinceLastDrop >= NO_FLOW_TIMEOUT_MS) {
            currentDpm = 0.0f;
        } else if (sumUs > 0) {
            unsigned long meanIntervalUs = sumUs / 5;
            currentDpm = 60000000.0f / float(meanIntervalUs);
        } else {
            currentDpm = 0.0f;
        }

        // Flow rate (mL/h) = DPM * 60 / DF
        currentFlowMlHr = (currentDpm * 60.0f) / DRIP_FACTOR;

        // Volume infused (mL) = total drops / DF
        volumeInfusedMl = float(totalDropCount) / DRIP_FACTOR;
        volumeRemainingMl = max(0.0f, INITIAL_BOTTLE_VOL_ML - volumeInfusedMl);

        // Evaluate Alert Conditions (Report Table 7.1)
        evaluateAlertConditions(timeSinceLastDrop);

        // Print Diagnostic telemetry to Serial Monitor
        Serial.printf("[TELEMETRY] Bed: %s | DPM: %5.1f | Flow: %5.1f mL/h | Infused: %5.1f mL | Rem: %5.1f mL | Status: %s\n",
                      DEVICE_ID, currentDpm, currentFlowMlHr, volumeInfusedMl, volumeRemainingMl, currentStatus.c_str());

        // Dispatch Telemetry or Buffer if Offline (Report Section 7.6)
        dispatchOrBufferTelemetry();
    }
}

// -------------------------------------------------------------------------------------
// DROP REGISTRATION VISUAL FEEDBACK
// -------------------------------------------------------------------------------------
void onDropRegistered() {
    digitalWrite(PIN_LED, HIGH);
    delayMicroseconds(5000); // 5ms visible pulse
    digitalWrite(PIN_LED, LOW);
}

// -------------------------------------------------------------------------------------
// EVALUATE ALERT CONDITIONS (REPORT TABLE 7.1)
// -------------------------------------------------------------------------------------
void evaluateAlertConditions(unsigned long timeSinceLastDrop) {
    if (volumeRemainingMl <= 50.0f) {
        // Low volume <= 10% of 500 mL bag -> Slow beep
        currentStatus = "LOW_VOLUME";
        triggerBuzzerPattern(1, 200);
    } else if (timeSinceLastDrop >= NO_FLOW_TIMEOUT_MS) {
        // No drop for > 30 s while active -> Continuous alarm
        currentStatus = "NO_FLOW";
        triggerBuzzerPattern(3, 150);
    } else if (currentDpm > 150.0f) {
        // DPM > 150 -> Free flow risk -> Fast beep
        currentStatus = "FREE_FLOW";
        triggerBuzzerPattern(4, 80);
    } else if (abs(currentFlowMlHr - PRESCRIBED_FLOW_ML_HR) > (PRESCRIBED_FLOW_ML_HR * 0.20f)) {
        // Flow outside +/- 20% of prescribed rate -> Double beep
        currentStatus = "RATE_ERR";
        triggerBuzzerPattern(2, 100);
    } else {
        currentStatus = "NORMAL";
        digitalWrite(PIN_BUZZER, LOW);
    }
}

// -------------------------------------------------------------------------------------
// BUZZER CADENCE CONTROLLER (DRIVES GPIO 18 OR BC547 TRANSISTOR BASE)
// -------------------------------------------------------------------------------------
void triggerBuzzerPattern(int beeps, int durationMs) {
    for (int i = 0; i < beeps; i++) {
        digitalWrite(PIN_BUZZER, HIGH);
        delay(durationMs);
        digitalWrite(PIN_BUZZER, LOW);
        if (i < beeps - 1) delay(durationMs);
    }
}

void chirpBuzzer(int count, int durationMs) {
    for (int i = 0; i < count; i++) {
        digitalWrite(PIN_BUZZER, HIGH);
        delay(durationMs);
        digitalWrite(PIN_BUZZER, LOW);
        if (i < count - 1) delay(durationMs);
    }
}

// -------------------------------------------------------------------------------------
// WI-FI & TELEMETRY DISPATCH (REPORT TABLE 9.1 & SECTION 7.6)
// -------------------------------------------------------------------------------------
void connectWiFi() {
    Serial.printf("[WIFI] Connecting to SSID: %s ", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 25) {
        delay(500);
        Serial.print(".");
        retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WIFI] Connected successfully!");
        Serial.printf("[WIFI] IP Address: %s | Gateway API: %s\n", WiFi.localIP().toString().c_str(), SERVER_URL);
    } else {
        Serial.println("\n[WIFI] Connection timeout. Operating in OFFLINE mode (buffering telemetry).");
    }
}

void dispatchOrBufferTelemetry() {
    if (WiFi.status() != WL_CONNECTED) {
        bufferTelemetry(currentDpm, currentFlowMlHr, volumeInfusedMl, volumeRemainingMl, currentStatus);
        WiFi.reconnect();
        return;
    }

    // Flush any pending buffered records first
    flushOfflineBuffer();

    // Send current telemetry packet (Report Section 15 sample payload)
    HTTPClient http;
    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> doc;
    doc["device_id"]        = DEVICE_ID;
    doc["dpm"]              = round(currentDpm * 10.0f) / 10.0f;
    doc["flow_ml_hr"]       = round(currentFlowMlHr * 10.0f) / 10.0f;
    doc["vol_infused_ml"]   = round(volumeInfusedMl * 10.0f) / 10.0f;
    doc["vol_remaining_ml"] = round(volumeRemainingMl * 10.0f) / 10.0f;
    doc["status"]           = currentStatus;

    String jsonString;
    serializeJson(doc, jsonString);

    int httpCode = http.POST(jsonString);
    if (httpCode == 201 || httpCode == 200) {
        Serial.printf("[HTTP] Logged successfully (HTTP %d)\n", httpCode);
    } else {
        Serial.printf("[HTTP] POST failed (Code: %d). Buffering packet.\n", httpCode);
        bufferTelemetry(currentDpm, currentFlowMlHr, volumeInfusedMl, volumeRemainingMl, currentStatus);
    }
    http.end();
}

void bufferTelemetry(float dpm, float flow, float infused, float rem, String stat) {
    ringBuffer[bufferHead].dpm = dpm;
    ringBuffer[bufferHead].flow_ml_hr = flow;
    ringBuffer[bufferHead].vol_infused_ml = infused;
    ringBuffer[bufferHead].vol_remaining_ml = rem;
    strncpy(ringBuffer[bufferHead].status, stat.c_str(), sizeof(ringBuffer[bufferHead].status) - 1);

    bufferHead = (bufferHead + 1) % BUFFER_SIZE;
    if (bufferCount < BUFFER_SIZE) bufferCount++;
    Serial.printf("[BUFFER] Offline record stored. Buffer count: %d/%d\n", bufferCount, BUFFER_SIZE);
}

void flushOfflineBuffer() {
    if (bufferCount == 0) return;
    Serial.printf("[BUFFER] Network restored. Flushing %d buffered records...\n", bufferCount);

    HTTPClient http;
    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/json");

    int startIdx = (bufferHead - bufferCount + BUFFER_SIZE) % BUFFER_SIZE;
    while (bufferCount > 0) {
        StaticJsonDocument<256> doc;
        doc["device_id"]        = DEVICE_ID;
        doc["dpm"]              = ringBuffer[startIdx].dpm;
        doc["flow_ml_hr"]       = ringBuffer[startIdx].flow_ml_hr;
        doc["vol_infused_ml"]   = ringBuffer[startIdx].vol_infused_ml;
        doc["vol_remaining_ml"] = ringBuffer[startIdx].vol_remaining_ml;
        doc["status"]           = ringBuffer[startIdx].status;

        String json;
        serializeJson(doc, json);
        int code = http.POST(json);

        if (code == 201 || code == 200) {
            startIdx = (startIdx + 1) % BUFFER_SIZE;
            bufferCount--;
        } else {
            break;
        }
    }
    http.end();
}
