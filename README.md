# PanicPedal

A wireless pedal system using ESP-NOW for low-latency communication between pedal transmitters and a USB HID keyboard receiver. Perfectly adequate for arcade gaming!

**Original Design**: This project is based on the [Perfectly Adequate Arcade Pedal (Wireless)](https://www.printables.com/model/1220746-perfectly-adequate-arcade-pedal-wireless) design on Printables. All initial ideas and the pedal model come from that design.

**PCB Project**: The custom PCB design is called **PanicPedal Pro** and includes an ESP32-S3-WROOM with automatic pedal detection. See `kicad-pcb/` for the KiCad project files and `esp32/panicpedal-pro/` for the firmware.

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
- **PanicPedal Pro** - Custom PCB project with ESP32-S3-WROOM featuring automatic pedal detection (see `esp32/panicpedal-pro/README.md` for details)

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

## Setup and Usage

For detailed setup instructions, usage guide, configuration options, debug monitor setup, and troubleshooting, see [INSTRUCTIONS.md](INSTRUCTIONS.md).

## License

This project is provided as-is for educational and personal use.

## Contributing

Feel free to submit issues or pull requests for improvements!

