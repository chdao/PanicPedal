# ESP-NOW Pedal Keyboard

A wireless pedal system using ESP-NOW for low-latency communication between pedal transmitters and a USB HID keyboard receiver.

## Overview

This project consists of:
- **Transmitter**: ESP32-based pedal device that sends key press/release events via ESP-NOW
- **Receiver**: ESP32-S2/S3 device that receives ESP-NOW messages and types keys via USB HID Keyboard

## Features

- **Low latency**: ESP-NOW provides fast, direct communication without WiFi connection
- **Multiple pedal modes**: Supports single pedal (LEFT or RIGHT) or dual pedal configurations
- **Press and hold**: Keys stay pressed until pedal is released
- **Independent operation**: Both pedals can be pressed simultaneously
- **Battery efficient**: Includes inactivity timeout and deep sleep support

## Hardware Requirements

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

### Receiver
- Uses USB for communication (no GPIO pins needed for functionality)

## Setup Instructions

### 1. Configure Transmitter

Open `transmitter/transmitter.ino` and set:

```cpp
#define PEDAL_MODE 0  // 0 = DUAL_PEDAL, 1 = SINGLE_PEDAL_1, 2 = SINGLE_PEDAL_2
```

Update the receiver MAC address:
```cpp
uint8_t broadcastAddress[] = {0xa0, 0x85, 0xe3, 0xe0, 0x8e, 0xa8};  // Your receiver's MAC
```

### 2. Configure Receiver

1. Upload `receiver/receiver.ino` to your ESP32-S2/S3
2. Open Serial Monitor at 115200 baud
3. Note the MAC address displayed
4. Update the MAC address in the transmitter code

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

