#include "DebugMonitor.h"
#include <stdarg.h>
#include <string.h>
#include <Arduino.h>
#include "Persistence.h"

void debugMonitor_init(DebugMonitor* monitor, ReceiverEspNowTransport* transport, unsigned long bootTime) {
  monitor->transport = transport;
  monitor->bootTime = bootTime;
  monitor->paired = false;
  monitor->espNowInitialized = false;
  memset(monitor->mac, 0, 6);
}

void debugMonitor_handleDiscoveryRequest(DebugMonitor* monitor, const uint8_t* monitorMAC, uint8_t channel) {
  bool isSavedMonitor = (memcmp(monitorMAC, monitor->mac, 6) == 0);
  
  if (!monitor->paired || !isSavedMonitor) {
    memcpy(monitor->mac, monitorMAC, 6);
    monitor->paired = true;
    
    receiverEspNowTransport_addPeer(monitor->transport, monitorMAC, channel);
    
    // Send confirmation message
    debug_message confirmMsg = {MSG_DEBUG, {0}};
    unsigned long timeSinceBoot = millis() - monitor->bootTime;
    snprintf(confirmMsg.message, sizeof(confirmMsg.message), 
             "[%lu ms] Debug monitor paired: %02X:%02X:%02X:%02X:%02X:%02X",
             timeSinceBoot, monitorMAC[0], monitorMAC[1], monitorMAC[2],
             monitorMAC[3], monitorMAC[4], monitorMAC[5]);
    receiverEspNowTransport_send(monitor->transport, monitorMAC, (uint8_t*)&confirmMsg, sizeof(confirmMsg));
    
    debugMonitor_save(monitor);
    debugMonitor_print(monitor, "Debug monitor test message - receiver is working!");
  } else {
    // Update channel
    receiverEspNowTransport_addPeer(monitor->transport, monitorMAC, channel);
  }
}

void debugMonitor_print(DebugMonitor* monitor, const char* format, ...) {
  if (!monitor->espNowInitialized) {
    return;  // ESP-NOW not initialized yet
  }
  if (!monitor->paired) {
    return;  // Debug monitor not paired
  }
  
  // Check if MAC is valid (not all zeros)
  bool macValid = false;
  for (int i = 0; i < 6; i++) {
    if (monitor->mac[i] != 0) {
      macValid = true;
      break;
    }
  }
  if (!macValid) {
    return;  // No valid MAC address
  }
  
  char buffer[200];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  
  unsigned long timeSinceBoot = millis() - monitor->bootTime;
  char timestampedBuffer[220];
  snprintf(timestampedBuffer, sizeof(timestampedBuffer), "[DEBUG] [%lu ms] %s", timeSinceBoot, buffer);
  
  debug_message msg = {MSG_DEBUG, {0}};
  strncpy(msg.message, timestampedBuffer, sizeof(msg.message) - 1);
  msg.message[sizeof(msg.message) - 1] = '\0';
  
  bool sent = receiverEspNowTransport_send(monitor->transport, monitor->mac, (uint8_t*)&msg, sizeof(msg));
  // Note: We can't debug the debug monitor, so we silently fail if send fails
}

void debugMonitor_load(DebugMonitor* monitor) {
  persistence_loadDebugMonitor(monitor->mac, &monitor->paired);
}

void debugMonitor_save(DebugMonitor* monitor) {
  if (monitor->paired) {
    persistence_saveDebugMonitor(monitor->mac);
  }
}

