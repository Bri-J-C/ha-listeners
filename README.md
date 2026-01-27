# HA Intercom

[![Open your Home Assistant instance and show the add add-on repository dialog with this repository pre-filled.](https://my.home-assistant.io/badges/supervisor_add_addon_repository.svg)](https://my.home-assistant.io/redirect/supervisor_add_addon_repository/?repository_url=https%3A%2F%2Fgithub.com%2FBri-J-C%2Fha-intercom)

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
│  │              Intercom Hub Add-on                        │ │
│  │  • MQTT device discovery    • TTS bridge (Piper)       │ │
│  │  • Room routing             • Services & automations   │ │
│  └────────────────────────┬───────────────────────────────┘ │
└───────────────────────────┼─────────────────────────────────┘
                            │ MQTT (discovery & control)
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
| `firmware/` | ESP32-S3 firmware (PlatformIO/ESP-IDF) |
| `intercom_hub/` | Home Assistant add-on for TTS and routing |
| `tools/` | Python test client for protocol development |

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

**Required components (~$16 total):**
- ESP32-S3-DevKitC-1 (~$8) - has built-in BOOT button and RGB LED
- INMP441 I2S Microphone (~$3)
- MAX98357A I2S Amp + Speaker (~$5)

See `firmware/README.md` for wiring and flashing instructions.

### 3. Home Assistant Add-on

**Option A: One-click install**

Click the button at the top of this README, or:

[![Add Repository](https://my.home-assistant.io/badges/supervisor_add_addon_repository.svg)](https://my.home-assistant.io/redirect/supervisor_add_addon_repository/?repository_url=https%3A%2F%2Fgithub.com%2FBri-J-C%2Fha-intercom)

**Option B: Manual install**

1. Go to **Settings → Add-ons → Add-on Store**
2. Click the 3-dot menu (top right) → **Repositories**
3. Add: `https://github.com/Bri-J-C/ha-intercom`
4. Find "Intercom Hub" in the store and install

**Configuration:**
- Configure MQTT broker settings (usually auto-detected)
- Configure Piper TTS host if using text-to-speech
- ESP32 devices auto-discover via MQTT

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
- Codec: Opus @ 12kbps (VBR, voice mode)

### Discovery

Devices register via MQTT and appear automatically in Home Assistant.

## License

MIT
