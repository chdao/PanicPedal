#include "EspNowTransport.h"
#include <esp_now.h>
#include <WiFi.h>
#include <string.h>
#include <Arduino.h>
#include "../shared/messages.h"

static ReceiverMessageCallback g_receiveCallback = nullptr;

void OnDataRecvWrapper(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (g_receiveCallback) {
    uint8_t* senderMAC = (uint8_t*)info->src_addr;
    uint8_t channel = info->rx_ctrl ? info->rx_ctrl->channel : 0;
    g_receiveCallback(senderMAC, data, len, channel);
  }
}

void receiverEspNowTransport_init(ReceiverEspNowTransport* transport) {
  WiFi.mode(WIFI_STA);
  delay(100);
  WiFi.disconnect();
  delay(100);
  
  if (esp_now_init() == ESP_OK) {
    transport->initialized = true;
  } else {
    transport->initialized = false;
  }
}

bool receiverEspNowTransport_send(ReceiverEspNowTransport* transport, const uint8_t* mac, const uint8_t* data, int len) {
  if (!transport->initialized) return false;
  
  // Ensure peer exists before sending
  esp_now_peer_info_t peerInfo;
  if (esp_now_get_peer(mac, &peerInfo) != ESP_OK) {
    receiverEspNowTransport_addPeer(transport, mac, 0);
  }
  
  esp_err_t result = esp_now_send(mac, data, len);
  return (result == ESP_OK);
}

bool receiverEspNowTransport_addPeer(ReceiverEspNowTransport* transport, const uint8_t* mac, uint8_t channel) {
  if (!transport->initialized) return false;
  
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = channel;
  peerInfo.encrypt = false;
  
  esp_err_t result = esp_now_add_peer(&peerInfo);
  return (result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST);
}

void receiverEspNowTransport_registerReceiveCallback(ReceiverEspNowTransport* transport, ReceiverMessageCallback callback) {
  if (!transport->initialized) return;
  
  g_receiveCallback = callback;
  esp_now_register_recv_cb(OnDataRecvWrapper);
}

void receiverEspNowTransport_broadcast(ReceiverEspNowTransport* transport, const uint8_t* data, int len) {
  uint8_t broadcastMAC[] = BROADCAST_MAC;
  receiverEspNowTransport_send(transport, broadcastMAC, data, len);
}

