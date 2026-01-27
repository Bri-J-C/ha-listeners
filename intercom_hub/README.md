# Intercom Hub - Home Assistant Add-on

A Home Assistant add-on that acts as a multicast intercom node, enabling TTS announcements to ESP32 intercom devices.

## Features

- Appears as a **notify** entity in Home Assistant
- Broadcasts TTS/audio to all ESP32 intercoms via UDP multicast
- Discover and route to individual ESP32 devices
- Volume and mute controls via MQTT entities
- Works with Piper TTS for text-to-speech

## Installation

### Option 1: Local Add-on (Development)

1. Copy the `ha-addon` folder to `/addons/intercom_hub/` on your HA instance
2. Go to **Settings → Add-ons → Add-on Store**
3. Click the three dots menu → **Check for updates**
4. Find "Intercom Hub" under **Local add-ons**
5. Click **Install**

### Option 2: GitHub Repository

1. Go to **Settings → Add-ons → Add-on Store**
2. Click three dots → **Repositories**
3. Add: `https://github.com/yourusername/ha-intercom-addon`
4. Find and install "Intercom Hub"

## Configuration

```yaml
mqtt_host: core-mosquitto      # MQTT broker hostname
mqtt_port: 1883                # MQTT broker port
mqtt_user: ""                  # MQTT username (optional)
mqtt_password: ""              # MQTT password (optional)
device_name: "Intercom Hub"    # Display name in HA
multicast_group: "224.0.0.100" # Must match ESP32 firmware
multicast_port: 5005           # Must match ESP32 firmware
```

## Usage

### TTS Announcements

```yaml
service: notify.intercom_hub
data:
  message: "Dinner is ready!"
```

### Broadcast to Specific Room

```yaml
service: notify.intercom_hub
data:
  message: "Come to the kitchen!"
  target: "living_room"  # Optional: specific room or "all" for broadcast
```

### Automations

```yaml
automation:
  - alias: "Doorbell Announcement"
    trigger:
      - platform: state
        entity_id: binary_sensor.doorbell
        to: "on"
    action:
      - service: notify.intercom_hub
        data:
          message: "Someone is at the door"
```

## Entities Created

| Entity | Type | Description |
|--------|------|-------------|
| `notify.intercom_hub` | Notify | Send TTS announcements |
| `sensor.intercom_hub_state` | Sensor | idle/transmitting |
| `number.intercom_hub_volume` | Number | Volume 0-100% |
| `switch.intercom_hub_mute` | Switch | Mute toggle |
| `select.intercom_hub_target` | Select | Target room selector |

## Requirements

- Home Assistant with MQTT integration
- ESP32 intercoms on the same network subnet
- Mosquitto broker (or compatible MQTT broker)

## Technical Details

- Uses UDP multicast (224.0.0.100:5005) to broadcast audio
- Audio encoded as Opus at 16kHz mono, 12kbps (matches ESP32 firmware)
- Host networking required for multicast to work

## Version

1.3.0
