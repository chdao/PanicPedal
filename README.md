# ESPNow Perfectly Adequate Arcade Pedal

A wireless pedal system using ESP-NOW for low-latency communication between pedal transmitters and a USB HID keyboard receiver. Perfectly adequate for arcade gaming!

**Original Design**: This project is based on the [Perfectly Adequate Arcade Pedal (Wireless)](https://www.printables.com/model/1220746-perfectly-adequate-arcade-pedal-wireless) design on Printables. All initial ideas and the pedal model come from that design.

**⚠️ IMPORTANT CORRECTION**: The original schematic shows TP4056 OUT+ connected to the 3V3 pin. This is **INCORRECT** and can damage the ESP32. The TP4056 OUT+ must connect to **VCC** instead. See the [Wiring Diagram](#wiring-diagram) section below for the correct connections.

## Overview

This project consists of:
- **Transmitter**: ESP32-based pedal device that sends key press/release events via ESP-NOW (`transmitter/transmitter.ino`)
- **Receiver**: ESP32-S2/S3 device that receives ESP-NOW messages and types keys via USB HID Keyboard (`receiver/receiver.ino`)

**Note**: `espnow-pedal.cpp` is included as a reference file showing the original implementation. The actual project files are in the `transmitter/` and `receiver/` directories.

## Features

- **Low latency**: ESP-NOW provides fast, direct communication without WiFi connection
- **Multiple pedal modes**: Supports single pedal (LEFT or RIGHT) or dual pedal configurations
- **Press and hold**: Keys stay pressed until pedal is released
- **Independent operation**: Both pedals can be pressed simultaneously
- **Battery efficient**: Includes inactivity timeout and deep sleep support

## Hardware Requirements

### Recommended Boards

**Transmitter:**
- [FireBeetle 2 ESP32-E](https://www.dfrobot.com/product-2195.html) - Optimized for low power consumption, ideal for battery-powered pedal transmitters

**Receiver:**
- [ESP32-S3-DevKitC-1-N16R8](https://www.amazon.com/dp/B0CC8NYXRG) - ESP32-S3 board with native USB support for HID Keyboard functionality

### Transmitter
- ESP32 (tested with FireBeetle 2 ESP32-E)
- Pedal switches (normally-open, connected to GPIO with pull-up)
- Optional: LED for status indication

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

### 1. Get Receiver MAC Address

**⚠️ Important**: You must use `receiver/run-this-first.ino` to get the MAC address. The main receiver sketch (`receiver/receiver.ino`) initializes USB HID Keyboard which disables Serial output, so you won't be able to see the MAC address.

1. Upload `receiver/run-this-first.ino` to your ESP32-S2/S3
2. Open Serial Monitor at 115200 baud
3. The sketch will display the MAC address in both formats:
   - Human-readable: `a0:85:e3:e0:8e:a8`
   - Code format ready to copy: `{0xa0, 0x85, 0xe3, 0xe0, 0x8e, 0xa8}`
4. Copy the code format and use it in the transmitter configuration

### 2. Configure Transmitter

Open `transmitter/transmitter.ino` and make these changes:

**Set the pedal mode:**
```cpp
#define PEDAL_MODE 0  // 0 = DUAL_PEDAL, 1 = SINGLE_PEDAL_1, 2 = SINGLE_PEDAL_2
```

**Update the receiver MAC address (around line 45):**
```cpp
// ESPNOW peer address - MUST match the receiver's MAC address
// Receiver MAC: a0:85:e3:e0:8e:a8
uint8_t broadcastAddress[] = {0xa0, 0x85, 0xe3, 0xe0, 0x8e, 0xa8};
```

**How to convert MAC address format:**
- If your receiver shows: `a0:85:e3:e0:8e:a8`
- Convert to hex bytes: `{0xa0, 0x85, 0xe3, 0xe0, 0x8e, 0xa8}`
- Replace the values in the `broadcastAddress` array with your receiver's MAC address

**⚠️ CRITICAL**: The transmitter will NOT work unless the MAC address matches your receiver's MAC address exactly!

### 3. Customize Keys

In `transmitter/transmitter.ino`, change the keys to send:
```cpp
#define LEFT_PEDAL_KEY 'l'   // Key for left pedal
#define RIGHT_PEDAL_KEY 'r'  // Key for right pedal
```

## Usage

1. Power on the receiver first
2. Power on the transmitter(s)
3. Press pedals to type keys
4. Keys stay pressed until pedal is released
5. Both pedals can be pressed simultaneously

## Configuration Options

### Transmitter Settings

- `PEDAL_MODE`: Pedal configuration (0=dual, 1=single left, 2=single right)
- `INACTIVITY_TIMEOUT`: Time before entering deep sleep (default: 10 minutes)
- `DEBOUNCE_DELAY`: Debounce delay in milliseconds (default: 50ms)
- `LEFT_PEDAL_KEY` / `RIGHT_PEDAL_KEY`: Keys to send

## Known Limitations

- **Serial Output**: On ESP32-S2/S3, Serial output may not be available when USB HID Keyboard is active. This is a hardware limitation, but keyboard functionality works correctly.
- **USB Composite**: Not all ESP32-S2/S3 boards support USB composite mode (CDC + HID simultaneously).

## Troubleshooting

### Keys not typing
- Verify MAC address is correct in transmitter
- Check that receiver is powered on first
- Ensure both devices are ESP32 variants that support ESP-NOW

### Serial Monitor not working on receiver
- This is expected when Keyboard is active on ESP32-S2/S3
- Keyboard functionality should still work
- Use hardware UART if Serial debugging is needed

### Multiple key presses
- Adjust `DEBOUNCE_DELAY` if experiencing contact bounce
- Check pedal switch connections

## License

This project is provided as-is for educational and personal use.

## Contributing

Feel free to submit issues or pull requests for improvements!

