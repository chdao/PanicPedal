#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

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
#define PEDAL_MODE 1  // 0=DUAL, 1=SINGLE (GPIO7 only)
#define DEBUG_ENABLED 1  // Set to 0 to disable Serial output and save battery
// ============================================================================

// GPIO Pin Definitions (PanicPedal Pro - ESP32-S3-WROOM)
#define LED_PIN 2              // Inolux_IN-PI554FCH LED control
#define BATTERY_VOLTAGE_PIN 3  // Battery voltage sensing (before TLV75733PDBV)
#define BATTERY_STAT1_PIN 4    // Battery STAT1 from MCP73871
#define SWITCH_POS1_PIN 5      // 2-position switch position 1 (unused)
#define SWITCH_POS2_PIN 6      // 2-position switch position 2 (unused)
#define PEDAL_1_PIN 7          // Foot switch
#define PEDAL_2_PIN 14         // Not used (single pedal mode)

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

// Forward declarations
void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);
void onPaired(const uint8_t* receiverMAC);
void onActivity();

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
  Serial.print("Mode: ");
  Serial.println(PEDAL_MODE == 0 ? "DUAL" : "SINGLE");
  #endif

  // Battery optimization
  setCpuFrequencyMhz(80);
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
  
  bootTime = millis();
  lastActivityTime = millis();
  
  // Initialize domain layer
  pairingState_init(&pairingState);
  pedalReader_init(&pedalReader, PEDAL_1_PIN, PEDAL_2_PIN, PEDAL_MODE);
  
  // Initialize infrastructure layer
  espNowTransport_init(&transport);
  
  // Add broadcast peer
  uint8_t broadcastMAC[] = BROADCAST_MAC;
  espNowTransport_addPeer(&transport, broadcastMAC, 0);
  espNowTransport_registerReceiveCallback(&transport, onMessageReceived);
  
  // Initialize application layer
  pairingService_init(&pairingService, &pairingState, &transport, PEDAL_MODE, bootTime);
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
