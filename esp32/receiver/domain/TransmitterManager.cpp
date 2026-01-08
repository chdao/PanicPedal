#include "TransmitterManager.h"
#include <string.h>
#include <Arduino.h>

void transmitterManager_init(TransmitterManager* manager) {
  memset(manager->transmitters, 0, sizeof(manager->transmitters));
  manager->count = 0;
  manager->slotsUsed = 0;
}

int transmitterManager_findIndex(const TransmitterManager* manager, const uint8_t* mac) {
  for (int i = 0; i < manager->count; i++) {
    if (memcmp(mac, manager->transmitters[i].mac, 6) == 0) {
      return i;
    }
  }
  return -1;
}

bool transmitterManager_add(TransmitterManager* manager, const uint8_t* mac, uint8_t pedalMode) {
  int index = transmitterManager_findIndex(manager, mac);
  if (index >= 0) {
    // Already exists - update last seen
    manager->transmitters[index].lastSeen = millis();
    manager->transmitters[index].seenOnBoot = true;
    return true;
  }
  
  int slotsNeeded = (pedalMode == 0) ? 2 : 1;
  if (manager->slotsUsed + slotsNeeded > MAX_PEDAL_SLOTS) {
    return false;  // Not enough slots
  }
  
  memcpy(manager->transmitters[manager->count].mac, mac, 6);
  manager->transmitters[manager->count].pedalMode = pedalMode;
  manager->transmitters[manager->count].seenOnBoot = true;
  manager->transmitters[manager->count].lastSeen = millis();
  manager->count++;
  manager->slotsUsed += slotsNeeded;
  
  return true;
}

void transmitterManager_remove(TransmitterManager* manager, int index) {
  if (index < 0 || index >= manager->count) return;
  
  int slotsFreed = (manager->transmitters[index].pedalMode == 0) ? 2 : 1;
  
  for (int i = index; i < manager->count - 1; i++) {
    manager->transmitters[i] = manager->transmitters[i + 1];
  }
  
  manager->count--;
  manager->slotsUsed -= slotsFreed;
  memset(&manager->transmitters[manager->count], 0, sizeof(TransmitterInfo));
}

bool transmitterManager_hasFreeSlots(const TransmitterManager* manager, int slotsNeeded) {
  return (manager->slotsUsed + slotsNeeded <= MAX_PEDAL_SLOTS);
}

int transmitterManager_getAvailableSlots(const TransmitterManager* manager) {
  return MAX_PEDAL_SLOTS - manager->slotsUsed;
}

char transmitterManager_getAssignedKey(const TransmitterManager* manager, int index) {
  return (index == 0) ? 'l' : 'r';
}

