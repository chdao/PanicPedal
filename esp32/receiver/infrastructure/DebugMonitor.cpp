#include "DebugMonitor.h"
#include "../shared/debug_format.h"
#include "../shared/messages.h"
#include "Persistence.h"
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <stdarg.h>

void debugMonitor_init(DebugMonitor* monitor, ReceiverEspNowTransport* transport, unsigned long bootTime) {
  if (!monitor) return;
  monitor->transport = transport;
  monitor->bootTime = bootTime;
  monitor->paired = false;
  monitor->espNowInitialized = false;
  memset(monitor->mac, 0, 6);
  monitor->bufferCount = 0;
  monitor->bufferIndex = 0;
  memset(monitor->messageBuffer, 0, sizeof(monitor->messageBuffer));
}

void debugMonitor_load(DebugMonitor* monitor) {
  if (!monitor) return;
  // Load debug monitor pairing state from persistence
  bool isPaired = false;
  persistence_loadDebugMonitor(monitor->mac, &isPaired);
  monitor->paired = isPaired;
}

void debugMonitor_save(DebugMonitor* monitor) {
  if (!monitor) return;
  // Save debug monitor pairing state to persistence
  if (monitor->paired) {
    persistence_saveDebugMonitor(monitor->mac);
  }
}

static void debugMonitor_bufferMessage(DebugMonitor* monitor, const char* message) {
  if (!monitor || !message || strlen(message) == 0) return;
  
  // Use circular buffer - overwrite oldest messages if buffer is full
  int index = monitor->bufferIndex % MAX_BUFFERED_MESSAGES;
  size_t msgLen = strlen(message);
  size_t maxLen = sizeof(monitor->messageBuffer[0]) - 1;
  
  if (msgLen > maxLen) {
    msgLen = maxLen;
  }
  
  memcpy(monitor->messageBuffer[index], message, msgLen);
  monitor->messageBuffer[index][msgLen] = '\0';
  
  monitor->bufferIndex++;
  if (monitor->bufferCount < MAX_BUFFERED_MESSAGES) {
    monitor->bufferCount++;
  }
}

void debugMonitor_sendBufferedMessages(DebugMonitor* monitor) {
  if (!monitor || !monitor->paired || !monitor->espNowInitialized) {
    return;
  }
  
  if (monitor->bufferCount == 0) {
    return;  // No buffered messages
  }
  
  // Add peer once
  receiverEspNowTransport_addPeer(monitor->transport, monitor->mac, 0);
  delay(10);
  
  // Send all buffered messages
  int messagesToSend = monitor->bufferCount;
  int startIndex = (monitor->bufferIndex - monitor->bufferCount) % MAX_BUFFERED_MESSAGES;
  if (startIndex < 0) startIndex += MAX_BUFFERED_MESSAGES;
  
  for (int i = 0; i < messagesToSend; i++) {
    int index = (startIndex + i) % MAX_BUFFERED_MESSAGES;
    
    debug_message debugMsg;
    debugMsg.deviceType = DEVICE_TYPE_RECEIVER;
    debugMsg.msgType = MSG_DEBUG;
    
    size_t msgLen = strlen(monitor->messageBuffer[index]);
    if (msgLen > sizeof(debugMsg.message) - 1) {
      msgLen = sizeof(debugMsg.message) - 1;
    }
    
    memcpy(debugMsg.message, monitor->messageBuffer[index], msgLen);
    debugMsg.message[msgLen] = '\0';
    
    receiverEspNowTransport_send(monitor->transport, monitor->mac, (uint8_t*)&debugMsg, sizeof(debugMsg));
    
    // Small delay between messages to avoid overwhelming ESP-NOW stack
    delay(20);
  }
  
  // Clear buffer after sending
  monitor->bufferCount = 0;
  monitor->bufferIndex = 0;
  memset(monitor->messageBuffer, 0, sizeof(monitor->messageBuffer));
}

void debugMonitor_print(DebugMonitor* monitor, const char* format, ...) {
  if (!monitor) return;
  
  // Get receiver MAC address
  uint8_t receiverMAC[6];
  WiFi.macAddress(receiverMAC);
  
  // Format message with standardized format using shared code
  char buffer[250];
  va_list args;
  va_start(args, format);
  debugFormat_message_va(buffer, sizeof(buffer), receiverMAC, true, monitor->bootTime, format, args);
  va_end(args);
  
  // If debug monitor is paired and ESP-NOW is initialized, send directly
  if (monitor->paired && monitor->espNowInitialized) {
    // Send to debug monitor via ESP-NOW
    debug_message debugMsg;
    debugMsg.deviceType = DEVICE_TYPE_RECEIVER;
    debugMsg.msgType = MSG_DEBUG;
    // Remove trailing newline if present
    int len = strlen(buffer);
    if (len > 0 && buffer[len-1] == '\n') {
      buffer[len-1] = '\0';
      len--;
    }
    strncpy(debugMsg.message, buffer, sizeof(debugMsg.message) - 1);
    debugMsg.message[sizeof(debugMsg.message) - 1] = '\0';
    
    receiverEspNowTransport_send(monitor->transport, monitor->mac, (uint8_t*)&debugMsg, sizeof(debugMsg));
  } else {
    // If debug monitor is not paired yet, buffer the message for later
    // This allows debug monitor to receive messages from boot when it connects
    debugMonitor_bufferMessage(monitor, buffer);
  }
}

void debugMonitor_handleDiscoveryRequest(DebugMonitor* monitor, const uint8_t* senderMAC, uint8_t channel) {
  if (!monitor) return;
  
  // Store the debug monitor MAC
  memcpy(monitor->mac, senderMAC, 6);
  monitor->paired = true;
  
  // Add as peer
  receiverEspNowTransport_addPeer(monitor->transport, senderMAC, channel);
  
  // Save pairing state
  debugMonitor_save(monitor);
  
  // Send all buffered messages from boot
  debugMonitor_sendBufferedMessages(monitor);
}
