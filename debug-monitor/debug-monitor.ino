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

#define MSG_DEBUG 0x04

uint8_t receiverMAC[6] = {0};  // Will be set via Serial input or auto-discovery
bool isPaired = false;
bool discoveryMode = true;
unsigned long discoveryStartTime = 0;
unsigned long lastDiscoverySend = 0;

#define DISCOVERY_TIMEOUT 10000  // 10 seconds
#define DISCOVERY_SEND_INTERVAL 1000  // Send discovery every 1 second
uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Discovery request structure (matches receiver)
typedef struct __attribute__((packed)) discovery_req {
  uint8_t msgType;   // 0x05 = debug monitor discovery request
  uint8_t reserved[3];
} discovery_req;

#define MSG_DEBUG_MONITOR_REQ 0x05

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < sizeof(debug_message)) return;
  
  debug_message msg;
  // Ensure message is null-terminated
  memset(&msg, 0, sizeof(msg));
  memcpy(&msg, data, len < (int)sizeof(debug_message) ? len : sizeof(debug_message));
  msg.message[sizeof(msg.message) - 1] = '\0';  // Ensure null termination
  
  if (msg.msgType == MSG_DEBUG) {
    // Print sender MAC address in standardized format: [MAC]
    // Use a larger buffer and ensure proper formatting
    char macStr[20];  // Larger buffer to ensure no truncation
    int macLen = snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                          info->src_addr[0], info->src_addr[1], info->src_addr[2],
                          info->src_addr[3], info->src_addr[4], info->src_addr[5]);
    if (macLen > 0 && macLen < (int)sizeof(macStr)) {
      Serial.print("[");
      Serial.print(macStr);
      Serial.print("] ");
    } else {
      // Fallback if formatting failed
      Serial.print("[MAC_ERROR] ");
    }
    
    // Print the debug message (which contains [R/T] [timestamp] message format)
    // Ensure message is properly null-terminated before printing
    Serial.print(msg.message);
    Serial.println();  // Explicit newline
    Serial.flush();    // Ensure output is complete
    delay(1);          // Small delay to prevent buffer overflow
  }
}

void sendDiscoveryRequest() {
  discovery_req req = {MSG_DEBUG_MONITOR_REQ, {0, 0, 0}};
  esp_now_send(broadcastMAC, (uint8_t*)&req, sizeof(req));
}

void startDiscovery() {
  discoveryMode = true;
  discoveryStartTime = millis();
  lastDiscoverySend = 0;
  
  Serial.println("Starting discovery for receiver...");
  Serial.println("Waiting for receiver to respond...");
  
  esp_now_register_recv_cb(OnDataRecv);
  
  esp_now_peer_info_t broadcastPeer = {};
  memcpy(broadcastPeer.peer_addr, broadcastMAC, 6);
  broadcastPeer.channel = 0;
  broadcastPeer.encrypt = false;
  esp_now_add_peer(&broadcastPeer);
}

void setup() {
  Serial.begin(115200);
  // Increase Serial buffer size to prevent truncation
  Serial.setTxBufferSize(1024);
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
  
  // Print MAC address for manual pairing if needed
  Serial.print("Debug Monitor MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println();
  
  startDiscovery();
}

void loop() {
  if (discoveryMode) {
    if (millis() - discoveryStartTime > DISCOVERY_TIMEOUT) {
      Serial.println("Discovery timeout - receiver not found");
      Serial.println("Make sure receiver is powered on and running latest code");
      discoveryMode = false;
    } else if (millis() - lastDiscoverySend > DISCOVERY_SEND_INTERVAL) {
      sendDiscoveryRequest();
      lastDiscoverySend = millis();
    }
    delay(100);
    return;
  }
  
  // Monitor is ready - debug messages will be received via callback
  delay(100);
}

