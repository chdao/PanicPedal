#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

// Clean Architecture: Include shared and domain modules
#include "shared/messages.h"
#include "domain/PairingState.h"
#include "domain/PedalReader.h"
#include "infrastructure/EspNowTransport.h"
#include "infrastructure/DebugMonitor.h"
#include "application/PairingService.h"
#include "application/PedalService.h"

// ============================================================================
// CONFIGURATION
// ============================================================================
#define PEDAL_MODE 1  // 0=DUAL (pins 13 & 14), 1=SINGLE (pin 13 only)
// NOTE: Debug mode is now controlled by the button on pin 27 (FireBeetle).
//       Press the button to enable debug output (both Serial and debug monitor).
//       Debug state is runtime-controlled, not compile-time.
// ============================================================================

#define PEDAL_1_PIN 13
#define PEDAL_2_PIN 14
#define DEBUG_BUTTON_PIN 27  // FireBeetle button pin
#define INACTIVITY_TIMEOUT 120000  // 2 minutes - reduced for better battery life
#define IDLE_DELAY_PAIRED_MIN 10  // 10ms minimum delay for responsiveness after activity
#define IDLE_DELAY_PAIRED_MAX 20  // 20ms maximum delay when idle
#define IDLE_DELAY_UNPAIRED 200  // 200ms delay when not paired
#define ACTIVITY_BOOST_DURATION 2000  // Keep responsive for 2 seconds after pedal press
#define SERIAL_INIT_DELAY_MS 100
#define HEARTBEAT_INTERVAL_MS 5000  // 5 seconds
#define DEBUG_BUTTON_DEBOUNCE_MS 50  // Button debounce time

// Domain layer instances
PairingState pairingState;
PedalReader pedalReader;
EspNowTransport transport;
DebugMonitor debugMonitor;

// Application layer instances
PairingService pairingService;
PedalService pedalService;

// System state
unsigned long lastActivityTime = 0;
unsigned long lastPedalActivityTime = 0;  // Track last pedal press for dynamic delay
unsigned long bootTime = 0;
bool debugEnabled = false;  // Runtime debug state (controlled by button toggle)
bool lastButtonState = HIGH;
unsigned long lastButtonChangeTime = 0;
bool buttonPressed = false;  // Track if button press has been processed

// Forward declarations
void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);
void onPaired(const uint8_t* receiverMAC);
void onActivity();
void debugPrint(const char* format, ...);

void debugPrint(const char* format, ...) {
  if (!debugEnabled) return;
  
  char buffer[200];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  
  // Send to debug monitor only (debugMonitor_print adds [TRANSMITTER] prefix and timestamp)
  if (debugMonitor.paired && debugMonitor.espNowInitialized) {
    debugMonitor_print(&debugMonitor, "%s", buffer);
  }
}

void onPaired(const uint8_t* receiverMAC) {
  if (!debugEnabled) return;
  
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           receiverMAC[0], receiverMAC[1], receiverMAC[2],
           receiverMAC[3], receiverMAC[4], receiverMAC[5]);
  
  debugPrint("Paired with receiver: %s\n", macStr);
}

void onActivity() {
  lastActivityTime = millis();
  lastPedalActivityTime = millis();  // Track pedal activity for dynamic delay
}

void sendDeleteRecordMessage(const uint8_t* receiverMAC) {
  struct_message deleteMsg = {MSG_DELETE_RECORD, 0, false, 0};
  espNowTransport_send(&transport, receiverMAC, (uint8_t*)&deleteMsg, sizeof(deleteMsg));
  // Debug message is sent by caller for context
}

void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel) {
  if (len < 1) return;
  
  uint8_t msgType = data[0];
  
  // Handle debug monitor discovery request (legacy)
  if (msgType == MSG_DEBUG_MONITOR_REQ) {
    debugMonitor_handleDiscoveryRequest(&debugMonitor, senderMAC, channel);
    return;  // No debug spam for monitor internal operations
  }
  
  // Handle debug monitor beacon (new proactive discovery)
  if (msgType == MSG_DEBUG_MONITOR_BEACON && len >= sizeof(debug_monitor_beacon_message)) {
    debug_monitor_beacon_message* beacon = (debug_monitor_beacon_message*)data;
    debugMonitor_handleBeacon(&debugMonitor, beacon->monitorMAC, channel);
    return;  // No debug spam for monitor internal operations
  }
  
  // Handle beacon message (silently - beacons are routine)
  if (msgType == MSG_BEACON && len >= sizeof(beacon_message)) {
    beacon_message* beacon = (beacon_message*)data;
    pairingService_handleBeacon(&pairingService, senderMAC, beacon);
    return;
  }
  
  // Handle other messages
  if (len < sizeof(struct_message)) {
    return;  // Silently ignore short messages
  }
  
  struct_message* msg = (struct_message*)data;
  
  if (pairingState_isPaired(&pairingState)) {
    // Already paired - check if message is from our paired receiver
    if (macEqual(senderMAC, pairingState.pairedReceiverMAC)) {
      // Message from our paired receiver - silently accept (no debug spam for normal operation)
    } else {
      // Message from different receiver - conflict detected
      if (msg->msgType == MSG_ALIVE || msg->msgType == MSG_DISCOVERY_RESP) {
        if (debugEnabled) {
          debugPrint("Conflict: message from different receiver, sending DELETE_RECORD\n");
        }
        
        espNowTransport_addPeer(&transport, senderMAC, channel);
        sendDeleteRecordMessage(senderMAC);
      }
    }
  } else {
    // Not paired - handle pairing messages
    if (msg->msgType == MSG_DISCOVERY_RESP) {
      pairingService_handleDiscoveryResponse(&pairingService, senderMAC, channel);
    } else if (msg->msgType == MSG_ALIVE) {
      pairingService_handleAlive(&pairingService, senderMAC, channel);
    }
  }
}


void goToDeepSleep() {
  if (debugEnabled) {
    debugPrint("Going to deep sleep...\n");
  }
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PEDAL_1_PIN, LOW);
  esp_deep_sleep_start();
}

void setup() {
  // Initialize Serial (minimal use - only for startup message)
  Serial.begin(115200);
  delay(SERIAL_INIT_DELAY_MS);
  
  // Initialize debug button (pin 27 on FireBeetle) - toggle switch
  pinMode(DEBUG_BUTTON_PIN, INPUT_PULLUP);
  lastButtonState = digitalRead(DEBUG_BUTTON_PIN);
  lastButtonChangeTime = millis();
  buttonPressed = false;

  // Battery optimization: Reduce CPU frequency and enable WiFi power save
  setCpuFrequencyMhz(80);  // 80MHz is sufficient for pedal operations
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);  // Maximum WiFi power saving
  
  // Reduce WiFi transmit power for better battery life (pedals are used close to receiver)
  esp_wifi_set_max_tx_power(40);  // Reduce from default 84 (20dBm) to 40 (10dBm)
  
  bootTime = millis();
  lastActivityTime = millis();
  
  // Initialize domain layer
  pairingState_init(&pairingState);
  pedalReader_init(&pedalReader, PEDAL_1_PIN, PEDAL_2_PIN, PEDAL_MODE);
  
  // Initialize infrastructure layer
  espNowTransport_init(&transport);
  
  // Initialize debug monitor (needed before loading debug state)
  debugMonitor_init(&debugMonitor, &transport, bootTime);
  debugMonitor_load(&debugMonitor);
  debugMonitor.espNowInitialized = transport.initialized;
  
  // Get device MAC address now that WiFi is initialized
  // WiFi.mode(WIFI_STA) was called in espNowTransport_init, so MAC should be available
  if (transport.initialized) {
    WiFi.macAddress(debugMonitor.deviceMAC);
    // Verify MAC was read correctly (not all zeros)
    if (!isValidMAC(debugMonitor.deviceMAC)) {
      // Retry after a small delay
      delay(50);
      WiFi.macAddress(debugMonitor.deviceMAC);
    }
  }
  
  // Load saved debug state (restored after deep sleep)
  debugEnabled = debugMonitor_loadDebugState();
  
  // Add broadcast peer (only if initialized)
  if (transport.initialized) {
    uint8_t broadcastMAC[] = BROADCAST_MAC;
    espNowTransport_addPeer(&transport, broadcastMAC, 0);
    
    // Add saved debug monitor as peer (if it was saved)
    if (debugMonitor.paired) {
      espNowTransport_addPeer(&transport, debugMonitor.mac, 0);
      delay(DEBUG_MONITOR_PEER_READY_DELAY_MS);
    }
    
    espNowTransport_registerReceiveCallback(&transport, onMessageReceived);
  }
  
  // Initialize application layer
  pairingService_init(&pairingService, &pairingState, &transport, PEDAL_MODE, bootTime);
  pairingService.onPaired = onPaired;
  
  pedalService_init(&pedalService, &pedalReader, &pairingState, &transport, &lastActivityTime, bootTime);
  pedalService.onActivity = onActivity;
  pedalService_setPairingService(&pairingService);
  
  // Broadcast that we're online
  pairingService_broadcastOnline(&pairingService);
  
  // Only print startup message if debug monitor is paired
  if (debugMonitor.paired && debugMonitor.espNowInitialized) {
    Serial.println("Debug logs are being sent to the debug monitor");
    debugMonitor_print(&debugMonitor, "ESP-NOW initialized");
    debugMonitor_print(&debugMonitor, "=== Transmitter Ready ===");
  }
}

void loop() {
  unsigned long currentTime = millis();
  
  // Check debug button state (toggle on press, with debouncing)
  bool currentButtonState = digitalRead(DEBUG_BUTTON_PIN);
  
  // Detect button press (transition from HIGH to LOW)
  if (currentButtonState == LOW && lastButtonState == HIGH) {
    // Button just pressed - start debounce timer
    lastButtonChangeTime = currentTime;
    buttonPressed = false;  // Reset press flag
  }
  
  // Check if button has been held down long enough (debounce)
  if (currentButtonState == LOW && !buttonPressed && 
      (currentTime - lastButtonChangeTime) >= DEBUG_BUTTON_DEBOUNCE_MS) {
    // Button press confirmed - toggle debug state
    buttonPressed = true;
    debugEnabled = !debugEnabled;
    
    // Save debug state to persist across deep sleep
    debugMonitor_saveDebugState(debugEnabled);
    
    if (debugEnabled && debugMonitor.paired && debugMonitor.espNowInitialized) {
      debugMonitor_print(&debugMonitor, "Debug mode enabled via button toggle");
    }
  }
  
  // Update last button state
  if (currentButtonState == HIGH && lastButtonState == LOW) {
    // Button released - reset for next press
    buttonPressed = false;
  }
  lastButtonState = currentButtonState;
  
  // Check inactivity timeout
  if (currentTime - lastActivityTime > INACTIVITY_TIMEOUT) {
    goToDeepSleep();
  }
  
  // Update pedal service (handles pedal reading and events)
  pedalService_update(&pedalService);
  
  // Update debug monitor (checks for beacons, manages connection)
  debugMonitor_update(&debugMonitor, currentTime);
  
  // Periodic status when not paired (only when debug enabled)
  if (debugEnabled) {
    static unsigned long lastHeartbeat = 0;
    if (!pairingState_isPaired(&pairingState) && (currentTime - lastHeartbeat > HEARTBEAT_INTERVAL_MS)) {
      debugPrint("Waiting for receiver...\n");
      lastHeartbeat = currentTime;
    }
  }
  
  // Battery optimization: Dynamic delay based on pairing status and recent activity
  if (pairingState_isPaired(&pairingState)) {
    // Use shorter delay immediately after pedal activity for better responsiveness
    unsigned long timeSinceActivity = currentTime - lastPedalActivityTime;
    if (timeSinceActivity < ACTIVITY_BOOST_DURATION) {
      // Recently pressed - use minimal delay for best responsiveness
      delay(IDLE_DELAY_PAIRED_MIN);
    } else {
      // Idle - use normal delay for better power savings
      delay(IDLE_DELAY_PAIRED_MAX);
    }
  } else {
    // Not paired - use longer delay to save battery while searching
    delay(IDLE_DELAY_UNPAIRED);
  }
}

// Include implementation files (Arduino IDE doesn't auto-compile .cpp files in subdirectories)
#include "domain/PairingState.cpp"
#include "domain/PedalReader.cpp"
#include "infrastructure/EspNowTransport.cpp"
#include "infrastructure/DebugMonitor.cpp"
#include "application/PairingService.cpp"
#include "application/PedalService.cpp"
