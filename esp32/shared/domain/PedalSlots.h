#ifndef PEDAL_SLOTS_H
#define PEDAL_SLOTS_H

#include <stdint.h>

// 0 = DUAL (2 slots), 1 = SINGLE (1 slot)
static inline int getSlotsNeeded(uint8_t pedalMode) {
  return (pedalMode == 0) ? 2 : 1;
}

#endif // PEDAL_SLOTS_H
