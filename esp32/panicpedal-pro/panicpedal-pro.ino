#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <Preferences.h>

// Include auto-generated version header (if it exists)
// Run get_version.ps1 (Windows) or get_version.sh (Linux/Mac) before building
#ifdef __has_include
  #if __has_include("version.h")
    #include "version.h"
  #endif
#endif

// Clean Architecture: Include shared and domain modules
#include "shared/messages.h"
#include "domain/PairingState.h"
#include "domain/PedalReader.h"
#include "infrastructure/EspNowTransport.h"
#include "application/PairingService.h"
#include "application/PedalService.h"

// ============================================================================
// CONFIGURATION
// ============================================================================
#define PEDAL_MODE_AUTO -1     // Auto-detect based on connected switches (recommended)
#define PEDAL_MODE_DUAL 0      // Force dual pedal mode (GPIO7 & GPIO21)
#define PEDAL_MODE_SINGLE 1    // Force single pedal mode (GPIO7 only)
#define PEDAL_MODE PEDAL_MODE_AUTO  // Change to PEDAL_MODE_DUAL or PEDAL_MODE_SINGLE to override auto-detection
#define DEBUG_ENABLED 1  // Set to 0 to disable Serial output and save battery

// Firmware version - auto-generated from git commit hash (see get_version.ps1/get_version.sh)
// Falls back to manual version if version.h doesn't exist
#ifdef FIRMWARE_VERSION
  // Version from auto-generated version.h (git commit hash)
#else
  // Fallback manual version - update this if not using git hash
  #define FIRMWARE_VERSION "1.0.0"
#endif
// ============================================================================

// GPIO Pin Definitions (PanicPedal Pro - ESP32-S3-WROOM)
#define LED_PIN 2              // Inolux_IN-PI554FCH LED control
#define BATTERY_VOLTAGE_PIN 3  // Battery voltage sensing (before TLV75733PDBV)
#define BATTERY_STAT1_PIN 4    // Battery STAT1 from MCP73871
#define SWITCH_POS1_PIN 5      // 2-position switch position 1 (unused)
#define SWITCH_POS2_PIN 6      // 2-position switch position 2 (unused)
#define PEDAL_1_PIN 7          // First foot switch NO (normally open)
#define PEDAL_2_PIN 21         // Second foot switch NO (normally open)
#define PEDAL_1_NC_PIN 45      // First foot switch NC (normally closed) - for detection
#define PEDAL_2_NC_PIN 46      // Second foot switch NC (normally closed) - for detection

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

// NVS storage for pedal mode
Preferences preferences;
#define NVS_NAMESPACE "pedal"
#define NVS_KEY_PEDAL_MODE "mode"
#define NVS_KEY_FIRMWARE_VERSION "fw_ver"

// Forward declarations
void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);
void onPaired(const uint8_t* receiverMAC);
void onActivity();
uint8_t detectPedalMode();

void onPaired(const uint8_t* receiverMAC) {
  #if DEBUG_ENABLED
  Serial.print("[");
  Serial.print(millis() - bootTime);
  Serial.print(" ms] Successfully paired with receiver: ");
  for (int i = 0; i < 6; i++) {
    Serial.print(receiverMAC[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
  #endif
}

void onActivity() {
  lastActivityTime = millis();
}

void sendDeleteRecordMessage(const uint8_t* receiverMAC) {
  struct_message deleteMsg = {MSG_DELETE_RECORD, 0, false, 0};
  espNowTransport_send(&transport, receiverMAC, (uint8_t*)&deleteMsg, sizeof(deleteMsg));
  #if DEBUG_ENABLED
  Serial.print("[");
  Serial.print(millis() - bootTime);
  Serial.print(" ms] Sent delete record message to receiver: ");
  for (int i = 0; i < 6; i++) {
    Serial.print(receiverMAC[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
  #endif
}

void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel) {
  if (len < 1) return;
  
  #if DEBUG_ENABLED
  unsigned long timeSinceBoot = millis() - bootTime;
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
  #endif
  
  uint8_t msgType = data[0];
  
  // Handle beacon message
  if (msgType == MSG_BEACON && len >= sizeof(beacon_message)) {
    beacon_message* beacon = (beacon_message*)data;
    pairingService_handleBeacon(&pairingService, senderMAC, beacon);
    
    #if DEBUG_ENABLED
    Serial.print("[");
    Serial.print(timeSinceBoot);
    Serial.print(" ms] Received MSG_BEACON: slots=");
    Serial.print(beacon->availableSlots);
    Serial.print("/");
    Serial.print(beacon->totalSlots);
    Serial.println();
    #endif
    return;
  }
  
  // Handle other messages
  if (len < sizeof(struct_message)) {
    #if DEBUG_ENABLED
    Serial.print("[");
    Serial.print(timeSinceBoot);
    Serial.println(" ms] Message too short");
    #endif
    return;
  }
  
  struct_message* msg = (struct_message*)data;
  
  #if DEBUG_ENABLED
  Serial.print("[");
  Serial.print(timeSinceBoot);
  Serial.print(" ms] Message type=");
  Serial.print(msg->msgType);
  Serial.print(", isPaired=");
  Serial.println(pairingState_isPaired(&pairingState));
  #endif
  
  if (pairingState_isPaired(&pairingState)) {
    // Already paired - check if message is from our paired receiver
    if (memcmp(senderMAC, pairingState.pairedReceiverMAC, 6) == 0) {
      // Message from our paired receiver - accept it
      #if DEBUG_ENABLED
      Serial.print("[");
      Serial.print(timeSinceBoot);
      Serial.print(" ms] Received message from paired receiver (type=");
      Serial.print(msg->msgType);
      Serial.println(")");
      #endif
    } else {
      // Message from different receiver - send DELETE_RECORD
      if (msg->msgType == MSG_ALIVE || msg->msgType == MSG_DISCOVERY_RESP) {
        #if DEBUG_ENABLED
        Serial.print("[");
        Serial.print(timeSinceBoot);
        Serial.println(" ms] Received message from different receiver - sending DELETE_RECORD");
        #endif
        
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


uint8_t detectPedalMode() {
  // Configure NC pins as inputs with pull-ups to detect switch connections
  pinMode(PEDAL_1_NC_PIN, INPUT_PULLUP);
  pinMode(PEDAL_2_NC_PIN, INPUT_PULLUP);
  
  // Small delay to allow pins to stabilize
  delay(10);
  
  // Read NC pins - LOW means switch is connected (NC contact is grounded)
  bool pedal1Connected = (digitalRead(PEDAL_1_NC_PIN) == LOW);
  bool pedal2Connected = (digitalRead(PEDAL_2_NC_PIN) == LOW);
  
  #if DEBUG_ENABLED
  Serial.print("Pedal detection: Pedal 1=");
  Serial.print(pedal1Connected ? "CONNECTED" : "NOT CONNECTED");
  Serial.print(", Pedal 2=");
  Serial.println(pedal2Connected ? "CONNECTED" : "NOT CONNECTED");
  #endif
  
  // Determine mode based on detected switches
  uint8_t detectedMode;
  if (pedal1Connected && pedal2Connected) {
    detectedMode = PEDAL_MODE_DUAL;
  } else if (pedal1Connected) {
    detectedMode = PEDAL_MODE_SINGLE;
  } else {
    // Default to single mode if no switches detected (fallback)
    detectedMode = PEDAL_MODE_SINGLE;
  }
  
  // Store detected mode and firmware version in NVS so it persists across reboots
  preferences.begin(NVS_NAMESPACE, false);
  preferences.putUChar(NVS_KEY_PEDAL_MODE, detectedMode);
  preferences.putString(NVS_KEY_FIRMWARE_VERSION, FIRMWARE_VERSION);
  preferences.end();
  
  #if DEBUG_ENABLED
  Serial.print("Stored pedal mode in NVS: ");
  Serial.print(detectedMode == PEDAL_MODE_DUAL ? "DUAL" : "SINGLE");
  Serial.print(", firmware version: ");
  Serial.println(FIRMWARE_VERSION);
  #endif
  
  return detectedMode;
}

uint8_t loadPedalModeFromNVS(bool* needsRedetection) {
  preferences.begin(NVS_NAMESPACE, true);  // Read-only mode
  uint8_t storedMode = preferences.getUChar(NVS_KEY_PEDAL_MODE, 255);  // 255 = not found
  String storedVersion = preferences.getString(NVS_KEY_FIRMWARE_VERSION, "");
  preferences.end();
  
  if (storedMode == 255 || storedVersion.length() == 0) {
    // No stored value found - needs detection
    if (needsRedetection) *needsRedetection = true;
    return 255;
  }
  
  // Check if firmware version matches
  if (storedVersion != String(FIRMWARE_VERSION)) {
    #if DEBUG_ENABLED
    Serial.print("Firmware version mismatch - stored: ");
    Serial.print(storedVersion);
    Serial.print(", current: ");
    Serial.println(FIRMWARE_VERSION);
    #endif
    if (needsRedetection) *needsRedetection = true;
    return 255;  // Force re-detection
  }
  
  // Version matches, use stored mode
  if (needsRedetection) *needsRedetection = false;
  return storedMode;
}

void goToDeepSleep() {
  #if DEBUG_ENABLED
  Serial.println("Going to deep sleep...");
  #endif
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PEDAL_1_PIN, LOW);
  esp_deep_sleep_start();
}

void setup() {
  #if DEBUG_ENABLED
  Serial.begin(115200);
  delay(100);
  Serial.println("ESP-NOW Pedal Transmitter - PanicPedal Pro");
  #endif

  // Battery optimization
  setCpuFrequencyMhz(80);
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
  
  bootTime = millis();
  lastActivityTime = millis();
  
  // Determine pedal mode
  uint8_t detectedMode = PEDAL_MODE;
  
  if (PEDAL_MODE == PEDAL_MODE_AUTO) {
    // Check NVS first - if stored value exists and firmware version matches, use it
    bool needsRedetection = false;
    uint8_t storedMode = loadPedalModeFromNVS(&needsRedetection);
    
    if (!needsRedetection && storedMode != 255) {
      // Use stored mode from NVS (version matches)
      detectedMode = storedMode;
      #if DEBUG_ENABLED
      Serial.print("Loaded pedal mode from NVS: ");
      Serial.print(detectedMode == PEDAL_MODE_DUAL ? "DUAL (GPIO7 & GPIO21)" : "SINGLE (GPIO7)");
      Serial.print(", firmware version: ");
      Serial.println(FIRMWARE_VERSION);
      #endif
    } else {
      // No stored value or firmware version changed - run detection and store it
      detectedMode = detectPedalMode();
      #if DEBUG_ENABLED
      if (needsRedetection) {
        Serial.print("Firmware version changed - re-detecting pedal mode: ");
      } else {
        Serial.print("Auto-detected mode (first boot): ");
      }
      Serial.println(detectedMode == PEDAL_MODE_DUAL ? "DUAL (GPIO7 & GPIO21)" : "SINGLE (GPIO7)");
      #endif
    }
  } else {
    // Manual override mode
    #if DEBUG_ENABLED
    Serial.print("Mode (manual override): ");
    Serial.println(detectedMode == PEDAL_MODE_DUAL ? "DUAL (GPIO7 & GPIO21)" : "SINGLE (GPIO7)");
    #endif
  }
  
  // Initialize domain layer
  pairingState_init(&pairingState);
  pedalReader_init(&pedalReader, PEDAL_1_PIN, PEDAL_2_PIN, detectedMode);
  
  // Initialize infrastructure layer
  espNowTransport_init(&transport);
  
  // Add broadcast peer
  uint8_t broadcastMAC[] = BROADCAST_MAC;
  espNowTransport_addPeer(&transport, broadcastMAC, 0);
  espNowTransport_registerReceiveCallback(&transport, onMessageReceived);
  
  // Initialize application layer
  pairingService_init(&pairingService, &pairingState, &transport, detectedMode, bootTime);
  pairingService.onPaired = onPaired;
  
  pedalService_init(&pedalService, &pedalReader, &pairingState, &transport, &lastActivityTime);
  pedalService.onActivity = onActivity;
  pedalService_setPairingService(&pairingService);
  
  // Broadcast that we're online
  pairingService_broadcastOnline(&pairingService);
  
  #if DEBUG_ENABLED
  Serial.println("ESP-NOW initialized");
  #endif
}

void loop() {
  unsigned long currentTime = millis();
  
  // Check discovery timeout
  if (pairingService_checkDiscoveryTimeout(&pairingService, currentTime)) {
    #if DEBUG_ENABLED
    Serial.println("Discovery response timeout");
    #endif
  }
  
  // Check inactivity timeout
  if (currentTime - lastActivityTime > INACTIVITY_TIMEOUT) {
    goToDeepSleep();
  }
  
  // Update pedal service (handles pedal reading and events)
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
#include "domain/PedalReader.cpp"
#include "infrastructure/EspNowTransport.cpp"
#include "application/PairingService.cpp"
#include "application/PedalService.cpp"
