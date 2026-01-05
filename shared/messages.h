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
#define MSG_DEBUG_MONITOR_BEACON 0x08  // Debug monitor broadcasts its MAC
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

// Debug monitor beacon message structure (monitor broadcasts its MAC)
typedef struct __attribute__((packed)) debug_monitor_beacon_message {
  uint8_t msgType;        // 0x08 = MSG_DEBUG_MONITOR_BEACON
  uint8_t monitorMAC[6];
} debug_monitor_beacon_message;

// Broadcast MAC address
#define BROADCAST_MAC {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}

// MAC address validation utility (optimized for 6-byte MACs)
static inline bool isValidMAC(const uint8_t* mac) {
  if (!mac) return false;
  
  // Fast check: use 32-bit and 16-bit comparisons for 6 bytes
  // Check first 4 bytes as uint32_t, last 2 bytes as uint16_t
  const uint32_t* mac32 = (const uint32_t*)mac;
  const uint16_t* mac16 = (const uint16_t*)(mac + 4);
  
  // Check if all zeros (invalid) - compare first 4 bytes and last 2 bytes
  if (*mac32 == 0 && *mac16 == 0) return false;
  
  // Broadcast is valid (0xFFFFFFFF for first 4, 0xFFFF for last 2)
  return true;
}

// Fast MAC address comparison (6 bytes) - faster than memcmp
static inline bool macEqual(const uint8_t* mac1, const uint8_t* mac2) {
  if (!mac1 || !mac2) return false;
  const uint32_t* m1_32 = (const uint32_t*)mac1;
  const uint32_t* m2_32 = (const uint32_t*)mac2;
  const uint16_t* m1_16 = (const uint16_t*)(mac1 + 4);
  const uint16_t* m2_16 = (const uint16_t*)(mac2 + 4);
  return (*m1_32 == *m2_32 && *m1_16 == *m2_16);
}

// Fast MAC address copy (6 bytes) - faster than memcpy for fixed size
static inline void macCopy(uint8_t* dst, const uint8_t* src) {
  if (!dst || !src) return;
  uint32_t* dst32 = (uint32_t*)dst;
  const uint32_t* src32 = (const uint32_t*)src;
  uint16_t* dst16 = (uint16_t*)(dst + 4);
  const uint16_t* src16 = (const uint16_t*)(src + 4);
  *dst32 = *src32;
  *dst16 = *src16;
}

// Optimized slot calculation - inline for speed (shared utility)
static inline int getSlotsNeeded(uint8_t pedalMode) {
  return (pedalMode == 0) ? 2 : 1;
}

#endif // MESSAGES_H
