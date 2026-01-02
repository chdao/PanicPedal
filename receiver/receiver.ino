#include <WiFi.h>
#include <esp_now.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <Preferences.h>
#include <stdarg.h>
#include <Adafruit_NeoPixel.h>

USBHIDKeyboard Keyboard;
Preferences preferences;

// Message structure (must match transmitter)
typedef struct __attribute__((packed)) struct_message {
  uint8_t msgType;   // 0x00=pedal event, 0x01=discovery req, 0x02=discovery resp
  char key;          // '1' for pin 13, '2' for pin 14
  bool pressed;      // true = press, false = release
  uint8_t pedalMode; // 0=DUAL, 1=SINGLE (discovery only)
} struct_message;

#define MSG_PEDAL_EVENT    0x00
#define MSG_DISCOVERY_REQ  0x01
#define MSG_DISCOVERY_RESP 0x02
#define MSG_ALIVE          0x03
#define MSG_BEACON         0x07  // Broadcast beacon during grace period with receiver MAC and available slots
#define MSG_DELETE_RECORD  0x06  // Transmitter telling receiver to delete its record
#define MSG_DEBUG          0x04  // Must match debug monitor
#define MSG_DEBUG_MONITOR_REQ 0x05  // Must match debug monitor
#define MSG_TRANSMITTER_ONLINE 0x09  // Transmitter broadcasting its MAC address on startup
#define MSG_TRANSMITTER_PAIRED 0x0A  // Transmitter broadcasting when it pairs with a receiver

#define MAX_PEDAL_SLOTS 2
#define BEACON_INTERVAL 2000
#define TRANSMITTER_TIMEOUT 30000  // 30 seconds

// RGB LED (WS2812 NeoPixel on ESP32-S3-DevKitC-1)
#define LED_PIN 48
#define NUM_LEDS 1
Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

uint8_t pairedTransmitters[2][6] = {{0}, {0}};
uint8_t transmitterPedalModes[2] = {0, 0};
bool transmitterSeenOnBoot[2] = {false, false};  // Track if EEPROM-loaded transmitters sent discovery
unsigned long transmitterLastSeen[2] = {0, 0};  // Track when each transmitter last sent a message
int pairedCount = 0;
int pedalSlotsUsed = 0;
unsigned long lastBeaconTime = 0;
unsigned long bootTime = 0;
bool gracePeriodCheckDone = false;  // Flag to ensure grace period check runs only once
uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool keysPressed[256];

// Transmitter replacement mechanism
uint8_t pendingNewTransmitterMAC[6] = {0};
bool waitingForAliveResponses = false;
unsigned long aliveResponseTimeout = 0;
bool transmitterResponded[2] = {false, false};
#define ALIVE_RESPONSE_TIMEOUT 2000  // 2 seconds to wait for responses

// Debug monitor
uint8_t debugMonitorMAC[6] = {0};
bool debugMonitorPaired = false;
bool espNowInitialized = false;  // Track if ESP-NOW is ready to send messages

// LED control functions
void setupLED() {
  pixels.begin();
  pixels.clear();
  pixels.show();  // Turn off LED initially
}

void setLEDColor(uint8_t red, uint8_t green, uint8_t blue) {
  pixels.setPixelColor(0, pixels.Color(red, green, blue));
  pixels.show();
}

void updateLEDStatus() {
  unsigned long timeSinceBoot = millis() - bootTime;
  if (timeSinceBoot < TRANSMITTER_TIMEOUT) {
    // Grace period - set LED to blue
    setLEDColor(0, 0, 255);
  } else {
    // After grace period - turn LED off
    setLEDColor(0, 0, 0);
  }
}

// Beacon message structure (broadcast during grace period with receiver MAC and slots)
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

// Debug message structure
typedef struct __attribute__((packed)) debug_message {
  uint8_t msgType;   // 0x04 = MSG_DEBUG
  char message[200]; // Debug message text (null-terminated)
} debug_message;

void initializeKeysPressed() {
  memset(keysPressed, 0, sizeof(keysPressed));
}

int getTransmitterIndex(const uint8_t* mac) {
  for (int i = 0; i < pairedCount; i++) {
    if (memcmp(mac, pairedTransmitters[i], 6) == 0) {
      return i;
    }
  }
  return -1;
}

char getAssignedKey(int transmitterIndex) {
  return (transmitterIndex == 0) ? 'l' : 'r';
}

void savePairingState() {
  preferences.begin("pedal", false);
  preferences.putInt("pairedCount", pairedCount);
  preferences.putInt("pedalSlotsUsed", pedalSlotsUsed);
  
  for (int i = 0; i < pairedCount; i++) {
    char macKey[12];
    char modeKey[12];
    snprintf(macKey, sizeof(macKey), "mac%d", i);
    snprintf(modeKey, sizeof(modeKey), "mode%d", i);
    
    for (int j = 0; j < 6; j++) {
      char key[15];
      snprintf(key, sizeof(key), "%s_%d", macKey, j);
      preferences.putUChar(key, pairedTransmitters[i][j]);
    }
    preferences.putUChar(modeKey, transmitterPedalModes[i]);
  }
  
  // Save debug monitor MAC if paired
  if (debugMonitorPaired) {
    for (int j = 0; j < 6; j++) {
      char key[15];
      snprintf(key, sizeof(key), "dbgmon_%d", j);
      preferences.putUChar(key, debugMonitorMAC[j]);
    }
    preferences.putBool("dbgmon_paired", true);
  }
  
  preferences.end();
}

void loadPairingState() {
  preferences.begin("pedal", true);
  pairedCount = preferences.getInt("pairedCount", 0);
  pedalSlotsUsed = preferences.getInt("pedalSlotsUsed", 0);
  
  for (int i = 0; i < pairedCount && i < 2; i++) {
    char macKey[12];
    char modeKey[12];
    snprintf(macKey, sizeof(macKey), "mac%d", i);
    snprintf(modeKey, sizeof(modeKey), "mode%d", i);
    
    for (int j = 0; j < 6; j++) {
      char key[15];
      snprintf(key, sizeof(key), "%s_%d", macKey, j);
      pairedTransmitters[i][j] = preferences.getUChar(key, 0);
    }
    transmitterPedalModes[i] = preferences.getUChar(modeKey, 0);
    transmitterSeenOnBoot[i] = false;  // Mark as not seen yet - waiting for discovery message
    transmitterLastSeen[i] = 0;  // Reset last seen time on boot
  }
  
  // Load debug monitor MAC if saved
  bool dbgMonSaved = preferences.getBool("dbgmon_paired", false);
  if (dbgMonSaved) {
    bool allZero = true;
    for (int j = 0; j < 6; j++) {
      char key[15];
      snprintf(key, sizeof(key), "dbgmon_%d", j);
      debugMonitorMAC[j] = preferences.getUChar(key, 0);
      if (debugMonitorMAC[j] != 0) allZero = false;
    }
    if (!allZero) {
      debugMonitorPaired = true;  // Mark as paired so debugPrint works immediately
    }
  }
  
  preferences.end();
}

void removeTransmitter(int index) {
  if (index < 0 || index >= pairedCount) {
    debugPrint("removeTransmitter: invalid index %d", index);
    return;
  }
  
  uint8_t* mac = pairedTransmitters[index];
  int slotsFreed = (transmitterPedalModes[index] == 0) ? 2 : 1;
  
  debugPrint("Removing transmitter %d: %02X:%02X:%02X:%02X:%02X:%02X (frees %d slot(s))",
             index, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], slotsFreed);
  
  for (int i = index; i < pairedCount - 1; i++) {
    memcpy(pairedTransmitters[i], pairedTransmitters[i + 1], 6);
    transmitterPedalModes[i] = transmitterPedalModes[i + 1];
    transmitterSeenOnBoot[i] = transmitterSeenOnBoot[i + 1];
    transmitterLastSeen[i] = transmitterLastSeen[i + 1];
  }
  
  pairedCount--;
  pedalSlotsUsed -= slotsFreed;
  
  memset(pairedTransmitters[pairedCount], 0, 6);
  transmitterPedalModes[pairedCount] = 0;
  transmitterSeenOnBoot[pairedCount] = false;
  transmitterLastSeen[pairedCount] = 0;
  
  debugPrint("  -> Transmitter removed, slots now: %d/%d", pedalSlotsUsed, MAX_PEDAL_SLOTS);
  
  savePairingState();
}

bool addTransmitter(const uint8_t* mac, uint8_t pedalMode) {
  int index = getTransmitterIndex(mac);
  if (index >= 0) {
    // Already paired - mark as seen on boot if it was loaded from EEPROM
    debugPrint("  -> Transmitter already paired (index %d), updating last seen", index);
    transmitterSeenOnBoot[index] = true;
    transmitterLastSeen[index] = millis();  // Update last seen time
    return true;
  }
  
  int slotsNeeded = (pedalMode == 0) ? 2 : 1;
  if (pedalSlotsUsed + slotsNeeded > MAX_PEDAL_SLOTS) {
    debugPrint("  -> Cannot add: would exceed max slots (%d + %d > %d)",
               pedalSlotsUsed, slotsNeeded, MAX_PEDAL_SLOTS);
    return false;
  }
  
  memcpy(pairedTransmitters[pairedCount], mac, 6);
  transmitterPedalModes[pairedCount] = pedalMode;
  transmitterSeenOnBoot[pairedCount] = true;  // New transmitter - mark as seen
  transmitterLastSeen[pairedCount] = millis();  // Set initial last seen time
  pairedCount++;
  pedalSlotsUsed += slotsNeeded;
  
  debugPrint("  -> Added transmitter at index %d, assigned key: '%c'",
             pairedCount - 1, getAssignedKey(pairedCount - 1));
  
  savePairingState();
  return true;
}

void sendAvailabilityBeacon() {
  // Only send beacons during the first 30 seconds after boot (discovery period)
  unsigned long timeSinceBoot = millis() - bootTime;
  if (timeSinceBoot >= TRANSMITTER_TIMEOUT) {
    return;  // Discovery period ended, stop sending beacons
  }
  
  if (pedalSlotsUsed >= MAX_PEDAL_SLOTS) return;  // Receiver is full
  
  // Broadcast availability beacon with receiver MAC and available slots
  beacon_message beacon;
  beacon.msgType = MSG_BEACON;
  WiFi.macAddress(beacon.receiverMAC);  // Get receiver's MAC address
  beacon.availableSlots = MAX_PEDAL_SLOTS - pedalSlotsUsed;
  beacon.totalSlots = MAX_PEDAL_SLOTS;
  
  esp_now_send(broadcastMAC, (uint8_t*)&beacon, sizeof(beacon));
  
  // Log occasionally
  static unsigned long lastBeaconLog = 0;
  unsigned long currentTime = millis();
  if (currentTime - lastBeaconLog > 10000) {  // Log every 10 seconds
    debugPrint("Sending availability beacon: MAC=%02X:%02X:%02X:%02X:%02X:%02X, slots=%d/%d",
               beacon.receiverMAC[0], beacon.receiverMAC[1], beacon.receiverMAC[2],
               beacon.receiverMAC[3], beacon.receiverMAC[4], beacon.receiverMAC[5],
               beacon.availableSlots, beacon.totalSlots);
    lastBeaconLog = currentTime;
  }
}

void pingKnownTransmitters() {
  // Only send MSG_ALIVE during grace period (first 30 seconds after boot)
  unsigned long timeSinceBoot = millis() - bootTime;
  if (timeSinceBoot >= TRANSMITTER_TIMEOUT) {
    return;  // Grace period ended, stop pinging
  }
  
  if (pairedCount == 0) return;  // No known transmitters to ping
  
  // Send ping (alive message) to known transmitters that haven't been seen yet
  struct_message ping = {MSG_ALIVE, 0, false, 0};
  for (int i = 0; i < pairedCount; i++) {
    if (!transmitterSeenOnBoot[i]) {
      // Only ping transmitters we haven't seen yet during grace period
      esp_now_send(pairedTransmitters[i], (uint8_t*)&ping, sizeof(ping));
    }
  }
  
  // Log occasionally
  static unsigned long lastPingLog = 0;
  unsigned long currentTime = millis();
  if (currentTime - lastPingLog > 5000) {  // Log every 5 seconds
    int unseenCount = 0;
    for (int i = 0; i < pairedCount; i++) {
      if (!transmitterSeenOnBoot[i]) unseenCount++;
    }
    if (unseenCount > 0) {
      debugPrint("Pinging %d unseen transmitter(s) during grace period", unseenCount);
    }
    lastPingLog = currentTime;
  }
}

// Send debug message to debug monitor via ESP-NOW
void debugPrint(const char* format, ...) {
  // Don't send if ESP-NOW isn't initialized yet
  if (!espNowInitialized) return;
  
  // If debug monitor isn't paired, we can't send messages yet
  // But we'll queue them or at least prepare them for when monitor pairs
  if (!debugMonitorPaired) return;
  
  char buffer[200];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  
  // Prepend timestamp (time since boot in milliseconds)
  unsigned long timeSinceBoot = millis() - bootTime;
  char timestampedBuffer[220];
  snprintf(timestampedBuffer, sizeof(timestampedBuffer), "[%lu ms] %s", timeSinceBoot, buffer);
  
  debug_message msg = {MSG_DEBUG, {0}};
  strncpy(msg.message, timestampedBuffer, sizeof(msg.message) - 1);
  msg.message[sizeof(msg.message) - 1] = '\0';
  
  esp_err_t result = esp_now_send(debugMonitorMAC, (uint8_t*)&msg, sizeof(msg));
  // Silently ignore send errors - debug monitor might not be ready yet
}

void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len) {
  // Handle debug monitor discovery request (different structure)
  if (len >= 1 && incomingData[0] == MSG_DEBUG_MONITOR_REQ) {
    uint8_t* monitorMAC = esp_now_info->src_addr;
    
    // Check if this is a new monitor or the saved one reconnecting
    bool isSavedMonitor = (memcmp(monitorMAC, debugMonitorMAC, 6) == 0);
    
    if (!debugMonitorPaired || !isSavedMonitor) {
      memcpy(debugMonitorMAC, monitorMAC, 6);
      debugMonitorPaired = true;
      
      // Add debug monitor as peer
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, debugMonitorMAC, 6);
      peerInfo.channel = esp_now_info->rx_ctrl->channel;
      peerInfo.encrypt = false;
      esp_err_t addResult = esp_now_add_peer(&peerInfo);
      
      if (addResult == ESP_OK || addResult == ESP_ERR_ESPNOW_EXIST) {
        // Small delay to ensure peer is ready
        delay(10);
        
        // Send immediate confirmation message so monitor knows pairing succeeded
        debug_message confirmMsg = {MSG_DEBUG, {0}};
        unsigned long timeSinceBoot = millis() - bootTime;
        snprintf(confirmMsg.message, sizeof(confirmMsg.message), 
                 "[%lu ms] Debug monitor paired: %02X:%02X:%02X:%02X:%02X:%02X",
                 timeSinceBoot, debugMonitorMAC[0], debugMonitorMAC[1], debugMonitorMAC[2],
                 debugMonitorMAC[3], debugMonitorMAC[4], debugMonitorMAC[5]);
        esp_now_send(debugMonitorMAC, (uint8_t*)&confirmMsg, sizeof(confirmMsg));
        delay(10);
        
        debugPrint("Debug monitor paired: %02X:%02X:%02X:%02X:%02X:%02X",
                   debugMonitorMAC[0], debugMonitorMAC[1], debugMonitorMAC[2],
                   debugMonitorMAC[3], debugMonitorMAC[4], debugMonitorMAC[5]);
        if (isSavedMonitor) {
          debugPrint("Saved debug monitor reconnected");
        } else {
          debugPrint("New debug monitor - saving to EEPROM");
        }
        savePairingState();  // Save debug monitor MAC to EEPROM
        // Send a test message to confirm pairing works
        debugPrint("Debug monitor test message - receiver is working!");
      } else {
        debugPrint("Failed to add debug monitor as peer (error %d)", addResult);
      }
    } else {
      // Already paired with this monitor - just update channel if needed
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, debugMonitorMAC, 6);
      peerInfo.channel = esp_now_info->rx_ctrl->channel;
      peerInfo.encrypt = false;
      esp_now_add_peer(&peerInfo);  // Update peer info
    }
    return;
  }
  
  // Handle transmitter online broadcast (different structure)
  if (len >= sizeof(transmitter_online_message)) {
    transmitter_online_message* onlineMsg = (transmitter_online_message*)incomingData;
    if (onlineMsg->msgType == MSG_TRANSMITTER_ONLINE) {
      uint8_t* txMAC = esp_now_info->src_addr;
      
      // Check if this is a known transmitter
      int transmitterIndex = getTransmitterIndex(txMAC);
      if (transmitterIndex >= 0) {
        // Known transmitter - check if receiver has free slots before sending MSG_ALIVE
        if (pedalSlotsUsed >= MAX_PEDAL_SLOTS) {
          debugPrint("Received MSG_TRANSMITTER_ONLINE from known transmitter %d - receiver full, not sending MSG_ALIVE", transmitterIndex);
        } else {
          debugPrint("Received MSG_TRANSMITTER_ONLINE from known transmitter %d - sending MSG_ALIVE", transmitterIndex);
          
          // Add as peer if not already added
          esp_now_peer_info_t peerInfo = {};
          memcpy(peerInfo.peer_addr, txMAC, 6);
          peerInfo.channel = esp_now_info->rx_ctrl->channel;
          peerInfo.encrypt = false;
          esp_now_add_peer(&peerInfo);
          
          // Send MSG_ALIVE immediately
          struct_message alive = {MSG_ALIVE, 0, false, 0};
          esp_now_send(txMAC, (uint8_t*)&alive, sizeof(alive));
          
          // Update last seen time
          transmitterLastSeen[transmitterIndex] = millis();
        }
      } else {
        // Unknown transmitter - if receiver is full, try to replace unresponsive transmitters
        if (pedalSlotsUsed >= MAX_PEDAL_SLOTS) {
          debugPrint("Received MSG_TRANSMITTER_ONLINE from unknown transmitter - receiver full, checking for unresponsive transmitters");
          
          // Store the new transmitter MAC for potential pairing
          memcpy(pendingNewTransmitterMAC, txMAC, 6);
          
          // Send MSG_ALIVE to all currently paired transmitters to check if they're responsive
          struct_message ping = {MSG_ALIVE, 0, false, 0};
          for (int i = 0; i < pairedCount; i++) {
            transmitterResponded[i] = false;  // Reset response flags
            esp_now_send(pairedTransmitters[i], (uint8_t*)&ping, sizeof(ping));
          }
          
          // Set timeout for waiting for responses
          waitingForAliveResponses = true;
          aliveResponseTimeout = millis() + ALIVE_RESPONSE_TIMEOUT;
          
          debugPrint("Sent MSG_ALIVE to %d paired transmitter(s), waiting %dms for responses", pairedCount, ALIVE_RESPONSE_TIMEOUT);
        } else {
          debugPrint("Received MSG_TRANSMITTER_ONLINE from unknown transmitter - receiver has free slots, waiting for discovery request");
        }
      }
      return;
    }
  }
  
  // Handle transmitter paired broadcast (different structure)
  if (len >= sizeof(transmitter_paired_message)) {
    transmitter_paired_message* pairedMsg = (transmitter_paired_message*)incomingData;
    if (pairedMsg->msgType == MSG_TRANSMITTER_PAIRED) {
      uint8_t* txMAC = pairedMsg->transmitterMAC;
      uint8_t* rxMAC = pairedMsg->receiverMAC;
      
      // Get our own MAC address
      uint8_t ourMAC[6];
      WiFi.macAddress(ourMAC);
      
      // Check if this transmitter is in our list
      int transmitterIndex = getTransmitterIndex(txMAC);
      
      // Check if the transmitter is paired with us or another receiver
      bool pairedWithUs = (memcmp(rxMAC, ourMAC, 6) == 0);
      
      if (transmitterIndex >= 0 && !pairedWithUs) {
        // Transmitter is in our list but paired with another receiver - delete it
        debugPrint("Received MSG_TRANSMITTER_PAIRED: transmitter %d is paired with another receiver - removing", transmitterIndex);
        removeTransmitter(transmitterIndex);
      } else if (transmitterIndex >= 0 && pairedWithUs) {
        // Transmitter is paired with us - update last seen time
        transmitterLastSeen[transmitterIndex] = millis();
        if (!gracePeriodCheckDone) {
          transmitterSeenOnBoot[transmitterIndex] = true;
        }
        debugPrint("Received MSG_TRANSMITTER_PAIRED: transmitter %d is paired with us", transmitterIndex);
      }
      return;
    }
  }
  
  if (len < sizeof(struct_message)) return;
  
  struct_message msg;
  memcpy(&msg, incomingData, len);
  
  switch (msg.msgType) {
    case MSG_DELETE_RECORD: {
      // Transmitter is telling us to delete its record (it's paired with another receiver)
      int transmitterIndex = getTransmitterIndex(esp_now_info->src_addr);
      if (transmitterIndex >= 0) {
        debugPrint("Received delete record request from transmitter %d - removing", transmitterIndex);
        removeTransmitter(transmitterIndex);
      }
      break;
    }
    
    case MSG_DISCOVERY_REQ: {
      uint8_t* txMAC = esp_now_info->src_addr;
      debugPrint("Discovery request from %02X:%02X:%02X:%02X:%02X:%02X (mode=%d)",
                 txMAC[0], txMAC[1], txMAC[2], txMAC[3], txMAC[4], txMAC[5], msg.pedalMode);
      
      // Check if this is a known transmitter (from EEPROM)
      int knownIndex = getTransmitterIndex(txMAC);
      bool isKnownTransmitter = (knownIndex >= 0);
      
      if (isKnownTransmitter) {
        debugPrint("  -> Known transmitter (index %d)", knownIndex);
      } else {
        debugPrint("  -> Unknown transmitter");
      }
      
      // Check if we're still in the discovery period (first 30 seconds)
      unsigned long timeSinceBoot = millis() - bootTime;
      bool inDiscoveryPeriod = (timeSinceBoot < TRANSMITTER_TIMEOUT);
      
      // After 30 seconds, only accept known transmitters (from EEPROM)
      if (!inDiscoveryPeriod && !isKnownTransmitter) {
        debugPrint("  -> Rejected: discovery period ended, unknown transmitter");
        break;
      }
      
      if (isKnownTransmitter) {
        // Known transmitter - mark as seen immediately
        debugPrint("  -> Known transmitter - marking as seen on boot");
        addTransmitter(txMAC, msg.pedalMode);  // This will mark it as seen
      }
      
      // If receiver is full, don't accept new transmitters
      if (pedalSlotsUsed >= MAX_PEDAL_SLOTS) {
        if (isKnownTransmitter) {
          // Known transmitter during grace period - already marked as seen above
          debugPrint("  -> Known transmitter already paired, receiver full");
        } else {
          debugPrint("  -> Rejected: receiver full");
        }
        break;
      }
      
      int slotsNeeded = (msg.pedalMode == 0) ? 2 : 1;
      debugPrint("  -> Slots needed: %d, currently used: %d/%d", 
                 slotsNeeded, pedalSlotsUsed, MAX_PEDAL_SLOTS);
      
      if (pedalSlotsUsed + slotsNeeded <= MAX_PEDAL_SLOTS) {
        // Add transmitter as ESP-NOW peer before sending response
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, txMAC, 6);
        peerInfo.channel = esp_now_info->rx_ctrl->channel;
        peerInfo.encrypt = false;
        esp_err_t addPeerResult = esp_now_add_peer(&peerInfo);
        
        if (addPeerResult != ESP_OK && addPeerResult != ESP_ERR_ESPNOW_EXIST) {
          debugPrint("  -> Failed to add transmitter as peer (error %d)", addPeerResult);
          break;
        }
        
        struct_message response = {MSG_DISCOVERY_RESP, 0, false, 0};
        esp_err_t sendResult = esp_now_send(txMAC, (uint8_t*)&response, sizeof(response));
        if (sendResult == ESP_OK) {
          debugPrint("  -> Sending discovery response");
          bool added = addTransmitter(txMAC, msg.pedalMode);
          if (added) {
            debugPrint("  -> Transmitter paired successfully: mode=%d, slots now=%d/%d", 
                       msg.pedalMode, pedalSlotsUsed, MAX_PEDAL_SLOTS);
          } else {
            debugPrint("  -> Failed to add transmitter");
          }
        } else {
          debugPrint("  -> Failed to send discovery response (error %d)", sendResult);
        }
      } else {
        debugPrint("  -> Rejected: receiver full (%d/%d slots)", 
                   pedalSlotsUsed, MAX_PEDAL_SLOTS);
      }
      break;
    }
      
    case MSG_PEDAL_EVENT: {
      int transmitterIndex = getTransmitterIndex(esp_now_info->src_addr);
      if (transmitterIndex < 0) {
        // Received message from transmitter not in our list - it's paired with another receiver
        debugPrint("Received pedal event from unknown transmitter %02X:%02X:%02X:%02X:%02X:%02X - not in our list",
                   esp_now_info->src_addr[0], esp_now_info->src_addr[1],
                   esp_now_info->src_addr[2], esp_now_info->src_addr[3],
                   esp_now_info->src_addr[4], esp_now_info->src_addr[5]);
        break;
      }
      
      // Update last seen time when receiving pedal events
      transmitterLastSeen[transmitterIndex] = millis();
      
      // Mark as seen on boot if this is during the grace period
      // Pedal events count as "activity" - transmitter is clearly alive and communicating
      if (!gracePeriodCheckDone) {
        transmitterSeenOnBoot[transmitterIndex] = true;
        debugPrint("  -> Transmitter %d marked as seen (pedal event during grace period)", transmitterIndex);
      }
      
      char keyToPress;
      if (transmitterPedalModes[transmitterIndex] == 0) {
        // DUAL pedal: '1' -> 'l', '2' -> 'r'
        keyToPress = (msg.key == '1') ? 'l' : 'r';
      } else {
        // SINGLE pedal: '1' -> assigned key based on pairing order
        if (msg.key != '1') return;
        keyToPress = getAssignedKey(transmitterIndex);
      }
      
      uint8_t keyIndex = (uint8_t)keyToPress;
      
      if (msg.pressed) {
        if (!keysPressed[keyIndex]) {
          Keyboard.press(keyToPress);
          keysPressed[keyIndex] = true;
          debugPrint("Key '%c' pressed (transmitter %d)", keyToPress, transmitterIndex);
        }
      } else {
        if (keysPressed[keyIndex]) {
          Keyboard.release(keyToPress);
          keysPressed[keyIndex] = false;
          debugPrint("Key '%c' released (transmitter %d)", keyToPress, transmitterIndex);
        }
      }
      break;
    }

    case MSG_ALIVE: {
      // Transmitter is acknowledging our alive message or sending heartbeat
      int transmitterIndex = getTransmitterIndex(esp_now_info->src_addr);
      if (transmitterIndex >= 0) {
        // Update last seen time for known transmitters
        transmitterLastSeen[transmitterIndex] = millis();
        
        // If we're waiting for responses to replace unresponsive transmitters, mark this one as responded
        if (waitingForAliveResponses) {
          transmitterResponded[transmitterIndex] = true;
          debugPrint("  -> Transmitter %d responded to MSG_ALIVE (will keep)", transmitterIndex);
        }
        
        // Mark as seen on boot if this is during the grace period
        if (!gracePeriodCheckDone) {
          transmitterSeenOnBoot[transmitterIndex] = true;
          debugPrint("  -> Transmitter %d marked as seen (alive message during grace period)", transmitterIndex);
        }
      } else {
        // Received message from transmitter not in our list - it's paired with another receiver
        debugPrint("Received alive message from unknown transmitter %02X:%02X:%02X:%02X:%02X:%02X - not in our list",
                   esp_now_info->src_addr[0], esp_now_info->src_addr[1],
                   esp_now_info->src_addr[2], esp_now_info->src_addr[3],
                   esp_now_info->src_addr[4], esp_now_info->src_addr[5]);
      }
      break;
    }
  }
}

void setup() {
  initializeKeysPressed();
  bootTime = millis();
  loadPairingState();

  debugPrint("=== Receiver Starting ===");
  debugPrint("Loaded %d transmitter(s) from EEPROM", pairedCount);
  debugPrint("Pedal slots used: %d/%d", pedalSlotsUsed, MAX_PEDAL_SLOTS);
  
  if (pairedCount > 0) {
    for (int i = 0; i < pairedCount; i++) {
      uint8_t* mac = pairedTransmitters[i];
      debugPrint("  Transmitter %d: %02X:%02X:%02X:%02X:%02X:%02X (mode=%d, key='%c')",
                 i, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 transmitterPedalModes[i], getAssignedKey(i));
    }
  }

  WiFi.mode(WIFI_STA);
  delay(100);
  WiFi.disconnect();
  delay(100);

  if (esp_now_init() != ESP_OK) {
    // Can't use debugPrint here - ESP-NOW not initialized yet
    return;
  }
  espNowInitialized = true;  // Mark ESP-NOW as ready for sending
  debugPrint("ESP-NOW initialized");

  // Add saved debug monitor as peer (if saved)
  if (debugMonitorPaired) {
    esp_now_peer_info_t dbgPeerInfo = {};
    memcpy(dbgPeerInfo.peer_addr, debugMonitorMAC, 6);
    dbgPeerInfo.channel = 0;  // Will be updated when monitor sends discovery
    dbgPeerInfo.encrypt = false;
    esp_err_t dbgResult = esp_now_add_peer(&dbgPeerInfo);
    if (dbgResult == ESP_OK || dbgResult == ESP_ERR_ESPNOW_EXIST) {
      debugPrint("Added saved debug monitor as peer: %02X:%02X:%02X:%02X:%02X:%02X",
                 debugMonitorMAC[0], debugMonitorMAC[1], debugMonitorMAC[2],
                 debugMonitorMAC[3], debugMonitorMAC[4], debugMonitorMAC[5]);
    } else {
      // If adding fails, mark as not paired so it can re-pair
      debugMonitorPaired = false;
      memset(debugMonitorMAC, 0, 6);
    }
  }

  // Add saved transmitters as peers
  for (int i = 0; i < pairedCount; i++) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, pairedTransmitters[i], 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_err_t result = esp_now_add_peer(&peerInfo);
    if (result == ESP_OK) {
      debugPrint("Added transmitter %d as peer", i);
    } else {
      debugPrint("Failed to add transmitter %d as peer (error %d)", i, result);
    }
  }

  // Add broadcast peer
  esp_now_peer_info_t broadcastPeer = {};
  memcpy(broadcastPeer.peer_addr, broadcastMAC, 6);
  broadcastPeer.channel = 0;
  broadcastPeer.encrypt = false;
  esp_now_add_peer(&broadcastPeer);
  debugPrint("Broadcast peer added - ready for discovery");

  esp_now_register_recv_cb(OnDataRecv);
  debugPrint("Receive callback registered");

  USB.begin();
  delay(500);
  Keyboard.begin();
  delay(2000);
  
  debugPrint("USB HID Keyboard initialized");
  
  // Initialize RGB LED
  setupLED();
  debugPrint("RGB LED initialized");
  
  debugPrint("=== Receiver Ready ===");
  
  // If debug monitor was loaded from EEPROM but hasn't sent discovery yet,
  // send a message to let user know receiver is ready (will be sent once monitor pairs)
  if (debugMonitorPaired) {
    debugPrint("Waiting for debug monitor to reconnect...");
  } else {
    debugPrint("Debug monitor not paired - start debug monitor to see messages");
  }
}

void loop() {
  // Mark grace period as done after timeout (for tracking purposes only)
  if (!gracePeriodCheckDone) {
    unsigned long timeSinceBoot = millis() - bootTime;
    if (timeSinceBoot > TRANSMITTER_TIMEOUT) {
      gracePeriodCheckDone = true;
      debugPrint("=== Boot grace period ended ===");
    }
  }
  
  // Send availability beacon and ping known transmitters during grace period
  unsigned long timeSinceBoot = millis() - bootTime;
  if (timeSinceBoot < TRANSMITTER_TIMEOUT && (millis() - lastBeaconTime > BEACON_INTERVAL)) {
    sendAvailabilityBeacon();  // Broadcast availability to unpaired transmitters
    pingKnownTransmitters();   // Ping known transmitters during grace period only
    lastBeaconTime = millis();
  }
  
  // Check if we're waiting for alive responses and timeout has expired
  if (waitingForAliveResponses && millis() >= aliveResponseTimeout) {
    debugPrint("Alive response timeout expired - checking for unresponsive transmitters");
    
    // Remove transmitters that didn't respond (in reverse order to avoid index issues)
    for (int i = pairedCount - 1; i >= 0; i--) {
      if (!transmitterResponded[i]) {
        debugPrint("Transmitter %d did not respond - removing to make room for new transmitter", i);
        removeTransmitter(i);
      }
    }
    
    // If we now have free slots, send MSG_ALIVE to the new transmitter
    if (pedalSlotsUsed < MAX_PEDAL_SLOTS && memcmp(pendingNewTransmitterMAC, broadcastMAC, 6) != 0) {
      debugPrint("Sending MSG_ALIVE to new transmitter to initiate pairing");
      
      // Add new transmitter as peer
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, pendingNewTransmitterMAC, 6);
      peerInfo.channel = 0;  // Will be updated when we receive response
      peerInfo.encrypt = false;
      esp_now_add_peer(&peerInfo);
      
      // Send MSG_ALIVE to initiate pairing
      struct_message alive = {MSG_ALIVE, 0, false, 0};
      esp_now_send(pendingNewTransmitterMAC, (uint8_t*)&alive, sizeof(alive));
    }
    
    // Clear replacement state
    waitingForAliveResponses = false;
    memset(pendingNewTransmitterMAC, 0, 6);
    aliveResponseTimeout = 0;
  }
  
  // Update LED status based on grace period
  updateLEDStatus();
  
  delay(10);
}
