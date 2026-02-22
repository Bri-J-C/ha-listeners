# HA Intercom

[![Open your Home Assistant instance and show the add add-on repository dialog with this repository pre-filled.](https://my.home-assistant.io/badges/supervisor_add_addon_repository.svg)](https://my.home-assistant.io/redirect/supervisor_add_addon_repository/?repository_url=https%3A%2F%2Fgithub.com%2FBri-J-C%2Fha-intercom)

ESP32-S3-based multi-room intercom system with Home Assistant integration.

## Features

- **Push-to-talk** intercom between rooms with first-to-talk collision avoidance
- **Multicast** for broadcast to all rooms, **unicast** for room-to-room
- **Web PTT** browser-based push-to-talk via Home Assistant ingress
- **Lovelace PTT card** for seamless HA dashboard integration
- **OLED display** with room selector, settings page, and availability tracking
- **Priority levels** (Normal, High, Emergency) with preemption
- **Do Not Disturb** mode with emergency override
- **Call notifications** with chime and LED flash
- **Opus codec** at 32kbps VBR with PLC/FEC for packet loss recovery
- **AGC** (Automatic Gain Control) for consistent mic levels
- **AEC** (Acoustic Echo Cancellation) via ESP-SR
- **AES-256-GCM encryption** for stored credentials (WiFi, MQTT, web passwords)
- **OTA firmware updates** via web interface
- **Mobile device** auto-discovery and notification routing
- **TTS announcements** via Piper text-to-speech
- **Home Assistant integration** with MQTT auto-discovery, services, and automations
- **ESP32-S3** based hardware satellites (~$19/room with display)

## Architecture

### System Overview

```mermaid
graph TB
    subgraph HA["Home Assistant (10.0.0.8)"]
        MQTT["MQTT Broker\n(port 1883)"]
        subgraph HUB["Intercom Hub Add-on (Python)"]
            HUB_MQTT["MQTT Client\nauto-discovery · state sync\nvolume · mute · targeting"]
            HUB_WS["WebSocket Server\naudio + control\nclient ID tracking"]
            HUB_ROUTE["Audio Router\nESP32↔ESP32\nESP32↔Web · Web↔Web\nTTS→all"]
            HUB_TTS["TTS Bridge\nPiper · channel-busy wait"]
            HUB_CARD["Lovelace PTT Card\nintercom-ptt-card.js"]
        end
        MQTT <-->|"subscribe/publish"| HUB_MQTT
    end

    subgraph ESP_NODES["ESP32-S3 Room Nodes"]
        ESP1["Kitchen\nESP32-S3\n[OLED · LED]"]
        ESP2["Bedroom\nESP32-S3\n[OLED · LED]"]
        ESP3["Office\nESP32-S3\n[OLED · LED]"]
    end

    subgraph WEB_CLIENTS["Browser Clients"]
        WC1["Web PTT\n(desktop)"]
        WC2["Web PTT\n(mobile browser)"]
    end

    MOB["Mobile Devices\n(HA Companion)\nnotification-only"]

    HUB_MQTT <-->|"MQTT\ndiscovery · commands\nstate · LWT"| ESP_NODES
    HUB_WS <-->|"WebSocket\nbinary PCM + JSON"| WEB_CLIENTS
    HUB_MQTT -->|"push notification"| MOB

    ESP_NODES <-->|"UDP Audio\nmulticast 224.0.0.100:5005\nor unicast :5005"| ESP_NODES
    HUB_ROUTE <-->|"UDP Audio\nmulticast / unicast :5005"| ESP_NODES
```

### ESP32 Audio Flow

```mermaid
flowchart LR
    subgraph TX["TX Path (Transmit)"]
        direction LR
        MIC["INMP441\nMEMS Mic"]
        I2S_IN["I2S Bus 0\nSCK=4 WS=5 SD=6"]
        CONV["32→16 bit\nconversion"]
        AGC_B["AGC\n(gain control)"]
        AEC_B["AEC\n(echo cancel)"]
        ENC["Opus Encoder\n32kbps VBR\n16kHz · 20ms frames\n(internal RAM)"]
        UDP_TX["UDP TX\nmulticast or unicast\nport 5005"]

        MIC --> I2S_IN --> CONV --> AGC_B --> AEC_B --> ENC --> UDP_TX
    end

    subgraph RX["RX Path (Receive)"]
        direction LR
        UDP_RX["UDP RX\nport 5005"]
        DEC["Opus Decoder\nPLC + FEC\n(PSRAM)"]
        VOL["Volume · Mute\nscaling"]
        STEREO["Mono → Stereo\nconversion"]
        I2S_OUT["I2S Bus 1\nSCK=15 WS=16 SD=17"]
        SPK["MAX98357A\nAmp + Speaker"]

        UDP_RX --> DEC --> VOL --> STEREO --> I2S_OUT --> SPK
    end
```

### Packet Format

```mermaid
packet-beta
  0-63: "device_id (8 bytes)"
  64-95: "sequence (4 bytes)"
  96-103: "priority (1 byte)"
  104-167: "opus_frame (~40–80 bytes, variable)"
```

**Header is always 13 bytes.** Priority values: `0` = Normal, `1` = High, `2` = Emergency.

## Components

| Directory | Description |
|-----------|-------------|
| `firmware/` | ESP32-S3 firmware (PlatformIO/ESP-IDF) |
| `intercom_hub/` | Home Assistant add-on for PTT hub, TTS, and routing |
| `tools/` | Python test client for protocol development |

## Quick Start

### 1. ESP32-S3 Hardware

**Required components (~$16 minimum):**
- ESP32-S3-DevKitC-1 (~$8) - has built-in BOOT button and RGB LED
- INMP441 I2S Microphone (~$3)
- MAX98357A I2S Amp + Speaker (~$5)

**Optional (~$3 extra):**
- SSD1306 128x64 OLED Display (~$3) - room selector and status display
- Momentary push button for room cycling (or use any GPIO10-connected button)

**Flash the firmware:**
- **Easy:** [Web Flasher](https://bri-j-c.github.io/ha-intercom/) - flash directly from browser (Chrome/Edge)
- **Manual:** Download from [Releases](https://github.com/Bri-J-C/ha-intercom/releases) and use esptool

See `firmware/README.md` for wiring instructions.

### 2. Home Assistant Add-on

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
- Mobile devices can be added in the add-on config

### 3. Web PTT (Browser)

Access the Web PTT interface through the **Intercom** panel in Home Assistant's sidebar (added automatically by the add-on via ingress). Features:
- Push-to-talk button with visual TX/RX state
- Room/device selector dropdown
- Call button to ring specific rooms
- Works on desktop and mobile browsers

## Protocol

### Audio Transport

| Property | Value |
|----------|-------|
| Transport | UDP |
| Broadcast address | `224.0.0.100:5005` (multicast, all rooms) |
| Room-to-room | `<device-ip>:5005` (unicast) |
| Loopback prevention | `IP_MULTICAST_LOOP=0` on TX socket (hub and firmware) |

### Audio Format

| Property | Value |
|----------|-------|
| Codec | Opus VBR, voice mode, complexity 5 |
| Bitrate | 32 kbps |
| Sample rate | 16000 Hz |
| Channels | 1 (mono) |
| Frame duration | 20 ms (320 samples) |
| Error recovery | PLC (Packet Loss Concealment) + FEC (Forward Error Correction) |

### Control Plane

| Channel | Purpose |
|---------|---------|
| MQTT (HA broker) | Device discovery, state sync, volume, mute, room targeting, LWT |
| WebSocket (hub) | Web PTT audio (binary PCM) and control messages (JSON) |
| mDNS | Local device discovery on LAN |
| HTTP | Web config portal and OTA firmware updates (per-device) |

### Discovery

Devices register via MQTT on connect and appear automatically in Home Assistant with auto-discovery entities (sensor, switch, number). The hub tracks online/offline status via MQTT Last Will and Testament (LWT).

## Credits

[Intercom icons created by Dixit Lakhani_02 - Flaticon](https://www.flaticon.com/free-icons/intercom)

## License

MIT
