# PanicPedal Pro Transmitter

This directory contains the firmware for the **PanicPedal Pro** transmitter PCB.

## Hardware

- **MCU**: ESP32-S3-WROOM
- **PCB**: Custom PanicPedal Pro PCB

## GPIO Pin Configuration

| GPIO | Function | Component |
|------|----------|-----------|
| GPIO2 | LED Control | Inolux_IN-PI554FCH LED |
| GPIO3 | Battery Voltage Sensing | Before TLV75733PDBV regulator |
| GPIO4 | Battery STAT1 | MCP73871 battery charger |
| GPIO5 | Switch Position 1 | 2-position switch (unused) |
| GPIO6 | Switch Position 2 | 2-position switch (unused) |
| GPIO7 | Foot Switch 1 | First pedal input |
| GPIO21 | Foot Switch 2 | Second pedal input |

## Configuration

- **Pedal Mode**: Single pedal (GPIO7) by default
- **Dual Pedal Mode**: Set `PEDAL_MODE` to 0 to enable GPIO21 as second pedal
- **Deep Sleep Wakeup**: GPIO7 (LOW trigger)

## Building and Uploading

1. Open `panicpedal-pro.ino` in Arduino IDE
2. Select your ESP32-S3 board from the board manager
3. Configure upload settings for your ESP32-S3-WROOM module
4. Upload the sketch

## Notes

- This code is specifically designed for the PanicPedal Pro PCB
- Two foot switches are connected to GPIO7 and GPIO21
- Battery monitoring pins are available but not yet implemented in the firmware
- LED control (GPIO2) is defined but not yet implemented
- Set `PEDAL_MODE` to 0 for dual pedal mode or 1 for single pedal (GPIO7 only)
