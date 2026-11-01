/**
 * =====================================================================================
 * Smart IoT IV Fluid Infusion Monitoring and Drip-Rate Telematics System
 * Telematics Header & Data Structures
 * =====================================================================================
 */

#pragma once

#include <Arduino.h>
#include "config.h"

// Alert Status Enum matching Report Table 7.1 and Table 9.2
enum ClinicalStatus {
    STATUS_NORMAL,
    STATUS_LOW_VOLUME,
    STATUS_NO_FLOW,
    STATUS_FREE_FLOW,
    STATUS_RATE_ERR
};

// Telemetry Data Record for Live Transmission & Ring Buffer
struct TelemetryPacket {
    char deviceId[32];
    float dpm;
    float flowMlHr;
    float volInfusedMl;
    float volRemainingMl;
    char status[16];
    unsigned long timestampMs;
};

// Helper: Convert ClinicalStatus enum to standard string
inline const char* statusToString(ClinicalStatus stat) {
    switch (stat) {
        case STATUS_LOW_VOLUME: return "LOW_VOLUME";
        case STATUS_NO_FLOW:    return "NO_FLOW";
        case STATUS_FREE_FLOW:  return "FREE_FLOW";
        case STATUS_RATE_ERR:   return "RATE_ERR";
        case STATUS_NORMAL:
        default:                return "NORMAL";
    }
}

// Function Declarations
void initTelematicsHardware();
void isrDropDetected();
void updateTelematicsCalculations();
void evaluateClinicalAlerts();
void handleAcousticAndVisualIndicators();
bool sendTelemetryToGateway(const TelemetryPacket& packet);
bool fetchDashboardPrescription();
void pushToRingBuffer(const TelemetryPacket& packet);
void transmitBufferedPackets();
void connectWiFi();
