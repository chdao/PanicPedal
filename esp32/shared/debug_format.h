#ifndef DEBUG_FORMAT_H
#define DEBUG_FORMAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

// Format debug message with standardized format
// Format: [R/T] [timestamp] message
// Note: MAC address is shown by debug monitor from ESP-NOW packet info, not in message
// R = Receiver, T = Transmitter
void debugFormat_message(char* buffer, size_t bufferSize, const uint8_t* mac, bool isReceiver, 
                        unsigned long bootTime, const char* format, ...);
void debugFormat_message_va(char* buffer, size_t bufferSize, const uint8_t* mac, bool isReceiver,
                            unsigned long bootTime, const char* format, va_list args);

// Format pedal event message
// Format: [R/T] [timestamp] T%d: '%c' ▼/▲
// Note: MAC address is shown by debug monitor from ESP-NOW packet info, not in message
void debugFormat_pedalEvent(char* buffer, size_t bufferSize, const uint8_t* mac, bool isReceiver,
                           unsigned long bootTime, int transmitterIndex, char key, bool pressed);

#endif // DEBUG_FORMAT_H
