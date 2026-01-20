#ifndef TRANSMITTER_CONFIG_H
#define TRANSMITTER_CONFIG_H

#include <Arduino.h>
#include "TransmitterConstants.h"

// ============================================================================
// Hardware Configuration
// ============================================================================
// Each device includes its own config.h that defines these values
// NOTE: config.h must be included BEFORE this file, after TransmitterConstants.h

// GPIO Pin Definitions (must be defined in device config.h)
#ifndef PEDAL_1_PIN
#error "PEDAL_1_PIN must be defined in device config.h"
#endif

#ifndef PEDAL_2_PIN
#error "PEDAL_2_PIN must be defined in device config.h"
#endif

#ifndef DEBUG_BUTTON_PIN
#error "DEBUG_BUTTON_PIN must be defined in device config.h"
#endif

// Optional GPIO pins (define as 255 if not available)
#ifndef LED_PIN
#define LED_PIN 255  // No LED
#endif

#ifndef PEDAL_1_NC_PIN
#define PEDAL_1_NC_PIN 255  // No NC pin for auto-detect
#endif

#ifndef PEDAL_2_NC_PIN
#define PEDAL_2_NC_PIN 255  // No NC pin for auto-detect
#endif

// Feature flags (define as 0 or 1 in device config.h)
#ifndef FEATURE_LED
#define FEATURE_LED 0  // LED support disabled by default
#endif

#ifndef FEATURE_AUTO_DETECT
#define FEATURE_AUTO_DETECT 0  // Auto-detect pedal mode disabled by default
#endif

#ifndef FEATURE_BATTERY_MONITOR
#define FEATURE_BATTERY_MONITOR 0  // Battery monitoring disabled by default
#endif

// Deep sleep wakeup: Always uses EXT1 to support both pedals (no configuration needed)

// PEDAL_MODE must be defined in device config.h
#ifndef PEDAL_MODE
#error "PEDAL_MODE must be defined in device config.h"
#endif

// Debug configuration
#ifndef DEBUG_ENABLED
#define DEBUG_ENABLED 1
#endif

// Power management (can be overridden in device config.h)
#ifndef INACTIVITY_TIMEOUT_MS
#define INACTIVITY_TIMEOUT_MS 300000  // 5 minutes
#endif

#ifndef IDLE_DELAY_PAIRED_MS
#define IDLE_DELAY_PAIRED_MS 10
#endif

#ifndef IDLE_DELAY_UNPAIRED_MS
#define IDLE_DELAY_UNPAIRED_MS 200
#endif

#ifndef DEBUG_BUTTON_DEBOUNCE_TIME_MS
#define DEBUG_BUTTON_DEBOUNCE_TIME_MS 50
#endif

// Helper macros
#define HAS_LED() (FEATURE_LED == 1 && LED_PIN != 255)
#define HAS_AUTO_DETECT() (FEATURE_AUTO_DETECT == 1 && PEDAL_1_NC_PIN != 255)
#define HAS_BATTERY_MONITOR() (FEATURE_BATTERY_MONITOR == 1)

#endif // TRANSMITTER_CONFIG_H
