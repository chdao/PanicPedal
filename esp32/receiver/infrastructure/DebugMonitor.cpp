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

void debugMonitor_print(DebugMonitor* monitor, const char* format, ...) {
  if (!monitor || !monitor->paired || !monitor->espNowInitialized) return;
  
  // Get receiver MAC address
  uint8_t receiverMAC[6];
  WiFi.macAddress(receiverMAC);
  
  // Format message with standardized format using shared code
  char buffer[250];
  va_list args;
  va_start(args, format);
  debugFormat_message_va(buffer, sizeof(buffer), receiverMAC, true, monitor->bootTime, format, args);
  va_end(args);
  
  // Send to debug monitor via ESP-NOW
  debug_message debugMsg;
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
}
