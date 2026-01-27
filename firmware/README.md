# HA Intercom ESP32-S3 Firmware

PlatformIO-based firmware for the intercom satellite.

## Hardware Requirements

**Minimum:**
- ESP32-S3-DevKitC-1 or similar (~$8) - has built-in BOOT button and RGB LED
- INMP441 I2S MEMS Microphone (~$3)
- MAX98357A I2S Amplifier + Speaker (~$5)

**Total: ~$16**

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

### Button & LED (Built-in on DevKitC-1)
| Component | ESP32-S3 | Notes |
|-----------|----------|-------|
| Button    | GPIO0    | Built-in BOOT button |
| LED       | GPIO48   | Built-in WS2812 RGB LED |

No external button or LED required - the DevKitC-1 has these built-in.

## Building

### Prerequisites

1. Install [PlatformIO](https://platformio.org/install):
   ```bash
   pip install platformio
   ```

### Configure

WiFi and MQTT are configured via the web interface after first boot.
The device starts in AP mode if no WiFi is configured.

(Optional) Set default WiFi credentials in `main/main.c` for development:
```c
#define DEFAULT_WIFI_SSID       "your_wifi_ssid"
#define DEFAULT_WIFI_PASSWORD   "your_wifi_password"
```

### Build & Flash

```bash
cd firmware

# Build
pio run

# Flash (connect ESP32-S3 via USB)
pio run -t upload

# Monitor logs
pio device monitor
```

Or use VS Code with the PlatformIO extension for a graphical interface.

## Usage

1. **Power on** - LED blinks red while connecting to WiFi
2. **Connected (idle)** - LED dim white
3. **Press button** - Start talking (LED green)
4. **Release button** - Stop talking (LED returns to white)
5. **Hold button 2s** - Broadcast to all rooms (instead of default target)
6. **Receiving audio** - LED blue
7. **Muted** - LED red
8. **Error** - LED blinking red

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
- Ensure Intercom Hub add-on is installed
- Check MQTT broker is running (Mosquitto add-on)
- Verify MQTT settings in web config match HA's broker
- Check both devices on same network

## Pin Configuration

Default pins can be changed in the header files:

- `audio_input.h` - Microphone I2S pins
- `audio_output.h` - Speaker I2S pins
- `button.h` - Button and LED pins
