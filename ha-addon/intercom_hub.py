#!/usr/bin/env python3
"""
Intercom Hub - Home Assistant Add-on

Multicast intercom node that integrates with HA via MQTT.
Receives TTS/audio from HA and broadcasts to ESP32 intercoms.
Also receives audio from ESP32 intercoms and updates status.

Entities created:
- notify.intercom_hub: Send text (TTS) or URL to broadcast
- sensor.intercom_hub_status: Current state (idle/transmitting/receiving)
- number.intercom_hub_volume: Volume control
- switch.intercom_hub_mute: Mute toggle
"""

import os
import sys
import json
import socket
import struct
import hashlib
import threading
import subprocess
import time
import paho.mqtt.client as mqtt

try:
    import opuslib
except ImportError:
    print("opuslib not available")
    opuslib = None

# Configuration from environment
MQTT_HOST = os.environ.get('MQTT_HOST', 'core-mosquitto')
MQTT_PORT = int(os.environ.get('MQTT_PORT', '1883'))
MQTT_USER = os.environ.get('MQTT_USER', '')
MQTT_PASSWORD = os.environ.get('MQTT_PASSWORD', '')
DEVICE_NAME = os.environ.get('DEVICE_NAME', 'Intercom Hub')
MULTICAST_GROUP = os.environ.get('MULTICAST_GROUP', '224.0.0.100')
MULTICAST_PORT = int(os.environ.get('MULTICAST_PORT', '5005'))
PIPER_HOST = os.environ.get('PIPER_HOST', '') or 'core-piper'
_piper_port = os.environ.get('PIPER_PORT', '') or '10200'
PIPER_PORT = int(_piper_port) if _piper_port.isdigit() else 10200

# Audio settings (must match ESP32 firmware)
SAMPLE_RATE = 16000
CHANNELS = 1
FRAME_DURATION_MS = 20
FRAME_SIZE = SAMPLE_RATE * FRAME_DURATION_MS // 1000  # 320 samples
OPUS_BITRATE = 12000

# Generate unique device ID from hostname
def generate_device_id():
    hostname = socket.gethostname()
    h = hashlib.md5(hostname.encode()).digest()
    return h[:8]

DEVICE_ID = generate_device_id()
DEVICE_ID_STR = DEVICE_ID.hex()
UNIQUE_ID = f"intercom_{DEVICE_ID[4:].hex()}"

# MQTT topics
BASE_TOPIC = f"intercom/{UNIQUE_ID}"
AVAILABILITY_TOPIC = f"{BASE_TOPIC}/status"
STATE_TOPIC = f"{BASE_TOPIC}/state"
VOLUME_STATE_TOPIC = f"{BASE_TOPIC}/volume"
VOLUME_CMD_TOPIC = f"{BASE_TOPIC}/volume/set"
MUTE_STATE_TOPIC = f"{BASE_TOPIC}/mute"
MUTE_CMD_TOPIC = f"{BASE_TOPIC}/mute/set"

# Notify specific topics
NOTIFY_CMD_TOPIC = f"{BASE_TOPIC}/notify"

# State
current_volume = 100
is_muted = False
current_state = "idle"
sequence_num = 0
mqtt_client = None
tx_socket = None
rx_socket = None
last_rx_time = 0
rx_timeout = 0.5  # seconds before going back to idle


def create_tx_socket():
    """Create UDP socket for sending multicast."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    return sock


def create_rx_socket():
    """Create UDP socket for receiving multicast."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    # Bind to the multicast port
    sock.bind(('', MULTICAST_PORT))

    # Join multicast group
    mreq = struct.pack('4sl', socket.inet_aton(MULTICAST_GROUP), socket.INADDR_ANY)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    # Non-blocking for timeout handling
    sock.settimeout(0.1)

    return sock


def send_audio_packet(opus_data):
    """Send an audio packet via multicast."""
    global sequence_num, tx_socket

    if tx_socket is None:
        return

    # Build packet: device_id (8) + sequence (4) + opus_data
    packet = DEVICE_ID + struct.pack('>I', sequence_num) + opus_data
    sequence_num += 1

    try:
        tx_socket.sendto(packet, (MULTICAST_GROUP, MULTICAST_PORT))
    except Exception as e:
        print(f"Error sending multicast: {e}")


def receive_thread():
    """Thread to receive multicast audio from ESP32 devices."""
    global current_state, last_rx_time, rx_socket

    print("Receive thread started")

    while True:
        try:
            data, addr = rx_socket.recvfrom(1024)

            if len(data) < 12:  # Minimum: 8 byte ID + 4 byte seq
                continue

            # Parse packet
            sender_id = data[:8]

            # Ignore our own packets
            if sender_id == DEVICE_ID:
                continue

            sender_id_str = sender_id.hex()

            # Update state to receiving if we weren't already
            now = time.time()
            if current_state != "receiving" and current_state != "transmitting":
                current_state = "receiving"
                publish_state()
                print(f"Receiving audio from {sender_id_str}")

            last_rx_time = now

        except socket.timeout:
            # Check if we should go back to idle
            if current_state == "receiving":
                if time.time() - last_rx_time > rx_timeout:
                    current_state = "idle"
                    publish_state()
                    print("Receive ended, back to idle")
        except Exception as e:
            print(f"RX error: {e}")
            time.sleep(0.1)


def text_to_speech(text):
    """Convert text to PCM audio using Wyoming/Piper TTS."""
    print(f"TTS: {text}")

    try:
        import asyncio
        from wyoming.audio import AudioChunk, AudioStop
        from wyoming.tts import Synthesize
        from wyoming.event import Event, async_read_event, async_write_event

        async def do_tts():
            audio_data = b''
            sample_rate = 22050  # Piper default

            reader, writer = await asyncio.open_connection(PIPER_HOST, PIPER_PORT)

            # Send synthesize request
            synth = Synthesize(text=text)
            await async_write_event(synth.event(), writer)

            # Receive audio chunks
            while True:
                event = await async_read_event(reader)
                if event is None:
                    break

                if AudioChunk.is_type(event.type):
                    chunk = AudioChunk.from_event(event)
                    audio_data += chunk.audio
                    sample_rate = chunk.rate

                elif AudioStop.is_type(event.type):
                    break

            writer.close()
            await writer.wait_closed()

            return audio_data, sample_rate

        # Run async function
        audio_data, sample_rate = asyncio.run(do_tts())

        if len(audio_data) == 0:
            print("No audio data received from TTS")
            return None

        print(f"TTS complete: {len(audio_data)} bytes at {sample_rate}Hz")

        # Convert to our target format (16kHz mono 16-bit)
        if sample_rate != SAMPLE_RATE:
            cmd = [
                'ffmpeg', '-f', 's16le', '-ar', str(sample_rate), '-ac', '1',
                '-i', 'pipe:0',
                '-ar', str(SAMPLE_RATE), '-ac', str(CHANNELS),
                '-f', 's16le', '-acodec', 'pcm_s16le',
                'pipe:1'
            ]
            result = subprocess.run(cmd, input=audio_data, capture_output=True, timeout=30)
            if result.returncode == 0:
                return result.stdout
            else:
                print(f"Resample error: {result.stderr.decode()}")
                return audio_data

        return audio_data

    except ConnectionRefusedError:
        print(f"TTS connection refused - is Piper running at {PIPER_HOST}:{PIPER_PORT}?")
        return None
    except Exception as e:
        print(f"TTS error: {e}")
        import traceback
        traceback.print_exc()
        return None


def fetch_and_convert_audio(url):
    """Fetch audio from URL and convert to 16kHz mono PCM."""
    print(f"Fetching audio: {url}")

    try:
        # Use ffmpeg to fetch and convert in one step
        # Output: 16kHz, mono, 16-bit signed little-endian PCM
        cmd = [
            'ffmpeg', '-i', url,
            '-ar', str(SAMPLE_RATE),
            '-ac', str(CHANNELS),
            '-f', 's16le',
            '-acodec', 'pcm_s16le',
            '-'
        ]

        result = subprocess.run(cmd, capture_output=True, timeout=30)

        if result.returncode != 0:
            print(f"ffmpeg error: {result.stderr.decode()}")
            return None

        return result.stdout

    except subprocess.TimeoutExpired:
        print("Audio fetch timeout")
        return None
    except Exception as e:
        print(f"Error fetching audio: {e}")
        return None


def encode_and_broadcast(pcm_data):
    """Encode PCM to Opus and broadcast via multicast."""
    global current_state

    if len(pcm_data) == 0:
        return

    print(f"Broadcasting {len(pcm_data)} bytes of audio...")
    current_state = "transmitting"
    publish_state()

    try:
        # Create Opus encoder
        encoder = opuslib.Encoder(SAMPLE_RATE, CHANNELS, opuslib.APPLICATION_VOIP)
        encoder.bitrate = OPUS_BITRATE

        # Create silence frame for padding
        silence_pcm = bytes(FRAME_SIZE * 2)  # 640 bytes of silence
        silence_opus = encoder.encode(silence_pcm, FRAME_SIZE)

        # Use precise timing - calculate when each frame SHOULD be sent
        # This prevents timing drift over long transmissions
        frame_interval = FRAME_DURATION_MS / 1000.0  # 0.02 seconds
        start_time = time.monotonic()
        frame_count = 0

        # Send 10 silence frames first (200ms) to let ESP32 stabilize
        print("Sending lead-in silence...")
        for _ in range(10):
            send_audio_packet(silence_opus)
            frame_count += 1
            # Sleep until the next frame should be sent
            next_frame_time = start_time + (frame_count * frame_interval)
            sleep_time = next_frame_time - time.monotonic()
            if sleep_time > 0:
                time.sleep(sleep_time)

        # Process actual audio in 20ms frames (320 samples = 640 bytes)
        frame_bytes = FRAME_SIZE * 2  # 16-bit samples
        offset = 0

        while offset + frame_bytes <= len(pcm_data):
            frame = pcm_data[offset:offset + frame_bytes]

            # Encode frame to Opus
            opus_data = encoder.encode(frame, FRAME_SIZE)

            # Send via multicast
            send_audio_packet(opus_data)
            frame_count += 1

            # Sleep until the next frame should be sent (precise timing)
            next_frame_time = start_time + (frame_count * frame_interval)
            sleep_time = next_frame_time - time.monotonic()
            if sleep_time > 0:
                time.sleep(sleep_time)

            offset += frame_bytes

        # Send trailing silence (400ms) to flush ESP32 DMA buffers
        print("Sending trail-out silence...")
        for _ in range(20):
            send_audio_packet(silence_opus)
            frame_count += 1
            next_frame_time = start_time + (frame_count * frame_interval)
            sleep_time = next_frame_time - time.monotonic()
            if sleep_time > 0:
                time.sleep(sleep_time)

        print(f"Broadcast complete ({offset // frame_bytes} frames)")

    except Exception as e:
        print(f"Error encoding audio: {e}")

    finally:
        current_state = "idle"
        publish_state()


def play_media(url):
    """Handle play_media command - fetch, convert, broadcast."""
    # Run in thread to not block MQTT
    def _play():
        pcm_data = fetch_and_convert_audio(url)
        if pcm_data:
            encode_and_broadcast(pcm_data)

    thread = threading.Thread(target=_play)
    thread.start()


def announce(message):
    """Handle announce - either URL or text for TTS."""
    def _announce():
        if message.startswith("http://") or message.startswith("https://"):
            # It's a URL, fetch and play
            pcm_data = fetch_and_convert_audio(message)
        else:
            # It's text, use TTS
            pcm_data = text_to_speech(message)

        if pcm_data:
            encode_and_broadcast(pcm_data)

    thread = threading.Thread(target=_announce)
    thread.start()


def publish_discovery():
    """Publish Home Assistant MQTT discovery configs."""
    global mqtt_client

    device_info = {
        "identifiers": [DEVICE_ID_STR],
        "name": DEVICE_NAME,
        "model": "Intercom Hub",
        "manufacturer": "guywithacomputer",
        "sw_version": "1.2.0"
    }

    # Notify entity - send text (TTS) or URL to broadcast
    notify_config = {
        "name": DEVICE_NAME,
        "unique_id": f"{UNIQUE_ID}_notify",
        "object_id": f"{UNIQUE_ID}",
        "device": device_info,
        "availability_topic": AVAILABILITY_TOPIC,
        "command_topic": NOTIFY_CMD_TOPIC,
        "icon": "mdi:bullhorn"
    }

    mqtt_client.publish(
        f"homeassistant/notify/{UNIQUE_ID}/config",
        json.dumps(notify_config),
        retain=True
    )

    # Also clear the old media_player config if it exists
    mqtt_client.publish(
        f"homeassistant/media_player/{UNIQUE_ID}/config",
        "",
        retain=True
    )

    # Status sensor (same as ESP32)
    sensor_config = {
        "name": f"{DEVICE_NAME} Status",
        "unique_id": f"{UNIQUE_ID}_state",
        "object_id": f"{UNIQUE_ID}_state",
        "device": device_info,
        "state_topic": STATE_TOPIC,
        "availability_topic": AVAILABILITY_TOPIC,
        "icon": "mdi:phone-classic"
    }

    mqtt_client.publish(
        f"homeassistant/sensor/{UNIQUE_ID}_state/config",
        json.dumps(sensor_config),
        retain=True
    )

    # Volume number
    volume_config = {
        "name": f"{DEVICE_NAME} Volume",
        "unique_id": f"{UNIQUE_ID}_volume",
        "object_id": f"{UNIQUE_ID}_volume",
        "device": device_info,
        "state_topic": VOLUME_STATE_TOPIC,
        "command_topic": VOLUME_CMD_TOPIC,
        "availability_topic": AVAILABILITY_TOPIC,
        "min": 0,
        "max": 100,
        "step": 5,
        "unit_of_measurement": "%",
        "icon": "mdi:volume-high",
        "mode": "slider"
    }

    mqtt_client.publish(
        f"homeassistant/number/{UNIQUE_ID}_volume/config",
        json.dumps(volume_config),
        retain=True
    )

    # Mute switch
    mute_config = {
        "name": f"{DEVICE_NAME} Mute",
        "unique_id": f"{UNIQUE_ID}_mute",
        "object_id": f"{UNIQUE_ID}_mute",
        "device": device_info,
        "state_topic": MUTE_STATE_TOPIC,
        "command_topic": MUTE_CMD_TOPIC,
        "availability_topic": AVAILABILITY_TOPIC,
        "payload_on": "ON",
        "payload_off": "OFF",
        "icon": "mdi:volume-off"
    }

    mqtt_client.publish(
        f"homeassistant/switch/{UNIQUE_ID}_mute/config",
        json.dumps(mute_config),
        retain=True
    )

    print("Published HA discovery configs")


def publish_state():
    """Publish current state."""
    if mqtt_client and mqtt_client.is_connected():
        mqtt_client.publish(STATE_TOPIC, current_state, retain=True)


def publish_volume():
    """Publish current volume."""
    if mqtt_client and mqtt_client.is_connected():
        mqtt_client.publish(VOLUME_STATE_TOPIC, str(current_volume), retain=True)


def publish_mute():
    """Publish mute state."""
    if mqtt_client and mqtt_client.is_connected():
        mqtt_client.publish(MUTE_STATE_TOPIC, "ON" if is_muted else "OFF", retain=True)


def on_mqtt_connect(client, userdata, flags, reason_code, properties=None):
    """Handle MQTT connection."""
    print(f"Connected to MQTT broker (rc={reason_code})")

    # Subscribe to command topics
    client.subscribe(VOLUME_CMD_TOPIC)
    client.subscribe(MUTE_CMD_TOPIC)
    client.subscribe(NOTIFY_CMD_TOPIC)

    # Publish discovery
    publish_discovery()

    # Publish online status
    client.publish(AVAILABILITY_TOPIC, "online", retain=True)

    # Publish current state
    publish_state()
    publish_volume()
    publish_mute()


def on_mqtt_message(client, userdata, msg):
    """Handle MQTT messages."""
    global current_volume, is_muted

    topic = msg.topic
    payload = msg.payload.decode('utf-8')

    print(f"MQTT: {topic} = {payload}")

    if topic == VOLUME_CMD_TOPIC:
        try:
            current_volume = int(float(payload))
            current_volume = max(0, min(100, current_volume))
            publish_volume()
        except ValueError:
            pass

    elif topic == MUTE_CMD_TOPIC:
        is_muted = (payload.upper() == "ON")
        publish_mute()

    elif topic == NOTIFY_CMD_TOPIC:
        # Notify service sends message - can be text (TTS) or URL
        # HA sends: just the message text, or JSON with "message" field
        message = payload
        try:
            data = json.loads(payload)
            if isinstance(data, dict):
                message = data.get("message", payload)
            elif isinstance(data, str):
                message = data
        except json.JSONDecodeError:
            pass

        if message:
            announce(message)


def on_mqtt_disconnect(client, userdata, disconnect_flags, reason_code, properties=None):
    """Handle MQTT disconnection."""
    print(f"Disconnected from MQTT (rc={reason_code})")


def main():
    global mqtt_client, tx_socket, rx_socket

    print("=" * 50)
    print("Intercom Hub v1.2.0")
    print(f"Device ID: {DEVICE_ID_STR}")
    print(f"Unique ID: {UNIQUE_ID}")
    print("=" * 50)

    # Create multicast sockets
    tx_socket = create_tx_socket()
    print(f"TX socket ready: {MULTICAST_GROUP}:{MULTICAST_PORT}")

    rx_socket = create_rx_socket()
    print(f"RX socket ready (joined multicast group)")

    # Start receive thread
    rx_thread = threading.Thread(target=receive_thread, daemon=True)
    rx_thread.start()

    # Connect to MQTT
    mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)

    if MQTT_USER:
        mqtt_client.username_pw_set(MQTT_USER, MQTT_PASSWORD)

    # Set last will (offline status)
    mqtt_client.will_set(AVAILABILITY_TOPIC, "offline", retain=True)

    mqtt_client.on_connect = on_mqtt_connect
    mqtt_client.on_message = on_mqtt_message
    mqtt_client.on_disconnect = on_mqtt_disconnect

    print(f"Connecting to MQTT: {MQTT_HOST}:{MQTT_PORT}")

    try:
        mqtt_client.connect(MQTT_HOST, MQTT_PORT, 60)
    except Exception as e:
        print(f"Failed to connect to MQTT: {e}")
        sys.exit(1)

    # Run MQTT loop
    print("Intercom Hub running...")
    mqtt_client.loop_forever()


if __name__ == "__main__":
    main()
