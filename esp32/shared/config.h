#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// System Configuration
// ============================================================================

// Maximum number of pedal slots supported by receiver
#define MAX_PEDAL_SLOTS 2

// ============================================================================
// Timing Configuration - Pairing & Discovery
// ============================================================================

// Grace period timeout - receiver actively seeks transmitters during this time
#define TRANSMITTER_TIMEOUT_MS 30000  // 30 seconds

// Initial ping wait period - delay before starting grace period
#define INITIAL_PING_WAIT_MS 1000  // 1 second

// Beacon interval - how often receiver broadcasts availability
#define BEACON_INTERVAL_MS 2000  // 2 seconds

// Alive response timeout - wait time for transmitters to respond to ping
#define ALIVE_RESPONSE_TIMEOUT_MS 2000  // 2 seconds

// Discovery request interval (debug monitor)
#define DISCOVERY_SEND_INTERVAL_MS 3000  // 3 seconds

// ============================================================================
// Timing Configuration - Power Management
// ============================================================================

// Inactivity timeout before entering deep sleep (transmitter)
#define INACTIVITY_TIMEOUT_MS 300000  // 5 minutes

// Idle loop delays (transmitter)
#define IDLE_DELAY_PAIRED_MS 10      // When paired - responsive
#define IDLE_DELAY_UNPAIRED_MS 200   // When not paired - power saving

// Debug monitor adaptive delays
#define DEBUG_MONITOR_DELAY_ACTIVE_MS 20   // When messages are queued (50Hz)
#define DEBUG_MONITOR_DELAY_IDLE_MS 100    // When idle (10Hz)

// ============================================================================
// Timing Configuration - Hardware
// ============================================================================

// Pedal debounce time
#define DEBOUNCE_TIME_MS 50

// Debug button debounce time
#define DEBUG_BUTTON_DEBOUNCE_TIME_MS 50

// ESP-NOW peer ready delay (ESP32-S3 requires this)
#define ESPNOW_PEER_READY_DELAY_MS 2

// WiFi initialization delays (receiver)
#define WIFI_INIT_DELAY_MS 100
#define WIFI_DISCONNECT_DELAY_MS 100

// ============================================================================
// Timing Configuration - Monitoring
// ============================================================================

// Heartbeat interval (receiver status updates)
#define HEARTBEAT_INTERVAL_MS 60000  // 1 minute

// Slot calculation cache duration (receiver)
#define SLOT_CALCULATION_CACHE_MS 100

#endif // CONFIG_H
