#include "EspNowTransport.h"
#include <esp_now.h>
#include <WiFi.h>
#include <string.h>
#include <Arduino.h>
#include "../messages.h"

static MessageReceivedCallback g_receiveCallback = nullptr;

void OnDataRecvWrapper(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (g_receiveCallback) {
    uint8_t* senderMAC = (uint8_t*)info->src_addr;
    uint8_t channel = info->rx_ctrl ? info->rx_ctrl->channel : 0;
    g_receiveCallback(senderMAC, data, len, channel);
  }
}

void espNowTransport_init(EspNowTransport* transport) {
  // ESP-NOW requires WiFi to be initialized in STA mode (but not connected)
  // ESP-NOW uses the WiFi radio hardware but operates independently
  WiFi.mode(WIFI_STA);
  
  if (esp_now_init() == ESP_OK) {
    transport->initialized = true;
  } else {
    transport->initialized = false;
  }
}

bool espNowTransport_send(EspNowTransport* transport, const uint8_t* mac, const uint8_t* data, int len) {
  if (!transport->initialized) return false;
  
  // Ensure peer is added before sending (ESP-NOW requires peer to be added)
  esp_now_peer_info_t peerInfo;
  esp_err_t getPeerResult = esp_now_get_peer(mac, &peerInfo);
  
  if (getPeerResult != ESP_OK) {
    // Peer doesn't exist - add it with channel 0 (ESP-NOW uses current WiFi channel)
    espNowTransport_addPeer(transport, mac, 0);
    // Small delay for peer to be ready (ESP32-S3 needs this)
    delay(2);
    // Verify peer was added
    getPeerResult = esp_now_get_peer(mac, &peerInfo);
    if (getPeerResult != ESP_OK) {
      return false;  // Failed to add peer
    }
  }
  
  // Try sending - esp_now_send is asynchronous, returns ESP_OK if queued successfully
  esp_err_t result = esp_now_send(mac, data, len);
  
  // Note: esp_now_send returns ESP_OK if the packet was queued successfully
  // Actual delivery is asynchronous and may fail, but we can't detect that here
  return (result == ESP_OK);
}

bool espNowTransport_addPeer(EspNowTransport* transport, const uint8_t* mac, uint8_t channel) {
  if (!transport->initialized) return false;
  
  // Use channel 0 if provided channel is 0 (ESP-NOW will use current channel)
  // Otherwise use the provided channel
  uint8_t actualChannel = channel;
  
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = actualChannel;
  peerInfo.encrypt = false;
  
  esp_err_t result = esp_now_add_peer(&peerInfo);
  return (result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST);
}

void espNowTransport_registerReceiveCallback(EspNowTransport* transport, MessageReceivedCallback callback) {
  if (!transport->initialized) return;
  
  g_receiveCallback = callback;
  esp_now_register_recv_cb(OnDataRecvWrapper);
}

void espNowTransport_broadcast(EspNowTransport* transport, const uint8_t* data, int len) {
  if (!transport->initialized) return;
  
  uint8_t broadcastMAC[] = BROADCAST_MAC;
  
  // Check if broadcast peer exists, add if not
  esp_now_peer_info_t peerInfo;
  if (esp_now_get_peer(broadcastMAC, &peerInfo) != ESP_OK) {
    // Broadcast peer doesn't exist - add it (channel 0 = use current channel)
    espNowTransport_addPeer(transport, broadcastMAC, 0);
  }
  
  // Send broadcast message
  espNowTransport_send(transport, broadcastMAC, data, len);
}
