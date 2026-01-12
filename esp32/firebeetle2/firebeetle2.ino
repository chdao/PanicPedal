#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <stdarg.h>
#include <Preferences.h>

// Clean Architecture: Include shared and domain modules
#include "shared/messages.h"
#include "shared/debug_format.h"
#include "domain/PairingState.h"
#include "shared/domain/PedalReader.h"

// Forward declarations for ISR functions
void IRAM_ATTR pedal1ISR();
void IRAM_ATTR pedal2ISR();
void IRAM_ATTR debugToggleISR();
#include "infrastructure/EspNowTransport.h"
#include "application/PairingService.h"
#include "application/PedalService.h"

// ============================================================================
// CONFIGURATION
// ============================================================================
#define PEDAL_MODE 1  // 0=DUAL (pins 13 & 14), 1=SINGLE (pin 13 only)
#define DEBUG_ENABLED 1  // Set to 0 to disable Serial output and save battery
// ============================================================================

#define PEDAL_1_PIN 13
#define PEDAL_2_PIN 14
#define DEBUG_PIN 27  // GPIO 27 (A5) - Ground this pin to enable debug output
#define INACTIVITY_TIMEOUT 600000  // 10 minutes
#define IDLE_DELAY_PAIRED 20  // 20ms delay when paired
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

// Helper function to send debug message to debug monitor (works even when debugEnabled is false)
void sendDebugMessage(const char* formattedMessage) {
  if (!g_debugTransport) return;
  
  debug_message debugMsg;
  debugMsg.msgType = MSG_DEBUG;
  
  // Remove trailing newline if present
  int len = strlen(formattedMessage);
  char messageCopy[250];
  strncpy(messageCopy, formattedMessage, sizeof(messageCopy) - 1);
  messageCopy[sizeof(messageCopy) - 1] = '\0';
  
  if (len > 0 && messageCopy[len-1] == '\n') {
    messageCopy[len-1] = '\0';
    len--;
  }
  
  strncpy(debugMsg.message, messageCopy, sizeof(debugMsg.message) - 1);
  debugMsg.message[sizeof(debugMsg.message) - 1] = '\0';
  
  // Broadcast debug message so debug monitor can receive it directly
  uint8_t broadcastMAC[] = BROADCAST_MAC;
  espNowTransport_broadcast(g_debugTransport, (uint8_t*)&debugMsg, sizeof(debugMsg));
}

void debugPrint(const char* format, ...) {
  if (!debugEnabled) return;
  
  // Get transmitter MAC address
  uint8_t transmitterMAC[6];
  WiFi.macAddress(transmitterMAC);
  
  // Format message with standardized format
  char buffer[250];  // Larger buffer for formatted message
  va_list args;
  va_start(args, format);
  debugFormat_message_va(buffer, sizeof(buffer), transmitterMAC, false, bootTime, format, args);
  va_end(args);
  
  // Output to Serial
  Serial.print(buffer);
  if (buffer[strlen(buffer)-1] != '\n') {
    Serial.println();  // Ensure newline
  }
  
  // Send to debug monitor via ESP-NOW (broadcast so debug monitor can receive directly)
  if (g_debugTransport) {
    debug_message debugMsg;
    debugMsg.msgType = MSG_DEBUG;
    // Remove trailing newline if present
    int len = strlen(buffer);
    if (len > 0 && buffer[len-1] == '\n') {
      buffer[len-1] = '\0';
      len--;
    }
    strncpy(debugMsg.message, buffer, sizeof(debugMsg.message) - 1);
    debugMsg.message[sizeof(debugMsg.message) - 1] = '\0';  // Ensure null termination
    
    // Broadcast debug message so debug monitor can receive it directly
    uint8_t broadcastMAC[] = BROADCAST_MAC;
    espNowTransport_broadcast(g_debugTransport, (uint8_t*)&debugMsg, sizeof(debugMsg));
  }
}

// Forward declarations
void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);
void onPaired(const uint8_t* receiverMAC);
void onActivity();

void onPaired(const uint8_t* receiverMAC) {
  if (debugEnabled) {
    Serial.print("[");
    Serial.print(millis() - bootTime);
    Serial.print(" ms] Successfully paired with receiver: ");
    for (int i = 0; i < 6; i++) {
      Serial.print(receiverMAC[i], HEX);
      if (i < 5) Serial.print(":");
    }
    Serial.println();
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
      // Message from our paired receiver - accept it
      if (debugEnabled) {
        Serial.print("[");
        Serial.print(timeSinceBoot);
        Serial.print(" ms] Received message from paired receiver (type=");
        Serial.print(msg->msgType);
        Serial.println(")");
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
      pairingService_handleDiscoveryResponse(&pairingService, senderMAC, channel);
    } else if (msg->msgType == MSG_ALIVE) {
      pairingService_handleAlive(&pairingService, senderMAC, channel);
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
  
  // Initialize domain layer
  pairingState_init(&pairingState);
  pedalReader_init(&pedalReader, PEDAL_1_PIN, PEDAL_2_PIN, PEDAL_MODE);
  
  // Attach interrupts for event-driven pedal detection
  attachInterrupt(digitalPinToInterrupt(PEDAL_1_PIN), pedal1ISR, CHANGE);
  if (PEDAL_MODE == 0) {  // DUAL mode
    attachInterrupt(digitalPinToInterrupt(PEDAL_2_PIN), pedal2ISR, CHANGE);
  }
  
  // Initialize infrastructure layer
  espNowTransport_init(&transport);
  
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
  
  // Check discovery timeout
  if (pairingService_checkDiscoveryTimeout(&pairingService, currentTime)) {
    if (debugEnabled) {
      Serial.println("Discovery response timeout");
    }
  }
  
  // Check inactivity timeout
  if (currentTime - lastActivityTime > INACTIVITY_TIMEOUT) {
    goToDeepSleep();
  }
  
  // Update pedal service only when interrupts occur or debouncing needs checking
  // This eliminates unnecessary polling - pedalService_update() checks internally
  pedalService_update(&pedalService);
  
  // Battery optimization: Variable delay based on pairing status
  if (pairingState_isPaired(&pairingState)) {
    delay(IDLE_DELAY_PAIRED);
  } else {
    delay(IDLE_DELAY_UNPAIRED);
  }
}

// Include implementation files (Arduino IDE doesn't auto-compile .cpp files in subdirectories)
#include "domain/PairingState.cpp"
#include "shared/domain/PedalReader.cpp"
#include "shared/debug_format.cpp"
#include "infrastructure/EspNowTransport.cpp"
#include "application/PairingService.cpp"
#include "application/PedalService.cpp"
