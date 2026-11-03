/**
 * =====================================================================================
 * Smart IoT IV Fluid Infusion Monitoring and Drip-Rate Telematics System
 * PlatformIO Embedded Firmware Configuration
 * Amal Jyothi College of Engineering - MCA 24MCAT213 (Internet of Things)
 * =====================================================================================
 */

#pragma once

#include <Arduino.h>

// -------------------------------------------------------------------------------------
// 1. WI-FI & FOG GATEWAY NETWORK CONFIGURATION
// -------------------------------------------------------------------------------------
// Replace with your hospital ward Wi-Fi SSID / password or laptop mobile hotspot
#define WIFI_SSID             "Redmi Note 11"
#define WIFI_PASSWORD         "Albin@123"
#define WIFI_RETRY_INTERVAL_MS 10000

// Ward Gateway Host (Render Cloud Deployment)
#define GATEWAY_HOST          "https://iv-fluid-monitoring-iot.onrender.com"
#define TELEMETRY_POST_URL    GATEWAY_HOST "/api/telemetry"
#define TELEMETRY_GET_URL     GATEWAY_HOST "/api/latest/"

// Device & Bed Identifier (Report Table 9.2)
#define DEVICE_ID             "WARD-A-BED-07"

// -------------------------------------------------------------------------------------
// 2. HARDWARE PIN DEFINITIONS (Report Table 8.1 & Section 16.1)
// -------------------------------------------------------------------------------------
// TCRT5000 Optical Infrared Drop Sensor DO -> ESP32 GPIO 19 (External Interrupt)
#define PIN_TCRT5000_DO       19

// Active 5V Buzzer -> Driven by BC547 NPN Transistor from ESP32 GPIO 18
#define PIN_BUZZER            18

// ESP32 Onboard Diagnostic Blue LED -> GPIO 2
#define PIN_STATUS_LED        2

// -------------------------------------------------------------------------------------
// 3. CLINICAL INFUSION PARAMETERS (Report Section 7.4)
// -------------------------------------------------------------------------------------
// Standard Adult Macro Drip Set (Romsons): 20 drops = 1 mL
#define DRIP_FACTOR           20.0f

// Standard IV Normal Saline / Dextrose Bag Volume (mL)
// Matches user's 150-200 mL IV container (can also be 150.0f, 200.0f, or 500.0f)
#define INITIAL_BOTTLE_VOL_ML 200.0f

// Prescribed Clinical Infusion Rate (mL/h) -> 40 DPM on Macro 20 Set
#define PRESCRIBED_FLOW_ML_HR 120.0f

// Allowable Clinical Rate Tolerance (±20% per Report Table 7.1)
#define RATE_ERROR_TOLERANCE  0.20f

// -------------------------------------------------------------------------------------
// 4. TIMING & ALERT THRESHOLDS (Report Table 7.1)
// -------------------------------------------------------------------------------------
// Hardware ISR Debounce Lockout in microseconds (50 ms = 50,000 µs)
#define DEBOUNCE_MICROS       50000UL

// No-Drop Occlusion / Empty Bag Timeout in milliseconds (8 seconds for prompt live alert)
#define NO_FLOW_TIMEOUT_MS    8000UL

// Free-Flow Runaway Alarm Threshold (Drops per Minute)
#define FREE_FLOW_DPM_LIMIT   150.0f

// Low-Volume Reservoir Warning Threshold (≤ 15% of 200 mL = 30 mL)
#define LOW_VOLUME_THRESHOLD  30.0f

// Periodic Telemetry Transmission Interval (Report Section 7.6)
#define TELEMETRY_INTERVAL_MS 5000UL

// Periodic Dashboard Prescription Fetch Interval (Data fetching from Dashboard)
#define FETCH_INTERVAL_MS     15000UL

// Moving Average Window for DPM (Average of last 5 drop intervals)
#define DPM_AVG_WINDOW        5

// Offline Ring Buffer Capacity (Holds 60 packets = 5 minutes during network outages)
#define RING_BUFFER_CAPACITY  60
