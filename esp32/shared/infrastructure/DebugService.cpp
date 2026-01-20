#include "DebugService.h"
#include "../debug_format.h"
#include "../messages.h"
#include <WiFi.h>
#include <stdarg.h>

// Forward declaration
extern unsigned long bootTime;

EspNowTransport* DebugService::g_debugTransport = nullptr;
DebugService::SendFunction DebugService::g_sendFunction = nullptr;
DebugService::AddPeerFunction DebugService::g_addPeerFunction = nullptr;
DebugService::IsInitializedFunction DebugService::g_isInitializedFunction = nullptr;
bool DebugService::g_useFunctionPointers = false;
bool DebugService::debugEnabled = false;
uint8_t DebugService::g_cachedMAC[6] = {0};
bool DebugService::g_macCached = false;
bool DebugService::hasPendingDebugMessage = false;
char DebugService::pendingDebugMessage[200] = {0};
uint8_t DebugService::g_debugMonitorMAC[6] = {0};
bool DebugService::g_debugMonitorKnown = false;
uint8_t DebugService::g_deviceType = DEVICE_TYPE_TRANSMITTER;  // Default to transmitter
char DebugService::g_messageBuffer[DebugService::MAX_BUFFERED_MESSAGES][200] = {0};
int DebugService::g_bufferCount = 0;
int DebugService::g_bufferIndex = 0;

void DebugService::init(EspNowTransport* transport) {
  g_debugTransport = transport;
  g_sendFunction = nullptr;
  g_addPeerFunction = nullptr;
  g_isInitializedFunction = nullptr;
  g_useFunctionPointers = false;
  g_debugMonitorKnown = false;
  g_deviceType = DEVICE_TYPE_TRANSMITTER;  // Default to transmitter
  memset(g_debugMonitorMAC, 0, 6);
  g_bufferCount = 0;
  g_bufferIndex = 0;
  memset(g_messageBuffer, 0, sizeof(g_messageBuffer));
  cacheMAC();
}

void DebugService::init(SendFunction sendFunc, AddPeerFunction addPeerFunc, IsInitializedFunction isInitFunc) {
  g_debugTransport = nullptr;
  g_sendFunction = sendFunc;
  g_addPeerFunction = addPeerFunc;
  g_isInitializedFunction = isInitFunc;
  g_useFunctionPointers = true;
  g_debugMonitorKnown = false;
  g_deviceType = DEVICE_TYPE_TRANSMITTER;  // Default to transmitter (can be changed with setDeviceType)
  memset(g_debugMonitorMAC, 0, 6);
  g_bufferCount = 0;
  g_bufferIndex = 0;
  memset(g_messageBuffer, 0, sizeof(g_messageBuffer));
  cacheMAC();
}

void DebugService::setDeviceType(uint8_t deviceType) {
  g_deviceType = deviceType;
}

void DebugService::setEnabled(bool enabled) {
  debugEnabled = enabled;
}

bool DebugService::isEnabled() {
  return debugEnabled;
}

void DebugService::cacheMAC() {
  if (!g_macCached) {
    WiFi.macAddress(g_cachedMAC);
    g_macCached = true;
  }
}

void DebugService::print(const char* format, ...) {
  // Runtime gate: when debug is disabled, do not emit logs to Serial or debug monitor.
  // This keeps normal operation quiet and reduces the chance of WDT issues from heavy I/O.
  if (!debugEnabled) {
    return;
  }

  // Use cached MAC address
  cacheMAC();
  
  // Format message with standardized format
  char buffer[250];
  va_list args;
  va_start(args, format);
  debugFormat_message_va(buffer, sizeof(buffer), g_cachedMAC, false, bootTime, format, args);
  va_end(args);
  
  // Send to Serial (if DEBUG_ENABLED) - non-blocking write in chunks
  #ifdef DEBUG_ENABLED
  if (DEBUG_ENABLED) {
    size_t len = strlen(buffer);
    if (len > 0) {
      size_t written = 0;
      while (written < len) {
        size_t chunkSize = min(len - written, (size_t)64);
        Serial.write((const uint8_t*)(buffer + written), chunkSize);
        written += chunkSize;
        yield();
      }
      
      if (buffer[len-1] != '\n') {
        Serial.println();
        yield();
      }
    }
  }
  #endif
  
  // Send to debug monitor via ESP-NOW (if transport available)
  // Same message goes to both Serial and debug monitor for consistency
  bool transportReady = false;
  if (g_useFunctionPointers) {
    transportReady = (g_sendFunction && g_addPeerFunction && g_isInitializedFunction && g_isInitializedFunction());
  } else {
    transportReady = (g_debugTransport && g_debugTransport->initialized);
  }
  
  if (transportReady) {
    debug_message debugMsg;
    debugMsg.deviceType = g_deviceType;  // Use configured device type
    debugMsg.msgType = MSG_DEBUG;
    
    int len = strlen(buffer);
    if (len > 0 && buffer[len-1] == '\n') {
      buffer[len-1] = '\0';
      len--;
    }
    
    size_t maxMsgLen = sizeof(debugMsg.message) - 1;
    if (len > (int)maxMsgLen) {
      len = maxMsgLen;
    }
    
    memcpy(debugMsg.message, buffer, len);
    debugMsg.message[len] = '\0';
    
    // Small delay to ensure ESP-NOW stack is ready (especially after sending other messages)
    yield();
    delay(10);
    
    // Only send to debug monitor if known - don't broadcast (reduces network spam)
    if (g_debugMonitorKnown) {
      // Send directly to known debug monitor
      if (g_useFunctionPointers) {
        g_addPeerFunction(g_debugMonitorMAC, 0);
        delay(5);
        g_sendFunction(g_debugMonitorMAC, (uint8_t*)&debugMsg, sizeof(debugMsg));
      } else {
        espNowTransport_addPeer(g_debugTransport, g_debugMonitorMAC, 0);
        delay(5);
        espNowTransport_send(g_debugTransport, g_debugMonitorMAC, (uint8_t*)&debugMsg, sizeof(debugMsg));
      }
      
      // Small delay after send to ensure it completes
      yield();
      delay(5);
    } else {
      // If no debug monitor is known, buffer the message for later
      // This allows debug monitor to receive messages from boot when it connects
      bufferMessage(buffer);
    }
  }
}

void DebugService::serialPrint(const char* format, ...) {
  // Always print to Serial (boot/config messages).
  // For debug monitor output, we only forward when debugEnabled is true via print().
  #ifdef DEBUG_ENABLED
  if (!DEBUG_ENABLED) {
    return;
  }

  va_list args;
  va_start(args, format);
  char buffer[250];
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  Serial.println(buffer);

  // Forward same message to debug monitor if enabled (keeps outputs consistent when debugging).
  if (debugEnabled) {
    print("%s", buffer);
  }
  #endif
}

void DebugService::sendDebugMessage(const char* formattedMessage) {
  print("%s", formattedMessage);
}

void DebugService::processPending() {
  if (hasPendingDebugMessage) {
    hasPendingDebugMessage = false;
    print("%s", pendingDebugMessage);
  }
}

EspNowTransport* DebugService::getDebugTransport() {
  return g_debugTransport;
}

void DebugService::handleDebugMonitorDiscovery(const uint8_t* monitorMAC) {
  if (!monitorMAC) return;
  
  // Store the debug monitor's MAC address
  memcpy(g_debugMonitorMAC, monitorMAC, 6);
  g_debugMonitorKnown = true;
  
  #ifdef DEBUG_ENABLED
  if (DEBUG_ENABLED && debugEnabled) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             monitorMAC[0], monitorMAC[1], monitorMAC[2],
             monitorMAC[3], monitorMAC[4], monitorMAC[5]);
    serialPrint("Debug monitor discovered: %s - will send debug messages directly", macStr);
  }
  #endif
  
  // Send all buffered messages from boot
  sendBufferedMessages();
}

void DebugService::bufferMessage(const char* message) {
  if (!message || strlen(message) == 0) return;
  
  // Use circular buffer - overwrite oldest messages if buffer is full
  int index = g_bufferIndex % MAX_BUFFERED_MESSAGES;
  size_t msgLen = strlen(message);
  size_t maxLen = sizeof(g_messageBuffer[0]) - 1;
  
  if (msgLen > maxLen) {
    msgLen = maxLen;
  }
  
  memcpy(g_messageBuffer[index], message, msgLen);
  g_messageBuffer[index][msgLen] = '\0';
  
  g_bufferIndex++;
  if (g_bufferCount < MAX_BUFFERED_MESSAGES) {
    g_bufferCount++;
  }
}

void DebugService::sendBufferedMessages() {
  bool transportReady = false;
  if (g_useFunctionPointers) {
    transportReady = (g_sendFunction && g_addPeerFunction && g_isInitializedFunction && g_isInitializedFunction());
  } else {
    transportReady = (g_debugTransport && g_debugTransport->initialized);
  }
  
  if (!transportReady || !g_debugMonitorKnown) {
    return;
  }
  
  if (g_bufferCount == 0) {
    return;  // No buffered messages
  }
  
  #ifdef DEBUG_ENABLED
  if (DEBUG_ENABLED && debugEnabled) {
    serialPrint("Sending %d buffered debug message(s) to debug monitor", g_bufferCount);
  }
  #endif
  
  // Add peer once
  if (g_useFunctionPointers) {
    g_addPeerFunction(g_debugMonitorMAC, 0);
  } else {
    espNowTransport_addPeer(g_debugTransport, g_debugMonitorMAC, 0);
  }
  delay(10);
  
  // Send all buffered messages
  int messagesToSend = g_bufferCount;
  int startIndex = (g_bufferIndex - g_bufferCount) % MAX_BUFFERED_MESSAGES;
  if (startIndex < 0) startIndex += MAX_BUFFERED_MESSAGES;
  
  for (int i = 0; i < messagesToSend; i++) {
    int index = (startIndex + i) % MAX_BUFFERED_MESSAGES;
    
    debug_message debugMsg;
    debugMsg.deviceType = g_deviceType;  // Use configured device type
    debugMsg.msgType = MSG_DEBUG;
    
    size_t msgLen = strlen(g_messageBuffer[index]);
    if (msgLen > sizeof(debugMsg.message) - 1) {
      msgLen = sizeof(debugMsg.message) - 1;
    }
    
    memcpy(debugMsg.message, g_messageBuffer[index], msgLen);
    debugMsg.message[msgLen] = '\0';
    
    if (g_useFunctionPointers) {
      g_sendFunction(g_debugMonitorMAC, (uint8_t*)&debugMsg, sizeof(debugMsg));
    } else {
      espNowTransport_send(g_debugTransport, g_debugMonitorMAC, (uint8_t*)&debugMsg, sizeof(debugMsg));
    }
    
    // Small delay between messages to avoid overwhelming ESP-NOW stack
    yield();
    delay(20);
  }
  
  // Clear buffer after sending
  g_bufferCount = 0;
  g_bufferIndex = 0;
  memset(g_messageBuffer, 0, sizeof(g_messageBuffer));
}


// Global functions for backward compatibility
void debugPrint(const char* format, ...) {
  va_list args;
  va_start(args, format);
  char buffer[250];
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  DebugService::print("%s", buffer);
}

void serialPrint(const char* format, ...) {
  va_list args;
  va_start(args, format);
  char buffer[250];
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  DebugService::serialPrint("%s", buffer);
}

void sendDebugMessage(const char* formattedMessage) {
  DebugService::sendDebugMessage(formattedMessage);
}
