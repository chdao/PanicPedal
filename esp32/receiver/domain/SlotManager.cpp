#include "SlotManager.h"
#include "../shared/domain/PedalSlots.h"
#include "../shared/config.h"

bool slotManager_canFitNewTransmitter(const TransmitterManager* manager, int slotsNeeded) {
  int currentSlots = transmitterManager_calculateSlotsUsed(manager);
  return (currentSlots + slotsNeeded <= MAX_PEDAL_SLOTS);
}

SlotAvailabilityResult slotManager_checkModeChange(const TransmitterManager* manager, 
                                                    int transmitterIndex, 
                                                    int newSlotsNeeded) {
  SlotAvailabilityResult result = {false, 0, newSlotsNeeded, 0};
  
  if (transmitterIndex < 0 || transmitterIndex >= MAX_PEDAL_SLOTS) {
    return result;
  }
  
  result.currentSlotsUsed = transmitterManager_calculateSlotsUsed(manager);
  int oldSlotsNeeded = getSlotsNeeded(manager->transmitters[transmitterIndex].pedalMode);
  
  if (newSlotsNeeded == oldSlotsNeeded) {
    // No change - always fits
    result.canFit = true;
    result.slotsAfterChange = result.currentSlotsUsed;
    return result;
  }
  
  // Calculate slots after mode change
  result.slotsAfterChange = result.currentSlotsUsed - oldSlotsNeeded + newSlotsNeeded;
  result.canFit = (result.slotsAfterChange <= MAX_PEDAL_SLOTS);
  
  return result;
}

SlotAvailabilityResult slotManager_checkReconnection(const TransmitterManager* manager,
                                                      int transmitterIndex,
                                                      int slotsNeeded) {
  SlotAvailabilityResult result = {false, 0, slotsNeeded, 0};
  
  if (transmitterIndex < 0 || transmitterIndex >= MAX_PEDAL_SLOTS) {
    return result;
  }
  
  bool wasAlreadyResponsive = manager->transmitters[transmitterIndex].seenOnBoot;
  result.currentSlotsUsed = transmitterManager_calculateSlotsUsed(manager);
  
  if (wasAlreadyResponsive) {
    // Already responsive - no change in slot usage
    result.canFit = true;
    result.slotsAfterChange = result.currentSlotsUsed;
  } else {
    // Becoming responsive - add its slots
    result.slotsAfterChange = result.currentSlotsUsed + slotsNeeded;
    result.canFit = (result.slotsAfterChange <= MAX_PEDAL_SLOTS);
  }
  
  return result;
}

int slotManager_getCurrentSlotsUsed(const TransmitterManager* manager) {
  return transmitterManager_calculateSlotsUsed(manager);
}

int slotManager_getAvailableSlots(const TransmitterManager* manager) {
  return MAX_PEDAL_SLOTS - slotManager_getCurrentSlotsUsed(manager);
}

bool slotManager_areAllSlotsFull(const TransmitterManager* manager) {
  return slotManager_getCurrentSlotsUsed(manager) >= MAX_PEDAL_SLOTS;
}
