#ifndef DEBUG_MONITOR_H
#define DEBUG_MONITOR_H

#include <stdint.h>
#include <stdbool.h>
#include "../infrastructure/EspNowTransport.h"
#include "../shared/messages.h"

typedef struct {
  uint8_t mac[6];
  bool paired;
  ReceiverEspNowTransport* transport;
  unsigned long bootTime;
  bool espNowInitialized;
} DebugMonitor;

void debugMonitor_init(DebugMonitor* monitor, ReceiverEspNowTransport* transport, unsigned long bootTime);
void debugMonitor_handleDiscoveryRequest(DebugMonitor* monitor, const uint8_t* monitorMAC, uint8_t channel);
void debugMonitor_print(DebugMonitor* monitor, const char* format, ...);
void debugMonitor_load(DebugMonitor* monitor);
void debugMonitor_save(DebugMonitor* monitor);

#endif // DEBUG_MONITOR_H

