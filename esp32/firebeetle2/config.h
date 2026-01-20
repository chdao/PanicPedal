#ifndef FIREBEETLE_CONFIG_H
#define FIREBEETLE_CONFIG_H

// ============================================================================
// FireBeetle 2 Hardware Configuration
// ============================================================================

// GPIO Pin Definitions (FireBeetle 2 ESP32-E)
#define PEDAL_1_PIN 13           // Left pedal switch NO (normally open)
#define PEDAL_2_PIN 14           // Right pedal switch NO (normally open) - dual mode only
#define DEBUG_BUTTON_PIN 27      // GPIO 27 (A5) - Ground this pin to enable debug output

// No NC pins for auto-detection
// No LED support

// Feature flags
#define FEATURE_LED 0                    // No LED support
#define FEATURE_AUTO_DETECT 0            // No auto-detect (manual mode only)
#define FEATURE_BATTERY_MONITOR 0        // No battery monitoring

// Pedal mode configuration
#define PEDAL_MODE 1     // 0=DUAL (pins 13 & 14), 1=SINGLE (pin 13 only)

// Debug configuration
#define DEBUG_ENABLED 1  // Set to 0 to disable Serial output and save battery (temporarily enabled for debugging)

// Power management
#define INACTIVITY_TIMEOUT_MS 300000  // 5 minutes of inactivity before deep sleep
#define IDLE_DELAY_PAIRED_MS 50       // 50ms delay when paired and idle
#define IDLE_DELAY_UNPAIRED_MS 200    // 200ms delay when not paired (longer to save power)

// Debug button
#define DEBUG_BUTTON_DEBOUNCE_TIME_MS 50

#endif // FIREBEETLE_CONFIG_H
