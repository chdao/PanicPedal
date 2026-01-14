#include "TransmitterUtils.h"
#include "../messages.h"
#include "../debug_format.h"
#include <WiFi.h>
#include <string.h>
#include <stdarg.h>
#include <Arduino.h>

// External references (defined in each project's .ino file)
extern EspNowTransport* g_debugTransport;
extern unsigned long bootTime;

void transmitterUtils_formatMAC(char* buffer, size_t bufferSize, const uint8_t* mac) {
  snprintf(buffer, bufferSize, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void transmitterUtils_sendDebugMessage(const char* formattedMessage) {
  // Use unified debugPrint function to ensure same message goes to Serial and debug monitor
  extern void debugPrint(const char* format, ...);
  debugPrint("%s", formattedMessage);
}

void transmitterUtils_serialPrint(const char* format, ...) {
  va_list args;
  va_start(args, format);
  transmitterUtils_serialPrint_va(format, args);
  va_end(args);
}

void transmitterUtils_serialPrint_va(const char* format, va_list args) {
  // This function is kept for backward compatibility
  // It now formats the message and calls debugPrint (unified function)
  // Note: debugPrint is defined in the .ino file and will be available when this is included
  char buffer[250];
  vsnprintf(buffer, sizeof(buffer), format, args);
  
  // Forward declaration - debugPrint is defined in transmitter.ino
  extern void debugPrint(const char* format, ...);
  debugPrint("%s", buffer);
}
