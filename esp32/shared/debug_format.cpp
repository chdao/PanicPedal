#include "debug_format.h"
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>

void debugFormat_message(char* buffer, size_t bufferSize, const uint8_t* mac, bool isReceiver,
                        unsigned long bootTime, const char* format, ...) {
  va_list args;
  va_start(args, format);
  debugFormat_message_va(buffer, bufferSize, mac, isReceiver, bootTime, format, args);
  va_end(args);
}

void debugFormat_message_va(char* buffer, size_t bufferSize, const uint8_t* mac, bool isReceiver,
                            unsigned long bootTime, const char* format, va_list args) {
  if (!buffer || bufferSize == 0) return;
  
  // Clear buffer to ensure clean output
  memset(buffer, 0, bufferSize);
  
  // Format MAC address
  char macStr[18];
  if (mac) {
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else {
    strcpy(macStr, "UNKNOWN");
  }
  
  // Calculate timestamp
  unsigned long timestamp = millis() - bootTime;
  float timestampSeconds = timestamp / 1000.0f;
  
  // Format prefix: [R/T] [timestamp] (MAC is shown by debug monitor, not in message)
  int prefixLen = snprintf(buffer, bufferSize, "[%c] [%.3fs] ", 
                           isReceiver ? 'R' : 'T', timestampSeconds);
  
  if (prefixLen < 0 || prefixLen >= (int)bufferSize) {
    return;  // Buffer too small
  }
  
  // Append formatted message
  vsnprintf(buffer + prefixLen, bufferSize - prefixLen, format, args);
}

void debugFormat_pedalEvent(char* buffer, size_t bufferSize, const uint8_t* mac, bool isReceiver,
                           unsigned long bootTime, int transmitterIndex, char key, bool pressed) {
  if (!buffer || bufferSize == 0) return;
  
  // Format MAC address
  char macStr[18];
  if (mac) {
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else {
    strcpy(macStr, "UNKNOWN");
  }
  
  // Calculate timestamp
  unsigned long timestamp = millis() - bootTime;
  float timestampSeconds = timestamp / 1000.0f;
  
  // Format: [R/T] [timestamp] T%d: '%c' ▼/▲ (MAC is shown by debug monitor, not in message)
  snprintf(buffer, bufferSize, "[%c] [%.3fs] T%d: '%c' %s",
           isReceiver ? 'R' : 'T', timestampSeconds, 
           transmitterIndex, key, pressed ? "▼" : "▲");
}
