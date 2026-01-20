#ifndef DEBUG_SERVICE_H
#define DEBUG_SERVICE_H

#include <Arduino.h>
#include "EspNowTransport.h"

// Debug system for Serial and ESP-NOW debug monitor
// Works with both EspNowTransport (transmitters) and ReceiverEspNowTransport (receiver)
class DebugService {
public:
  // Initialize with EspNowTransport (for transmitters)
  static void init(EspNowTransport* transport);
  
  // Initialize with function pointers (for receiver or custom transports)
  typedef bool (*SendFunction)(const uint8_t* mac, const uint8_t* data, int len);
  typedef bool (*AddPeerFunction)(const uint8_t* mac, uint8_t channel);
  typedef bool (*IsInitializedFunction)();
  static void init(SendFunction sendFunc, AddPeerFunction addPeerFunc, IsInitializedFunction isInitFunc);
  
  static void setEnabled(bool enabled);
  static bool isEnabled();
  static void print(const char* format, ...) __attribute__((format(printf, 1, 2)));
  static void serialPrint(const char* format, ...) __attribute__((format(printf, 1, 2)));
  static void sendDebugMessage(const char* formattedMessage);
  
  // Process pending debug messages (deferred from callbacks)
  static void processPending();
  
  // Get debug transport (for PowerManagement) - only works with EspNowTransport
  static EspNowTransport* getDebugTransport();
  
  // Handle debug monitor discovery (when receiving MSG_DEBUG_MONITOR_REQ from monitor)
  // This is called when debug monitor responds to MSG_ONLINE
  static void handleDebugMonitorDiscovery(const uint8_t* monitorMAC);
  
  // Set device type for debug messages (defaults to DEVICE_TYPE_TRANSMITTER)
  static void setDeviceType(uint8_t deviceType);
  
private:
  static EspNowTransport* g_debugTransport;
  static SendFunction g_sendFunction;
  static AddPeerFunction g_addPeerFunction;
  static IsInitializedFunction g_isInitializedFunction;
  static bool g_useFunctionPointers;
  static bool debugEnabled;
  static uint8_t g_cachedMAC[6];
  static bool g_macCached;
  static bool hasPendingDebugMessage;
  static char pendingDebugMessage[200];
  static uint8_t g_debugMonitorMAC[6];
  static bool g_debugMonitorKnown;
  static uint8_t g_deviceType;  // Device type for debug messages (DEVICE_TYPE_TRANSMITTER, DEVICE_TYPE_RECEIVER, etc.)
  
  // Message buffer for messages sent before debug monitor discovery
  static const int MAX_BUFFERED_MESSAGES = 50;  // Limit buffer size to prevent memory issues
  static char g_messageBuffer[MAX_BUFFERED_MESSAGES][200];
  static int g_bufferCount;
  static int g_bufferIndex;  // For circular buffer
  
  static void cacheMAC();
  static void bufferMessage(const char* message);
  static void sendBufferedMessages();
};

#endif // DEBUG_SERVICE_H
