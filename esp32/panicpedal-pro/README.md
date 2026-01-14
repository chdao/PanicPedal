# PanicPedal Pro Transmitter

This directory contains the firmware for the **PanicPedal Pro** transmitter PCB.

## Hardware

- **MCU**: ESP32-S3-WROOM-1-N8
- **PCB**: Custom PanicPedal Pro PCB

## GPIO Pin Configuration

### Used GPIOs

| GPIO | Function | Component | Notes |
|------|----------|-----------|-------|
| GPIO1 | Pedal Right NO | Right pedal normally-open contact | Interrupt-driven |
| GPIO2 | Pedal Left NO | Left pedal normally-open contact | Interrupt-driven, deep sleep wakeup |
| GPIO9 | Battery Voltage Sensing | Before TLV75733PDBV regulator | |
| GPIO4 | STAT1/LBO | MCP73871 battery charger (charging status) | |
| GPIO5 | Toggle Switch | Toggle switch | |
| GPIO6 | Pedal Left NC | Left pedal normally-closed contact (for detection) | |
| GPIO7 | Pedal Right NC | Right pedal normally-closed contact (for detection) | |
| GPIO8 | WS2812B LED Control | WS2812B LED data pin | |
| GPIO10 | Debug Button | Debug pushbutton toggle (toggles debug mode on/off) | Pull-up, active LOW |

### GPIOs Never Configured (Critical/Internal)

| GPIO | Reason | Notes |
|------|--------|-------|
| GPIO0 | Boot mode strapping pin | CRITICAL - Never pull down |
| GPIO3 | Strapping pin | CRITICAL - Never touch |
| GPIO19, 20 | USB D-/D+ | USB Serial/JTAG communication |
| GPIO26-34 | Flash/PSRAM pins | Internal pins - not exposed on PCB, causes crashes if configured |
| GPIO43, 44 | USB D-/D+ (alternative) | Alternative USB/JTAG pins |

### Unused GPIOs

All other GPIOs (GPIO11-18, GPIO21-25, GPIO35-40, GPIO42, GPIO45-48) are automatically configured as **INPUT** (no pull-up/pull-down) on boot to:
- Prevent floating pins from causing erratic behavior
- Reduce power consumption
- Allow external pull-up/pull-down resistors on PCB to control pin state

**Note**: GPIO45 and GPIO46 are pulled down by PCB hardware.

## Configuration

- **Pedal Mode**: Auto-detected on every boot
  - Detection runs automatically on every boot to ensure correct configuration
  - If both switches are connected: Dual pedal mode (GPIO1 & GPIO2)
  - If only left switch is connected: Single pedal mode (GPIO2)
  - Detection uses NC contacts (GPIO6 & GPIO7) to sense switch presence
  - No NVS storage - always detects fresh on boot for maximum reliability
- **Manual Override**: Set `PEDAL_MODE` to `PEDAL_MODE_DUAL` (0) or `PEDAL_MODE_SINGLE` (1) to override auto-detection
- **Deep Sleep Wakeup**: GPIO2 (LOW trigger) - Left pedal

## Building and Uploading

1. Open `panicpedal-pro.ino` in Arduino IDE
2. Select **ESP32S3 Dev Module** from the board manager
3. Configure upload settings:
   - **USB CDC On Boot**: `Enabled` (CRITICAL for serial output)
   - **USB Mode**: `Hardware CDC and JTAG` or `USB-OTG (TinyUSB)`
   - **Flash Size**: `8MB (64Mb)` (for ESP32-S3-WROOM-1-N8)
   - **Partition Scheme**: `Default 4MB with spiffs` or similar
   - **CPU Frequency**: `240MHz` (or `80MHz` for battery optimization)
4. Upload the sketch

**Note**: Pedal detection runs automatically on every boot - no configuration needed!

## Power Optimization

- **CPU Frequency**: Reduced to 80MHz for battery savings
- **WiFi Power Save**: Maximum modem sleep mode enabled
- **Bluetooth**: Disabled on boot (not needed for ESP-NOW)
- **Event-Driven Pedal Detection**: Interrupt-based (no polling)
- **Dynamic Delays**: Longer delays when idle, shorter when active
- **Deep Sleep**: Enters deep sleep after 5 minutes of inactivity
- **GPIO Configuration**: Unused GPIOs set as INPUT (no pull resistors) to reduce power

## Notes

- This code is specifically designed for the PanicPedal Pro PCB
- **Automatic Switch Detection**: The firmware automatically detects which switches are connected by reading the NC (normally-closed) contacts on GPIO6 and GPIO7
  - No need to manually configure pedal mode - it's detected at boot
  - If a switch is connected, its NC contact will read LOW
  - This eliminates the need to hardcode pedal configuration
- Two foot switches can be connected:
  - Left pedal: NO on GPIO2, NC on GPIO6
  - Right pedal: NO on GPIO1, NC on GPIO7
- Battery monitoring pins (GPIO9, GPIO4) are available
- **LED Control**: WS2812B LED on GPIO8 (currently disabled - LED service needs update for WS2812B protocol)
- **GPIO Configuration**: Unused GPIOs are automatically configured as INPUT on boot to prevent floating pins and reduce power consumption