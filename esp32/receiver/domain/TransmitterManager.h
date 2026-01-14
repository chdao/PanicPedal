#ifndef TRANSMITTER_MANAGER_H
#define TRANSMITTER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_PEDAL_SLOTS 2

typedef struct {
  uint8_t mac[6];
  uint8_t pedalMode;
  bool seenOnBoot;
  unsigned long lastSeen;
} TransmitterInfo;

typedef struct {
  TransmitterInfo transmitters[MAX_PEDAL_SLOTS];
  int count;
  int slotsUsed;
} TransmitterManager;

void transmitterManager_init(TransmitterManager* manager);
int transmitterManager_findIndex(const TransmitterManager* manager, const uint8_t* mac);
bool transmitterManager_add(TransmitterManager* manager, const uint8_t* mac, uint8_t pedalMode);
void transmitterManager_remove(TransmitterManager* manager, int index);
int transmitterManager_calculateSlotsUsed(const TransmitterManager* manager);  // Count only responsive transmitters
bool transmitterManager_hasFreeSlots(const TransmitterManager* manager, int slotsNeeded);
int transmitterManager_getAvailableSlots(const TransmitterManager* manager);
char transmitterManager_getAssignedKey(const TransmitterManager* manager, int index);

#endif // TRANSMITTER_MANAGER_H

