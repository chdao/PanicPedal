# ESP-NOW Pedal Pairing State Machine

This document describes the pairing flow and state machine for the ESP-NOW pedal system.

## Overview

The system consists of:
- **Transmitters** (pedals): ESP32 devices that send pedal press/release events
- **Receiver**: ESP32 device that receives events and converts them to keyboard input

### Key Concepts

- **Known Transmitters**: Transmitters that were previously paired and stored in receiver's EEPROM
- **Currently Paired**: Transmitters that have `seenOnBoot = true` (responded after receiver boot)
- **MSG_PAIRING_CONFIRMED** (0x07): 
  - Sent by receiver to restore pairing with known transmitters (replaces MSG_ALIVE for known transmitters)
  - Sent by transmitter to saved receiver on deep sleep wake (requesting reconnection)
  - Bidirectional: receiver → transmitter (pairing confirmation), transmitter → receiver (reconnection request)
- **MSG_PAIRING_CONFIRMED_ACK** (0x09):
  - Sent by transmitter to acknowledge it received and accepted the receiver's `MSG_PAIRING_CONFIRMED`
  - Prevents message loops (different from MSG_PAIRING_CONFIRMED)
- **MSG_ALIVE**: Now only used to request discovery from unknown transmitters during grace period
- **MSG_TRANSMITTER_ONLINE**: Only sent when transmitter comes online (boot or reset), not as a response to `MSG_PAIRING_CONFIRMED`
- **Initial Ping Wait**: 1-second period after `MSG_PAIRING_CONFIRMED` is sent where receiver waits for transmitters to respond before starting grace period
- **LED States**:
  - **GREEN** (solid): During initial 1-second wait after ping sent
  - **BLUE** (breathing): During grace period (smoothly pulses from dim to bright in 2-second cycle)
  - **OFF**: Grace period ended or all slots filled

## Pairing Flow

### Transmitter States

```
[UNPAIRED] ──(Beacon/MSG_ALIVE)──> [DISCOVERED] ──(Discovery Request)──> [PAIRING] ──(Discovery Response)──> [PAIRED]
     │                                                                                                              │
     │                                                                                                              │
     └──────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
                                                          (Deep Sleep / Reset)
```

### Receiver States

```
[BOOT] ──> [GRACE_PERIOD] ──> [NORMAL_OPERATION]
   │            │                      │
   │            │                      │
   └────────────┴──────────────────────┘
         (Slots Full)
```

## Detailed State Transitions

### Transmitter: UNPAIRED State

**Entry Conditions:**
- Device boots without saved pairing
- Pairing cleared/reset

**Behaviors:**
- Broadcasts `MSG_TRANSMITTER_ONLINE` (only when no MAC saved, for discovery)
- Listens for `MSG_BEACON` or `MSG_ALIVE` from receivers
- Does not send pedal events

**Transitions:**
- `MSG_BEACON` received → Store receiver info → Move to DISCOVERED
- `MSG_ALIVE` received → Store receiver info → Move to DISCOVERED (from unknown receiver requesting discovery)

### Transmitter: DISCOVERED State

**Entry Conditions:**
- Receiver discovered via beacon or MSG_ALIVE
- Receiver has available slots

**Behaviors:**
- Stores receiver MAC and channel
- Waits for pedal press or automatic discovery trigger
- Can initiate pairing when pedal pressed (if slots available)

**Transitions:**
- Pedal pressed + slots available → Send `MSG_DISCOVERY_REQ` → Move to PAIRING
- `MSG_ALIVE` from discovered receiver → Defer discovery request to main loop → Move to PAIRING
- Timeout → Return to UNPAIRED

### Transmitter: PAIRING State

**Entry Conditions:**
- Discovery request sent
- Waiting for discovery response

**Behaviors:**
- Waits for `MSG_DISCOVERY_RESP`
- Tracks discovery timeout
- Can retry if timeout occurs

**Transitions:**
- `MSG_DISCOVERY_RESP` received → Send `MSG_TRANSMITTER_PAIRED` → Move to PAIRED
- Discovery timeout → Return to DISCOVERED or UNPAIRED
- `MSG_DELETE_RECORD` received → Return to UNPAIRED

### Transmitter: PAIRED State

**Entry Conditions:**
- Discovery response received
- Pairing confirmed

**Behaviors:**
- Sends `MSG_PEDAL_EVENT` on pedal press/release
- Responds to `MSG_ALIVE` from paired receiver by sending `MSG_TRANSMITTER_ONLINE` (deferred to main loop)
- Responds to `MSG_ALIVE` from different receiver by sending `MSG_DELETE_RECORD`
- Sends `MSG_DELETE_RECORD` to other receivers if they request pairing
- Can enter deep sleep (preserves pairing in NVS)
- On wake from deep sleep: Sends `MSG_PAIRING_CONFIRMED` directly to saved receiver (not broadcast)
- On boot/reset: Only broadcasts `MSG_TRANSMITTER_ONLINE` if no MAC saved (for discovery)
- If `MSG_PAIRING_CONFIRMED` sent and no `MSG_PAIRING_CONFIRMED_ACK` received within 1 second: Broadcasts `MSG_TRANSMITTER_ONLINE` (fallback to discovery)

**Transitions:**
- `MSG_PAIRING_CONFIRMED` received from paired receiver → Confirm pairing → Reply with `MSG_PAIRING_CONFIRMED_ACK`
- `MSG_PAIRING_CONFIRMED` received from different receiver → Send `MSG_DELETE_RECORD` → Return to UNPAIRED
- `MSG_PAIRING_CONFIRMED` received (not paired) → Restore pairing state immediately → Reply with `MSG_PAIRING_CONFIRMED_ACK`
- `MSG_PAIRING_CONFIRMED_ACK` received → Clear waiting flag → Restore pairing state if needed
- Deep sleep → Pairing saved to NVS
- Wake from deep sleep → Load pairing from NVS → Send `MSG_PAIRING_CONFIRMED` to saved receiver → Wait for ACK (1s timeout)
- If `MSG_PAIRING_CONFIRMED` timeout (no ACK within 1s) → Broadcast `MSG_TRANSMITTER_ONLINE` for discovery
- Reset → Clear pairing → Return to UNPAIRED → Broadcast `MSG_TRANSMITTER_ONLINE` (no MAC saved)

### Receiver: BOOT State

**Entry Conditions:**
- Device boots
- ESP-NOW initialized

**Behaviors:**
- Loads known transmitters from EEPROM
- Sends `MSG_PAIRING_CONFIRMED` to all previously known transmitters that are NOT currently paired (`seenOnBoot = false`)
- Records `initialPingTime` when ping is actually sent (after ESP-NOW initialization)
- This happens BEFORE grace period starts, giving known transmitters priority
- LED indicator: **GREEN** (solid) during initial wait period
- Waits `INITIAL_PING_WAIT_MS` (1 second) from `initialPingTime` before checking responses
- After wait: Checks if any known transmitters responded (by checking `seenOnBoot` flag)
  - If none responded: Logs "No known pedals replied to initial ping - preserving loaded transmitters"
  - If slots fill immediately: Bypasses grace period entirely

**Transitions:**
- After `INITIAL_PING_WAIT_MS` from `initialPingTime` → Check responses → Move to GRACE_PERIOD (if slots available)
- If all slots filled immediately → Bypass grace period → Move to NORMAL_OPERATION

### Receiver: GRACE_PERIOD State

**Entry Conditions:**
- Initial ping wait completed (`INITIAL_PING_WAIT_MS`)
- Slots not full (checked continuously)

**Behaviors:**
- Broadcasts `MSG_BEACON` periodically (only if slots available)
- Accepts discovery requests from known and unknown transmitters
- Sends `MSG_ALIVE` to unknown transmitters that send pedal events during grace period (to request discovery)
- LED indicator: **BLUE** (breathing/pulsing animation) - smoothly pulses from dim to bright in 2-second cycle
- Actively seeks to pair transmitters
- Continuously checks if all slots filled → Ends grace period early if so

**Transitions:**
- `TRANSMITTER_TIMEOUT_MS` elapsed → Move to NORMAL_OPERATION
  - If no pedals paired: Logs "Grace period ended: No pedals paired - preserving loaded transmitters"
  - If pedals paired: Logs "Grace period ended: X pedal(s) paired (Y/2 slots used)"
- All slots filled → End grace period immediately → Move to NORMAL_OPERATION
  - Logs "Grace period ended early: X pedal(s) paired (Y/2 slots used)"
- Discovery request received → Process → Stay in GRACE_PERIOD (or end if slots fill)

### Receiver: NORMAL_OPERATION State

**Entry Conditions:**
- Grace period ended OR slots full

**Behaviors:**
- Only accepts discovery from known transmitters
- Processes pedal events
- Handles `MSG_PAIRING_CONFIRMED` from transmitters (reconnection request after deep sleep):
  - If currently paired: Always responds with `MSG_PAIRING_CONFIRMED` (reconfirm pairing)
  - If not currently paired but slots available: Responds with `MSG_PAIRING_CONFIRMED`
  - If not currently paired and slots full: Does not respond
- Handles `MSG_PAIRING_CONFIRMED_ACK` from transmitters (acknowledgment that they received our pairing confirmation):
  - Marks transmitter as `seenOnBoot = true` when `MSG_PAIRING_CONFIRMED_ACK` is received
  - Updates `lastSeen` time
- Sends `MSG_PAIRING_CONFIRMED` to known transmitters that send `MSG_TRANSMITTER_ONLINE`:
  - If currently paired (`seenOnBoot = true`): Always sends `MSG_PAIRING_CONFIRMED` (reconfirm pairing)
  - If not currently paired but slots available: Sends `MSG_PAIRING_CONFIRMED`
  - If not currently paired and slots full: Does not respond
- Marks transmitters as `seenOnBoot = true` when they send pedal events
- LED indicator: **OFF**
- Can replace unresponsive transmitters if slots full

**Transitions:**
- `MSG_TRANSMITTER_ONLINE` from known transmitter (currently paired) → Always send `MSG_PAIRING_CONFIRMED` (reconfirm pairing)
- `MSG_TRANSMITTER_ONLINE` from known transmitter (not currently paired) → Check slots → Send `MSG_PAIRING_CONFIRMED` if available
- `MSG_TRANSMITTER_ONLINE` from unknown → Check slots → Send `MSG_ALIVE` to request discovery if available
- `MSG_PAIRING_CONFIRMED` from known transmitter → Check slots → Send `MSG_PAIRING_CONFIRMED` back if available
- `MSG_PAIRING_CONFIRMED_ACK` from known transmitter → Mark as `seenOnBoot = true` (acknowledgment received)
- `MSG_PEDAL_EVENT` from known transmitter → Mark as `seenOnBoot = true`
- `MSG_DELETE_RECORD` received → Remove transmitter from list

## Message Flow Diagrams

### Initial Pairing

```
Transmitter                    Receiver
     │                             │
     │── MSG_TRANSMITTER_ONLINE ──>│
     │                             │
     │<── MSG_BEACON ──────────────│
     │                             │
     │── MSG_DISCOVERY_REQ ───────>│
     │                             │
     │<── MSG_DISCOVERY_RESP ──────│
     │                             │
     │── MSG_TRANSMITTER_PAIRED ──>│
     │                             │
```

### Reconnection After Deep Sleep

```
Transmitter                    Receiver
     │                             │
     │── MSG_PAIRING_CONFIRMED ───>│ (on wake from deep sleep, to saved receiver)
     │── [waiting for ACK, 1s timeout]
     │                             │
     │<── MSG_PAIRING_CONFIRMED_ACK│ (receiver acknowledges and confirms pairing)
     │                             │
     │── Pairing restored          │
     │── MSG_PEDAL_EVENT ──────────>│ (immediately if pedal pressed)
     │                             │
```

**Alternative Flow (Timeout):**
```
Transmitter                    Receiver
     │                             │
     │── MSG_PAIRING_CONFIRMED ───>│ (on wake from deep sleep, to saved receiver)
     │── [waiting for ACK, 1s timeout]
     │                             │
     │── [No ACK received]         │
     │── MSG_TRANSMITTER_ONLINE ──>│ (broadcast, fallback to discovery)
     │                             │
```

**Note:** 
- Transmitter sends `MSG_PAIRING_CONFIRMED` directly to saved receiver (not broadcast). 
- If receiver has slots available or transmitter is currently paired, receiver responds with `MSG_PAIRING_CONFIRMED_ACK` within 1 second.
- Transmitter restores pairing state when receiving `MSG_PAIRING_CONFIRMED_ACK` and clears waiting flag.
- **If no ACK received within 1 second**: Transmitter broadcasts `MSG_TRANSMITTER_ONLINE` as fallback to discovery mode.
- If no MAC saved on wake: Transmitter broadcasts `MSG_TRANSMITTER_ONLINE` immediately for discovery.

### Receiver Boot - Known Transmitter Reconnection

```
Receiver                       Transmitter
     │                             │
     │── MSG_PAIRING_CONFIRMED ───>│ (on boot, after ESP-NOW init, to known transmitters)
     │                             │
     │                             │── Restores pairing state
     │                             │── Saves to NVS
     │                             │
     │<── MSG_PAIRING_CONFIRMED_ACK│ (transmitter acknowledges)
     │                             │
     │── Marks as seenOnBoot=true  │
     │── Checks responses after 1s │
     │── Ends grace period early   │ (if all slots filled)
     │                             │
```

**Timing:**
- Receiver sends `MSG_PAIRING_CONFIRMED` after ESP-NOW initialization (~2.8s after boot)
- Records `initialPingTime` when ping is sent
- Waits 1 second from `initialPingTime` before checking responses
- LED: **GREEN** (solid) during this 1-second wait

**Note:** Transmitter replies with `MSG_PAIRING_CONFIRMED_ACK` (not `MSG_PAIRING_CONFIRMED`) to acknowledge receipt.

### Grace Period Discovery (Unknown Transmitter)

```
Transmitter                    Receiver
     │                             │
     │── MSG_PEDAL_EVENT ──────────>│ (unknown transmitter)
     │                             │
     │<── MSG_ALIVE ────────────────│ (receiver requests discovery)
     │                             │
     │── MSG_DISCOVERY_REQ ───────>│
     │                             │
     │<── MSG_DISCOVERY_RESP ──────│
     │                             │
     │── MSG_TRANSMITTER_PAIRED ──>│
     │                             │
```

### Known Transmitter Reconnection During Grace Period

```
Receiver                       Transmitter
     │                             │
     │── MSG_PAIRING_CONFIRMED ───>│ (on boot, before grace period)
     │                             │
     │                             │── Restores pairing state
     │                             │── Saves to NVS
     │                             │
     │<── MSG_PAIRING_CONFIRMED_ACK│ (transmitter acknowledges)
     │                             │
     │── Marks as seenOnBoot=true  │
     │── Checks if slots full      │
     │── Ends grace period early   │ (if all slots filled)
     │                             │
```

**Note:** Transmitter replies with `MSG_PAIRING_CONFIRMED_ACK` (not `MSG_PAIRING_CONFIRMED`). Transmitter can also respond with `MSG_PEDAL_EVENT`, which also marks it as `seenOnBoot = true`.

## Slot Management

### Slot Allocation Rules

1. **Single Pedal**: Uses 1 slot
2. **Dual Pedal**: Uses 2 slots
3. **Maximum Slots**: 2 total slots per receiver

### Slot Assignment Logic

- First pedal always gets slot 0 (left pedal, key 'l')
- Second pedal gets slot 1 (right pedal, key 'r')
- Slots can be empty (gaps allowed)
- New transmitters fill empty slots starting from index 0

### Slot Availability Checks

The receiver checks slot availability in these scenarios:

1. **New Transmitter Discovery**:
   - `currentSlots + slotsNeeded <= MAX_PEDAL_SLOTS`

2. **Existing Transmitter Reconnection**:
   - If currently paired: Always allowed (reclaiming own slots)
   - If not currently paired: `currentSlots + slotsNeeded <= MAX_PEDAL_SLOTS`

3. **Pedal Mode Change**:
   - `currentSlots - oldSlots + newSlots <= MAX_PEDAL_SLOTS`

## Error Handling

### Discovery Timeout
- Transmitter waits `DISCOVERY_TIMEOUT_MS` for response
- On timeout, can retry or return to DISCOVERED state

### Slot Full
- Receiver rejects discovery requests if slots full
- Exception: Known transmitters reclaiming their slots (always allowed)
- Receiver does not respond to `MSG_PAIRING_CONFIRMED` or `MSG_TRANSMITTER_ONLINE` from known transmitters if slots full and transmitter is not currently paired

### Different Receiver Pairing Attempt
- If transmitter receives `MSG_PAIRING_CONFIRMED` from a different receiver while already paired:
  - Transmitter sends `MSG_DELETE_RECORD` to that receiver
  - Transmitter does not send `MSG_PAIRING_CONFIRMED_ACK` (rejects pairing)

### Channel Mismatch
- ESP-NOW handles channel automatically
- Peers added with channel 0 use current WiFi channel

## Deep Sleep Behavior

### Transmitter Deep Sleep
- Enters deep sleep after `INACTIVITY_TIMEOUT_MS` of no activity
- Wakes on GPIO1 or GPIO2 LOW (pedal press)
- Preserves pairing in NVS across sleep cycles
- Clears pairing on full reset (not deep sleep)

### Wake from Deep Sleep
- Loads paired receiver MAC from NVS
- If MAC saved: Sends `MSG_PAIRING_CONFIRMED` directly to saved receiver (not broadcast) - requests reconnection
- Records send time and sets waiting flag for `MSG_PAIRING_CONFIRMED_ACK`
- If pedal pressed on wake, sends pedal event immediately (after pairing is restored)
- Receiver responds with `MSG_PAIRING_CONFIRMED_ACK` if:
  - Transmitter is currently paired (always responds to reconfirm)
  - Transmitter is not currently paired but slots are available
- Transmitter restores pairing state when receiving `MSG_PAIRING_CONFIRMED_ACK` (clears waiting flag, no further message sent)
- **If no `MSG_PAIRING_CONFIRMED_ACK` received within 1 second**: Broadcasts `MSG_TRANSMITTER_ONLINE` (fallback to discovery mode)
- If receiver doesn't respond (slots full and not currently paired), transmitter broadcasts `MSG_TRANSMITTER_ONLINE` after timeout
- If transmitter receives `MSG_PAIRING_CONFIRMED` from different receiver, sends `MSG_DELETE_RECORD` to that receiver
- If no MAC saved: Broadcasts `MSG_TRANSMITTER_ONLINE` immediately (for discovery)

## Configuration

See `esp32/shared/config.h` for all timing constants and configuration values.

## Debugging

Enable debug mode to see state transitions:
- Transmitter: Press GPIO10 debug button
- Receiver: Always enabled
- Debug Monitor: Connects via ESP-NOW to view all messages
