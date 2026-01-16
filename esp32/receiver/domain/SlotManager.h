#ifndef SLOT_MANAGER_H
#define SLOT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "TransmitterManager.h"
#include "../shared/domain/PedalSlots.h"
#include "../shared/config.h"

// Slot availability check result
typedef struct {
  bool canFit;
  int currentSlotsUsed;
  int slotsNeeded;
  int slotsAfterChange;
} SlotAvailabilityResult;

// Check if a new transmitter can fit given current slot usage
bool slotManager_canFitNewTransmitter(const TransmitterManager* manager, int slotsNeeded);

// Check if an existing transmitter can change pedal mode
SlotAvailabilityResult slotManager_checkModeChange(const TransmitterManager* manager, 
                                                    int transmitterIndex, 
                                                    int newSlotsNeeded);

// Check if an existing transmitter can reconnect (become responsive)
SlotAvailabilityResult slotManager_checkReconnection(const TransmitterManager* manager,
                                                      int transmitterIndex,
                                                      int slotsNeeded);

// Get current slot usage (responsive transmitters only)
int slotManager_getCurrentSlotsUsed(const TransmitterManager* manager);

// Get available slots
int slotManager_getAvailableSlots(const TransmitterManager* manager);

// Check if all slots are full
bool slotManager_areAllSlotsFull(const TransmitterManager* manager);

#endif // SLOT_MANAGER_H
