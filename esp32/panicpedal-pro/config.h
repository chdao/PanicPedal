#ifndef PANICPEDAL_PRO_CONFIG_H
#define PANICPEDAL_PRO_CONFIG_H

// ============================================================================
// PanicPedal Pro Hardware Configuration
// ============================================================================

// GPIO Pin Definitions (PanicPedal Pro - ESP32-S3-WROOM)
#define PEDAL_1_PIN 2           // Left pedal switch NO (normally open) - GPIO2
#define PEDAL_2_PIN 1           // Right pedal switch NO (normally open) - GPIO1
#define PEDAL_1_NC_PIN 6        // Left pedal switch NC (normally closed) - for auto-detection
#define PEDAL_2_NC_PIN 7        // Right pedal switch NC (normally closed) - for auto-detection
#define DEBUG_BUTTON_PIN 10     // Debug pushbutton toggle
#define LED_PIN 8               // WS2812B LED control

// Optional hardware pins
#define BATTERY_VOLTAGE_PIN 9   // Battery voltage sensing (before TLV75733PDBV)
#define BATTERY_STAT1_PIN 4     // STAT1/LBO from MCP73871 (charging status)
#define TOGGLE_SWITCH_PIN 5     // Toggle switch

// Feature flags
#define FEATURE_LED 1                    // WS2812B LED support
#define FEATURE_AUTO_DETECT 1             // Auto-detect pedal mode via NC pins
#define FEATURE_BATTERY_MONITOR 0         // Battery monitoring (not yet implemented)

// Pedal mode configuration
// PEDAL_MODE_AUTO (-1) = Auto-detect based on connected switches (recommended)
// PEDAL_MODE_DUAL (0) = Force dual pedal mode
// PEDAL_MODE_SINGLE (1) = Force single pedal mode
#define PEDAL_MODE PEDAL_MODE_AUTO  // Change to PEDAL_MODE_DUAL or PEDAL_MODE_SINGLE to override

// Debug configuration
#define DEBUG_ENABLED 1  // Set to 0 to disable Serial output and save battery

// Power management
#define INACTIVITY_TIMEOUT_MS 300000  // 5 minutes of inactivity before deep sleep
#define IDLE_DELAY_PAIRED_MS 10       // 10ms delay when paired and idle
#define IDLE_DELAY_UNPAIRED_MS 200    // 200ms delay when not paired (longer to save power)

// Debug button
#define DEBUG_BUTTON_DEBOUNCE_TIME_MS 50

#endif // PANICPEDAL_PRO_CONFIG_H
