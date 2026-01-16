#include "TransmitterManager.h"
#include <string.h>
#include <Arduino.h>

void transmitterManager_init(TransmitterManager* manager) {
  memset(manager->transmitters, 0, sizeof(manager->transmitters));
  manager->count = 0;
  manager->slotsUsed = 0;
}

int transmitterManager_findIndex(const TransmitterManager* manager, const uint8_t* mac) {
  // Search all slots (not just up to count) since we now allow gaps
  for (int i = 0; i < MAX_PEDAL_SLOTS; i++) {
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
  
  // Find first empty slot (starting from index 0) to ensure first pedal gets slot 0 (pedal 1)
  int emptyIndex = -1;
  for (int i = 0; i < MAX_PEDAL_SLOTS; i++) {
    // Check if slot is empty (all zeros in MAC means empty)
    bool isEmpty = true;
    for (int j = 0; j < 6; j++) {
      if (manager->transmitters[i].mac[j] != 0) {
        isEmpty = false;
        break;
      }
    }
    if (isEmpty) {
      emptyIndex = i;
      break;
    }
  }
  
  // If no empty slot found, append at the end
  if (emptyIndex < 0) {
    emptyIndex = manager->count;
  }
  
  memcpy(manager->transmitters[emptyIndex].mac, mac, 6);
  manager->transmitters[emptyIndex].pedalMode = pedalMode;
  manager->transmitters[emptyIndex].seenOnBoot = true;
  manager->transmitters[emptyIndex].lastSeen = millis();
  
  // Update count if we added beyond current count
  if (emptyIndex >= manager->count) {
    manager->count = emptyIndex + 1;
  }
  
  manager->slotsUsed += slotsNeeded;
  
  return true;
}

void transmitterManager_remove(TransmitterManager* manager, int index) {
  if (index < 0 || index >= MAX_PEDAL_SLOTS) return;
  
  // Check if slot is actually occupied
  bool isEmpty = true;
  for (int j = 0; j < 6; j++) {
    if (manager->transmitters[index].mac[j] != 0) {
      isEmpty = false;
      break;
    }
  }
  if (isEmpty) return;  // Already empty
  
  int slotsFreed = (manager->transmitters[index].pedalMode == 0) ? 2 : 1;
  
  // Clear the slot instead of shifting - this allows new transmitters to fill empty slots
  // starting from index 0, ensuring first pedal always gets slot 0 (pedal 1)
  memset(&manager->transmitters[index], 0, sizeof(TransmitterInfo));
  
  manager->slotsUsed -= slotsFreed;
  
  // Update count - find the highest occupied index
  manager->count = 0;
  for (int i = MAX_PEDAL_SLOTS - 1; i >= 0; i--) {
    bool slotOccupied = false;
    for (int j = 0; j < 6; j++) {
      if (manager->transmitters[i].mac[j] != 0) {
        slotOccupied = true;
        break;
      }
    }
    if (slotOccupied) {
      manager->count = i + 1;
      break;
    }
  }
}

// Calculate slots used by only counting transmitters that have responded (seenOnBoot == true)
int transmitterManager_calculateSlotsUsed(const TransmitterManager* manager) {
  int slots = 0;
  for (int i = 0; i < MAX_PEDAL_SLOTS; i++) {
    // Check if slot is occupied
    bool slotOccupied = false;
    for (int j = 0; j < 6; j++) {
      if (manager->transmitters[i].mac[j] != 0) {
        slotOccupied = true;
        break;
      }
    }
    // Only count slots occupied by transmitters that have responded
    if (slotOccupied && manager->transmitters[i].seenOnBoot) {
      int slotsForThis = (manager->transmitters[i].pedalMode == 0) ? 2 : 1;
      slots += slotsForThis;
    }
  }
  return slots;
}

// Calculate slots reserved by ALL loaded transmitters (including unresponsive ones)
// This is used to determine if grace period should be bypassed
int transmitterManager_calculateReservedSlots(const TransmitterManager* manager) {
  int slots = 0;
  for (int i = 0; i < MAX_PEDAL_SLOTS; i++) {
    // Check if slot is occupied
    bool slotOccupied = false;
    for (int j = 0; j < 6; j++) {
      if (manager->transmitters[i].mac[j] != 0) {
        slotOccupied = true;
        break;
      }
    }
    // Count ALL loaded transmitters (responsive or not) - they reserve slots
    if (slotOccupied) {
      int slotsForThis = (manager->transmitters[i].pedalMode == 0) ? 2 : 1;
      slots += slotsForThis;
    }
  }
  return slots;
}

bool transmitterManager_hasFreeSlots(const TransmitterManager* manager, int slotsNeeded) {
  // Use calculated slots (only responsive transmitters) instead of stored slotsUsed
  int currentSlots = transmitterManager_calculateSlotsUsed(manager);
  return (currentSlots + slotsNeeded <= MAX_PEDAL_SLOTS);
}

int transmitterManager_getAvailableSlots(const TransmitterManager* manager) {
  // Use calculated slots (only responsive transmitters) instead of stored slotsUsed
  int currentSlots = transmitterManager_calculateSlotsUsed(manager);
  return MAX_PEDAL_SLOTS - currentSlots;
}

char transmitterManager_getAssignedKey(const TransmitterManager* manager, int index) {
  return (index == 0) ? 'l' : 'r';
}

