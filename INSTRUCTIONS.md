# Setup and Usage Instructions

## Setup Instructions

### 0. Uploading Code to FireBeetle 2 ESP32-E

**⚠️ IMPORTANT**: To upload sketches to the FireBeetle 2 ESP32-E, you must ground GPIO 0 (D5) during upload:

1. Connect GPIO 0 (D5) to GND using a jumper wire or button
2. Press and hold the RESET button
3. While holding RESET, release GPIO 0 from GND (if using a button, release the button)
4. Release RESET button
5. The board should now be in download mode
6. Upload your sketch from Arduino IDE
7. After upload completes, disconnect GPIO 0 from GND

**Alternative method**: Some FireBeetle boards have a BOOT button that does this automatically. If your board has a BOOT button, hold BOOT while pressing RESET, then release RESET first, then release BOOT.

### 1. Configure Transmitter

**FireBeetle 2**: Open `esp32/firebeetle2/firebeetle2.ino` and set the pedal mode:

```cpp
#define PEDAL_MODE 1  // 0 = DUAL_PEDAL, 1 = SINGLE_PEDAL
```

**PanicPedal Pro**: No configuration needed! The firmware automatically detects whether 1 or 2 pedals are connected using the NC (normally-closed) contacts. Detection runs on every boot.

**Note**: The receiver automatically assigns keys based on pairing order:
- First paired transmitter: LEFT pedal ('l')
- Second paired transmitter: RIGHT pedal ('r')
- For DUAL mode transmitters: '1' maps to 'l', '2' maps to 'r'

### 2. Configure Receiver

The receiver requires no configuration - it automatically discovers and pairs with transmitters!

**LED Status Indicator** (ESP32-S3-DevKitC-1-N16R8):
- **Blue LED**: Receiver is in grace period (first 30 seconds after boot)
- **LED Off**: Normal operation after grace period

### 3. Upload Code

1. Upload `esp32/receiver/receiver.ino` to your ESP32-S2/S3 receiver board
2. Upload `esp32/firebeetle2/firebeetle2.ino` to your ESP32 transmitter board(s)
3. No MAC address configuration needed - everything is automatic!

## Usage

### Initial Pairing

1. **Power on the receiver first** - The receiver will start broadcasting availability beacons
2. **Power on the transmitter(s)** - Transmitters will automatically discover the receiver
3. **Press a pedal** - When you press a pedal for the first time, the transmitter will automatically pair with the receiver
4. The receiver LED will be blue during the 30-second grace period (discovery window)

### Normal Operation

- **Press pedals to type keys** - Keys stay pressed until pedal is released
- **Both pedals can be pressed simultaneously** (for dual pedal mode)
- **Automatic reconnection** - If a transmitter reboots, it will automatically reconnect to its paired receiver
- **Slot management** - The receiver supports up to 2 pedal slots total (e.g., 2 single pedals or 1 dual pedal)

### Discovery and Pairing Process

**How it works:**
1. **Receiver broadcasts beacons** during the first 30 seconds after boot, announcing its MAC address and available slots
2. **Transmitter learns receiver MAC** from beacon messages (only stores MAC if receiver has free slots)
3. **Transmitter sends discovery request** when pedal is pressed (if not already paired)
4. **Receiver responds** with discovery response to complete pairing
5. **Transmitter broadcasts pairing** so other receivers know it's taken

**Automatic Reconnection:**
- When a transmitter boots, it broadcasts its MAC address (`MSG_TRANSMITTER_ONLINE`)
- If the receiver recognizes the transmitter (from previous pairing), it immediately sends an `MSG_ALIVE` message
- The transmitter can then automatically reconnect without needing to press a pedal

**Grace Period:**
- First 30 seconds after receiver boot
- Receiver broadcasts availability beacons every 2 seconds
- Receiver pings known transmitters that haven't been seen yet
- LED indicator shows blue during grace period

## Configuration Options

### Transmitter Settings

- `PEDAL_MODE`: Pedal configuration (0=DUAL, 1=SINGLE)
- `INACTIVITY_TIMEOUT`: Time before entering deep sleep (default: 10 minutes)
- `DEBOUNCE_DELAY`: Debounce delay in milliseconds (default: 20ms)
- `DEBUG_ENABLED`: Enable/disable Serial debug output (default: 0 for battery saving)
- `IDLE_DELAY_PAIRED`: Delay in loop when paired (default: 10ms for better responsiveness)
- `IDLE_DELAY_UNPAIRED`: Delay in loop when not paired (default: 200ms)

### Receiver Settings

- `MAX_PEDAL_SLOTS`: Maximum number of pedal slots (default: 2)
- `BEACON_INTERVAL`: Interval between beacon broadcasts during grace period (default: 2000ms)
- `TRANSMITTER_TIMEOUT`: Grace period duration (default: 30000ms = 30 seconds)

**Note**: Keys are automatically assigned by the receiver based on pairing order:
- First transmitter: LEFT pedal ('l')
- Second transmitter: RIGHT pedal ('r')

## Debug Monitor

Since the receiver uses USB HID Keyboard, Serial output is not available for debugging. The receiver includes a **debug monitor** feature that sends debug messages via ESP-NOW to a separate ESP32 device.

### Setup

1. **Upload debug monitor code** to a second ESP32 board:
   - Use `esp32/debug-monitor/debug-monitor.ino`
   - Any ESP32 board with Serial/USB support works (e.g., ESP32-S3-DevKitC-1)

2. **Power on the debug monitor** - It will automatically discover and pair with the receiver

3. **Open Serial Monitor** on the debug monitor board at 115200 baud to see debug messages

### How It Works

- The debug monitor sends a discovery request (`MSG_DEBUG_MONITOR_REQ`) via ESP-NOW broadcast
- The receiver automatically pairs with the debug monitor and saves its MAC address
- All `debugPrint()` messages from the receiver are sent to the debug monitor via ESP-NOW
- Messages are displayed on the Serial Monitor of the debug monitor device
- The receiver remembers the debug monitor across reboots

### Benefits

- **Real-time debugging** without interfering with USB HID Keyboard functionality
- **Wireless debugging** - no physical connection needed
- **Persistent pairing** - debug monitor reconnects automatically after receiver reboot
- **Timestamped messages** - all debug messages include timestamps (milliseconds since boot)

## Troubleshooting

### Transmitter not pairing
- **Power on receiver first** - Receiver must be broadcasting beacons
- **Check grace period** - Pairing happens automatically during the 30-second grace period after receiver boot
- **Check receiver slots** - Receiver may be full (2 slots already used)
- **Press pedal** - Transmitter pairs when pedal is pressed (if receiver discovered)
- **Enable debug** - Set `DEBUG_ENABLED 1` in transmitter to see pairing messages

### Keys not typing
- **Check pairing status** - Transmitter must be paired with receiver
- **Verify receiver is powered on** - Receiver must be running
- **Check slot availability** - Receiver may have reached maximum slots (2)
- **Ensure both devices are ESP32 variants** that support ESP-NOW

### Transmitter not reconnecting after reboot
- **Wait for grace period** - Receiver sends `MSG_ALIVE` during grace period
- **Check pairing persistence** - Receiver remembers paired transmitters across reboots
- **Enable debug** - Check Serial output to see if `MSG_TRANSMITTER_ONLINE` is being sent/received

### Serial Monitor not working on receiver
- This is expected when Keyboard is active on ESP32-S2/S3
- Keyboard functionality should still work
- **Use the debug monitor** - Upload `esp32/debug-monitor/debug-monitor.ino` to a second ESP32 board to receive debug messages wirelessly via ESP-NOW
- Debug messages are automatically sent to the paired debug monitor device

### Multiple key presses
- Adjust `DEBOUNCE_DELAY` if experiencing contact bounce
- Check pedal switch connections
- Default debounce delay is 20ms (optimized for most switches)
