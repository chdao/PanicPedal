# FireBeetle 2 ESP32-E Pinout Reference

This document describes the pinout for the FireBeetle 2 ESP32-E development board (ESP32-WROOM-32E module).

![FireBeetle 2 ESP32-E Pinout](Firebeetle2.png)

## Power Pins

- **VCC** (Red): Power input pin. Connect TP4056 OUT+ or battery here. Accepts 3.7-4.2V from battery or ~4.7V from USB.
- **3.3V** (Red): Regulated 3.3V OUTPUT pin. Do NOT connect battery voltage here - this is an output, not an input!
- **GND** (Black): Ground connection. Multiple GND pins available.

## GPIO Pins Used in This Project

### Left Pedal / Single Pedal
- **GPIO 13** (D7): Used for LEFT pedal in dual mode, or single pedal in single pedal mode
  - Also functions as: ADC2_CH4, TOUCH4, RTC_GPIO14
  - Supports deep sleep wakeup

### Right Pedal (Dual Mode Only)
- **GPIO 14** (D6): Used for RIGHT pedal in dual mode
  - Also functions as: ADC2_CH6, TOUCH6, RTC_GPIO16
  - Supports deep sleep wakeup

### LED (Optional)
- **GPIO 2** (D9): Used for status LED
  - Also functions as: ADC2_CH2, TOUCH2, RTC_GPIO12
  - Supports deep sleep wakeup

### Not Used (But Mentioned in Original Schematic)
- **GPIO 26** (D3): Mode Select Switch pin from original schematic
  - Not used in ESP-NOW only version
  - Also functions as: ADC2_CH9, RTC_GPIO7

## Other Available GPIO Pins

### SPI Pins
- **GPIO 18** (SCK): SPI Clock
- **GPIO 23** (MOSI): SPI Master Out Slave In
- **GPIO 19** (MISO): SPI Master In Slave Out

### I2C Pins
- **GPIO 22** (SCL): I2C Clock
- **GPIO 21** (SDA): I2C Data

### UART Pins
- **GPIO 1** (TX): UART Transmit
- **GPIO 3** (RX): UART Receive

### Analog Input Pins
- **GPIO 0** (D5): ADC2_CH1, TOUCH1, RTC_GPIO11
  - **⚠️ IMPORTANT**: Must be grounded (connected to GND) to enter download/boot mode for uploading sketches
- **GPIO 2** (D9): ADC2_CH2, TOUCH2, RTC_GPIO12 (LED pin)
- **GPIO 4** (D12): ADC1_CH0, TOUCH0, RTC_GPIO10
- **GPIO 12** (D13): ADC2_CH5, TOUCH5, RTC_GPIO15
- **GPIO 13** (D7): ADC2_CH4, TOUCH4, RTC_GPIO14 (Pedal pin)
- **GPIO 14** (D6): ADC2_CH6, TOUCH6, RTC_GPIO16 (Pedal pin)
- **GPIO 15** (A4): ADC2_CH3, TOUCH3, RTC_GPIO13
- **GPIO 25** (D2): ADC2_CH8, RTC_GPIO6
- **GPIO 26** (D3): ADC2_CH9, RTC_GPIO7
- **GPIO 27** (A5): ADC2_CH7, TOUCH7, RTC_GPIO17
- **GPIO 32** (A6): ADC1_CH4, RTC_GPIO9
- **GPIO 33** (A7): ADC1_CH5, RTC_GPIO8
- **GPIO 34** (A2): ADC1_CH6, RTC_GPIO4 (Input only)
- **GPIO 35** (A3): ADC1_CH7, RTC_GPIO5 (Input only)
- **GPIO 36** (A0): ADC1_CH0, RTC_GPIO0 (Input only)
- **GPIO 39** (A1): ADC1_CH3, RTC_GPIO3 (Input only)

### Control Pins
- **EN**: Enable pin (yellow)
- **RESET**: Reset button (yellow)

## Pin Categories

- **Control**: Yellow pins (EN, RESET)
- **TOUCH**: Beige pins (capacitive touch capable)
- **Analog**: Light blue pins (ADC capable)
- **Port PIN**: Orange pins (general purpose I/O)
- **IDE**: Blue pins (commonly used for SPI, I2C, UART)
- **RTC PIN**: Dark blue pins (retain state during deep sleep)
- **GND**: Black pins (ground)
- **Power**: Red pins (VCC input, 3.3V output)

## Important Notes

1. **VCC vs 3.3V**: Always connect battery/TP4056 OUT+ to **VCC**, never to 3.3V. The 3.3V pin is an output, not an input.

2. **Download Mode (GPIO 0)**: To upload sketches to the FireBeetle, GPIO 0 (D5) must be grounded (connected to GND) during the upload process. See the main README for detailed instructions.

3. **Deep Sleep Wakeup**: GPIO 13 and GPIO 14 support deep sleep wakeup, which is why they're used for the pedals.

4. **Input-Only Pins**: GPIO 34, 35, 36, and 39 are input-only (no internal pull-up/pull-down resistors).

5. **RTC Pins**: Pins marked as RTC can retain their state during deep sleep, which is useful for wakeup functionality.

## Reference

This pinout is based on the FireBeetle 2 ESP32-E development board featuring the ESP32-WROOM-32E module.

