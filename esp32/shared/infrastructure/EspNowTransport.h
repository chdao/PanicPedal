#ifndef ESPNOW_TRANSPORT_H
#define ESPNOW_TRANSPORT_H

#include <stdint.h>
#include <stdbool.h>

// ESP-NOW transport abstraction
typedef struct {
  bool initialized;
} EspNowTransport;

typedef void (*MessageReceivedCallback)(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);

void espNowTransport_init(EspNowTransport* transport);
bool espNowTransport_send(EspNowTransport* transport, const uint8_t* mac, const uint8_t* data, int len);
bool espNowTransport_addPeer(EspNowTransport* transport, const uint8_t* mac, uint8_t channel);
void espNowTransport_registerReceiveCallback(EspNowTransport* transport, MessageReceivedCallback callback);
void espNowTransport_broadcast(EspNowTransport* transport, const uint8_t* data, int len);

#endif // ESPNOW_TRANSPORT_H
