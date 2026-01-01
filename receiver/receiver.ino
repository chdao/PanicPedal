#include <WiFi.h>
#include <esp_now.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <Preferences.h>

USBHIDKeyboard Keyboard;
Preferences preferences;

// Message structure (must match transmitter)
typedef struct __attribute__((packed)) struct_message {
  uint8_t msgType;   // 0x00=pedal event, 0x01=discovery req, 0x02=discovery resp
  char key;          // '1' for pin 13, '2' for pin 14
  bool pressed;      // true = press, false = release
  uint8_t pedalMode; // 0=DUAL, 1=SINGLE_1, 2=SINGLE_2 (discovery only)
} struct_message;

#define MSG_PEDAL_EVENT    0x00
#define MSG_DISCOVERY_REQ  0x01
#define MSG_DISCOVERY_RESP 0x02

#define MAX_PEDAL_SLOTS 2
#define BEACON_INTERVAL 2000
#define TRANSMITTER_TIMEOUT 30000  // 30 seconds

uint8_t pairedTransmitters[2][6] = {{0}, {0}};
uint8_t transmitterPedalModes[2] = {0, 0};
unsigned long transmitterLastSeen[2] = {0, 0};
int pairedCount = 0;
int pedalSlotsUsed = 0;
unsigned long lastBeaconTime = 0;
unsigned long bootTime = 0;
uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool keysPressed[256];

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
    transmitterLastSeen[i] = 0;
  }
  
  preferences.end();
}

void removeTransmitter(int index) {
  if (index < 0 || index >= pairedCount) return;
  
  int slotsFreed = (transmitterPedalModes[index] == 0) ? 2 : 1;
  
  for (int i = index; i < pairedCount - 1; i++) {
    memcpy(pairedTransmitters[i], pairedTransmitters[i + 1], 6);
    transmitterPedalModes[i] = transmitterPedalModes[i + 1];
    transmitterLastSeen[i] = transmitterLastSeen[i + 1];
  }
  
  pairedCount--;
  pedalSlotsUsed -= slotsFreed;
  
  memset(pairedTransmitters[pairedCount], 0, 6);
  transmitterPedalModes[pairedCount] = 0;
  transmitterLastSeen[pairedCount] = 0;
  
  savePairingState();
}

bool addTransmitter(const uint8_t* mac, uint8_t pedalMode) {
  int index = getTransmitterIndex(mac);
  if (index >= 0) {
    transmitterLastSeen[index] = millis();
    return true;
  }
  
  int slotsNeeded = (pedalMode == 0) ? 2 : 1;
  if (pedalSlotsUsed + slotsNeeded > MAX_PEDAL_SLOTS) {
    return false;
  }
  
  memcpy(pairedTransmitters[pairedCount], mac, 6);
  transmitterPedalModes[pairedCount] = pedalMode;
  transmitterLastSeen[pairedCount] = millis();
  pairedCount++;
  pedalSlotsUsed += slotsNeeded;
  
  savePairingState();
  return true;
}

void sendAvailabilityBeacon() {
  if (pedalSlotsUsed >= MAX_PEDAL_SLOTS) return;
  
  struct_message beacon = {MSG_DISCOVERY_RESP, 0, false, 0};
  esp_now_send(broadcastMAC, (uint8_t*)&beacon, sizeof(beacon));
}

void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len) {
  if (len < sizeof(struct_message)) return;
  
  struct_message msg;
  memcpy(&msg, incomingData, len);
  
  switch (msg.msgType) {
    case MSG_DISCOVERY_REQ: {
      int slotsNeeded = (msg.pedalMode == 0) ? 2 : 1;
      
      if (pedalSlotsUsed + slotsNeeded <= MAX_PEDAL_SLOTS) {
        struct_message response = {MSG_DISCOVERY_RESP, 0, false, 0};
        esp_now_send(esp_now_info->src_addr, (uint8_t*)&response, sizeof(response));
        addTransmitter(esp_now_info->src_addr, msg.pedalMode);
      }
      break;
    }
      
    case MSG_PEDAL_EVENT: {
      int transmitterIndex = getTransmitterIndex(esp_now_info->src_addr);
      if (transmitterIndex < 0) break;
      
      transmitterLastSeen[transmitterIndex] = millis();
      
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
        }
      } else {
        if (keysPressed[keyIndex]) {
          Keyboard.release(keyToPress);
          keysPressed[keyIndex] = false;
        }
      }
      break;
    }
  }
}

void setup() {
  initializeKeysPressed();
  bootTime = millis();
  loadPairingState();

  WiFi.mode(WIFI_STA);
  delay(100);
  WiFi.disconnect();
  delay(100);

  if (esp_now_init() != ESP_OK) return;

  // Add saved transmitters as peers
  for (int i = 0; i < pairedCount; i++) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, pairedTransmitters[i], 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  }

  // Add broadcast peer
  esp_now_peer_info_t broadcastPeer = {};
  memcpy(broadcastPeer.peer_addr, broadcastMAC, 6);
  broadcastPeer.channel = 0;
  broadcastPeer.encrypt = false;
  esp_now_add_peer(&broadcastPeer);

  esp_now_register_recv_cb(OnDataRecv);

  USB.begin();
  delay(500);
  Keyboard.begin();
  delay(2000);
}

void loop() {
  // Check for unresponsive transmitters after timeout period
  if (millis() - bootTime > TRANSMITTER_TIMEOUT) {
    for (int i = pairedCount - 1; i >= 0; i--) {
      unsigned long timeSinceSeen = (transmitterLastSeen[i] == 0) 
        ? (millis() - bootTime) 
        : (millis() - transmitterLastSeen[i]);
      
      if (timeSinceSeen > TRANSMITTER_TIMEOUT) {
        removeTransmitter(i);
      }
    }
  }
  
  // Send availability beacon if not full
  if (pedalSlotsUsed < MAX_PEDAL_SLOTS && (millis() - lastBeaconTime > BEACON_INTERVAL)) {
    sendAvailabilityBeacon();
    lastBeaconTime = millis();
  }
  
  delay(10);
}
