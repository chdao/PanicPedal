#ifndef MESSAGES_H
#define MESSAGES_H

#include <stdint.h>

// Device type definitions (identifies the sender)
#define DEVICE_TYPE_TRANSMITTER    0x01
#define DEVICE_TYPE_RECEIVER       0x02
#define DEVICE_TYPE_DEBUG_MONITOR  0x03

// Message type definitions
// Core functionality and pairing (0x00-0x0F)
#define MSG_PEDAL_EVENT        0x00
#define MSG_DISCOVERY_REQ      0x01
#define MSG_DISCOVERY_RESP     0x02
#define MSG_ALIVE              0x03
#define MSG_BEACON             0x04
#define MSG_ONLINE 0x05
#define MSG_TRANSMITTER_PAIRED 0x06
#define MSG_PAIRING_CONFIRMED  0x07
#define MSG_PAIRING_CONFIRMED_ACK 0x09
#define MSG_DELETE_RECORD      0x08

// Debug/monitoring (0x50-0x5F)
#define MSG_DEBUG              0x50
#define MSG_DEBUG_MONITOR_REQ  0x51

// Common message structure (must match between transmitter and receiver)
typedef struct __attribute__((packed)) struct_message {
  uint8_t deviceType;     // DEVICE_TYPE_TRANSMITTER, DEVICE_TYPE_RECEIVER, or DEVICE_TYPE_DEBUG_MONITOR
  uint8_t msgType;
  char key;          // '1' for pin 13, '2' for pin 14
  bool pressed;
  uint8_t pedalMode; // 0=DUAL, 1=SINGLE
} struct_message;

// Beacon message structure
typedef struct __attribute__((packed)) beacon_message {
  uint8_t deviceType;     // DEVICE_TYPE_RECEIVER
  uint8_t msgType;        // 0x04 = MSG_BEACON
  uint8_t receiverMAC[6];
  uint8_t availableSlots;
  uint8_t totalSlots;
} beacon_message;

// Online message structure (any device type can send this)
typedef struct __attribute__((packed)) online_message {
  uint8_t deviceType;     // DEVICE_TYPE_TRANSMITTER, DEVICE_TYPE_RECEIVER, or DEVICE_TYPE_DEBUG_MONITOR
  uint8_t msgType;        // 0x05 = MSG_ONLINE
  uint8_t deviceMAC[6];   // MAC address of the device that's online
} online_message;

// Transmitter paired message structure
typedef struct __attribute__((packed)) transmitter_paired_message {
  uint8_t deviceType;     // DEVICE_TYPE_TRANSMITTER
  uint8_t msgType;        // 0x06 = MSG_TRANSMITTER_PAIRED
  uint8_t transmitterMAC[6];
  uint8_t receiverMAC[6];
} transmitter_paired_message;

// Pairing confirmed message structure (receiver tells transmitter "you're paired with me")
typedef struct __attribute__((packed)) pairing_confirmed_message {
  uint8_t deviceType;     // DEVICE_TYPE_RECEIVER
  uint8_t msgType;        // 0x07 = MSG_PAIRING_CONFIRMED
  uint8_t receiverMAC[6];
} pairing_confirmed_message;

// Pairing confirmed acknowledgment message structure (transmitter acknowledges receiver's MSG_PAIRING_CONFIRMED)
typedef struct __attribute__((packed)) pairing_confirmed_ack_message {
  uint8_t deviceType;     // DEVICE_TYPE_TRANSMITTER or DEVICE_TYPE_RECEIVER (depending on sender)
  uint8_t msgType;        // 0x09 = MSG_PAIRING_CONFIRMED_ACK
  uint8_t receiverMAC[6];  // Echo receiver's MAC to confirm
} pairing_confirmed_ack_message;

// Debug message structure
typedef struct __attribute__((packed)) debug_message {
  uint8_t deviceType;     // DEVICE_TYPE_TRANSMITTER, DEVICE_TYPE_RECEIVER, or DEVICE_TYPE_DEBUG_MONITOR
  uint8_t msgType;        // 0x50 = MSG_DEBUG
  char message[200];
} debug_message;

// Debug monitor request message structure
typedef struct __attribute__((packed)) debug_monitor_req_message {
  uint8_t deviceType;     // DEVICE_TYPE_DEBUG_MONITOR (when responding) or DEVICE_TYPE_TRANSMITTER/RECEIVER (when requesting)
  uint8_t msgType;        // 0x51 = MSG_DEBUG_MONITOR_REQ
  uint8_t monitorMAC[6];
} debug_monitor_req_message;

// Broadcast MAC address
#define BROADCAST_MAC {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}

#endif // MESSAGES_H

