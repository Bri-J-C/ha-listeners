# Intercom Hub - Home Assistant Add-on

A Home Assistant add-on that acts as the central hub for the HA Intercom system — coordinating MQTT discovery, audio routing, Web PTT clients, TTS announcements, and call notifications.

## Features

- **Web PTT** — Browser-based push-to-talk via WebSocket (accessible through HA ingress)
- **Per-client tracking** — Each web client gets a unique ID with independent state
- **Audio routing** — ESP32↔ESP32, ESP32↔Web, Web↔Web, TTS→all
- **Priority routing** — Normal, High, Emergency levels with preemption; trail-out silence uses active PTT priority
- **DND awareness** — Respects Do Not Disturb mode, allows emergency override
- **Call notifications** — Ring/chime between all node types (ESP32, web, mobile)
- **Mobile device support** — Auto-discovery from HA companion apps
- **TTS announcements** — Via Piper text-to-speech with channel-busy waiting
- **MQTT auto-discovery** — Devices appear automatically in Home Assistant
- **Lovelace PTT card** — Custom card for HA dashboards (`intercom-ptt-card.js` v1.2.0) with ingress and direct WebSocket modes
- **Thread-safe state** — Concurrent client handling with proper locking

## Installation

### Option 1: Local Add-on (Development)

1. Copy the `intercom_hub` folder to `/addons/intercom_hub/` on your HA instance
2. Go to **Settings → Add-ons → Add-on Store**
3. Click the three dots menu → **Check for updates**
4. Find "Intercom Hub" under **Local add-ons**
5. Click **Install**

### Option 2: GitHub Repository

1. Go to **Settings → Add-ons → Add-on Store**
2. Click three dots → **Repositories**
3. Add: `https://github.com/Bri-J-C/ha-intercom`
4. Find and install "Intercom Hub"

## Configuration

```yaml
mqtt_host: core-mosquitto      # MQTT broker hostname
mqtt_port: 1883                # MQTT broker port
mqtt_user: "homeassistant"     # MQTT username (required)
mqtt_password: "your_password" # MQTT password (required)
device_name: "Intercom Hub"    # Display name in HA
multicast_group: "224.0.0.100" # Must match ESP32 firmware
multicast_port: 5005           # Must match ESP32 firmware
piper_host: "core-piper"      # Piper TTS addon hostname
piper_port: 10200              # Piper TTS addon port
log_level: "info"              # debug, info, warning, error
mobile_devices:                # Optional: mobile devices for notifications
  - name: "Phone"
    notify_service: "notify.mobile_app_phone"
```

## Usage

### Web PTT (Browser)

Access via the **Intercom** panel in Home Assistant's sidebar. The add-on provides an ingress-based web interface with:
- Push-to-talk button (hold to talk)
- Room/device selector dropdown
- Call button to ring specific rooms
- Connection status and TX/RX state indicators
- Device name input on first launch

### TTS Announcements

```yaml
action: notify.intercom_hub
data:
  message: "Dinner is ready!"
```

### Broadcast to Specific Room

```yaml
action: notify.intercom_hub
data:
  message: "Come to the kitchen!"
  target: "living_room"  # Optional: specific room or "all" for broadcast
```

### Lovelace PTT Card

Add the custom Lovelace card (v1.2.0) for dashboard integration:

1. Copy `intercom-ptt-card.js` to your HA `www/` directory
2. Add as a resource in **Settings → Dashboards → Resources**:
   - URL: `/local/intercom-ptt-card.js`
   - Type: JavaScript Module
3. Add to your dashboard:
   ```yaml
   type: custom:intercom-ptt-card
   ```

The card connects to the hub via HA ingress by default (no extra configuration needed when accessed through the HA frontend). For direct LAN access outside of HA, add the optional `hub_url` option:

```yaml
type: custom:intercom-ptt-card
hub_url: "ws://10.0.0.8:8099/ws"   # Optional: bypass ingress, connect directly
```

**v1.2.0 changes:** Fixed ingress WebSocket connection to use the add-on's stable ingress entry path (from `/addons/{slug}/info`) and set the ingress session cookie with the correct format matching the HA frontend (including the conditional `Secure` flag for HTTPS). Added `hub_url` direct connection option, inline SVG logo on the init overlay, centered header title via CSS grid, and card version indicator on the init overlay.

### Automations

```yaml
alias: Doorbell Announcement
description: "Announce when someone rings the doorbell"
triggers:
  - trigger: state
    entity_id:
      - binary_sensor.doorbell
    to: "on"
conditions: []
actions:
  - action: notify.intercom_hub
    data:
      message: "Someone is at the door"
mode: single
```

## Entities Created

| Entity | Type | Description |
|--------|------|-------------|
| `notify.intercom_hub` | Notify | Send TTS announcements |
| `sensor.intercom_hub_state` | Sensor | idle/transmitting/receiving |
| `number.intercom_hub_volume` | Number | Volume 0-100% |
| `switch.intercom_hub_mute` | Switch | Mute toggle |
| `select.intercom_hub_target` | Select | Target room selector |
| `switch.intercom_hub_agc` | Switch | Automatic Gain Control toggle |
| `select.intercom_hub_priority` | Select | Priority level (Normal/High/Emergency) |
| `switch.intercom_hub_dnd` | Switch | Do Not Disturb toggle |
| `button.intercom_hub_call` | Button | Send call/ring notification |

## Requirements

- Home Assistant with MQTT integration
- ESP32 intercoms on the same network subnet
- Mosquitto broker (or compatible MQTT broker)
- Piper add-on (optional, for TTS)

## Technical Details

- Uses UDP multicast (224.0.0.100:5005) to broadcast audio
- Audio encoded as Opus at 16kHz mono, 32kbps VBR (matches ESP32 firmware)
- WebSocket server for browser PTT clients (binary PCM + JSON control)
- Host networking required for multicast to work
- Individual client IDs prevent state cross-contamination between web clients
- First-to-talk collision avoidance with 500ms timeout

## Versions

| Component | Version |
|-----------|---------|
| Hub Python (`intercom_hub.py`) | 2.2.1 |
| Hub Add-on (`config.yaml`) | 2.1.0 |
| Lovelace PTT Card (`intercom-ptt-card.js`) | 1.2.0 |
