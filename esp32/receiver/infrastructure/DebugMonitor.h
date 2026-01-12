#ifndef DEBUG_MONITOR_H
#define DEBUG_MONITOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "../infrastructure/EspNowTransport.h"

typedef struct {
  ReceiverEspNowTransport* transport;
  uint8_t mac[6];
  bool paired;
  bool espNowInitialized;
  unsigned long bootTime;
} DebugMonitor;

void debugMonitor_init(DebugMonitor* monitor, ReceiverEspNowTransport* transport, unsigned long bootTime);
void debugMonitor_load(DebugMonitor* monitor);
void debugMonitor_save(DebugMonitor* monitor);
void debugMonitor_print(DebugMonitor* monitor, const char* format, ...);
void debugMonitor_handleDiscoveryRequest(DebugMonitor* monitor, const uint8_t* senderMAC, uint8_t channel);

#endif // DEBUG_MONITOR_H
