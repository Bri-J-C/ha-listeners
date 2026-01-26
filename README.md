# HA Intercom

ESP32-S3 based intercom system with Home Assistant integration.

Inspired by [PTTDroid](https://f-droid.org/en/packages/ro.ui.pttdroid/)'s simple UDP multicast/unicast approach.

## Features

- **Push-to-talk** intercom between rooms
- **Multicast** for broadcast to all rooms
- **Unicast** for room-to-room communication
- **Home Assistant integration** with services and automations
- **Opus codec** for high-quality, low-bandwidth audio
- **ESP32-S3** based hardware satellites (~$17/room)

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     HOME ASSISTANT                           │
│  ┌────────────────────────────────────────────────────────┐ │
│  │              ha_intercom integration                    │ │
│  │  • Device discovery         • TTS bridge (Piper)       │ │
│  │  • Room routing             • Services & automations   │ │
│  └────────────────────────┬───────────────────────────────┘ │
└───────────────────────────┼─────────────────────────────────┘
                            │ UDP Control (port 5004)
        ┌───────────────────┼───────────────────┐
        ▼                   ▼                   ▼
  ┌───────────┐       ┌───────────┐       ┌───────────┐
  │  ESP32-S3 │◄─────►│  ESP32-S3 │◄─────►│  ESP32-S3 │
  │  Kitchen  │       │  Bedroom  │       │  Office   │
  └───────────┘       └───────────┘       └───────────┘
              UDP Audio (port 5005)
              • Multicast 224.0.0.100 = all rooms
              • Unicast = room-to-room
```

## Components

| Directory | Description |
|-----------|-------------|
| `tools/` | Python test client for protocol development |
| `firmware/` | ESP32-S3 firmware (ESP-IDF) |
| `custom_components/ha_intercom/` | Home Assistant integration |

## Quick Start

### 1. Python Test Client

Test the protocol without hardware:

```bash
# Install dependencies
sudo apt install libopus-dev python3-pip
pip install -r tools/requirements.txt

# Terminal 1: Start first client
python tools/ptt_client.py --name kitchen

# Terminal 2: Start second client
python tools/ptt_client.py --name bedroom

# Press Enter to toggle PTT, 'q' to quit
```

### 2. ESP32-S3 Hardware

**Required components (~$17 total):**
- ESP32-S3-DevKitC-1 (~$8)
- INMP441 I2S Microphone (~$3)
- MAX98357A I2S Amp + Speaker (~$5)
- Push button + LED (~$1)

See `firmware/README.md` for wiring and flashing instructions.

### 3. Home Assistant Integration

```bash
# Copy to HA config directory
cp -r custom_components/ha_intercom /path/to/homeassistant/custom_components/

# Restart Home Assistant
# Devices will auto-discover via UDP broadcast
```

## Protocol

### Audio Packets (UDP port 5005)

```
┌──────────────┬──────────────┬─────────────────────┐
│ device_id    │ sequence     │ opus_frame          │
│ (8 bytes)    │ (4 bytes)    │ (~40-80 bytes)      │
└──────────────┴──────────────┴─────────────────────┘
```

- **Multicast** `224.0.0.100:5005` → All devices (broadcast)
- **Unicast** `<ip>:5005` → Single device (room-to-room)

### Audio Format

- Sample rate: 16000 Hz
- Channels: 1 (mono)
- Frame size: 20ms (320 samples)
- Codec: Opus @ 24kbps

### Discovery (UDP port 5004)

Devices announce themselves every 30 seconds. HA responds with configuration.

## License

MIT
