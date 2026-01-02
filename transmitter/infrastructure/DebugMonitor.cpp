#include "DebugMonitor.h"
#include <stdarg.h>
#include <string.h>
#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "../shared/messages.h"
#include <Preferences.h>

// Helper function to remove trailing newline
static void removeTrailingNewline(char* str) {
  int len = strlen(str);
  while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r')) {
    str[len - 1] = '\0';
    len--;
  }
}

static Preferences preferences;

void debugMonitor_init(DebugMonitor* monitor, EspNowTransport* transport, unsigned long bootTime) {
  monitor->transport = transport;
  monitor->bootTime = bootTime;
  monitor->paired = false;
  monitor->espNowInitialized = false;
  monitor->lastBeaconTime = 0;
  // Fast zero using 32-bit and 16-bit writes
  uint32_t* mac32 = (uint32_t*)monitor->mac;
  uint16_t* mac16 = (uint16_t*)(monitor->mac + 4);
  *mac32 = 0;
  *mac16 = 0;
  // Initialize device MAC to zero - will be set after WiFi is initialized
  uint32_t* devMac32 = (uint32_t*)monitor->deviceMAC;
  uint16_t* devMac16 = (uint16_t*)(monitor->deviceMAC + 4);
  *devMac32 = 0;
  *devMac16 = 0;
}

void debugMonitor_handleDiscoveryRequest(DebugMonitor* monitor, const uint8_t* monitorMAC, uint8_t channel) {
  // Legacy support: if monitor sends discovery request, pair immediately
  debugMonitor_handleBeacon(monitor, monitorMAC, channel);
}

void debugMonitor_handleBeacon(DebugMonitor* monitor, const uint8_t* monitorMAC, uint8_t channel) {
  if (!isValidMAC(monitorMAC)) return;
  
  bool isSavedMonitor = macEqual(monitorMAC, monitor->mac);
  bool isNewPairing = (!monitor->paired || !isSavedMonitor);
  
  if (isNewPairing) {
    macCopy(monitor->mac, monitorMAC);
    monitor->paired = true;
    monitor->lastBeaconTime = millis();
    monitor->espNowInitialized = monitor->transport->initialized;  // Ensure flag is set
    
    espNowTransport_addPeer(monitor->transport, monitorMAC, channel);
    
    // Small delay to ensure peer is ready
    delay(DEBUG_MONITOR_PEER_READY_DELAY_MS);
    
    debugMonitor_save(monitor);
    
    // Send initialization messages now that monitor is connected
    debugMonitor_print(monitor, "ESP-NOW initialized");
    debugMonitor_print(monitor, "=== Transmitter Ready ===");
  } else {
    // Update channel and reset beacon time
    monitor->lastBeaconTime = millis();
    monitor->espNowInitialized = monitor->transport->initialized;  // Ensure flag is set
    espNowTransport_addPeer(monitor->transport, monitorMAC, channel);
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
  if (!isValidMAC(monitor->mac)) {
    return;  // No valid MAC address
  }
  
  char buffer[200];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  
  // Remove trailing newline from buffer (debug monitor handles line breaks)
  removeTrailingNewline(buffer);
  
  // Format message with [T] prefix (MAC will be added by debug monitor from packet sender)
  debug_message msg = {MSG_DEBUG, {0}};
  int written = snprintf(msg.message, sizeof(msg.message), "[T] %s", buffer);
  if (written >= sizeof(msg.message)) {
    msg.message[sizeof(msg.message) - 1] = '\0';  // Ensure null termination
  }
  
  // Calculate actual message length (msgType + string length + null terminator)
  size_t messageLen = 1 + strlen(msg.message) + 1;  // msgType + message + null
  
  // Always try to send - no timeout/disconnect logic
  // If send fails, we'll keep trying (monitor will reconnect via beacon)
  espNowTransport_send(monitor->transport, monitor->mac, (uint8_t*)&msg, messageLen);
}

void debugMonitor_update(DebugMonitor* monitor, unsigned long currentTime) {
  // Check if we haven't seen a beacon in a while (monitor might be offline)
  // But don't unpair - just mark as potentially disconnected
  // Monitor will reconnect via beacon when it comes back online
  if (monitor->paired && monitor->lastBeaconTime > 0) {
    unsigned long timeSinceLastBeacon = currentTime - monitor->lastBeaconTime;
    // If no beacon for 30 seconds, mark as potentially disconnected
    // But keep paired state so we can reconnect quickly when beacon returns
    if (timeSinceLastBeacon > 30000) {
      // Monitor might be offline, but keep paired state for quick reconnect
      // This allows messages to queue and be sent when monitor comes back
    }
  }
}

void debugMonitor_load(DebugMonitor* monitor) {
  preferences.begin("debugmon", true);  // Read-only
  if (preferences.isKey("mac0")) {
    for (int i = 0; i < 6; i++) {
      char key[8];
      snprintf(key, sizeof(key), "mac%d", i);
      monitor->mac[i] = preferences.getUChar(key, 0);
    }
    monitor->paired = isValidMAC(monitor->mac);
  } else {
    monitor->paired = false;
  }
  preferences.end();
}

void debugMonitor_save(DebugMonitor* monitor) {
  if (monitor->paired) {
    preferences.begin("debugmon", false);  // Read-write
    for (int i = 0; i < 6; i++) {
      char key[8];
      snprintf(key, sizeof(key), "mac%d", i);
      preferences.putUChar(key, monitor->mac[i]);
    }
    preferences.end();
  }
}

// Save debug enabled state
void debugMonitor_saveDebugState(bool debugEnabled) {
  preferences.begin("debugmon", false);  // Read-write
  preferences.putBool("debugEnabled", debugEnabled);
  preferences.end();
}

// Load debug enabled state
bool debugMonitor_loadDebugState() {
  preferences.begin("debugmon", true);  // Read-only
  bool debugEnabled = preferences.getBool("debugEnabled", false);  // Default to false
  preferences.end();
  return debugEnabled;
}

