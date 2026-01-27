# Intercom Hub

A Home Assistant add-on that acts as a multicast intercom node, enabling TTS announcements to ESP32 intercom devices.

## Features

- Appears as a **notify** entity in Home Assistant
- Broadcasts TTS/audio to all ESP32 intercoms via UDP multicast
- Discover and route to individual ESP32 devices
- Volume and mute controls via MQTT entities
- Works with Piper TTS for text-to-speech

## Configuration

```yaml
mqtt_host: core-mosquitto      # MQTT broker hostname
mqtt_port: 1883                # MQTT broker port
mqtt_user: "homeassistant"     # MQTT username (required)
mqtt_password: "your_password" # MQTT password (required)
device_name: "Intercom Hub"    # Display name in HA
multicast_group: "224.0.0.100" # Must match ESP32 firmware
multicast_port: 5005           # Must match ESP32 firmware
piper_host: "core-piper"       # Piper TTS hostname
piper_port: 10200              # Piper TTS port
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
- Piper TTS add-on (for text-to-speech)

## ESP32 Intercom Devices

This add-on works with ESP32-S3 based intercom satellites. Each device acts as a room station with push-to-talk functionality.

**Hardware required per room (~$16):**
- ESP32-S3-DevKitC-1 (has built-in button and RGB LED)
- INMP441 I2S Microphone
- MAX98357A I2S Amplifier + Speaker

**Build instructions and firmware:**
[View the ESP32 firmware on GitHub](https://github.com/Bri-J-C/ha-intercom/tree/main/firmware)

## Technical Details

- Uses UDP multicast (224.0.0.100:5005) to broadcast audio
- Audio encoded as Opus at 16kHz mono, 12kbps (matches ESP32 firmware)
- Host networking required for multicast to work
