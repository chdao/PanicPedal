/*
 * ESP-NOW Debug Monitor for Pedal Receiver
 * 
 * This sketch runs on a secondary ESP32-S3-DevKit to receive and display
 * debug messages from the pedal receiver via ESP-NOW.
 * 
 * The receiver sends debug messages that are displayed on Serial (USB).
 */

#include <WiFi.h>
#include <esp_now.h>

// Debug message structure
typedef struct __attribute__((packed)) debug_message {
  uint8_t msgType;   // 0x04 = debug message
  char message[200]; // Debug message text (null-terminated)
} debug_message;

// Debug monitor beacon message structure
typedef struct __attribute__((packed)) debug_monitor_beacon_message {
  uint8_t msgType;        // 0x08 = MSG_DEBUG_MONITOR_BEACON
  uint8_t monitorMAC[6];
} debug_monitor_beacon_message;

#define MSG_DEBUG 0x04
#define MSG_DEBUG_MONITOR_BEACON 0x08

uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#define BEACON_SEND_INTERVAL 5000  // Broadcast beacon every 5 seconds
unsigned long lastBeaconSend = 0;

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  // Extract sender MAC from packet info
  const uint8_t* senderMAC = info->src_addr;
  
  // Add sender as peer if not already added (needed for reliable communication)
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, senderMAC, 6);
  peerInfo.channel = info->rx_ctrl ? info->rx_ctrl->channel : 0;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);  // Will return ESP_ERR_ESPNOW_EXIST if already exists, which is fine
  
  if (len < 1) return;
  
  uint8_t msgType = data[0];
  
  // Handle debug monitor beacon (ignore our own beacons)
  if (msgType == MSG_DEBUG_MONITOR_BEACON) {
    return;
  }
  
  // Check if this is a debug message (minimum size: msgType + at least 1 byte of message)
  if (msgType == MSG_DEBUG && len >= 2) {
    debug_message msg;
    // Zero out the message buffer first to avoid garbage data
    memset(&msg, 0, sizeof(msg));
    
    // Copy the received data (senders already null-terminate their messages)
    memcpy(&msg, data, len < sizeof(debug_message) ? len : sizeof(debug_message));
    
    // Safety: ensure null termination at end of buffer (in case of buffer overrun)
    msg.message[sizeof(msg.message) - 1] = '\0';
    
    // Build the complete output string first to avoid Serial buffer issues
    char output[256];
    int offset = 0;
    
    // Add MAC prefix
    output[offset++] = '[';
    for (int i = 0; i < 6; i++) {
      if (senderMAC[i] < 0x10) {
        output[offset++] = '0';
      }
      offset += sprintf(output + offset, "%02X", senderMAC[i]);
      if (i < 5) {
        output[offset++] = ':';
      }
    }
    offset += sprintf(output + offset, "] %s", msg.message);
    
    // Print the complete line at once to avoid corruption
    Serial.println(output);
    // Note: Serial.flush() waits for transmission to complete, which can block
    // For high-frequency messages, we rely on the larger buffer instead
  }
}

void sendBeacon() {
  uint8_t monitorMAC[6];
  WiFi.macAddress(monitorMAC);
  
  debug_monitor_beacon_message beacon;
  beacon.msgType = MSG_DEBUG_MONITOR_BEACON;
  memcpy(beacon.monitorMAC, monitorMAC, 6);
  
  esp_now_send(broadcastMAC, (uint8_t*)&beacon, sizeof(beacon));
}

void setup() {
  Serial.begin(115200);
  Serial.setRxBufferSize(2048);  // Increase RX buffer
  Serial.setTxBufferSize(2048);  // Increase TX buffer to handle burst messages
  delay(2000);
  
  Serial.println("========================================");
  Serial.println("ESP-NOW Debug Monitor");
  Serial.println("========================================");
  Serial.println();
  Serial.println("This monitor receives debug messages from the pedal receiver.");
  Serial.println("Waiting for receiver to start sending debug messages...");
  Serial.println();
  
  WiFi.mode(WIFI_STA);
  delay(100);
  WiFi.disconnect();
  delay(100);
  
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  
  Serial.println("ESP-NOW initialized");
  Serial.println();
  
  // Print MAC address
  Serial.print("Debug Monitor MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println();
  
  // Register receive callback
  esp_now_register_recv_cb(OnDataRecv);
  
  // Add broadcast peer
  esp_now_peer_info_t broadcastPeer = {};
  memcpy(broadcastPeer.peer_addr, broadcastMAC, 6);
  broadcastPeer.channel = 0;
  broadcastPeer.encrypt = false;
  esp_now_add_peer(&broadcastPeer);
  
  Serial.println("Broadcasting beacon - clients will connect automatically");
  Serial.println();
}

void loop() {
  // Broadcast beacon periodically so clients can discover and connect
  unsigned long currentTime = millis();
  if (currentTime - lastBeaconSend > BEACON_SEND_INTERVAL) {
    sendBeacon();
    lastBeaconSend = currentTime;
  }
  
  // Debug messages will be received via callback
  delay(100);
}

