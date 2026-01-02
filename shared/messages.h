#ifndef MESSAGES_H
#define MESSAGES_H

#include <stdint.h>

// Message type definitions
#define MSG_PEDAL_EVENT    0x00
#define MSG_DISCOVERY_REQ  0x01
#define MSG_DISCOVERY_RESP 0x02
#define MSG_ALIVE          0x03
#define MSG_DEBUG          0x04
#define MSG_DEBUG_MONITOR_REQ 0x05
#define MSG_DELETE_RECORD  0x06
#define MSG_BEACON         0x07
#define MSG_TRANSMITTER_ONLINE 0x09
#define MSG_TRANSMITTER_PAIRED 0x0A

// Common message structure (must match between transmitter and receiver)
typedef struct __attribute__((packed)) struct_message {
  uint8_t msgType;
  char key;          // '1' for pin 13, '2' for pin 14
  bool pressed;
  uint8_t pedalMode; // 0=DUAL, 1=SINGLE
} struct_message;

// Beacon message structure
typedef struct __attribute__((packed)) beacon_message {
  uint8_t msgType;        // 0x07 = MSG_BEACON
  uint8_t receiverMAC[6];
  uint8_t availableSlots;
  uint8_t totalSlots;
} beacon_message;

// Transmitter online message structure
typedef struct __attribute__((packed)) transmitter_online_message {
  uint8_t msgType;        // 0x09 = MSG_TRANSMITTER_ONLINE
  uint8_t transmitterMAC[6];
} transmitter_online_message;

// Transmitter paired message structure
typedef struct __attribute__((packed)) transmitter_paired_message {
  uint8_t msgType;        // 0x0A = MSG_TRANSMITTER_PAIRED
  uint8_t transmitterMAC[6];
  uint8_t receiverMAC[6];
} transmitter_paired_message;

// Debug message structure
typedef struct __attribute__((packed)) debug_message {
  uint8_t msgType;   // 0x04 = MSG_DEBUG
  char message[200];
} debug_message;

// Broadcast MAC address
#define BROADCAST_MAC {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}

#endif // MESSAGES_H

