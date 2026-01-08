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
| GPIO7 | Foot Switch 1 NO | First pedal normally-open contact |
| GPIO21 | Foot Switch 2 NO | Second pedal normally-open contact |
| GPIO45 | Foot Switch 1 NC | First pedal normally-closed contact (for detection) |
| GPIO46 | Foot Switch 2 NC | Second pedal normally-closed contact (for detection) |

## Configuration

- **Pedal Mode**: Auto-detected on first boot and stored in NVRAM
  - Detection runs on first boot or when firmware version changes
  - Detected mode is stored in NVS (Non-Volatile Storage) along with firmware version
  - If firmware version in code changes, detection will run again automatically
  - If both switches are connected: Dual pedal mode (GPIO7 & GPIO21)
  - If only first switch is connected: Single pedal mode (GPIO7)
  - Detection uses NC contacts (GPIO45 & GPIO46) to sense switch presence
  - To force re-detection: Increment `FIRMWARE_VERSION` in code or clear NVS manually
- **Manual Override**: Set `PEDAL_MODE` to `PEDAL_MODE_DUAL` (0) or `PEDAL_MODE_SINGLE` (1) to override auto-detection
- **Deep Sleep Wakeup**: GPIO7 (LOW trigger)

## Building and Uploading

### Using Git Commit Hash as Version (Recommended)

1. **Generate version header** (run before building):
   - **Windows**: Run `get_version.ps1` in PowerShell
   - **Linux/Mac**: Run `get_version.sh` (make it executable: `chmod +x get_version.sh`)
   - This generates `version.h` with the current git commit hash

2. Open `panicpedal-pro.ino` in Arduino IDE
3. Select your ESP32-S3 board from the board manager
4. Configure upload settings for your ESP32-S3-WROOM module
5. Upload the sketch

**Note**: The firmware version will automatically be set to the git commit hash. If `version.h` doesn't exist, it falls back to the manual version defined in the code.

### Manual Version (Alternative)

If you prefer not to use git commit hash, you can manually update `FIRMWARE_VERSION` in the code. Detection will run whenever the version changes.

## Notes

- This code is specifically designed for the PanicPedal Pro PCB
- **Automatic Switch Detection**: The firmware automatically detects which switches are connected by reading the NC (normally-closed) contacts on GPIO45 and GPIO46
  - No need to manually configure pedal mode - it's detected at boot
  - If a switch is connected, its NC contact will read LOW
  - This eliminates the need to hardcode pedal configuration
- Two foot switches can be connected:
  - First pedal: NO on GPIO7, NC on GPIO45
  - Second pedal: NO on GPIO21, NC on GPIO46
- Battery monitoring pins are available but not yet implemented in the firmware
- LED control (GPIO2) is defined but not yet implemented
