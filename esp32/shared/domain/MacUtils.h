#ifndef MAC_UTILS_H
#define MAC_UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static inline bool macIsZero(const uint8_t* mac) {
  if (!mac) return true;
  for (int i = 0; i < 6; i++) {
    if (mac[i] != 0) return false;
  }
  return true;
}

static inline bool macIsBroadcast(const uint8_t* mac) {
  if (!mac) return false;
  for (int i = 0; i < 6; i++) {
    if (mac[i] != 0xFF) return false;
  }
  return true;
}

static inline bool isValidMAC(const uint8_t* mac) {
  return mac && !macIsZero(mac) && !macIsBroadcast(mac);
}

static inline bool macEqual(const uint8_t* mac1, const uint8_t* mac2) {
  if (!mac1 || !mac2) return false;
  return memcmp(mac1, mac2, 6) == 0;
}

static inline void macCopy(uint8_t* dest, const uint8_t* src) {
  if (dest && src) {
    memcpy(dest, src, 6);
  }
}

#endif // MAC_UTILS_H
