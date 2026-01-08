# PanicPedal Pro Transmitter

This directory contains the firmware for the **PanicPedal Pro** transmitter PCB.

## Hardware

- **MCU**: ESP32-S3-WROOM
- **PCB**: Custom PanicPedal Pro PCB

## GPIO Pin Configuration

| GPIO | Function | Component |
|------|----------|-----------|
| GPIO1 | Foot Switch 1 NO | First pedal normally-open contact |
| GPIO2 | Foot Switch 2 NO | Second pedal normally-open contact |
| GPIO3 | Battery Voltage Sensing | Before TLV75733PDBV regulator |
| GPIO4 | STAT1/LBO | MCP73871 battery charger (charging status) |
| GPIO5 | Switch Position 1 | Single pole double throw switch position 1 |
| GPIO6 | Switch Position 2 | Single pole double throw switch position 2 |
| GPIO7 | LED DIN | Inolux_IN-PI554FCH LED |
| GPIO28 | Foot Switch 1 NC | First pedal normally-closed contact (for detection) |
| GPIO29 | Foot Switch 2 NC | Second pedal normally-closed contact (for detection) |

## Configuration

- **Pedal Mode**: Auto-detected on every boot
  - Detection runs automatically on every boot to ensure correct configuration
  - If both switches are connected: Dual pedal mode (GPIO1 & GPIO2)
  - If only first switch is connected: Single pedal mode (GPIO1)
  - Detection uses NC contacts (GPIO28 & GPIO29) to sense switch presence
  - No NVS storage - always detects fresh on boot for maximum reliability
- **Manual Override**: Set `PEDAL_MODE` to `PEDAL_MODE_DUAL` (0) or `PEDAL_MODE_SINGLE` (1) to override auto-detection
- **Deep Sleep Wakeup**: GPIO1 (LOW trigger)

## Building and Uploading

1. Open `panicpedal-pro.ino` in Arduino IDE
2. Select your ESP32-S3 board from the board manager
3. Configure upload settings for your ESP32-S3-WROOM module
4. Upload the sketch

**Note**: Pedal detection runs automatically on every boot - no configuration needed!

## Notes

- This code is specifically designed for the PanicPedal Pro PCB
- **Automatic Switch Detection**: The firmware automatically detects which switches are connected by reading the NC (normally-closed) contacts on GPIO28 and GPIO29
  - No need to manually configure pedal mode - it's detected at boot
  - If a switch is connected, its NC contact will read LOW
  - This eliminates the need to hardcode pedal configuration
- Two foot switches can be connected:
  - First pedal: NO on GPIO1, NC on GPIO28
  - Second pedal: NO on GPIO2, NC on GPIO29
- Battery monitoring pins (GPIO3, GPIO4) are available but not yet implemented in the firmware
- LED control (GPIO7) is defined but not yet implemented
