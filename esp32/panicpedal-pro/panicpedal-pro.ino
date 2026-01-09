#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

// Clean Architecture: Include shared and domain modules
#include "shared/messages.h"
#include "domain/PairingState.h"
#include "domain/PedalReader.h"
#include "infrastructure/EspNowTransport.h"
#include "infrastructure/LEDService.h"
#include "application/PairingService.h"
#include "application/PedalService.h"

// ============================================================================
// CONFIGURATION
// ============================================================================
#define PEDAL_MODE_AUTO -1     // Auto-detect based on connected switches (recommended)
#define PEDAL_MODE_DUAL 0      // Force dual pedal mode (GPIO1 & GPIO2)
#define PEDAL_MODE_SINGLE 1    // Force single pedal mode (GPIO1 only)
#define PEDAL_MODE PEDAL_MODE_AUTO  // Change to PEDAL_MODE_DUAL or PEDAL_MODE_SINGLE to override auto-detection
#define DEBUG_ENABLED 1  // Set to 0 to disable Serial output and save battery
// ============================================================================

// GPIO Pin Definitions (PanicPedal Pro - ESP32-S3-WROOM)
#define PEDAL_LEFT_NO_PIN 1    // Left pedal switch NO (normally open)
#define PEDAL_RIGHT_NO_PIN 2   // Right pedal switch NO (normally open)
#define BATTERY_VOLTAGE_PIN 3  // Battery voltage sensing (before TLV75733PDBV)
#define BATTERY_STAT1_PIN 4    // STAT1/LBO from MCP73871 (charging status)
#define SWITCH_POS1_PIN 5      // Single pole double throw switch position 1
#define SWITCH_POS2_PIN 6      // Single pole double throw switch position 2
#define LED_DIN_PIN 7          // APA102-2020 LED DIN
#define LED_CLK_PIN 41         // APA102-2020 LED CLK
#define PEDAL_LEFT_NC_PIN 35   // Left pedal switch NC (normally closed) - for detection
#define PEDAL_RIGHT_NC_PIN 36  // Right pedal switch NC (normally closed) - for detection

#define INACTIVITY_TIMEOUT 600000  // 10 minutes
#define IDLE_DELAY_PAIRED 20  // 20ms delay when paired
#define IDLE_DELAY_UNPAIRED 200  // 200ms delay when not paired

// Domain layer instances
PairingState pairingState;
PedalReader pedalReader;
EspNowTransport transport;

// Infrastructure layer instances
LEDService ledService;

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
  
  // Turn LED off after pairing to save battery
  ledService_setState(&ledService, LED_STATE_PAIRED);
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


bool isCharging() {
  // Read STAT1/LBO pin from MCP73871
  // Pin is LOW when charging, HIGH when not charging or low battery
  // Configure as input with pull-up
  pinMode(BATTERY_STAT1_PIN, INPUT_PULLUP);
  delay(1);  // Small delay for pin to stabilize
  return (digitalRead(BATTERY_STAT1_PIN) == LOW);
}

uint8_t detectPedalMode() {
  // Configure NC pins as inputs with pull-ups to detect switch connections
  pinMode(PEDAL_LEFT_NC_PIN, INPUT_PULLUP);
  pinMode(PEDAL_RIGHT_NC_PIN, INPUT_PULLUP);
  
  // Small delay to allow pins to stabilize
  delay(10);
  
  // Read NC pins - LOW means switch is connected (NC contact is grounded)
  bool pedal1Connected = (digitalRead(PEDAL_LEFT_NC_PIN) == LOW);
  bool pedal2Connected = (digitalRead(PEDAL_RIGHT_NC_PIN) == LOW);
  
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
  
  #if DEBUG_ENABLED
  Serial.print("Detected pedal mode: ");
  Serial.println(detectedMode == PEDAL_MODE_DUAL ? "DUAL" : "SINGLE");
  #endif
  
  return detectedMode;
}

void goToDeepSleep() {
  #if DEBUG_ENABLED
  Serial.println("Going to deep sleep...");
  #endif
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PEDAL_LEFT_NO_PIN, LOW);
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
  
  // Determine pedal mode - detect on every boot
  uint8_t detectedMode = PEDAL_MODE;
  
  if (PEDAL_MODE == PEDAL_MODE_AUTO) {
    // Auto-detect pedal mode on every boot
    detectedMode = detectPedalMode();
    #if DEBUG_ENABLED
    Serial.print("Auto-detected mode: ");
    Serial.println(detectedMode == PEDAL_MODE_DUAL ? "DUAL (GPIO1 & GPIO2)" : "SINGLE (GPIO1)");
    #endif
  } else {
    // Manual override mode
    #if DEBUG_ENABLED
    Serial.print("Mode (manual override): ");
    Serial.println(detectedMode == PEDAL_MODE_DUAL ? "DUAL (GPIO1 & GPIO2)" : "SINGLE (GPIO1)");
    #endif
  }
  
  // Initialize domain layer
  pairingState_init(&pairingState);
  pedalReader_init(&pedalReader, PEDAL_LEFT_NO_PIN, PEDAL_RIGHT_NO_PIN, detectedMode);
  
  // Initialize infrastructure layer
  espNowTransport_init(&transport);
  ledService_init(&ledService, LED_DIN_PIN, LED_CLK_PIN);
  
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
  pedalService_setLEDService(&ledService);
  
  // Set initial LED state to pairing
  ledService_setState(&ledService, LED_STATE_PAIRING);
  
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
  
  // Check charging status and update LED accordingly
  if (isCharging()) {
    // Battery is charging - show green LED
    if (ledService.state != LED_STATE_CHARGING) {
      ledService_setState(&ledService, LED_STATE_CHARGING);
    }
  } else if (pairingState_isPaired(&pairingState)) {
    // Paired and not charging - LED off to save battery
    if (ledService.state != LED_STATE_PAIRED) {
      ledService_setState(&ledService, LED_STATE_PAIRED);
    }
  }
  
  // Update LED service
  ledService_update(&ledService, currentTime);
  
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
#include "infrastructure/LEDService.cpp"
#include "application/PairingService.cpp"
#include "application/PedalService.cpp"
