#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

// ============================================================================
// CONFIGURATION
// ============================================================================
// Pedal mode: 0=DUAL (pins 13 & 14), 1=SINGLE (pin 13 only)
// Note: Receiver assigns keys based on pairing order, so SINGLE_1/SINGLE_2 distinction not needed
#define PEDAL_MODE 1

// Debug output: Set to 0 to disable Serial output and save battery
#define DEBUG_ENABLED 0
// ============================================================================

#define PEDAL_1_PIN 13
#define PEDAL_2_PIN 14
#define INACTIVITY_TIMEOUT 600000  // 10 minutes
#define DEBOUNCE_DELAY 20  // Reduced from 50ms - with 10ms loop delay, 20ms is sufficient for most switches
                           // If you experience false presses, increase to 30-50ms

#define DUAL_PEDAL 0
#define SINGLE_PEDAL 1

// Message structure (must match receiver)
typedef struct __attribute__((packed)) struct_message {
  uint8_t msgType;
  char key;          // '1' for pin 13, '2' for pin 14
  bool pressed;
  uint8_t pedalMode;
} struct_message;

#define MSG_PEDAL_EVENT    0x00
#define MSG_DISCOVERY_REQ  0x01
#define MSG_DISCOVERY_RESP 0x02
#define MSG_ALIVE          0x03
#define MSG_BEACON         0x07  // Broadcast beacon during grace period with receiver MAC and available slots
#define MSG_DELETE_RECORD  0x06  // Must match receiver
#define MSG_TRANSMITTER_ONLINE 0x09  // Transmitter broadcasting its MAC address on startup
#define MSG_TRANSMITTER_PAIRED 0x0A  // Transmitter broadcasting when it pairs with a receiver

uint8_t pairedReceiverMAC[6] = {0};
uint8_t discoveredReceiverMAC[6] = {0};  // Receiver MAC learned from beacon
uint8_t discoveredAvailableSlots = 0;  // Available slots learned from beacon
bool isPaired = false;
bool waitingForDiscoveryResponse = false;  // Track if we've sent a discovery request and are waiting for response
bool receiverBeaconReceived = false;  // Track if we've received a beacon from a receiver
unsigned long discoveryRequestTime = 0;  // When we sent the discovery request
unsigned long lastActivityTime = 0;
unsigned long bootTime = 0;
uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Beacon message structure (must match receiver)
typedef struct __attribute__((packed)) beacon_message {
  uint8_t msgType;        // 0x07 = MSG_BEACON
  uint8_t receiverMAC[6]; // Receiver's MAC address
  uint8_t availableSlots; // Number of available pedal slots (0-2)
  uint8_t totalSlots;     // Total number of slots (always 2)
} beacon_message;

// Transmitter online message structure (broadcast on startup)
typedef struct __attribute__((packed)) transmitter_online_message {
  uint8_t msgType;        // 0x09 = MSG_TRANSMITTER_ONLINE
  uint8_t transmitterMAC[6]; // Transmitter's MAC address
} transmitter_online_message;

// Transmitter paired message structure (broadcast when transmitter pairs with a receiver)
typedef struct __attribute__((packed)) transmitter_paired_message {
  uint8_t msgType;        // 0x0A = MSG_TRANSMITTER_PAIRED
  uint8_t transmitterMAC[6]; // Transmitter's MAC address
  uint8_t receiverMAC[6];   // Receiver's MAC address that transmitter is paired with
} transmitter_paired_message;

// Battery optimization: delay when paired and idle (increases delay to reduce CPU usage)
#define IDLE_DELAY_PAIRED 100  // 100ms delay when paired and idle (reduces CPU usage significantly)
#define IDLE_DELAY_UNPAIRED 200  // 200ms delay when not paired (longer delay since less activity expected)

struct PedalState {
  bool lastState;
  unsigned long debounceTime;
  bool debouncing;
};

PedalState pedal1State = {HIGH, 0, false};
PedalState pedal2State = {HIGH, 0, false};

void sendDeleteRecordMessage(const uint8_t* receiverMAC) {
  struct_message deleteMsg = {MSG_DELETE_RECORD, 0, false, 0};
  esp_now_send(receiverMAC, (uint8_t*)&deleteMsg, sizeof(deleteMsg));
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

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  uint8_t* senderMAC = (uint8_t*)info->src_addr;
  
  // Early return for invalid message length
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
  
  // Check message type first to avoid unnecessary parsing
  uint8_t msgType = data[0];
  
  // Check if this is a beacon message (MSG_BEACON) - handle separately as it has different structure
  if (msgType == MSG_BEACON && len >= sizeof(beacon_message)) {
    beacon_message* beacon = (beacon_message*)data;
    // This is a beacon - receiver is broadcasting its MAC and available slots
    // Only store the receiver MAC if it has enough free slots for this transmitter
    int slotsNeeded = (PEDAL_MODE == DUAL_PEDAL) ? 2 : 1;
    
    #if DEBUG_ENABLED
    Serial.print("[");
    Serial.print(timeSinceBoot);
    Serial.print(" ms] Received MSG_BEACON from receiver: MAC=");
    for (int i = 0; i < 6; i++) {
      Serial.print(beacon->receiverMAC[i], HEX);
      if (i < 5) Serial.print(":");
    }
    Serial.print(", available slots=");
    Serial.print(beacon->availableSlots);
    Serial.print("/");
    Serial.print(beacon->totalSlots);
    Serial.print(", needed=");
    Serial.print(slotsNeeded);
    #endif
    
    if (beacon->availableSlots >= slotsNeeded) {
      // Receiver has enough slots - store its MAC
      receiverBeaconReceived = true;
      memcpy(discoveredReceiverMAC, beacon->receiverMAC, 6);
      discoveredAvailableSlots = beacon->availableSlots;
      
      #if DEBUG_ENABLED
      Serial.println(" - storing receiver MAC");
      #endif
    } else {
      // Receiver doesn't have enough slots - don't store MAC
      receiverBeaconReceived = false;
      memset(discoveredReceiverMAC, 0, 6);
      discoveredAvailableSlots = 0;
      
      #if DEBUG_ENABLED
      Serial.println(" - not storing (insufficient slots)");
      #endif
    }
    
    // Note: We don't pair yet - wait for pedal press to send discovery request
    return;
  }
  
  // Parse message to check type (for non-beacon messages)
  // Already checked msgType above, now validate length
  if (len < sizeof(struct_message)) {
    #if DEBUG_ENABLED
    Serial.print("[");
    Serial.print(timeSinceBoot);
    Serial.print(" ms] Message too short: len=");
    Serial.print(len);
    Serial.print(" < sizeof(struct_message)=");
    Serial.println(sizeof(struct_message));
    #endif
    return;
  }
  
  // Optimize: use direct access instead of memcpy for small struct
  struct_message* msg = (struct_message*)data;
  
  #if DEBUG_ENABLED
  Serial.print("[");
  Serial.print(timeSinceBoot);
  Serial.print(" ms] Message: msgType=");
  Serial.print(msg->msgType);
  Serial.print(" (MSG_PEDAL_EVENT=0, MSG_DISCOVERY_REQ=1, MSG_DISCOVERY_RESP=2, MSG_ALIVE=3, MSG_BEACON=7)");
  Serial.print(", sender=");
  for (int i = 0; i < 6; i++) {
    Serial.print(senderMAC[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.print(", isPaired=");
  Serial.println(isPaired);
  #endif
  
  if (isPaired) {
    // Already paired - check if message is from our paired receiver
    if (memcmp(senderMAC, pairedReceiverMAC, 6) == 0) {
      // Message from our paired receiver - accept it
      #if DEBUG_ENABLED
      Serial.print("[");
      Serial.print(timeSinceBoot);
      Serial.print(" ms] Received message from paired receiver (type=");
      Serial.print(msg->msgType);
      Serial.println(")");
      #endif
    } else {
      // Message from different receiver - we're already paired with another receiver
      // Send DELETE_RECORD to tell this receiver to remove us from its list
      if (msg->msgType == MSG_ALIVE || msg->msgType == MSG_DISCOVERY_RESP) {
        #if DEBUG_ENABLED
        Serial.print("[");
        Serial.print(timeSinceBoot);
        Serial.print(" ms] Received message (type=");
        Serial.print(msg->msgType);
        Serial.print(") from different receiver (");
        for (int i = 0; i < 6; i++) {
          Serial.print(senderMAC[i], HEX);
          if (i < 5) Serial.print(":");
        }
        Serial.print(") - already paired with ");
        for (int i = 0; i < 6; i++) {
          Serial.print(pairedReceiverMAC[i], HEX);
          if (i < 5) Serial.print(":");
        }
        Serial.println(" - sending DELETE_RECORD");
        #endif
        
        // Add this receiver as peer so we can send the delete message
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, senderMAC, 6);
        peerInfo.channel = info->rx_ctrl->channel;
        peerInfo.encrypt = false;
        esp_err_t addResult = esp_now_add_peer(&peerInfo);
        
        if (addResult == ESP_OK || addResult == ESP_ERR_ESPNOW_EXIST) {
          sendDeleteRecordMessage(senderMAC);
          #if DEBUG_ENABLED
          Serial.println("DELETE_RECORD sent successfully");
          #endif
        } else {
          #if DEBUG_ENABLED
          Serial.print("Failed to add receiver as peer for DELETE_RECORD, error: ");
          Serial.println(addResult);
          #endif
        }
      } else {
        #if DEBUG_ENABLED
        Serial.print("[");
        Serial.print(timeSinceBoot);
        Serial.print(" ms] Received message (type=");
        Serial.print(msg->msgType);
        Serial.println(") from different receiver - ignoring");
        #endif
      }
    }
  } else {
    // Not paired - handle different message types
    if (msg->msgType == MSG_DISCOVERY_RESP && waitingForDiscoveryResponse) {
      // Received discovery response to our discovery request - pair!
      #if DEBUG_ENABLED
      Serial.print("[");
      Serial.print(timeSinceBoot);
      Serial.print(" ms] Received discovery response from receiver - pairing: ");
      for (int i = 0; i < 6; i++) {
        Serial.print(senderMAC[i], HEX);
        if (i < 5) Serial.print(":");
      }
      Serial.println();
      #endif
      
      // Learned receiver MAC from discovery response
      memcpy(pairedReceiverMAC, senderMAC, 6);
      isPaired = true;
      waitingForDiscoveryResponse = false;  // Clear the flag
      receiverBeaconReceived = false;  // Clear beacon flag since we're now paired
      discoveryRequestTime = 0;  // Reset timer
      
      // Broadcast that we've paired with this receiver
      uint8_t transmitterMAC[6];
      WiFi.macAddress(transmitterMAC);
      transmitter_paired_message pairedMsg;
      pairedMsg.msgType = MSG_TRANSMITTER_PAIRED;
      memcpy(pairedMsg.transmitterMAC, transmitterMAC, 6);
      memcpy(pairedMsg.receiverMAC, pairedReceiverMAC, 6);
      esp_now_send(broadcastMAC, (uint8_t*)&pairedMsg, sizeof(pairedMsg));
      
      #if DEBUG_ENABLED
      Serial.print("[");
      Serial.print(timeSinceBoot);
      Serial.print(" ms] Learned receiver MAC from discovery response: ");
      for (int i = 0; i < 6; i++) {
        Serial.print(pairedReceiverMAC[i], HEX);
        if (i < 5) Serial.print(":");
      }
      Serial.println();
      Serial.print("[");
      Serial.print(timeSinceBoot);
      Serial.println(" ms] Broadcasting MSG_TRANSMITTER_PAIRED");
      #endif
      
      // Add receiver as peer
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, pairedReceiverMAC, 6);
      peerInfo.channel = info->rx_ctrl->channel;
      peerInfo.encrypt = false;
      esp_err_t addResult = esp_now_add_peer(&peerInfo);
      #if DEBUG_ENABLED
      if (addResult != ESP_OK && addResult != ESP_ERR_ESPNOW_EXIST) {
        Serial.print("Failed to add receiver as peer, error: ");
        Serial.println(addResult);
      } else {
        Serial.print("Successfully paired with receiver: ");
        for (int i = 0; i < 6; i++) {
          Serial.print(pairedReceiverMAC[i], HEX);
          if (i < 5) Serial.print(":");
        }
        Serial.println();
      }
      #endif
    } else if (msg->msgType == MSG_ALIVE) {
      // Received alive message from receiver - check if this is the discovered receiver
      bool isDiscoveredReceiver = (memcmp(senderMAC, discoveredReceiverMAC, 6) == 0) && receiverBeaconReceived;
      
      if (isDiscoveredReceiver) {
        // Check if receiver has enough free slots for this transmitter
        int slotsNeeded = (PEDAL_MODE == DUAL_PEDAL) ? 2 : 1;
        if (discoveredAvailableSlots < slotsNeeded) {
          #if DEBUG_ENABLED
          Serial.print("[");
          Serial.print(timeSinceBoot);
          Serial.print(" ms] Received MSG_ALIVE from discovered receiver but no free slots (needed=");
          Serial.print(slotsNeeded);
          Serial.print(", available=");
          Serial.print(discoveredAvailableSlots);
          Serial.println(") - cannot pair");
          #endif
          return;  // Don't pair if receiver doesn't have free slots
        }
        
        // This is the receiver we discovered from the beacon - pair immediately!
        #if DEBUG_ENABLED
        Serial.print("[");
        Serial.print(timeSinceBoot);
        Serial.print(" ms] Received MSG_ALIVE from discovered receiver - pairing immediately: ");
        for (int i = 0; i < 6; i++) {
          Serial.print(senderMAC[i], HEX);
          if (i < 5) Serial.print(":");
        }
        Serial.print(" (available slots=");
        Serial.print(discoveredAvailableSlots);
        Serial.print(", needed=");
        Serial.print(slotsNeeded);
        Serial.println(")");
        #endif
        
        // Pair with this receiver
        memcpy(pairedReceiverMAC, senderMAC, 6);
        isPaired = true;
        waitingForDiscoveryResponse = false;  // Clear the flag
        receiverBeaconReceived = false;  // Clear beacon flag since we're now paired
        discoveryRequestTime = 0;  // Reset timer
        
        // Add receiver as peer
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, pairedReceiverMAC, 6);
        peerInfo.channel = info->rx_ctrl->channel;
        peerInfo.encrypt = false;
        esp_err_t addResult = esp_now_add_peer(&peerInfo);
        #if DEBUG_ENABLED
        if (addResult != ESP_OK && addResult != ESP_ERR_ESPNOW_EXIST) {
          Serial.print("Failed to add receiver as peer, error: ");
          Serial.println(addResult);
        } else {
          Serial.print("Successfully paired with receiver: ");
          for (int i = 0; i < 6; i++) {
            Serial.print(pairedReceiverMAC[i], HEX);
            if (i < 5) Serial.print(":");
          }
          Serial.println();
        }
        #endif
      } else {
        // Unknown receiver - send discovery request to initiate pairing
        // But only if we've received a beacon first (so we know available slots)
        if (receiverBeaconReceived) {
          // Check if receiver has enough free slots for this transmitter
          int slotsNeeded = (PEDAL_MODE == DUAL_PEDAL) ? 2 : 1;
          if (discoveredAvailableSlots < slotsNeeded) {
            #if DEBUG_ENABLED
            Serial.print("[");
            Serial.print(timeSinceBoot);
            Serial.print(" ms] Received MSG_ALIVE from unknown receiver but no free slots (needed=");
            Serial.print(slotsNeeded);
            Serial.print(", available=");
            Serial.print(discoveredAvailableSlots);
            Serial.println(") - cannot send discovery request");
            #endif
            return;  // Don't send discovery request if receiver doesn't have free slots
          }
        }
        
        #if DEBUG_ENABLED
        Serial.print("[");
        Serial.print(timeSinceBoot);
        Serial.print(" ms] Received MSG_ALIVE from unknown receiver - sending discovery request: ");
        for (int i = 0; i < 6; i++) {
          Serial.print(senderMAC[i], HEX);
          if (i < 5) Serial.print(":");
        }
        if (receiverBeaconReceived) {
          Serial.print(" (available slots=");
          Serial.print(discoveredAvailableSlots);
          Serial.print(", needed=");
          Serial.print((PEDAL_MODE == DUAL_PEDAL) ? 2 : 1);
          Serial.print(")");
        }
        Serial.println();
        #endif
        
        // Don't store receiver MAC without slot information from a beacon
        // We need a beacon message first to know if receiver has free slots
        // Clear any previously stored MAC
        receiverBeaconReceived = false;
        memset(discoveredReceiverMAC, 0, 6);
        
        // Add receiver as peer so we can send to it
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, senderMAC, 6);
        peerInfo.channel = info->rx_ctrl->channel;
        peerInfo.encrypt = false;
        esp_now_add_peer(&peerInfo);
        
        // Send discovery request to this specific receiver
        struct_message discovery = {MSG_DISCOVERY_REQ, 0, false, PEDAL_MODE};
        esp_now_send(senderMAC, (uint8_t*)&discovery, sizeof(discovery));
        waitingForDiscoveryResponse = true;  // Mark that we're waiting for a response
        discoveryRequestTime = millis();  // Record when we sent the request
      }
    } else if (msg->msgType != MSG_DISCOVERY_RESP) {
      // Received some other targeted message but we're not waiting for discovery response
      // Ignore it - we only pair via discovery response
      #if DEBUG_ENABLED
      Serial.print("[");
      Serial.print(timeSinceBoot);
      Serial.print(" ms] Received unexpected targeted message (type=");
      Serial.print(msg->msgType);
      Serial.println(") - ignoring");
      #endif
    }
  }
}


void handlePedal(uint8_t pin, PedalState& pedal, char key) {
  // Optimize: only read pin state once per call
  bool currentState = digitalRead(pin);
  
  // Early return if state hasn't changed and not debouncing
  if (currentState == pedal.lastState && !pedal.debouncing) {
    return;
  }
  
  unsigned long currentTime = millis();
  
  #if DEBUG_ENABLED
  // Debug: print state changes
  if (currentState != pedal.lastState && !pedal.debouncing) {
    Serial.print("Pin ");
    Serial.print(pin);
    Serial.print(" state changed: ");
    Serial.print(pedal.lastState ? "HIGH" : "LOW");
    Serial.print(" -> ");
    Serial.println(currentState ? "HIGH" : "LOW");
  }
  #endif
  
  if (currentState == LOW && pedal.lastState == HIGH) {
    if (!pedal.debouncing) {
      pedal.debounceTime = currentTime;
      pedal.debouncing = true;
    } else if (currentTime - pedal.debounceTime >= DEBOUNCE_DELAY) {
      if (digitalRead(pin) == LOW) {
        // If not paired and we've received a beacon, initiate pairing when pedal is pressed
        if (!isPaired && receiverBeaconReceived) {
          // Check if receiver has enough free slots for this transmitter
          int slotsNeeded = (PEDAL_MODE == DUAL_PEDAL) ? 2 : 1;
          if (discoveredAvailableSlots < slotsNeeded) {
            #if DEBUG_ENABLED
            Serial.print("[");
            Serial.print(currentTime - bootTime);
            Serial.print(" ms] Pedal pressed but receiver has no free slots (needed=");
            Serial.print(slotsNeeded);
            Serial.print(", available=");
            Serial.print(discoveredAvailableSlots);
            Serial.println(") - cannot pair");
            #endif
            // Cannot pair - just reset state and return
            pedal.lastState = LOW;
            resetInactivityTimer();
            pedal.debouncing = false;
            return;
          }
          
          #if DEBUG_ENABLED
          Serial.print("[");
          Serial.print(currentTime - bootTime);
          Serial.print(" ms] Pedal pressed but not paired - initiating pairing with receiver: ");
          for (int i = 0; i < 6; i++) {
            Serial.print(discoveredReceiverMAC[i], HEX);
            if (i < 5) Serial.print(":");
          }
          Serial.print(" (available slots=");
          Serial.print(discoveredAvailableSlots);
          Serial.print(", needed=");
          Serial.print(slotsNeeded);
          Serial.println(")");
          #endif
          
          // Add receiver as peer if not already added
          esp_now_peer_info_t peerInfo = {};
          memcpy(peerInfo.peer_addr, discoveredReceiverMAC, 6);
          peerInfo.channel = 0;  // Will be updated when we receive response
          peerInfo.encrypt = false;
          esp_now_add_peer(&peerInfo);
          
          // Send discovery request directly to the receiver we learned from the beacon
          // This initiates the pairing process - receiver will respond with discovery response
          struct_message discovery = {MSG_DISCOVERY_REQ, 0, false, PEDAL_MODE};
          esp_err_t sendResult = esp_now_send(discoveredReceiverMAC, (uint8_t*)&discovery, sizeof(discovery));
          waitingForDiscoveryResponse = true;  // Mark that we're waiting for a response
          discoveryRequestTime = currentTime;  // Record when we sent the request
          
          #if DEBUG_ENABLED
          if (sendResult == ESP_OK) {
            Serial.println("Discovery request sent - waiting for receiver response to complete pairing");
          } else {
            Serial.print("Failed to send discovery request, error: ");
            Serial.println(sendResult);
          }
          #endif
        } else if (!isPaired) {
          #if DEBUG_ENABLED
          Serial.print("[");
          Serial.print(currentTime - bootTime);
          Serial.println(" ms] Pedal pressed but not paired - waiting for beacon first");
          #endif
          // Not paired and no beacon received - just reset state
          pedal.lastState = LOW;
          resetInactivityTimer();
          pedal.debouncing = false;
          return;
        }
        
        // Paired or pairing initiated - send pedal event
        sendPedalEvent(key, true);
        pedal.lastState = LOW;
        resetInactivityTimer();
      }
      pedal.debouncing = false;
    }
  } else if (currentState == HIGH && pedal.lastState == LOW) {
    sendPedalEvent(key, false);
    pedal.lastState = HIGH;
    pedal.debouncing = false;
    resetInactivityTimer();
  } else if (currentState == HIGH && pedal.debouncing) {
    pedal.debouncing = false;
  }
}

void sendPedalEvent(char key, bool pressed) {
  if (!isPaired) {
    #if DEBUG_ENABLED
    Serial.println("Not paired - cannot send pedal event");
    #endif
    return;
  }
  
  struct_message msg = {MSG_PEDAL_EVENT, key, pressed, 0};
  esp_err_t result = esp_now_send(pairedReceiverMAC, (uint8_t*)&msg, sizeof(msg));
  #if DEBUG_ENABLED
  if (result == ESP_OK) {
    Serial.print("Sent pedal event: key=");
    Serial.print(key);
    Serial.print(", pressed=");
    Serial.println(pressed);
  } else {
    Serial.print("Failed to send pedal event, error: ");
    Serial.println(result);
  }
  #endif
}

void resetInactivityTimer() {
  lastActivityTime = millis();
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
  delay(100);  // Give Serial time to initialize
  Serial.println("ESP-NOW Pedal Transmitter");
  Serial.print("Mode: ");
  Serial.println(PEDAL_MODE == DUAL_PEDAL ? "DUAL (pins 13 & 14)" : "SINGLE (pin 13)");
  #endif

  // Battery optimization: Set CPU frequency to 80MHz (default is 240MHz)
  // This significantly reduces power consumption with minimal performance impact
  setCpuFrequencyMhz(80);
  
  pinMode(PEDAL_1_PIN, INPUT_PULLUP);
  if (PEDAL_MODE == DUAL_PEDAL) {
    pinMode(PEDAL_2_PIN, INPUT_PULLUP);
  }
 
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
  // Battery optimization: Enable WiFi power save mode
  // WIFI_PS_MAX_MODEM: Maximum power save mode - more aggressive power saving
  // ESP-NOW still works, but with slightly higher latency (acceptable for pedals)
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
  
  if (esp_now_init() != ESP_OK) {
    #if DEBUG_ENABLED
    Serial.println("Error initializing ESP-NOW");
    #endif
    return;
  }
  
  // Add broadcast peer to receive availability beacons
  esp_now_peer_info_t broadcastPeer = {};
  memcpy(broadcastPeer.peer_addr, broadcastMAC, 6);
  broadcastPeer.channel = 0;
  broadcastPeer.encrypt = false;
  esp_now_add_peer(&broadcastPeer);
  
  // Register receive callback to handle messages from receiver
  esp_now_register_recv_cb(OnDataRecv);
  
  // Send broadcast message with transmitter MAC address on startup
  uint8_t transmitterMAC[6];
  WiFi.macAddress(transmitterMAC);  // Get transmitter's MAC address
  transmitter_online_message onlineMsg;
  onlineMsg.msgType = MSG_TRANSMITTER_ONLINE;
  memcpy(onlineMsg.transmitterMAC, transmitterMAC, 6);
  esp_now_send(broadcastMAC, (uint8_t*)&onlineMsg, sizeof(onlineMsg));
  
  #if DEBUG_ENABLED
  Serial.println("ESP-NOW initialized");
  bootTime = millis();
  Serial.print("[0 ms] Boot complete - broadcasting transmitter MAC: ");
  for (int i = 0; i < 6; i++) {
    Serial.print(transmitterMAC[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
  #else
  bootTime = millis();
  #endif
  lastActivityTime = millis();
}

void loop() {
  unsigned long currentTime = millis();
  
  // If we're waiting for discovery response but haven't received one, clear the flag after a timeout
  // This prevents getting stuck waiting forever if receiver doesn't respond
  if (waitingForDiscoveryResponse && !isPaired && discoveryRequestTime > 0) {
    if (currentTime - discoveryRequestTime > 5000) {
      waitingForDiscoveryResponse = false;
      discoveryRequestTime = 0;
      #if DEBUG_ENABLED
      Serial.print("[");
      Serial.print(currentTime - bootTime);
      Serial.println(" ms] Discovery response timeout - cleared waiting flag");
      #endif
    }
  }
  
  // Check inactivity - go to sleep after 10 minutes of no pedal activity (regardless of pairing status)
  if (currentTime - lastActivityTime > INACTIVITY_TIMEOUT) {
    goToDeepSleep();
  }

  handlePedal(PEDAL_1_PIN, pedal1State, '1');
  
  if (PEDAL_MODE == DUAL_PEDAL) {
    handlePedal(PEDAL_2_PIN, pedal2State, '2');
  }

  // Battery optimization: Variable delay based on pairing status
  // ESP-NOW can still receive messages during this delay
  // Longer delay when not paired reduces power consumption when idle
  if (isPaired) {
    delay(IDLE_DELAY_PAIRED);  // 100ms when paired (lower latency)
  } else {
    delay(IDLE_DELAY_UNPAIRED);  // 200ms when not paired (better battery life)
  }
}
