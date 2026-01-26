# HA Intercom ESP32-S3 Firmware

ESP-IDF based firmware for the intercom satellite.

## Hardware Requirements

**Minimum:**
- ESP32-S3-DevKitC-1 or similar (~$8)
- INMP441 I2S MEMS Microphone (~$3)
- MAX98357A I2S Amplifier + Speaker (~$5)
- Push button (~$0.50)
- LED (~$0.50)

**Total: ~$17**

## Wiring

### INMP441 Microphone
| INMP441 | ESP32-S3 |
|---------|----------|
| VDD     | 3.3V     |
| GND     | GND      |
| SCK     | GPIO4    |
| WS      | GPIO5    |
| SD      | GPIO6    |
| L/R     | GND (left channel) |

### MAX98357A Amplifier
| MAX98357A | ESP32-S3 |
|-----------|----------|
| VIN       | 5V       |
| GND       | GND      |
| BCLK      | GPIO15   |
| LRC       | GPIO16   |
| DIN       | GPIO17   |
| GAIN      | NC or GND (15dB) |
| SD        | NC (always on) |

### Button & LED
| Component | ESP32-S3 |
|-----------|----------|
| Button    | GPIO0 (BOOT button) |
| LED       | GPIO48 (onboard RGB) |

## Building

### Prerequisites

1. Install ESP-IDF v5.x:
   ```bash
   # Follow: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/
   ```

2. Set up environment:
   ```bash
   . $HOME/esp/esp-idf/export.sh
   ```

### Configure

1. Set your WiFi credentials in `main/main.c`:
   ```c
   #define WIFI_SSID       "your_wifi_ssid"
   #define WIFI_PASSWORD   "your_wifi_password"
   ```

   Or use menuconfig:
   ```bash
   idf.py menuconfig
   ```

2. (Optional) Adjust GPIO pins in header files if your wiring differs.

### Build & Flash

```bash
cd firmware

# Set target
idf.py set-target esp32s3

# Build
idf.py build

# Flash (connect ESP32-S3 via USB)
idf.py -p /dev/ttyUSB0 flash

# Monitor logs
idf.py -p /dev/ttyUSB0 monitor
```

## Usage

1. **Power on** - LED turns red while connecting to WiFi
2. **Connected** - LED turns solid green
3. **Press button** - Start talking (LED blinks blue)
4. **Release button** - Stop talking
5. **Hold button 2s** - Broadcast to all rooms (instead of default target)
6. **Receiving audio** - LED solid blue

## Troubleshooting

### No audio output
- Check MAX98357A wiring
- Verify speaker is connected
- Check volume setting (default 80%)

### Microphone not working
- Verify INMP441 wiring (especially L/R to GND)
- Check I2S pins match configuration

### WiFi not connecting
- Verify SSID and password
- Check signal strength
- Monitor serial output for errors

### Device not discovered by HA
- Ensure HA integration is installed
- Check both devices on same network
- Verify UDP port 5004 not blocked

## Pin Configuration

Default pins can be changed in the header files:

- `audio_input.h` - Microphone I2S pins
- `audio_output.h` - Speaker I2S pins
- `button.h` - Button and LED pins
