/*
 * ESP-NOW Debug Monitor for Pedal Receiver
 * 
 * This sketch runs on a secondary ESP32-S3-DevKit to receive and display
 * debug messages from the pedal receiver via ESP-NOW.
 * 
 * The receiver sends debug messages that are displayed on Serial (USB).
 */

#include <Arduino.h>
#include <string.h>

// Reuse shared message definitions + ESP-NOW transport abstraction.
// This keeps message types/structs consistent across receiver/transmitters/debug monitor.
#include "../shared/messages.h"
#include "../shared/infrastructure/EspNowTransport.h"

static EspNowTransport g_transport;

static bool discoveryMode = true;
static unsigned long lastDiscoverySend = 0;

static bool gotAnyDebugMessage = false;

#define DISCOVERY_SEND_INTERVAL 3000     // Send discovery every 3 seconds

// Message queue to avoid printing from interrupt context (ESP-NOW callback)
// Copy formatted lines to queue, print from main loop to prevent truncation/interleaving
#define MAX_QUEUED_MESSAGES 16
#define MAX_MESSAGE_LENGTH 240  // [MAC] + message + \n

typedef struct {
  char line[MAX_MESSAGE_LENGTH];
} QueuedMessage;

static QueuedMessage g_messageQueue[MAX_QUEUED_MESSAGES];
static volatile int g_queueWriteIndex = 0;
static volatile int g_queueReadIndex = 0;
static volatile int g_queueCount = 0;

// Debug monitor discovery request (receiver only checks msgType).
typedef struct __attribute__((packed)) debug_monitor_req_message {
  uint8_t msgType;        // MSG_DEBUG_MONITOR_REQ
  uint8_t reserved[3];
} debug_monitor_req_message;

// Queue a formatted message line (called from ESP-NOW callback - must be fast/non-blocking)
static bool queueMessage(const char* formattedLine) {
  // Check if queue is full (atomic check)
  if (g_queueCount >= MAX_QUEUED_MESSAGES) {
    return false;  // Queue full, drop message
  }
  
  // Copy the formatted line to the queue
  int writeIdx = g_queueWriteIndex;
  int len = strlen(formattedLine);
  if (len >= MAX_MESSAGE_LENGTH) {
    len = MAX_MESSAGE_LENGTH - 1;
  }
  
  memcpy(g_messageQueue[writeIdx].line, formattedLine, len);
  g_messageQueue[writeIdx].line[len] = '\0';
  
  // Update write index and count (atomic operations)
  g_queueWriteIndex = (writeIdx + 1) % MAX_QUEUED_MESSAGES;
  g_queueCount++;
  
  return true;
}

// Process queued messages and print them (called from main loop - blocking OK)
static void processMessageQueue() {
  while (g_queueCount > 0) {
    int readIdx = g_queueReadIndex;
    
    // Print the entire line in one write to avoid truncation
    int len = strlen(g_messageQueue[readIdx].line);
    if (len > 0) {
      Serial.write((const uint8_t*)g_messageQueue[readIdx].line, len);
      // Ensure CRLF if missing (Windows serial terminals expect \r\n)
      if (len < 2 || g_messageQueue[readIdx].line[len - 2] != '\r' || g_messageQueue[readIdx].line[len - 1] != '\n') {
        // Line doesn't end with \r\n - add it
        Serial.write('\r');
        Serial.write('\n');
      }
    }






    // Update read index and count (atomic operations)
    g_queueReadIndex = (readIdx + 1) % MAX_QUEUED_MESSAGES;
    g_queueCount--;
  }
}

static void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t /*channel*/) {
  if (!senderMAC || !data || len < 1) return;

  uint8_t msgType = data[0];
  if (msgType != MSG_DEBUG) return;

  // Copy message data immediately (ESP-NOW buffer may be reused)
  debug_message msg;
  memset(&msg, 0, sizeof(msg));
  memcpy(&msg, data, (len < (int)sizeof(msg)) ? len : (int)sizeof(msg));
  msg.message[sizeof(msg.message) - 1] = '\0';

  // Format the entire line: [MAC] message\n
  char macStr[20];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           senderMAC[0], senderMAC[1], senderMAC[2],
           senderMAC[3], senderMAC[4], senderMAC[5]);

  char formattedLine[MAX_MESSAGE_LENGTH];
  int n = snprintf(formattedLine, sizeof(formattedLine), "[%s] %s\r\n", macStr, msg.message);
  if (n < 0 || n >= (int)sizeof(formattedLine)) {
    // Truncate if needed, ensure newline
    formattedLine[sizeof(formattedLine) - 3] = '\r';
    formattedLine[sizeof(formattedLine) - 2] = '\n';
    formattedLine[sizeof(formattedLine) - 1] = '\0';
  }

  // Queue the formatted line (non-blocking, fast)
  queueMessage(formattedLine);

  gotAnyDebugMessage = true;
  discoveryMode = false;
}

static void sendDiscoveryRequest() {
  debug_monitor_req_message req = {MSG_DEBUG_MONITOR_REQ, {0, 0, 0}};
  espNowTransport_broadcast(&g_transport, (uint8_t*)&req, sizeof(req));
}

static void startDiscovery() {
  discoveryMode = true;
  lastDiscoverySend = 0;

  // Queue discovery messages instead of blocking on Serial
  queueMessage("Starting discovery for receiver...\r\n");
  queueMessage("Waiting for receiver to respond...\r\n");
}

void setup() {
  Serial.begin(115200);
  Serial.setTxBufferSize(2048);  // Larger buffer to prevent truncation
  // Don't delay - device should boot immediately regardless of Serial connection
  
  // Initialize message queue FIRST (before any Serial output)
  memset(g_messageQueue, 0, sizeof(g_messageQueue));
  g_queueWriteIndex = 0;
  g_queueReadIndex = 0;
  g_queueCount = 0;
  
  // Initialize ESP-NOW immediately (don't wait for Serial)
  espNowTransport_init(&g_transport);
  if (!g_transport.initialized) {
    // Queue error message instead of blocking on Serial
    queueMessage("[SYSTEM] Error initializing ESP-NOW\r\n");
    return;
  }
  
  espNowTransport_registerReceiveCallback(&g_transport, onMessageReceived);
  
  // Queue startup messages (will be printed when Serial is available)
  queueMessage("========================================\r\n");
  queueMessage("ESP-NOW Debug Monitor\r\n");
  queueMessage("========================================\r\n");
  queueMessage("\r\n");
  queueMessage("This monitor receives debug messages from the pedal receiver.\r\n");
  queueMessage("Waiting for receiver to start sending debug messages...\r\n");
  queueMessage("\r\n");
  queueMessage("ESP-NOW initialized\r\n");
  queueMessage("\r\n");
  
  startDiscovery();
}

void loop() {
  // Process queued messages first (print from main loop, not callback)
  bool hadMessages = (g_queueCount > 0);
  processMessageQueue();

  if (discoveryMode) {
    // Keep sending discovery requests periodically (receiver may come online later)
    if (millis() - lastDiscoverySend > DISCOVERY_SEND_INTERVAL) {
      sendDiscoveryRequest();
      lastDiscoverySend = millis();
    }
    delay(100);
    return;
  }
  
  if (gotAnyDebugMessage) {
    // Monitor is ready - debug messages will be received via callback and queued
    // Use adaptive delay: shorter when processing messages, longer when idle
    if (hadMessages) {
      delay(20);  // Had messages - check more frequently (50Hz)
    } else {
      delay(100);  // No messages - check less frequently (10Hz)
    }
    return;
  }

  // If discovery timed out but we haven't seen any debug message yet, keep idling.
  delay(100);
}

// Arduino IDE doesn't automatically compile shared .cpp files outside the sketch folder.
// Include the transport implementation directly so the debug monitor shares the same ESP-NOW code.
#include "../shared/infrastructure/EspNowTransport.cpp"
