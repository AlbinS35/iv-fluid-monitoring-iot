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
volatile unsigned long dropIntervalsUs[DPM_AVG_WINDOW] = {0, 0, 0, 0, 0};
volatile uint8_t intervalIndex          = 0;
volatile uint8_t validIntervalCount     = 0;
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
unsigned long dropLedOffMillis       = 0;

float activeBottleVolMl              = INITIAL_BOTTLE_VOL_ML;
float currentDpm                     = 0.0f;
float currentFlowMlHr                = 0.0f;
float currentVolInfusedMl            = 0.0f;
float currentVolRemainingMl          = INITIAL_BOTTLE_VOL_ML;
ClinicalStatus currentStatus         = STATUS_NO_FLOW;

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

    // Hardware debounce filter (Report Section 7.2)
    if (lastDropMicros == 0 || diffUs >= DEBOUNCE_MICROS) {
        portENTER_CRITICAL_ISR(&isrMux);
        // Only treat diffUs as a continuous drop interval if drop occurred within 15 seconds
        if (lastDropMicros > 0 && diffUs <= 15000000UL) {
            dropIntervalsUs[intervalIndex] = diffUs;
            intervalIndex = (intervalIndex + 1) % DPM_AVG_WINDOW;
            if (validIntervalCount < DPM_AVG_WINDOW) {
                validIntervalCount = validIntervalCount + 1;
            }
        } else {
            // First drop or drop after a long pause: reset interval buffer
            validIntervalCount = 0;
            intervalIndex = 0;
        }

        lastDropMicros = nowUs;
        volatileDropCount = volatileDropCount + 1;
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

    // Initial hardware test chirp (2 short beeps & blinks)
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

    // Diagnostic sensor status check
    int sensorRaw = digitalRead(PIN_TCRT5000_DO);
    Serial.printf("[SENSOR] TCRT5000 DO (GPIO %d) initial state: %s\n",
                  PIN_TCRT5000_DO, (sensorRaw == HIGH) ? "HIGH (Idle - beam clear)" : "LOW (Triggered - beam blocked/reflecting)");
    if (sensorRaw == LOW) {
        Serial.println("[SENSOR WARNING] DO pin is reading LOW while idle!");
        Serial.println("[SENSOR WARNING] If no drop is in the chamber, adjust the blue potentiometer on the TCRT5000 module until its D0 LED turns OFF.");
    } else {
        Serial.println("[SENSOR OK] Sensor DO is HIGH (idle). The second LED on the sensor will blink on each drop.");
    }

    lastDropMillis = 0;
    lastDropMicros = 0;
}

// -------------------------------------------------------------------------------------
// 3. WI-FI CONNECTIVITY
// -------------------------------------------------------------------------------------
void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;

    Serial.printf("[WIFI] Connecting to SSID: %s ...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);           // CRITICAL: Disable modem sleep to prevent Wi-Fi dropouts on mobile hotspots
    WiFi.setAutoReconnect(true);    // Allow background reconnection without stalling loop
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
    uint8_t localValidCount = 0;
    bool localNewDrop = false;

    // Critical section to safely read volatile ISR variables
    portENTER_CRITICAL(&isrMux);
    localDropCount   = volatileDropCount;
    localNewDrop     = flagNewDropEvent;
    localValidCount  = validIntervalCount;
    flagNewDropEvent = false;
    for (int i = 0; i < DPM_AVG_WINDOW; i++) {
        localIntervals[i] = dropIntervalsUs[i];
    }
    portEXIT_CRITICAL(&isrMux);

    if (localNewDrop) {
        lastDropMillis = millis();
        // Visual drop tick indicator: keep status LED ON for 50ms (bright, human-visible blink)
        dropLedOffMillis = millis() + 50;
        digitalWrite(PIN_STATUS_LED, HIGH);
        Serial.printf("[DROP DETECTED] Drop #%lu | DO Pin Active\n", localDropCount);
    }

    totalDropCountSnapshot = localDropCount;

    // Compute moving average drop interval only over valid measured drop intervals
    float meanIntervalUs = 0.0f;
    if (localValidCount > 0) {
        unsigned long sumIntervals = 0;
        for (int i = 0; i < localValidCount; i++) {
            sumIntervals += localIntervals[i];
        }
        meanIntervalUs = (float)sumIntervals / (float)localValidCount;
    }

    // Time elapsed since the last registered drop
    unsigned long timeSinceLastDrop = (lastDropMillis > 0) ? (millis() - lastDropMillis) : NO_FLOW_TIMEOUT_MS;

    // Check if drops have stopped completely or no drops ever occurred
    if (totalDropCountSnapshot == 0 || timeSinceLastDrop >= NO_FLOW_TIMEOUT_MS) {
        currentDpm = 0.0f;
    } else if (meanIntervalUs > 0) {
        float expectedIntervalMs = meanIntervalUs / 1000.0f;
        // If elapsed time is more than 2.5x the expected drop interval (or > 4s), drops have stopped
        if (timeSinceLastDrop > 4000 && timeSinceLastDrop > (expectedIntervalMs * 2.5f)) {
            currentDpm = 0.0f;
        } else {
            currentDpm = 60000000.0f / meanIntervalUs;
            if (timeSinceLastDrop > 2500) {
                float instantDpm = 60000.0f / (float)timeSinceLastDrop;
                if (instantDpm < currentDpm) {
                    currentDpm = instantDpm;
                }
            }
        }
    } else {
        // Only 1 drop registered so far; waiting for second drop to calculate rate
        currentDpm = 0.0f;
    }

    // Flow Rate (mL/h) = DPM * 60 / DF (Report Section 7.4)
    currentFlowMlHr = (currentDpm * 60.0f) / DRIP_FACTOR;

    // Volume Infused (mL) = total drops / DF
    currentVolInfusedMl = (float)totalDropCountSnapshot / DRIP_FACTOR;

    // Volume Remaining (mL) = Container Volume - Volume Infused
    if (currentVolInfusedMl >= activeBottleVolMl) {
        currentVolRemainingMl = 0.0f;
    } else {
        currentVolRemainingMl = activeBottleVolMl - currentVolInfusedMl;
    }
}

// -------------------------------------------------------------------------------------
// 5. EDGE ALERT CLASSIFIER (Report Table 7.1)
// -------------------------------------------------------------------------------------
void evaluateClinicalAlerts() {
    unsigned long elapsedSinceDrop = (lastDropMillis > 0) ? (millis() - lastDropMillis) : NO_FLOW_TIMEOUT_MS;

    // 1. No Drop / Occlusion Alert (Highest Priority when line is blocked or drops stopped)
    if (totalDropCountSnapshot == 0 || currentDpm <= 0.5f || elapsedSinceDrop >= NO_FLOW_TIMEOUT_MS) {
        currentStatus = STATUS_NO_FLOW;
    }
    // 2. Low Volume Alert (≤ 30 mL / ≤ 15% of 200 mL container)
    else if (currentVolRemainingMl <= LOW_VOLUME_THRESHOLD) {
        currentStatus = STATUS_LOW_VOLUME;
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
    bool isDropFlashing    = (now < dropLedOffMillis);

    // --- IMMEDIATE SILENCE CUT ---
    // If bedside silence is active AND the buzzer is currently physically ON,
    // cut it immediately without waiting for the next timer tick.
    if (isBedsideSilenced && digitalRead(PIN_BUZZER) == HIGH) {
        digitalWrite(PIN_BUZZER, LOW);
    }

    switch (currentStatus) {
        case STATUS_NO_FLOW:
            // Continuous High-Alert Alarm (Report Table 7.1)
            // 800ms ON / 200ms OFF
            if (now - buzzerBeepTimer > (buzzerState ? 800 : 200)) {
                buzzerBeepTimer = now;
                buzzerState = !buzzerState;
                digitalWrite(PIN_BUZZER, (buzzerState && !isBedsideSilenced) ? HIGH : LOW);
                digitalWrite(PIN_STATUS_LED, buzzerState ? HIGH : (isDropFlashing ? HIGH : LOW));
            }
            break;

        case STATUS_FREE_FLOW:
            // Fast Urgent Beep (>150 DPM runaway)
            // 100ms ON / 100ms OFF
            if (now - buzzerBeepTimer > 100) {
                buzzerBeepTimer = now;
                buzzerState = !buzzerState;
                digitalWrite(PIN_BUZZER, (buzzerState && !isBedsideSilenced) ? HIGH : LOW);
                digitalWrite(PIN_STATUS_LED, buzzerState ? HIGH : (isDropFlashing ? HIGH : LOW));
            }
            break;

        case STATUS_LOW_VOLUME:
            // Slow Intermittent Warning Beep
            // 400ms ON / 1200ms OFF
            if (now - buzzerBeepTimer > (buzzerState ? 400 : 1200)) {
                buzzerBeepTimer = now;
                buzzerState = !buzzerState;
                digitalWrite(PIN_BUZZER, (buzzerState && !isBedsideSilenced) ? HIGH : LOW);
                digitalWrite(PIN_STATUS_LED, buzzerState ? HIGH : (isDropFlashing ? HIGH : LOW));
            }
            break;

        case STATUS_RATE_ERR:
            // Double Beep
            if (now - buzzerBeepTimer > (buzzerState ? 150 : 350)) {
                buzzerBeepTimer = now;
                buzzerState = !buzzerState;
                digitalWrite(PIN_BUZZER, (buzzerState && !isBedsideSilenced) ? HIGH : LOW);
                digitalWrite(PIN_STATUS_LED, buzzerState ? HIGH : (isDropFlashing ? HIGH : LOW));
            }
            break;

        case STATUS_NORMAL:
        default:
            // Normal Infusion: Buzzer OFF, LED blinks for 50ms upon each drop, otherwise OFF
            digitalWrite(PIN_BUZZER, LOW);
            digitalWrite(PIN_STATUS_LED, isDropFlashing ? HIGH : LOW);
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
    http.setTimeout(8000);

    StaticJsonDocument<256> doc;
    doc["device_id"]        = packet.deviceId;
    doc["dpm"]              = round(packet.dpm * 10.0f) / 10.0f;
    doc["flow_ml_hr"]       = round(packet.flowMlHr * 10.0f) / 10.0f;
    doc["vol_infused_ml"]   = round(packet.volInfusedMl * 10.0f) / 10.0f;
    doc["vol_remaining_ml"] = round(packet.volRemainingMl * 10.0f) / 10.0f;
    doc["container_vol_ml"] = round(activeBottleVolMl * 10.0f) / 10.0f;
    doc["status"]           = packet.status;

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
            float cVol = 0.0f;
            if (respDoc.containsKey("container_vol_ml")) {
                cVol = respDoc["container_vol_ml"].as<float>();
            } else if (respDoc.containsKey("bottle_vol_ml")) {
                cVol = respDoc["bottle_vol_ml"].as<float>();
            }
            if (cVol >= 50.0f && cVol <= 2000.0f && fabs(cVol - activeBottleVolMl) > 0.5f) {
                activeBottleVolMl = cVol;
            }
        }
        Serial.printf("[POST OK] %s | DPM: %.1f | Flow: %.1f mL/h | Rem: %.1f/%.1f mL | Status: %s (HTTP %d) | Silenced: %s\n",
                      packet.deviceId, packet.dpm, packet.flowMlHr, packet.volRemainingMl, activeBottleVolMl, packet.status, httpCode,
                      (millis() < bedsideSilenceUntilMillis) ? "YES" : "NO");
    } else {
        Serial.printf("[POST FAIL] HTTP Error: %d | Host: %s\n", httpCode, TELEMETRY_POST_URL);
    }

    http.end();
    secureClient.stop();
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
    http.setTimeout(8000);

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
            float cVol = 0.0f;
            if (data.containsKey("container_vol_ml")) {
                cVol = data["container_vol_ml"].as<float>();
            } else if (data.containsKey("bottle_vol_ml")) {
                cVol = data["bottle_vol_ml"].as<float>();
            }
            if (cVol >= 50.0f && cVol <= 2000.0f && fabs(cVol - activeBottleVolMl) > 0.5f) {
                activeBottleVolMl = cVol;
                Serial.printf("[FETCH SYNC] Container volume synchronized from dashboard: %.1f mL\n", activeBottleVolMl);
            }
            if (data.containsKey("bedside_silenced")) {
                if (data["bedside_silenced"].as<bool>()) {
                    unsigned long sec = data["silence_sec"] | 120;
                    bedsideSilenceUntilMillis = millis() + (sec * 1000UL);
                } else {
                    bedsideSilenceUntilMillis = 0;
                }
            }
            Serial.printf("[FETCH SYNC] Dashboard verified | Prescribed: %.1f mL/h | Container: %.1f mL | Silenced: %s\n",
                          activePrescribedFlow, activeBottleVolMl, (millis() < bedsideSilenceUntilMillis) ? "YES" : "NO");
        }
        http.end();
        secureClient.stop();
        return true;
    }

    http.end();
    secureClient.stop();
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

    // 4. Send telemetry packet every 5 seconds normally; every 4 seconds during alarms
    // 4s ensures fast alarm notification without saturating ESP32 lwIP TLS sockets
    unsigned long activeTelemetryInterval = (currentStatus != STATUS_NORMAL)
                                             ? 4000UL : TELEMETRY_INTERVAL_MS;
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
