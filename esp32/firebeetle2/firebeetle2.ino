#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_sleep.h"
#include <stdarg.h>
#include <Preferences.h>

// Clean Architecture: Include shared and domain modules
#include "shared/messages.h"
#include "shared/debug_format.h"
#include "shared/domain/PairingState.h"
#include "shared/domain/PedalReader.h"
#include "shared/domain/MacUtils.h"

// Forward declarations for ISR functions
void IRAM_ATTR pedal1ISR();
void IRAM_ATTR pedal2ISR();
void IRAM_ATTR debugToggleISR();
#include "shared/infrastructure/EspNowTransport.h"
#include "shared/application/PairingService.h"
#include "shared/application/PedalService.h"

// ============================================================================
// CONFIGURATION
// ============================================================================
#define PEDAL_MODE 1  // 0=DUAL (pins 13 & 14), 1=SINGLE (pin 13 only)
#define DEBUG_ENABLED 1  // Set to 0 to disable Serial output and save battery
// ============================================================================

#define PEDAL_1_PIN 13
#define PEDAL_2_PIN 14
#define DEBUG_PIN 27  // GPIO 27 (A5) - Ground this pin to enable debug output
#define INACTIVITY_TIMEOUT 300000  // 5 minutes
#define IDLE_DELAY_PAIRED 50  // 50ms delay when paired (reduced from 20ms since we don't poll GPIO anymore)
#define IDLE_DELAY_UNPAIRED 200  // 200ms delay when not paired

// Domain layer instances
PairingState pairingState;
PedalReader pedalReader;
EspNowTransport transport;

// Application layer instances
PairingService pairingService;
PedalService pedalService;

// System state
unsigned long lastActivityTime = 0;
unsigned long bootTime = 0;

// Debug support - toggle via GPIO27 button press
volatile bool debugToggleFlag = false;
bool debugEnabled = false;
EspNowTransport* g_debugTransport = nullptr;  // Set in setup()
Preferences debugPreferences; // NVS for storing debug state

void IRAM_ATTR debugToggleISR() {
  debugToggleFlag = true;
}

// Use shared utilities for debug/serial output
#include "shared/infrastructure/TransmitterUtils.h"

// Cache MAC address to avoid repeated WiFi.macAddress() calls (power optimization)
static uint8_t g_cachedMAC[6] = {0};
static bool g_macCached = false;

static void cacheMAC() {
  if (!g_macCached) {
    WiFi.macAddress(g_cachedMAC);
    g_macCached = true;
  }
}

// Unified debug function: always sends same message to Serial and debug monitor
// This ensures consistency - both outputs receive identical messages
// Note: debugEnabled flag can be checked before calling this function for conditional logic
void debugPrint(const char* format, ...) {
  // Runtime gate: when debug is disabled, do not emit logs to Serial or debug monitor.
  // This keeps normal operation quiet and reduces the chance of WDT issues from heavy I/O.
  if (!debugEnabled) {
    return;
  }

  // Use cached MAC address
  cacheMAC();
  
  // Format message with standardized format
  char buffer[250];
  va_list args;
  va_start(args, format);
  debugFormat_message_va(buffer, sizeof(buffer), g_cachedMAC, false, bootTime, format, args);
  va_end(args);
  
  // Send to Serial (if DEBUG_ENABLED) - non-blocking write in chunks
  if (DEBUG_ENABLED) {
    size_t len = strlen(buffer);
    if (len > 0) {
      size_t written = 0;
      while (written < len) {
        size_t chunkSize = min(len - written, (size_t)64);
        Serial.write((const uint8_t*)(buffer + written), chunkSize);
        written += chunkSize;
        yield();
      }
      
      if (buffer[len-1] != '\n') {
        Serial.println();
        yield();
      }
    }
  }
  
  // Send to debug monitor via ESP-NOW (if transport available)
  // Same message goes to both Serial and debug monitor for consistency
  if (g_debugTransport) {
    debug_message debugMsg;
    debugMsg.msgType = MSG_DEBUG;
    
    int len = strlen(buffer);
    if (len > 0 && buffer[len-1] == '\n') {
      buffer[len-1] = '\0';
      len--;
    }
    
    size_t maxMsgLen = sizeof(debugMsg.message) - 1;
    if (len > (int)maxMsgLen) {
      len = maxMsgLen;
    }
    
    memcpy(debugMsg.message, buffer, len);
    debugMsg.message[len] = '\0';
    
    uint8_t broadcastMAC[] = BROADCAST_MAC;
    espNowTransport_broadcast(g_debugTransport, (uint8_t*)&debugMsg, sizeof(debugMsg));
  }
}

// Legacy wrapper functions for compatibility (now use unified debugPrint)
// These ensure backward compatibility while using the unified debug function
void serialPrint(const char* format, ...) {
  // Always print to Serial (boot/config messages).
  // For debug monitor output, we only forward when debugEnabled is true via debugPrint().
  if (!DEBUG_ENABLED) {
    return;
  }

  va_list args;
  va_start(args, format);
  char buffer[250];
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  Serial.println(buffer);

  // Forward same message to debug monitor if enabled (keeps outputs consistent when debugging).
  if (debugEnabled) {
    debugPrint("%s", buffer);
  }
}

void sendDebugMessage(const char* formattedMessage) {
  debugPrint("%s", formattedMessage);
}

// Forward declarations
void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);
void onPaired(const uint8_t* receiverMAC);
void onActivity();

void onPaired(const uint8_t* receiverMAC) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           receiverMAC[0], receiverMAC[1], receiverMAC[2],
           receiverMAC[3], receiverMAC[4], receiverMAC[5]);
  debugPrint("Successfully paired with receiver: %s", macStr);
  
  // Save paired receiver MAC to NVS (persists across deep sleep)
  Preferences preferences;
  preferences.begin("pedal", false);  // Read-write mode
  preferences.putBytes("pairedMAC", receiverMAC, 6);
  preferences.end();
  
  if (debugEnabled) {
    debugPrint("Saved paired receiver MAC to NVS");
  }
}

void onActivity() {
  lastActivityTime = millis();
}

void sendDeleteRecordMessage(const uint8_t* receiverMAC) {
  struct_message deleteMsg = {MSG_DELETE_RECORD, 0, false, 0};
  espNowTransport_send(&transport, receiverMAC, (uint8_t*)&deleteMsg, sizeof(deleteMsg));
  if (debugEnabled) {
    Serial.print("[");
    Serial.print(millis() - bootTime);
    Serial.print(" ms] Sent delete record message to receiver: ");
    for (int i = 0; i < 6; i++) {
      Serial.print(receiverMAC[i], HEX);
      if (i < 5) Serial.print(":");
    }
    Serial.println();
  }
}

void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel) {
  if (len < 1) return;
  
  // Reset activity timer on any message received (prevents sleep during communication)
  onActivity();
  
  unsigned long timeSinceBoot = millis() - bootTime;
  if (debugEnabled) {
    Serial.print("[");
    Serial.print(timeSinceBoot);
    Serial.print(" ms] Received ESP-NOW message: len=");
    Serial.print(len);
    Serial.print(", sender=");
    for (int i = 0; i < 6; i++) {
      Serial.print(senderMAC[i], HEX);
      if (i < 5) Serial.print(":");
    }
    Serial.println();
  }
  
  uint8_t msgType = data[0];
  
  // Handle beacon message
  if (msgType == MSG_BEACON && len >= sizeof(beacon_message)) {
    beacon_message* beacon = (beacon_message*)data;
    pairingService_handleBeacon(&pairingService, senderMAC, beacon);
    
    if (debugEnabled) {
      Serial.print("[");
      Serial.print(timeSinceBoot);
      Serial.print(" ms] Received MSG_BEACON: slots=");
      Serial.print(beacon->availableSlots);
      Serial.print("/");
      Serial.print(beacon->totalSlots);
      Serial.println();
    }
    return;
  }
  
  // Handle pairing confirmed message (can be received before pairing state is set, e.g., after deep sleep)
  if (msgType == MSG_PAIRING_CONFIRMED && len >= sizeof(pairing_confirmed_message)) {
    pairing_confirmed_message* confirm = (pairing_confirmed_message*)data;
    
    // If we're not paired yet but receiver says we are, restore pairing state
    if (!pairingState_isPaired(&pairingState)) {
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               senderMAC[0], senderMAC[1], senderMAC[2],
               senderMAC[3], senderMAC[4], senderMAC[5]);
      if (debugEnabled) {
        debugPrint("Received MSG_PAIRING_CONFIRMED - restoring pairing state with receiver %s", macStr);
      }
      memcpy(pairingState.pairedReceiverMAC, senderMAC, 6);
      pairingState.isPaired = true;
      
      // Ensure peer is added
      espNowTransport_addPeer(&transport, senderMAC, channel);
      delay(10);
      
      // Save to NVS
      Preferences preferences;
      preferences.begin("pedal", false);
      preferences.putBytes("pairedMAC", senderMAC, 6);
      preferences.end();
      
      if (debugEnabled) {
        debugPrint("Pairing restored - ready to send pedal events");
      }
    } else {
      // Already paired - just ensure peer is added
      espNowTransport_addPeer(&transport, senderMAC, channel);
      delay(10);
      if (debugEnabled) {
        debugPrint("Received MSG_PAIRING_CONFIRMED from paired receiver - peer confirmed");
      }
    }
    return;
  }
  
  // Handle other messages
  if (len < sizeof(struct_message)) {
    if (debugEnabled) {
      Serial.print("[");
      Serial.print(timeSinceBoot);
      Serial.println(" ms] Message too short");
    }
    return;
  }
  
  struct_message* msg = (struct_message*)data;
  
  if (debugEnabled) {
    Serial.print("[");
    Serial.print(timeSinceBoot);
    Serial.print(" ms] Message type=");
    Serial.print(msg->msgType);
    Serial.print(", isPaired=");
    Serial.println(pairingState_isPaired(&pairingState));
  }
  
  if (pairingState_isPaired(&pairingState)) {
    // Already paired - check if message is from our paired receiver
    if (memcmp(senderMAC, pairingState.pairedReceiverMAC, 6) == 0) {
      // Message from our paired receiver - handle MSG_ALIVE to send discovery request
      if (msg->msgType == MSG_ALIVE) {
        if (debugEnabled) {
          Serial.print("[");
          Serial.print(timeSinceBoot);
          Serial.println(" ms] Received MSG_ALIVE from paired receiver - calling handleAlive");
        }
        pairingService_handleAlive(&pairingService, senderMAC, channel);
      } else {
        if (debugEnabled) {
          Serial.print("[");
          Serial.print(timeSinceBoot);
          Serial.print(" ms] Received message from paired receiver (type=");
          Serial.print(msg->msgType);
          Serial.println(")");
        }
      }
    } else {
      // Message from different receiver - send DELETE_RECORD
      if (msg->msgType == MSG_ALIVE || msg->msgType == MSG_DISCOVERY_RESP) {
        if (debugEnabled) {
          Serial.print("[");
          Serial.print(timeSinceBoot);
          Serial.println(" ms] Received message from different receiver - sending DELETE_RECORD");
        }
        
        espNowTransport_addPeer(&transport, senderMAC, channel);
        sendDeleteRecordMessage(senderMAC);
      }
    }
  } else {
    // Not paired - handle pairing messages
    if (msg->msgType == MSG_DISCOVERY_RESP) {
      if (debugEnabled) {
        Serial.print("[");
        Serial.print(timeSinceBoot);
        Serial.println(" ms] Handling MSG_DISCOVERY_RESP from receiver");
      }
      pairingService_handleDiscoveryResponse(&pairingService, senderMAC, channel);
    } else if (msg->msgType == MSG_ALIVE) {
      pairingService_handleAlive(&pairingService, senderMAC, channel);
    } else {
      if (debugEnabled) {
        Serial.print("[");
        Serial.print(timeSinceBoot);
        Serial.print(" ms] Unhandled message type=");
        Serial.print(msg->msgType);
        Serial.println(" when not paired");
      }
    }
  }
}


void goToDeepSleep() {
  if (debugEnabled) {
    Serial.println("Going to deep sleep...");
  }
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PEDAL_1_PIN, LOW);
  esp_deep_sleep_start();
}

void setup() {
  // Always initialize Serial first
  Serial.begin(115200);
  delay(100);
  
  // Set up debug toggle button on GPIO27
  pinMode(DEBUG_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(DEBUG_PIN), debugToggleISR, FALLING);
  
  // Load debug state from NVRAM
  debugPreferences.begin("debug", true); // Read-only mode
  debugEnabled = debugPreferences.getBool("enabled", false); // Default to false
  debugPreferences.end();
  
  Serial.println("ESP-NOW Pedal Transmitter");
  Serial.print("Mode: ");
  Serial.println(PEDAL_MODE == 0 ? "DUAL" : "SINGLE");
  Serial.print("Debug mode: ");
  Serial.println(debugEnabled ? "ENABLED" : "DISABLED");
  Serial.println("Press GPIO27 button to toggle debug mode");
  
  // Send startup messages to debug monitor (if transport is available)
  // Note: We send these even if debugEnabled is false, so debug monitor knows the device is online
  if (g_debugTransport) {
    uint8_t transmitterMAC[6];
    WiFi.macAddress(transmitterMAC);
    
    char buffer[250];
    debugFormat_message(buffer, sizeof(buffer), transmitterMAC, false, bootTime, "ESP-NOW Pedal Transmitter");
    sendDebugMessage(buffer);
    
    snprintf(buffer, sizeof(buffer), "Mode: %s", PEDAL_MODE == 0 ? "DUAL" : "SINGLE");
    debugFormat_message(buffer, sizeof(buffer), transmitterMAC, false, bootTime, buffer);
    sendDebugMessage(buffer);
    
    snprintf(buffer, sizeof(buffer), "Debug mode: %s", debugEnabled ? "ENABLED" : "DISABLED");
    debugFormat_message(buffer, sizeof(buffer), transmitterMAC, false, bootTime, buffer);
    sendDebugMessage(buffer);
    
    debugFormat_message(buffer, sizeof(buffer), transmitterMAC, false, bootTime, "Press GPIO27 button to toggle debug mode");
    sendDebugMessage(buffer);
  }

  // Battery optimization
  setCpuFrequencyMhz(80);
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
  
  bootTime = millis();
  lastActivityTime = millis();
  
  // Check wakeup cause to determine if we woke from deep sleep
  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  bool wokeFromDeepSleep = (wakeupCause == ESP_SLEEP_WAKEUP_EXT0 || wakeupCause == ESP_SLEEP_WAKEUP_EXT1);
  
  // Initialize domain layer FIRST (before attaching interrupts)
  pairingState_init(&pairingState);
  
  // Load persisted paired receiver MAC from NVS ONLY if waking from deep sleep
  // On full reset (UNDEFINED wakeup), forget the receiver
  if (wokeFromDeepSleep) {
    Preferences preferences;
    preferences.begin("pedal", true);  // Read-only mode
    uint8_t savedMAC[6];
    size_t macLen = preferences.getBytes("pairedMAC", savedMAC, 6);
    preferences.end();
    
    if (macLen == 6 && !macEqual(savedMAC, (uint8_t[6]){0,0,0,0,0,0})) {
      // Restore paired state from NVS (only after deep sleep wakeup)
      memcpy(pairingState.pairedReceiverMAC, savedMAC, 6);
      pairingState.isPaired = true;
      if (DEBUG_ENABLED) {
        debugPrint("Restored paired receiver from NVS (deep sleep wakeup): %02X:%02X:%02X:%02X:%02X:%02X",
                   savedMAC[0], savedMAC[1], savedMAC[2], savedMAC[3], savedMAC[4], savedMAC[5]);
      }
    }
  } else {
    // Full reset - clear any saved pairing from NVS
    Preferences preferences;
    preferences.begin("pedal", false);  // Read-write mode
    preferences.remove("pairedMAC");
    preferences.end();
    if (DEBUG_ENABLED) {
      debugPrint("Full reset - cleared saved pairing");
    }
  }
  
  pedalReader_init(&pedalReader, PEDAL_1_PIN, PEDAL_2_PIN, PEDAL_MODE);
  
  // Attach interrupts for event-driven pedal detection
  attachInterrupt(digitalPinToInterrupt(PEDAL_1_PIN), pedal1ISR, CHANGE);
  if (PEDAL_MODE == 0) {  // DUAL mode
    attachInterrupt(digitalPinToInterrupt(PEDAL_2_PIN), pedal2ISR, CHANGE);
  }
  
  // Initialize infrastructure layer
  espNowTransport_init(&transport);
  
  // Cache MAC address early (power optimization) - after WiFi is initialized
  cacheMAC();
  
  // Set debug transport early so startup messages can be sent to debug monitor
  g_debugTransport = &transport;
  
  // Add broadcast peer
  uint8_t broadcastMAC[] = BROADCAST_MAC;
  espNowTransport_addPeer(&transport, broadcastMAC, 0);
  espNowTransport_registerReceiveCallback(&transport, onMessageReceived);
  
  // Send startup messages to debug monitor (only if debug is enabled)
  if (debugEnabled) {
    debugPrint("ESP-NOW Pedal Transmitter");
    debugPrint("Mode: %s", PEDAL_MODE == 0 ? "DUAL" : "SINGLE");
    debugPrint("Debug mode: ENABLED");
    debugPrint("Press GPIO27 button to toggle debug mode");
  }
  
  // Initialize application layer
  pairingService_init(&pairingService, &pairingState, &transport, PEDAL_MODE, bootTime);
  pairingService.onPaired = onPaired;
  
  pedalService_init(&pedalService, &pedalReader, &pairingState, &transport, &lastActivityTime);
  pedalService.onActivity = onActivity;
  pedalService_setPairingService(&pairingService);
  
  // CRITICAL: If we restored pairing from NVS, add the peer now so pedal events can be sent
  if (pairingState_isPaired(&pairingState)) {
    bool peerAdded = espNowTransport_addPeer(&transport, pairingState.pairedReceiverMAC, 0);
    if (debugEnabled) {
      if (peerAdded) {
        debugPrint("Added restored receiver peer to ESP-NOW");
      } else {
        debugPrint("Failed to add restored receiver peer to ESP-NOW");
      }
    }
    delay(20);  // Delay to ensure peer is ready
  }
  
  // Broadcast that we're online
  pairingService_broadcastOnline(&pairingService);
  
  if (debugEnabled) {
    Serial.println("ESP-NOW initialized");
  }
}

void loop() {
  unsigned long currentTime = millis();
  
  // Check if debug toggle button was pressed
  if (debugToggleFlag) {
    debugToggleFlag = false;
    debugEnabled = !debugEnabled;
    
    // Save debug state to NVRAM
    debugPreferences.begin("debug", false); // Read-write mode
    debugPreferences.putBool("enabled", debugEnabled);
    debugPreferences.end();
    
    if (debugEnabled) {
      Serial.println("=== DEBUG MODE ENABLED ===");
      // Send debug message to debug monitor
      debugPrint("Debug mode ENABLED");
    } else {
      Serial.println("=== DEBUG MODE DISABLED ===");
      // Note: Can't use debugPrint here since debug is now disabled
      // Send message directly to debug monitor if transport is available
      if (g_debugTransport) {
        debug_message debugMsg;
        debugMsg.msgType = MSG_DEBUG;
        
        // Get transmitter MAC address
        uint8_t transmitterMAC[6];
        WiFi.macAddress(transmitterMAC);
        
        // Format message manually (since debugPrint won't work when disabled)
        char buffer[250];
        debugFormat_message(buffer, sizeof(buffer), transmitterMAC, false, bootTime, "Debug mode DISABLED");
        
        // Remove trailing newline if present
        int len = strlen(buffer);
        if (len > 0 && buffer[len-1] == '\n') {
          buffer[len-1] = '\0';
          len--;
        }
        strncpy(debugMsg.message, buffer, sizeof(debugMsg.message) - 1);
        debugMsg.message[sizeof(debugMsg.message) - 1] = '\0';
        
        // Broadcast debug message so debug monitor can receive it
        uint8_t broadcastMAC[] = BROADCAST_MAC;
        espNowTransport_broadcast(g_debugTransport, (uint8_t*)&debugMsg, sizeof(debugMsg));
      }
    }
  }
  
  // Process any pending discovery requests (deferred from ESP-NOW callback)
  // This must be done in main loop, not in callback context
  pairingService_processPendingDiscovery(&pairingService);
  
  // Check discovery timeout
  if (pairingService_checkDiscoveryTimeout(&pairingService, currentTime)) {
    if (debugEnabled) {
      Serial.println("Discovery response timeout");
    }
  }
  
  // CRITICAL: Check if pedal is currently pressed and reset activity timer
  // This prevents the timer from counting up while pedal is held down
  bool pedalPressed = (digitalRead(PEDAL_1_PIN) == LOW);
  if (PEDAL_MODE == 0) {  // DUAL mode
    bool rightPedalPressed = (digitalRead(PEDAL_2_PIN) == LOW);
    pedalPressed = pedalPressed || rightPedalPressed;
  }
  
  if (pedalPressed) {
    // Pedal is currently pressed - reset activity timer to prevent sleep
    onActivity();
  }
  
  // Check inactivity timeout and enter deep sleep if inactive for 5 minutes
  unsigned long timeSinceActivity = currentTime - lastActivityTime;
  if (timeSinceActivity > INACTIVITY_TIMEOUT) {
    // Don't go to sleep if pedal is currently pressed
    if (pedalPressed) {
      onActivity();
    } else {
      if (debugEnabled) {
        debugPrint("Inactivity timeout reached: %lu ms - entering deep sleep", timeSinceActivity);
      }
      goToDeepSleep();
    }
  }
  
  // Update pedal service only when interrupts occur or debouncing needs checking
  // This eliminates unnecessary polling - pedalService_update() checks internally
  bool hasWork = pedalService_update(&pedalService);
  
  // Battery optimization: Use shorter delay when debouncing (needs frequent checks)
  // or longer delay when idle (nothing to do)
  if (hasWork) {
    // Debouncing in progress - check frequently
    delay(20);
  } else if (pairingState_isPaired(&pairingState)) {
    // Paired and idle - can sleep longer
    delay(100);
  } else {
    // Unpaired and idle - even longer delay
    delay(IDLE_DELAY_UNPAIRED);
  }
}

// Include implementation files (Arduino IDE doesn't auto-compile .cpp files in subdirectories)
#include "shared/domain/PairingState.cpp"
#include "shared/domain/PedalReader.cpp"
#include "shared/debug_format.cpp"
#include "shared/infrastructure/EspNowTransport.cpp"
#include "shared/infrastructure/TransmitterUtils.cpp"
#include "shared/application/PairingService.cpp"
#include "shared/application/PedalService.cpp"
