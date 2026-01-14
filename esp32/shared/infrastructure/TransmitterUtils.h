#ifndef TRANSMITTER_UTILS_H
#define TRANSMITTER_UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "EspNowTransport.h"

// Global debug transport pointer (set by each project)
extern EspNowTransport* g_debugTransport;
extern unsigned long bootTime;

// Helper function to send debug message to debug monitor (works even when debugEnabled is false)
void transmitterUtils_sendDebugMessage(const char* formattedMessage);

// Always output to serial with standardized format (for boot messages)
// Also sends to debug monitor if available
void transmitterUtils_serialPrint(const char* format, ...);
void transmitterUtils_serialPrint_va(const char* format, va_list args);  // va_list version

// Format MAC address as string
void transmitterUtils_formatMAC(char* buffer, size_t bufferSize, const uint8_t* mac);

#endif // TRANSMITTER_UTILS_H
