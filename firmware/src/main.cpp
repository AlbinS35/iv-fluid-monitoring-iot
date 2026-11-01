/**
 * =====================================================================================
 * Project: Smart IoT IV Fluid Infusion Monitoring and Drip-Rate Telematics System
 * Institution: Amal Jyothi College of Engineering, Kanjirappally
 * Course: Master of Computer Applications (MCA) | 24MCAT213 - Internet of Things
 * Platform: PlatformIO (ESP32 DevKit V1 - Espressif 32)
 * Component: Embedded Edge Telematics Node (TCRT5000 IR -> ESP32 -> REST API Gateway)
 * =====================================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "telematics.h"

// -------------------------------------------------------------------------------------
// VOLATILE INTERRUPT VARIABLES (Protected by critical section)
// -------------------------------------------------------------------------------------
portMUX_TYPE isrMux = portMUX_INITIALIZER_UNLOCKED;

volatile unsigned long volatileDropCount = 0;
volatile unsigned long lastDropMicros    = 0;
volatile unsigned long dropIntervalsUs[DPM_AVG_WINDOW] = {1500000, 1500000, 1500000, 1500000, 1500000};
volatile uint8_t intervalIndex          = 0;
volatile bool flagNewDropEvent           = false;

// -------------------------------------------------------------------------------------
// RUNTIME TELEMETRICS STATE
// -------------------------------------------------------------------------------------
unsigned long totalDropCountSnapshot = 0;
unsigned long lastDropMillis         = 0;
unsigned long lastTelemetryMillis    = 0;
unsigned long lastFetchMillis        = 0;
unsigned long lastWiFiRetryMillis    = 0;
unsigned long lastBufferRetryMillis  = 0;

float currentDpm                     = 0.0f;
float currentFlowMlHr                = 0.0f;
float currentVolInfusedMl            = 0.0f;
float currentVolRemainingMl          = INITIAL_BOTTLE_VOL_ML;
ClinicalStatus currentStatus         = STATUS_NORMAL;

// Offline Circular Ring Buffer (Stores up to 60 packets during Wi-Fi disconnect)
TelemetryPacket ringBuffer[RING_BUFFER_CAPACITY];
uint8_t bufferHead  = 0;
uint8_t bufferTail  = 0;
uint8_t bufferCount = 0;

// Prescribed parameters (can be updated dynamically from Dashboard GET fetch)
float activePrescribedFlow = PRESCRIBED_FLOW_ML_HR;

// Buzzer timing pattern & Remote Bedside Silence
unsigned long buzzerBeepTimer = 0;
bool buzzerState              = false;
unsigned long bedsideSilenceUntilMillis = 0;

// -------------------------------------------------------------------------------------
// 1. HARDWARE INTERRUPT SERVICE ROUTINE (ISR) - REPORT SECTION 7.2
// -------------------------------------------------------------------------------------
void IRAM_ATTR isrDropDetected() {
    unsigned long nowUs = micros();
    unsigned long diffUs = nowUs - lastDropMicros;

    // 60 ms debounce hardware filter (Report Section 7.2)
    // Even an extreme gravity free-flow (>150 DPM) has drop intervals > 350 ms
    if (diffUs >= DEBOUNCE_MICROS) {
        lastDropMicros = nowUs;

        portENTER_CRITICAL_ISR(&isrMux);
        volatileDropCount = volatileDropCount + 1;
        dropIntervalsUs[intervalIndex] = diffUs;
        intervalIndex = (intervalIndex + 1) % DPM_AVG_WINDOW;
        flagNewDropEvent = true;
        portEXIT_CRITICAL_ISR(&isrMux);
    }
}

// -------------------------------------------------------------------------------------
// 2. HARDWARE INITIALIZATION
// -------------------------------------------------------------------------------------
void initTelematicsHardware() {
    // Configure TCRT5000 Digital Output pin with internal pull-up
    pinMode(PIN_TCRT5000_DO, INPUT_PULLUP);

    // Configure Active Buzzer and Diagnostic Status LED
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
    digitalWrite(PIN_STATUS_LED, LOW);

    // Initial hardware test chirp (2 short beeps)
    for (int i = 0; i < 2; i++) {
        digitalWrite(PIN_BUZZER, HIGH);
        digitalWrite(PIN_STATUS_LED, HIGH);
        delay(70);
        digitalWrite(PIN_BUZZER, LOW);
        digitalWrite(PIN_STATUS_LED, LOW);
        delay(70);
    }

    // Attach External Interrupt on Falling Edge (TCRT5000 DO is active-LOW on drop detection)
    attachInterrupt(digitalPinToInterrupt(PIN_TCRT5000_DO), isrDropDetected, FALLING);
    Serial.println("[ISR] Attached external interrupt on GPIO 19 (FALLING edge).");

    lastDropMillis = millis();
    lastDropMicros = micros();
}

// -------------------------------------------------------------------------------------
// 3. WI-FI CONNECTIVITY
// -------------------------------------------------------------------------------------
void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;

    Serial.printf("[WIFI] Connecting to SSID: %s ...\n", WIFI_SSID);
    WiFi.disconnect();
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 8000) {
        delay(250);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WIFI] Connected! Node IP: %s | RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        // Quick flash indicator
        digitalWrite(PIN_STATUS_LED, HIGH);
        delay(150);
        digitalWrite(PIN_STATUS_LED, LOW);
    } else {
        Serial.println("\n[WIFI] Connection pending/offline. Offline ring buffer activated.");
    }
}

// -------------------------------------------------------------------------------------
// 4. EDGE TELEMATICS MATHEMATICS (Report Section 7.3 & 7.4)
// -------------------------------------------------------------------------------------
void updateTelematicsCalculations() {
    unsigned long localDropCount = 0;
    unsigned long localIntervals[DPM_AVG_WINDOW];
    bool localNewDrop = false;

    // Critical section to safely read volatile ISR variables
    portENTER_CRITICAL(&isrMux);
    localDropCount = volatileDropCount;
    localNewDrop   = flagNewDropEvent;
    flagNewDropEvent = false;
    for (int i = 0; i < DPM_AVG_WINDOW; i++) {
        localIntervals[i] = dropIntervalsUs[i];
    }
    portEXIT_CRITICAL(&isrMux);

    if (localNewDrop) {
        lastDropMillis = millis();
        // Visual drop tick indicator on onboard blue LED
        digitalWrite(PIN_STATUS_LED, HIGH);
        delayMicroseconds(2000);
        digitalWrite(PIN_STATUS_LED, LOW);
    }

    totalDropCountSnapshot = localDropCount;

    // Compute moving average drop interval
    unsigned long sumIntervals = 0;
    for (int i = 0; i < DPM_AVG_WINDOW; i++) {
        sumIntervals += localIntervals[i];
    }
    float meanIntervalUs = (float)sumIntervals / (float)DPM_AVG_WINDOW;

    // Check for drop timeout (No drop detected for > 30s)
    unsigned long timeSinceLastDrop = millis() - lastDropMillis;
    if (timeSinceLastDrop > NO_FLOW_TIMEOUT_MS) {
        currentDpm = 0.0f;
    } else if (meanIntervalUs > 0) {
        // DPM = 60,000,000 µs / mean interval in µs (Report Section 7.3)
        currentDpm = 60000000.0f / meanIntervalUs;
    }

    // Flow Rate (mL/h) = DPM * 60 / DF (Report Section 7.4)
    // For Macro set (DF = 20): Flow = DPM * 3
    currentFlowMlHr = (currentDpm * 60.0f) / DRIP_FACTOR;

    // Volume Infused (mL) = total drops / DF
    currentVolInfusedMl = (float)totalDropCountSnapshot / DRIP_FACTOR;

    // Volume Remaining (mL) = Bag Volume - Volume Infused
    if (currentVolInfusedMl >= INITIAL_BOTTLE_VOL_ML) {
        currentVolRemainingMl = 0.0f;
    } else {
        currentVolRemainingMl = INITIAL_BOTTLE_VOL_ML - currentVolInfusedMl;
    }
}

// -------------------------------------------------------------------------------------
// 5. EDGE ALERT CLASSIFIER (Report Table 7.1)
// -------------------------------------------------------------------------------------
void evaluateClinicalAlerts() {
    unsigned long elapsedSinceDrop = millis() - lastDropMillis;

    // 1. Low Volume Alert (≤ 50 mL / ≤ 10% of 500 mL bag)
    if (currentVolRemainingMl <= LOW_VOLUME_THRESHOLD) {
        currentStatus = STATUS_LOW_VOLUME;
    }
    // 2. No Drop / Occlusion Alert (no drop for > 30s)
    else if (elapsedSinceDrop >= NO_FLOW_TIMEOUT_MS) {
        currentStatus = STATUS_NO_FLOW;
    }
    // 3. Free Flow Runaway Alarm (> 150 DPM)
    else if (currentDpm > FREE_FLOW_DPM_LIMIT) {
        currentStatus = STATUS_FREE_FLOW;
    }
    // 4. Rate Deviation Alarm (Outside ±20% of prescribed rate)
    else if (currentFlowMlHr > 0 && 
             fabs(currentFlowMlHr - activePrescribedFlow) > (activePrescribedFlow * RATE_ERROR_TOLERANCE)) {
        currentStatus = STATUS_RATE_ERR;
    }
    // 5. Normal Nominal State
    else {
        currentStatus = STATUS_NORMAL;
    }
}

// -------------------------------------------------------------------------------------
// 6. LOCAL ACOUSTIC BUZZER & LED CONTROL (WORKS OFFLINE - REPORT SECTION 7.5)
// -------------------------------------------------------------------------------------
void handleAcousticAndVisualIndicators() {
    unsigned long now = millis();
    bool isBedsideSilenced = (now < bedsideSilenceUntilMillis);

    // --- IMMEDIATE SILENCE CUT ---
    // If bedside silence is active AND the buzzer is currently physically ON,
    // cut it immediately without waiting for the next timer tick.
    // This eliminates the residual chirp when silence arrives mid-beep.
    if (isBedsideSilenced && digitalRead(PIN_BUZZER) == HIGH) {
        digitalWrite(PIN_BUZZER, LOW);
        // Don't reset buzzerState so LED visual pattern continues unaffected.
    }

    switch (currentStatus) {
        case STATUS_NO_FLOW:
            // Continuous High-Alert Alarm (Report Table 7.1)
            // 800ms ON / 200ms OFF
            if (now - buzzerBeepTimer > (buzzerState ? 800 : 200)) {
                buzzerBeepTimer = now;
                buzzerState = !buzzerState;
                digitalWrite(PIN_BUZZER, (buzzerState && !isBedsideSilenced) ? HIGH : LOW);
                digitalWrite(PIN_STATUS_LED, buzzerState ? HIGH : LOW);
            }
            break;

        case STATUS_FREE_FLOW:
            // Fast Urgent Beep (>150 DPM runaway)
            // 100ms ON / 100ms OFF
            if (now - buzzerBeepTimer > 100) {
                buzzerBeepTimer = now;
                buzzerState = !buzzerState;
                digitalWrite(PIN_BUZZER, (buzzerState && !isBedsideSilenced) ? HIGH : LOW);
                digitalWrite(PIN_STATUS_LED, buzzerState ? HIGH : LOW);
            }
            break;

        case STATUS_LOW_VOLUME:
            // Slow Intermittent Warning Beep
            // 400ms ON / 1200ms OFF
            if (now - buzzerBeepTimer > (buzzerState ? 400 : 1200)) {
                buzzerBeepTimer = now;
                buzzerState = !buzzerState;
                digitalWrite(PIN_BUZZER, (buzzerState && !isBedsideSilenced) ? HIGH : LOW);
                digitalWrite(PIN_STATUS_LED, buzzerState ? HIGH : LOW);
            }
            break;

        case STATUS_RATE_ERR:
            // Double Beep
            if (now - buzzerBeepTimer > (buzzerState ? 150 : 350)) {
                buzzerBeepTimer = now;
                buzzerState = !buzzerState;
                digitalWrite(PIN_BUZZER, (buzzerState && !isBedsideSilenced) ? HIGH : LOW);
                digitalWrite(PIN_STATUS_LED, buzzerState ? HIGH : LOW);
            }
            break;

        case STATUS_NORMAL:
        default:
            // Normal - Silence buzzer and LED
            digitalWrite(PIN_BUZZER, LOW);
            digitalWrite(PIN_STATUS_LED, LOW);
            buzzerState = false;
            break;
    }
}

// -------------------------------------------------------------------------------------
// 7. OFFLINE CIRCULAR RING BUFFER (Report Section 7.6)
// -------------------------------------------------------------------------------------
void pushToRingBuffer(const TelemetryPacket& packet) {
    ringBuffer[bufferHead] = packet;
    bufferHead = (bufferHead + 1) % RING_BUFFER_CAPACITY;

    if (bufferCount < RING_BUFFER_CAPACITY) {
        bufferCount++;
    } else {
        // Buffer full, tail advances to overwrite oldest
        bufferTail = (bufferTail + 1) % RING_BUFFER_CAPACITY;
    }
    Serial.printf("[BUFFER] Stored record. Buffered count: %d/%d\n", bufferCount, RING_BUFFER_CAPACITY);
}

void transmitBufferedPackets() {
    if (bufferCount == 0 || WiFi.status() != WL_CONNECTED) return;

    Serial.printf("[BUFFER] Reconnection burst: uploading %d pending records...\n", bufferCount);

    while (bufferCount > 0) {
        TelemetryPacket p = ringBuffer[bufferTail];
        if (sendTelemetryToGateway(p)) {
            bufferTail = (bufferTail + 1) % RING_BUFFER_CAPACITY;
            bufferCount--;
            delay(50); // slight throttle
        } else {
            Serial.println("[BUFFER] Re-transmission stalled; will retry next cycle.");
            break;
        }
    }
}

// -------------------------------------------------------------------------------------
// 8. HTTP TELEMETRY TRANSMISSION (Report Section 7.6)
// -------------------------------------------------------------------------------------
bool sendTelemetryToGateway(const TelemetryPacket& packet) {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    WiFiClientSecure secureClient;
    WiFiClient standardClient;

    if (String(TELEMETRY_POST_URL).startsWith("https://")) {
        secureClient.setInsecure();
        http.begin(secureClient, TELEMETRY_POST_URL);
    } else {
        http.begin(standardClient, TELEMETRY_POST_URL);
    }
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    StaticJsonDocument<256> doc;
    doc["device_id"]       = packet.deviceId;
    doc["dpm"]             = round(packet.dpm * 10.0f) / 10.0f;
    doc["flow_ml_hr"]      = round(packet.flowMlHr * 10.0f) / 10.0f;
    doc["vol_infused_ml"]  = round(packet.volInfusedMl * 10.0f) / 10.0f;
    doc["vol_remaining_ml"]= round(packet.volRemainingMl * 10.0f) / 10.0f;
    doc["status"]          = packet.status;

    String jsonPayload;
    serializeJson(doc, jsonPayload);

    int httpCode = http.POST(jsonPayload);
    bool success = (httpCode == 200 || httpCode == 201);

    if (success) {
        String respPayload = http.getString();
        StaticJsonDocument<384> respDoc;
        if (!deserializeJson(respDoc, respPayload)) {
            if (respDoc["bedside_silenced"] | false) {
                unsigned long sec = respDoc["silence_sec"] | 120;
                bedsideSilenceUntilMillis = millis() + (sec * 1000UL);
            } else {
                bedsideSilenceUntilMillis = 0;
            }
        }
        Serial.printf("[POST OK] %s | DPM: %.1f | Flow: %.1f mL/h | Rem: %.1f mL | Status: %s (HTTP %d) | Silenced: %s\n",
                      packet.deviceId, packet.dpm, packet.flowMlHr, packet.volRemainingMl, packet.status, httpCode,
                      (millis() < bedsideSilenceUntilMillis) ? "YES" : "NO");
    } else {
        Serial.printf("[POST FAIL] HTTP Error: %d | Host: %s\n", httpCode, TELEMETRY_POST_URL);
    }

    http.end();
    return success;
}

// -------------------------------------------------------------------------------------
// 9. DATA FETCHING FROM DASHBOARD (Bi-directional telemetry synchronization)
// -------------------------------------------------------------------------------------
bool fetchDashboardPrescription() {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    String url = String(TELEMETRY_GET_URL) + String(DEVICE_ID);
    WiFiClientSecure secureClient;
    WiFiClient standardClient;

    if (url.startsWith("https://")) {
        secureClient.setInsecure();
        http.begin(secureClient, url);
    } else {
        http.begin(standardClient, url);
    }
    http.setTimeout(5000);

    int httpCode = http.GET();
    if (httpCode == 200) {
        String payload = http.getString();
        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (!err && doc.containsKey("data")) {
            JsonObject data = doc["data"];
            float targetFlow = data["flow_ml_hr"] | PRESCRIBED_FLOW_ML_HR;
            if (targetFlow > 0 && targetFlow <= 500) {
                activePrescribedFlow = targetFlow;
            }
            if (data.containsKey("bedside_silenced")) {
                if (data["bedside_silenced"].as<bool>()) {
                    unsigned long sec = data["silence_sec"] | 120;
                    bedsideSilenceUntilMillis = millis() + (sec * 1000UL);
                } else {
                    bedsideSilenceUntilMillis = 0;
                }
            }
            Serial.printf("[FETCH SYNC] Dashboard verified | Prescribed: %.1f mL/h | Silenced: %s\n",
                          activePrescribedFlow, (millis() < bedsideSilenceUntilMillis) ? "YES" : "NO");
        }
        http.end();
        return true;
    }

    http.end();
    return false;
}

// -------------------------------------------------------------------------------------
// MAIN ARDUINO ENTRY POINTS
// -------------------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n==================================================================");
    Serial.println("  AMAL JYOTHI COLLEGE OF ENGINEERING - MCA (24MCAT213 - IoT)");
    Serial.println("  Smart IoT IV Fluid Infusion Monitoring Node (PlatformIO Edition)");
    Serial.printf("  Device: %s | Drip Factor: %.0f gtt/mL | Bag: %.0f mL\n",
                  DEVICE_ID, DRIP_FACTOR, INITIAL_BOTTLE_VOL_ML);
    Serial.println("==================================================================");

    initTelematicsHardware();
    connectWiFi();

    lastTelemetryMillis = millis();
    lastFetchMillis     = millis();
    lastWiFiRetryMillis = millis();
}

void loop() {
    unsigned long now = millis();

    // 1. Periodically check Wi-Fi reconnect
    if (WiFi.status() != WL_CONNECTED && now - lastWiFiRetryMillis > WIFI_RETRY_INTERVAL_MS) {
        lastWiFiRetryMillis = now;
        connectWiFi();
    }

    // 2. Flush offline buffered telemetry if connection restored (5s retry interval)
    if (bufferCount > 0 && WiFi.status() == WL_CONNECTED && (now - lastBufferRetryMillis >= 5000)) {
        lastBufferRetryMillis = now;
        transmitBufferedPackets();
    }

    // 3. Compute continuous edge flow and alert states
    updateTelematicsCalculations();
    evaluateClinicalAlerts();
    handleAcousticAndVisualIndicators();

    // 4. Send telemetry packet every 5 seconds normally; every 2 seconds during alarms
    // Shorter interval during active alarm ensures remote silence commands propagate quickly
    unsigned long activeTelemetryInterval = (currentStatus != STATUS_NORMAL)
                                             ? 2000UL : TELEMETRY_INTERVAL_MS;
    if (now - lastTelemetryMillis >= activeTelemetryInterval) {
        lastTelemetryMillis = now;

        TelemetryPacket packet;
        strncpy(packet.deviceId, DEVICE_ID, sizeof(packet.deviceId));
        packet.dpm            = currentDpm;
        packet.flowMlHr       = currentFlowMlHr;
        packet.volInfusedMl   = currentVolInfusedMl;
        packet.volRemainingMl = currentVolRemainingMl;
        strncpy(packet.status, statusToString(currentStatus), sizeof(packet.status));
        packet.timestampMs    = now;

        bool sent = sendTelemetryToGateway(packet);
        if (!sent) {
            pushToRingBuffer(packet);
        }
    }

    // 5. Fetch prescription & ward synchronization every 15 seconds
    if (now - lastFetchMillis >= FETCH_INTERVAL_MS) {
        lastFetchMillis = now;
        fetchDashboardPrescription();
    }

    // Yield CPU to background FreeRTOS tasks and Wi-Fi stack
    delay(10);
}
