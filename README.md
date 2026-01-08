# ESPNow Perfectly Adequate Arcade Pedal

A wireless pedal system using ESP-NOW for low-latency communication between pedal transmitters and a USB HID keyboard receiver. Perfectly adequate for arcade gaming!

**Original Design**: This project is based on the [Perfectly Adequate Arcade Pedal (Wireless)](https://www.printables.com/model/1220746-perfectly-adequate-arcade-pedal-wireless) design on Printables. All initial ideas and the pedal model come from that design.

**⚠️ IMPORTANT CORRECTION**: The original schematic shows TP4056 OUT+ connected to the 3V3 pin. This is **INCORRECT** and can damage the ESP32. The TP4056 OUT+ must connect to **VCC** instead. See the [Wiring Diagram](#wiring-diagram) section below for the correct connections.

## Overview

This project consists of:
- **Transmitters**: ESP32-based pedal devices that send key press/release events via ESP-NOW
  - **FireBeetle 2**: `esp32/firebeetle2/firebeetle2.ino` - Configurable single or dual pedal mode
  - **PanicPedal Pro**: `esp32/panicpedal-pro/panicpedal-pro.ino` - Custom PCB with automatic pedal detection (1 or 2 pedals)
- **Receiver**: ESP32-S2/S3 device that receives ESP-NOW messages and types keys via USB HID Keyboard (`esp32/receiver/receiver.ino`)

**Note**: `espnow-pedal.cpp` is included as a reference file showing the original implementation. The actual project files are in the `esp32/` directory.

## Features

- **Low latency**: ESP-NOW provides fast, direct communication without WiFi connection
- **Multiple pedal modes**: Supports single pedal (LEFT or RIGHT) or dual pedal configurations
- **Automatic pedal detection** (PanicPedal Pro): Automatically detects whether 1 or 2 pedals are connected using NC (normally-closed) contacts - no manual configuration needed
- **Press and hold**: Keys stay pressed until pedal is released
- **Independent operation**: Both pedals can be pressed simultaneously
- **Battery efficient**: Includes inactivity timeout and deep sleep support
- **Automatic discovery**: No manual MAC address configuration needed - transmitters automatically discover receivers
- **Automatic reconnection**: Transmitters automatically reconnect to receivers after reboot
- **Slot management**: Receiver tracks available slots and only accepts transmitters when slots are available
- **Grace period**: 30-second discovery period after receiver boot for initial pairing and verification

## Hardware Requirements

### Recommended Boards

**Transmitters:**
- [FireBeetle 2 ESP32-E](https://www.dfrobot.com/product-2195.html) - Optimized for low power consumption, ideal for battery-powered pedal transmitters
- **PanicPedal Pro** - Custom PCB with ESP32-S3-WROOM featuring automatic pedal detection (see `esp32/panicpedal-pro/README.md` for details)

**Receiver:**
- [ESP32-S3-DevKitC-1-N16R8](https://www.amazon.com/dp/B0CC8NYXRG) - ESP32-S3 board with native USB support for HID Keyboard functionality

### Transmitter
- ESP32 (tested with FireBeetle 2 ESP32-E and ESP32-S3-WROOM for PanicPedal Pro)
- Pedal switches (normally-open, connected to GPIO with pull-up)
- Optional: LED for status indication
- **PanicPedal Pro**: Uses NC (normally-closed) contacts for automatic detection of connected pedals

### Receiver
- ESP32-S2 or ESP32-S3 (for USB HID Keyboard support)
- USB connection to computer

## Pin Configuration

### Transmitter
- **Single Pedal Mode**: Connect pedal switch to GPIO 13
- **Dual Pedal Mode**: 
  - LEFT pedal: GPIO 13
  - RIGHT pedal: GPIO 14
- **LED**: GPIO 2 (optional)

**Pinout Reference**: See [FireBeetle 2 ESP32-E Pinout](docs/FireBeetle2_ESP32-E_Pinout.md) for complete pin details.

### Receiver
- Uses USB for communication (no GPIO pins needed for functionality)

## Wiring Diagram

### Transmitter Power Wiring (FireBeetle 2 ESP32-E)

**⚠️ IMPORTANT**: The TP4056 charging board OUT+ must connect to **VCC**, NOT 3V3!

```
TP4056 Charging Board          FireBeetle 2 ESP32-E
─────────────────────          ─────────────────────
OUT+ (red wire)    ──────────> VCC (or PH2.0 battery connector)
OUT- (black wire)  ──────────> GND
B+                 ──────────> 18650 Battery +
B-                 ──────────> 18650 Battery -
IN+                ──────────> Power Switch
IN-                ──────────> GND
```

**Why VCC and not 3V3?**
- **VCC** is the input voltage pin that accepts battery voltage (3.7-4.2V) or USB voltage (~4.7V)
- **3V3** is a regulated 3.3V OUTPUT pin - connecting 4-5V to it can damage the ESP32
- The FireBeetle's onboard regulator converts VCC to 3.3V internally

### Transmitter Signal Wiring

```
Left Pedal Switch (NO)  ───> GPIO 13
Left Pedal Switch (COM) ───> GND

Right Pedal Switch (NO)  ───> GPIO 14 (dual mode only)
Right Pedal Switch (COM) ───> GND

LED (anode)            ───> GPIO 2 (optional)
LED (cathode)          ───> GND
```

**Note**: The original schematic may show a "Mode Select Switch" on GPIO 26 for selecting between Bluetooth and ESP-NOW modes. This code uses ESP-NOW only, so that switch is not needed. Pedal mode (single/dual) is configured via the `PEDAL_MODE` define in the code.

**Note**: Switches should be normally-open (NO) type. The ESP32's internal pull-up resistors keep the pins HIGH when switches are open, and LOW when pressed.

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

Open `esp32/firebeetle2/firebeetle2.ino` and set the pedal mode:

```cpp
#define PEDAL_MODE 1  // 0 = DUAL_PEDAL, 1 = SINGLE_PEDAL
```

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
   - Use `debug-monitor/debug-monitor.ino`
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

## Known Limitations

- **Serial Output**: On ESP32-S2/S3, Serial output may not be available when USB HID Keyboard is active. This is a hardware limitation, but keyboard functionality works correctly. Use the debug monitor (ESP-NOW) for debugging instead.
- **USB Composite**: Not all ESP32-S2/S3 boards support USB composite mode (CDC + HID simultaneously).

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
- **Use the debug monitor** - Upload `debug-monitor/debug-monitor.ino` to a second ESP32 board to receive debug messages wirelessly via ESP-NOW
- Debug messages are automatically sent to the paired debug monitor device

### Multiple key presses
- Adjust `DEBOUNCE_DELAY` if experiencing contact bounce
- Check pedal switch connections
- Default debounce delay is 20ms (optimized for most switches)

## License

This project is provided as-is for educational and personal use.

## Contributing

Feel free to submit issues or pull requests for improvements!

