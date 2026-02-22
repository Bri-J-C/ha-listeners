# HA Intercom ESP32-S3 Firmware

PlatformIO-based firmware for the intercom satellite nodes.

## Features

- Push-to-talk via BOOT button with first-to-talk collision avoidance
- Opus codec (32kbps VBR, complexity 5) with PLC + FEC for packet loss recovery
- OLED room selector with cycle button (short press = next room, long press = call)
- OLED settings page (AGC, AEC, volume, DND, display brightness)
- Priority levels: Normal, High, Emergency (with preemption)
- Do Not Disturb mode with emergency override
- AGC (Automatic Gain Control) for consistent microphone levels
- AEC (Acoustic Echo Cancellation) via ESP-SR
- Incoming call chime with LED flash
- AES-256-GCM encrypted credential storage in NVS
- OTA firmware updates via web interface (`http://<device-ip>/update`)
- Web config portal for WiFi, MQTT, and device settings
- WiFi AP fallback mode for initial setup
- Availability tracking (online/offline per device via MQTT LWT)
- Mobile device detection in room list
- PSRAM support (decoder + audio buffers in PSRAM when available)
- Lead-in/trail-out silence frames for clean audio start/stop

## Hardware Requirements

**Minimum:**
- ESP32-S3-DevKitC-1 or similar (~$8) - has built-in BOOT button and RGB LED
- INMP441 I2S MEMS Microphone (~$3)
- MAX98357A I2S Amplifier + Speaker (~$5)

**Optional:**
- SSD1306 128x64 OLED Display (~$3) - room selector, status, and settings
- Momentary push button for room cycling (~$0.10)

**Total: ~$16 minimum, ~$19 with display**

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

### SSD1306 OLED Display (Optional)
| SSD1306 | ESP32-S3 |
|---------|----------|
| VCC     | 3.3V     |
| GND     | GND      |
| SDA     | GPIO8    |
| SCL     | GPIO9    |

I2C address: 0x3C (default for most SSD1306 modules)

### Cycle Button (Optional)
| Component | ESP32-S3 | Notes |
|-----------|----------|-------|
| Button    | GPIO10   | Active low, internal pull-up enabled |

Connect one leg to GPIO10 and the other to GND.

### Button & LED (Built-in on DevKitC-1)
| Component | ESP32-S3 | Notes |
|-----------|----------|-------|
| PTT Button | GPIO0  | Built-in BOOT button |
| LED        | GPIO48 | Built-in WS2812 RGB LED |

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

**Note:** After changing `sdkconfig.esp32s3` or `sdkconfig.defaults`, run `pio run -t fullclean` before building.

## Usage

### PTT Button (BOOT, GPIO0)
- **Press and hold** — Start talking (LED cyan, transmits to selected target)
- **Release** — Stop talking (LED returns to idle state)

### Cycle Button (GPIO10)
- **Short press** — Cycle to next room on OLED display
- **Long press (1s)** — Send call/ring notification to selected room
- **Long press on "Settings" entry** — Enter OLED settings page (cycle to the Settings entry at the end of the room list, then long press)

### LED States
| Color | State |
|-------|-------|
| White (dim) | Idle, connected |
| Cyan | Transmitting (PTT active) |
| Green | Receiving audio |
| Red | Muted |
| Orange | Channel busy (another device transmitting) |
| Purple | Do Not Disturb mode active |
| Blinking red | Error or connecting to WiFi |

### OLED Display
- **Main screen** — Shows device name, selected target room, and connection status
- **Room selector** — Use cycle button to browse available rooms with availability indicators
- **Settings page** — AGC on/off, AEC on/off, volume level, DND toggle, display brightness

### OTA Updates
Navigate to `http://<device-ip>/update` in a browser to upload new firmware over-the-air.

### Web Config
Navigate to `http://<device-ip>/` to configure WiFi, MQTT, device name, and other settings.

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
- Device will fall back to AP mode if WiFi fails

### Device not discovered by HA
- Ensure Intercom Hub add-on is installed
- Check MQTT broker is running (Mosquitto add-on)
- Verify MQTT settings in web config match HA's broker
- Check both devices on same network

### OLED not working
- Verify I2C wiring (SDA=GPIO8, SCL=GPIO9)
- Check I2C address is 0x3C (most common for SSD1306)
- Display is optional — firmware works without it

## Pin Configuration

Default pins can be changed in the header files:

- `audio_input.h` - Microphone I2S pins
- `audio_output.h` - Speaker I2S pins
- `button.h` - Button and LED pins
- `display.c` - OLED I2C pins
