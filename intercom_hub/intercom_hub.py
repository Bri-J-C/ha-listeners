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
import concurrent.futures
import subprocess
import time
import asyncio
import logging
import urllib.request
import urllib.error
import re
import html
import base64
from pathlib import Path
from typing import Optional, Dict, Any
import paho.mqtt.client as mqtt

# Setup logging
LOG_LEVEL = os.environ.get('LOG_LEVEL', 'info').upper()
LOG_LEVELS = {
    'DEBUG': logging.DEBUG,
    'INFO': logging.INFO,
    'WARNING': logging.WARNING,
    'ERROR': logging.ERROR
}
logging.basicConfig(
    level=LOG_LEVELS.get(LOG_LEVEL, logging.INFO),
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%H:%M:%S'
)
log = logging.getLogger('intercom_hub')

# =============================================================================
# Input Validation and Sanitization (Security)
# =============================================================================

# Maximum lengths for user inputs
MAX_CLIENT_ID_LENGTH = 64
MAX_ROOM_NAME_LENGTH = 32
MAX_MESSAGE_LENGTH = 1024
MAX_URL_LENGTH = 2048

# Allowed characters for client IDs and room names (alphanumeric, spaces, underscores, dashes)
SAFE_NAME_PATTERN = re.compile(r'^[\w\s\-\.]+$', re.UNICODE)

# URL whitelist patterns for audio fetch (restrict to safe sources)
ALLOWED_URL_PATTERNS = [
    r'^https?://[a-zA-Z0-9\.\-]+/.*\.(mp3|wav|ogg|m4a)$',  # Audio file URLs
    r'^https?://(localhost|127\.0\.0\.1|10\.0\.0\.\d+|homeassistant|supervisor)/api/.*$',  # HA API endpoints only
]


def sanitize_string(value: str, max_length: int = 64) -> str:
    """Sanitize a string by removing dangerous characters and limiting length."""
    if not isinstance(value, str):
        return ""
    # Remove control characters and null bytes
    value = ''.join(c for c in value if c.isprintable() or c in ' \t')
    # Trim to max length
    return value[:max_length].strip()


def sanitize_client_id(client_id: str) -> Optional[str]:
    """Sanitize and validate a client ID."""
    if not client_id or not isinstance(client_id, str):
        return None
    client_id = sanitize_string(client_id, MAX_CLIENT_ID_LENGTH)
    if not client_id or not SAFE_NAME_PATTERN.match(client_id):
        log.warning(f"Invalid client_id rejected: {repr(client_id[:20])}")
        return None
    return client_id


def sanitize_room_name(room: str) -> Optional[str]:
    """Sanitize and validate a room name."""
    if not room or not isinstance(room, str):
        return None
    room = sanitize_string(room, MAX_ROOM_NAME_LENGTH)
    if not room or not SAFE_NAME_PATTERN.match(room):
        log.warning(f"Invalid room name rejected: {repr(room[:20])}")
        return None
    return room


def validate_ip_address(ip: str) -> bool:
    """Validate an IP address format."""
    if not ip or not isinstance(ip, str):
        return False
    # Simple IPv4 validation
    parts = ip.split('.')
    if len(parts) != 4:
        return False
    try:
        return all(0 <= int(p) <= 255 for p in parts)
    except ValueError:
        return False


def validate_url(url: str) -> bool:
    """Validate URL against whitelist patterns (for audio fetch)."""
    if not url or not isinstance(url, str):
        return False
    if len(url) > MAX_URL_LENGTH:
        return False
    for pattern in ALLOWED_URL_PATTERNS:
        if re.match(pattern, url, re.IGNORECASE):
            return True
    return False


def sanitize_json_payload(payload: str) -> Optional[Dict[str, Any]]:
    """Parse and validate JSON payload."""
    if not payload or not isinstance(payload, str):
        return None
    if len(payload) > MAX_MESSAGE_LENGTH:
        log.warning(f"JSON payload too large: {len(payload)} bytes")
        return None
    try:
        return json.loads(payload)
    except json.JSONDecodeError:
        return None


def html_escape(text: str) -> str:
    """Escape HTML special characters to prevent XSS."""
    return html.escape(text, quote=True)


# Version - single source of truth
VERSION = "2.5.7"  # Add AudioCaptureBuffer, /api/audio_capture, TX stats in audio_stats

try:
    from aiohttp import web
except ImportError:
    log.warning("aiohttp not available - web PTT disabled")
    web = None

try:
    import opuslib
except ImportError:
    log.warning("opuslib not available")
    opuslib = None

# Configuration from environment
MQTT_HOST = os.environ.get('MQTT_HOST', 'core-mosquitto')
MQTT_PORT = int(os.environ.get('MQTT_PORT', '1883'))
MQTT_USER = os.environ.get('MQTT_USER', '')
MQTT_PASSWORD = os.environ.get('MQTT_PASSWORD', '')
DEVICE_NAME = os.environ.get('DEVICE_NAME', 'Intercom Hub')
MULTICAST_GROUP = os.environ.get('MULTICAST_GROUP', '239.255.0.100')
MULTICAST_PORT = int(os.environ.get('MULTICAST_PORT', '5005'))
PIPER_HOST = os.environ.get('PIPER_HOST', '') or 'core-piper'
_piper_port = os.environ.get('PIPER_PORT', '') or '10200'
PIPER_PORT = int(_piper_port) if _piper_port.isdigit() else 10200

# Audio settings (must match ESP32 firmware)
SAMPLE_RATE = 16000
CHANNELS = 1
FRAME_DURATION_MS = 20
FRAME_SIZE = SAMPLE_RATE * FRAME_DURATION_MS // 1000  # 320 samples
OPUS_BITRATE = 32000  # Match ESP32 for consistent quality

# Protocol v2.5.0+: packet header now 13 bytes (8 device_id + 4 seq + 1 priority)
PACKET_HEADER_SIZE = 13  # Updated from 12 to include priority byte
# Priority levels (must match firmware protocol.h)
PRIORITY_NORMAL = 0
PRIORITY_HIGH = 1
PRIORITY_EMERGENCY = 2

# Broadcast sync: delay added to multicast packets so all receivers play at the same time

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

# Target selector topics
TARGET_STATE_TOPIC = f"{BASE_TOPIC}/target"
TARGET_CMD_TOPIC = f"{BASE_TOPIC}/target/set"

# Priority select topics
PRIORITY_STATE_TOPIC = f"{BASE_TOPIC}/priority"
PRIORITY_CMD_TOPIC = f"{BASE_TOPIC}/priority/set"

# DND switch topics
DND_STATE_TOPIC = f"{BASE_TOPIC}/dnd"
DND_CMD_TOPIC = f"{BASE_TOPIC}/dnd/set"

# Device discovery topic
DEVICE_INFO_TOPIC = "intercom/devices/+/info"

# State
current_volume = 100
is_muted = False
current_state = "idle"
current_target = "All Rooms"  # Default target
sequence_num = 0
mqtt_client = None
tx_socket = None
rx_socket = None
last_rx_time = 0
rx_timeout = 0.5  # seconds before going back to idle
channel_wait_timeout = 5.0  # max seconds to wait for channel before sending

# Transmission lock - prevent concurrent transmissions
tx_lock = threading.Lock()

# State lock - protects compound read-modify-write on critical shared state:
# current_state, web_ptt_active, last_web_ptt_frame_time (used in is_channel_busy)
state_lock = threading.Lock()

# Priority state
current_tx_priority = PRIORITY_NORMAL  # Hub's own TX priority
hub_dnd_enabled = False               # Hub DND: only EMERGENCY plays when on
current_rx_priority = PRIORITY_NORMAL  # Priority of whoever is currently transmitting

# Discovered devices: {unique_id: {"room": "Kitchen", "ip": "192.1.8.4.50"}}
discovered_devices = {}

# Web PTT state
web_clients = set()  # Connected WebSocket clients
web_client_ids = {}  # Map WebSocket -> client_id (e.g., "Brians_Phone", "Web_A1B2")
web_client_topics = {}  # Map client_id -> {"info": topic, "status": topic}
web_ptt_active = False  # Is a web client transmitting
last_web_ptt_frame_time = 0.0  # monotonic timestamp of last Web PTT audio frame
WEB_PTT_IDLE_TIMEOUT = 5.0  # seconds with no audio frames before auto-resetting stuck PTT state
web_ptt_encoder = None  # Opus encoder for web PTT
web_event_loop = None  # Event loop for async web operations
web_tx_lock = None  # Async lock to serialize web PTT transmissions (created at runtime)
INGRESS_PORT = int(os.environ.get('INGRESS_PORT', '8099'))
WWW_PATH = Path(__file__).parent / 'www'
# Chimes live in /data/chimes (persistent across rebuilds).
# Bundled defaults are in /chimes (baked into the container image).
CHIMES_PATH = Path('/data/chimes')
BUNDLED_CHIMES_PATH = Path(__file__).parent / 'chimes'

# Mobile devices config and topics
MOBILE_DEVICES = []  # List of {"name": "...", "notify_service": "..."}
MOBILE_CALL_TOPIC = "intercom/call"  # Publish call notifications here

# Track recent incoming calls for auto-select in Web PTT
recent_call = {"caller": None, "target": None, "timestamp": 0}  # Caller, target, and time
RECENT_CALL_TIMEOUT = 60  # Seconds to consider a call "recent"

# Track ESP32 targets (for targeted audio forwarding)
esp32_targets = {}  # Map device_id -> target_room

# Track current audio sender (for routing to correct web client)
current_audio_sender = None  # The device_id hex string of current transmitter

# Reusable Opus encoder for TTS/media broadcast (lazily initialized)
tts_encoder = None


# =============================================================================
# Multicast Metrics
# =============================================================================

class MulticastMetrics:
    """Track multicast TX/RX packet statistics for debugging audio issues."""

    def __init__(self):
        self.tx_packets = 0
        self.tx_errors = 0
        self.rx_packets = 0
        self.sequence_gaps = 0
        self.duplicates = 0
        self._sender_seqs: Dict[str, int] = {}  # sender_id_hex -> last sequence
        self._lock = threading.Lock()
        self._last_report = time.monotonic()

    def record_tx(self, success: bool = True):
        with self._lock:
            if success:
                self.tx_packets += 1
            else:
                self.tx_errors += 1

    def record_rx(self, sender_id_hex: str, sequence: int):
        with self._lock:
            self.rx_packets += 1
            last_seq = self._sender_seqs.get(sender_id_hex)
            if last_seq is not None:
                if sequence == last_seq:
                    self.duplicates += 1
                elif sequence > last_seq + 1:
                    self.sequence_gaps += sequence - last_seq - 1
            self._sender_seqs[sender_id_hex] = sequence

    def maybe_log_report(self, interval: float = 60.0):
        """Log a summary every `interval` seconds. Call from any thread."""
        now = time.monotonic()
        with self._lock:
            if now - self._last_report < interval:
                return
            tx_p, tx_e = self.tx_packets, self.tx_errors
            rx_p, gaps, dupes = self.rx_packets, self.sequence_gaps, self.duplicates
            # Reset counters for next window
            self.tx_packets = self.tx_errors = 0
            self.rx_packets = self.sequence_gaps = self.duplicates = 0
            self._sender_seqs.clear()
            self._last_report = now

        log.info(
            f"Multicast stats (60s): TX {tx_p} sent / {tx_e} errors, "
            f"RX {rx_p} received / {gaps} gaps / {dupes} dupes"
        )


mcast_metrics = MulticastMetrics()


# =============================================================================
# Audio RX Statistics (for QA / audio-path verification)
# =============================================================================

class AudioRxStats:
    """Per-sender UDP packet statistics for the hub's audio receive path.

    Records metadata for every Opus packet received from an ESP32 UDP sender,
    keyed by the sender's 8-byte device_id in hex.  Only tracks packets from
    receive_thread() (UDP multicast); web PTT audio arrives via WebSocket and
    is not counted here.  Designed to be called from receive_thread() (a
    non-async background thread) while the HTTP endpoint queries it from the
    asyncio event loop — hence the lock.

    The lock is held only for dict lookups and small in-place mutations, so
    contention on the hot RX path is minimal.
    """

    def __init__(self):
        # sender_id_hex -> stats dict
        self._data: Dict[str, Dict[str, Any]] = {}
        self._lock = threading.Lock()

    def record(self, sender_id_hex: str, sequence: int, priority: int) -> None:
        """Record one received packet.  Called from receive_thread() hot path.

        Args:
            sender_id_hex: Hex string of the 8-byte device_id from packet header.
            sequence:       32-bit sequence number from packet header.
            priority:       Priority byte (0=Normal, 1=High, 2=Emergency).
        """
        now = time.time()
        with self._lock:
            entry = self._data.get(sender_id_hex)
            if entry is None:
                self._data[sender_id_hex] = {
                    "first_rx": now,
                    "last_rx": now,
                    "packet_count": 1,
                    "seq_min": sequence,
                    "seq_max": sequence,
                    "priority": priority,
                }
            else:
                entry["last_rx"] = now
                entry["packet_count"] += 1
                if sequence < entry["seq_min"]:
                    entry["seq_min"] = sequence
                if sequence > entry["seq_max"]:
                    entry["seq_max"] = sequence
                entry["priority"] = priority

    def get_stats(
        self,
        window: float = 60.0,
        sender: Optional[str] = None,
        since: Optional[float] = None,
    ) -> Dict[str, Dict[str, Any]]:
        """Return a snapshot of sender stats, filtered by recency.

        Args:
            window: Only include senders whose last_rx is within this many
                    seconds of now.  Pass 0 to disable the window filter.
            sender: If set, return only the entry for this sender_id_hex.
            since:  If set, only include entries with last_rx >= since.

        Returns:
            Dict mapping sender_id_hex -> stats dict augmented with
            ``age_seconds`` and ``duration_seconds`` computed fields.
            Returns an empty dict if no matching entries.
        """
        now = time.time()
        cutoff = (now - window) if window > 0 else 0.0

        with self._lock:
            # Deep-copy each entry so the caller can never alias live data.
            snapshot = {k: dict(v) for k, v in self._data.items()}

        result: Dict[str, Dict[str, Any]] = {}
        for sid, entry in snapshot.items():
            if sender is not None and sid != sender:
                continue
            last_rx = entry["last_rx"]
            if window > 0 and last_rx < cutoff:
                continue
            if since is not None and last_rx < since:
                continue
            result[sid] = {
                "first_rx": entry["first_rx"],
                "last_rx": last_rx,
                "packet_count": entry["packet_count"],
                "seq_min": entry["seq_min"],
                "seq_max": entry["seq_max"],
                "priority": entry["priority"],
                "age_seconds": round(now - last_rx, 3),
                "duration_seconds": round(last_rx - entry["first_rx"], 3),
            }
        return result

    def clear(self, older_than: float = 300.0) -> int:
        """Remove entries older than ``older_than`` seconds.

        Args:
            older_than: Remove entries whose last_rx is more than this many
                        seconds ago.  Pass 0 to clear all entries.

        Returns:
            Number of entries removed.
        """
        cutoff = time.time() - older_than
        with self._lock:
            if older_than <= 0:
                count = len(self._data)
                self._data.clear()
                return count
            to_remove = [sid for sid, e in self._data.items() if e["last_rx"] < cutoff]
            for sid in to_remove:
                del self._data[sid]
            return len(to_remove)


audio_rx_stats = AudioRxStats()


class AudioCaptureBuffer:
    """Ring buffer storing recent Opus frames for QA audio analysis.

    Captures frames from both RX (receive_thread) and TX (send_audio_packet,
    _stream_chime_blocking) paths. Disabled by default — QA enables via
    POST /api/audio_capture {"action":"start"}.
    """

    def __init__(self, max_frames=2000):
        self._lock = threading.Lock()
        self._frames = []
        self._max_frames = max_frames
        self._enabled = False

    @property
    def enabled(self):
        return self._enabled

    def enable(self):
        with self._lock:
            self._enabled = True
            self._frames.clear()

    def disable(self):
        with self._lock:
            self._enabled = False

    def clear(self):
        with self._lock:
            self._frames.clear()

    def record(self, direction, device_id_hex, sequence, priority,
               opus_data, src_ip=None, target_ip=None):
        if not self._enabled:
            return
        frame = {
            "ts": time.time(),
            "dir": direction,
            "dev": device_id_hex,
            "seq": sequence,
            "pri": priority,
            "opus_b64": base64.b64encode(opus_data).decode('ascii'),
            "opus_len": len(opus_data),
        }
        if src_ip:
            frame["src_ip"] = src_ip
        if target_ip:
            frame["target_ip"] = target_ip
        with self._lock:
            self._frames.append(frame)
            if len(self._frames) > self._max_frames:
                self._frames = self._frames[-self._max_frames:]

    def get_frames(self, direction=None, device_id=None, since=None, limit=500):
        with self._lock:
            frames = self._frames[:]
        if direction:
            frames = [f for f in frames if f["dir"] == direction]
        if device_id:
            frames = [f for f in frames if f["dev"] == device_id]
        if since:
            frames = [f for f in frames if f["ts"] >= since]
        return frames[-limit:]


audio_capture = AudioCaptureBuffer()


# Chime state: pre-encoded chime frames, keyed by chime name (e.g. "doorbell")
# Loaded at startup; each entry is a list of Opus-encoded bytes objects.
loaded_chimes: Dict[str, list] = {}

# Selected chime name (controlled via HA select entity)
current_chime = "doorbell"

# MQTT topics for chime select
CHIME_STATE_TOPIC = f"{BASE_TOPIC}/chime"
CHIME_CMD_TOPIC = f"{BASE_TOPIC}/chime/set"


def get_local_ip():
    """Get the local IP address of this machine."""
    try:
        # Create a socket to determine our IP (doesn't actually connect)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def get_web_client_topics(client_id):
    """Get MQTT topics for a web client, creating if needed."""
    if client_id not in web_client_topics:
        # Create unique ID for this client (sanitize client_id for topic)
        safe_id = client_id.replace(' ', '_').replace('/', '_').lower()
        unique_id = f"{UNIQUE_ID}_web_{safe_id}"
        web_client_topics[client_id] = {
            "info": f"intercom/devices/{unique_id}/info",
            "status": f"intercom/{unique_id}/status",
            "unique_id": unique_id
        }
    return web_client_topics[client_id]


def publish_web_client_online(client_id):
    """Publish a web client as a discoverable target via MQTT."""
    if not mqtt_client or not mqtt_client.is_connected():
        return

    topics = get_web_client_topics(client_id)
    info = {
        "room": client_id,
        "ip": get_local_ip(),
        "id": topics["unique_id"]
    }
    payload = json.dumps(info, separators=(',', ':'))  # No spaces - ESP32 parser expects this

    # Publish online status FIRST to ensure it's retained before device info arrives
    mqtt_client.publish(topics["status"], "online", retain=True)
    mqtt_client.publish(topics["info"], payload, retain=True)
    log.info(f"Web client online: {client_id} ({topics['unique_id']})")


def publish_web_client_offline(client_id):
    """Mark a web client as offline via MQTT."""
    if not mqtt_client or not mqtt_client.is_connected():
        return

    if client_id not in web_client_topics:
        return

    topics = web_client_topics[client_id]
    mqtt_client.publish(topics["info"], "", retain=True)
    mqtt_client.publish(topics["status"], "offline", retain=True)
    log.info(f"Web client offline: {client_id}")


def discover_mobile_devices_from_ha():
    """Auto-discover mobile app devices from Home Assistant."""
    discovered = []

    # Get Supervisor token (available to all add-ons)
    supervisor_token = os.environ.get('SUPERVISOR_TOKEN')
    log.info(f"Mobile auto-discovery: SUPERVISOR_TOKEN={'present' if supervisor_token else 'missing'}")

    if not supervisor_token:
        log.info("No SUPERVISOR_TOKEN - running outside HA, skipping auto-discovery")
        return discovered

    try:
        # First check our add-on info to verify homeassistant_api access
        self_url = "http://supervisor/addons/self/info"
        req = urllib.request.Request(self_url)
        req.add_header("Authorization", f"Bearer {supervisor_token}")
        with urllib.request.urlopen(req, timeout=10) as response:
            addon_info = json.loads(response.read().decode())
            has_ha_api = addon_info.get("data", {}).get("homeassistant_api", False)
            log.info(f"Add-on homeassistant_api permission: {has_ha_api}")

        # We'll get device names later using template API after we know the device tracker IDs
        device_names = {}  # Map notify_service_suffix -> friendly name

        # Query HA services via Supervisor API
        url = "http://supervisor/core/api/services"
        log.info(f"Querying HA API: {url}")
        req = urllib.request.Request(url)
        req.add_header("Authorization", f"Bearer {supervisor_token}")
        req.add_header("Content-Type", "application/json")

        with urllib.request.urlopen(req, timeout=10) as response:
            services = json.loads(response.read().decode())

        # Find notify domain and look for mobile_app services
        for domain in services:
            if domain.get("domain") == "notify":
                for service in domain.get("services", {}):
                    if service.startswith("mobile_app_"):
                        # Extract device_id from service name (e.g., mobile_app_brians_phone -> brians_phone)
                        device_suffix = service.replace("mobile_app_", "")

                        # Use template API to get device name via device_attr()
                        # This gets the user-customized name from device registry
                        friendly_name = None
                        try:
                            template_url = "http://supervisor/core/api/template"
                            # Template: get device_id from device_tracker entity, then get name_by_user
                            template = (
                                f"{{% set dev = device_id('device_tracker.{device_suffix}') %}}"
                                "{{ device_attr(dev, 'name_by_user') or device_attr(dev, 'name') or '' }}"
                            )
                            template_data = json.dumps({"template": template}).encode('utf-8')
                            req = urllib.request.Request(template_url, data=template_data, method='POST')
                            req.add_header("Authorization", f"Bearer {supervisor_token}")
                            req.add_header("Content-Type", "application/json")
                            with urllib.request.urlopen(req, timeout=5) as response:
                                result = response.read().decode().strip()
                                if result and result != "None":
                                    friendly_name = result
                                    log.info(f"Device name from registry: {device_suffix} -> '{friendly_name}'")
                        except Exception as e:
                            log.debug(f"Could not get device name for {device_suffix}: {e}")

                        if not friendly_name:
                            # Fallback: convert service name to friendly name
                            friendly_name = device_suffix.replace("_", " ").title()
                            friendly_name = friendly_name.replace("Iphone", "iPhone")
                            friendly_name = friendly_name.replace("Ipad", "iPad")

                        discovered.append({
                            "name": friendly_name,
                            "notify_service": service
                        })
                        log.info(f"Auto-discovered mobile: {friendly_name} ({service})")

    except urllib.error.URLError as e:
        log.warning(f"Could not connect to HA API for mobile discovery: {e}")
    except Exception as e:
        log.warning(f"Mobile device auto-discovery failed: {e}")

    return discovered


def load_mobile_devices():
    """Load mobile devices - auto-discover from HA and merge with manual config."""
    global MOBILE_DEVICES

    # First, auto-discover from Home Assistant
    auto_discovered = discover_mobile_devices_from_ha()

    # Then load any manual overrides from options
    manual_devices = []
    try:
        options_path = Path("/data/options.json")
        if options_path.exists():
            with open(options_path) as f:
                options = json.load(f)
                manual_devices = options.get("mobile_devices", [])
    except Exception as e:
        log.warning(f"Could not load mobile device options: {e}")

    # Merge: manual config takes precedence (by notify_service)
    manual_services = {d["notify_service"] for d in manual_devices}

    # Start with manual devices
    MOBILE_DEVICES = list(manual_devices)

    # Add auto-discovered devices that aren't manually configured
    for dev in auto_discovered:
        if dev["notify_service"] not in manual_services:
            MOBILE_DEVICES.append(dev)

    if MOBILE_DEVICES:
        log.info(f"Total mobile devices: {len(MOBILE_DEVICES)}")
        for dev in MOBILE_DEVICES:
            source = "manual" if dev["notify_service"] in manual_services else "auto"
            log.info(f"  - {dev['name']} -> {dev['notify_service']} ({source})")

    # Mobile devices are discovered for notifications only — NOT published
    # as selectable audio targets (sending audio to a phone is useless).


def mobile_refresh_thread():
    """Background thread to periodically refresh mobile device list."""
    REFRESH_INTERVAL = 300  # 5 minutes

    while True:
        time.sleep(REFRESH_INTERVAL)
        try:
            old_devices = {d["notify_service"]: d["name"] for d in MOBILE_DEVICES}
            load_mobile_devices()
            new_devices = {d["notify_service"]: d["name"] for d in MOBILE_DEVICES}

            # Log any changes
            if old_devices != new_devices:
                log.info("Mobile device list updated")
        except Exception as e:
            log.warning(f"Mobile refresh failed: {e}")


def publish_mobile_devices():
    """Publish mobile devices as discoverable targets."""
    if not mqtt_client or not mqtt_client.is_connected():
        return

    hub_ip = get_local_ip()  # Use hub's IP so ESP32 can unicast to us

    for i, device in enumerate(MOBILE_DEVICES):
        device_id = f"{UNIQUE_ID}_mobile_{i}"
        topic = f"intercom/devices/{device_id}/info"
        status_topic = f"intercom/{device_id}/status"

        info = {
            "room": device["name"],
            "ip": hub_ip,  # Hub's IP - ESP32 unicasts here, hub forwards to web client
            "id": device_id,
            "is_mobile": True  # Mark as mobile for special handling
        }
        payload = json.dumps(info, separators=(',', ':'))

        # Publish as online and available
        mqtt_client.publish(status_topic, "online", retain=True)
        mqtt_client.publish(topic, payload, retain=True)
        log.info(f"Published mobile device: {device['name']} at {hub_ip}")


def send_mobile_notification(device_name, caller_name):
    """Send notification to mobile device via HA REST API."""
    # Find the mobile device config
    device_config = None
    for dev in MOBILE_DEVICES:
        if dev["name"] == device_name:
            device_config = dev
            break

    if not device_config:
        log.warning(f"Mobile device not found: {device_name}")
        return

    notify_service = device_config["notify_service"]

    # Get Supervisor token for HA API access
    supervisor_token = os.environ.get('SUPERVISOR_TOKEN')
    if not supervisor_token:
        log.warning("No SUPERVISOR_TOKEN - cannot send notification")
        return

    try:
        # Call HA notification service via REST API
        url = f"http://supervisor/core/api/services/notify/{notify_service}"

        # URL-safe caller name for auto-select in Web PTT
        # Include both caller (who's calling) and device (this mobile's identity)
        caller_safe = caller_name.replace(' ', '_')
        device_safe = device_name.replace(' ', '_')
        ingress_url = f"/hassio/ingress/local_intercom_hub?caller={caller_safe}&device={device_safe}"

        payload = {
            "title": "Intercom Call",
            "message": f"{caller_name} is calling",
            "data": {
                "channel": "intercom",
                "importance": "high",
                "ttl": 0,
                "priority": "high",
                "tag": "intercom_call",
                "clickAction": ingress_url,
                "actions": [
                    {
                        "action": "URI",
                        "title": "Answer",
                        "uri": ingress_url
                    }
                ]
            }
        }

        data = json.dumps(payload).encode('utf-8')
        req = urllib.request.Request(url, data=data, method='POST')
        req.add_header("Authorization", f"Bearer {supervisor_token}")
        req.add_header("Content-Type", "application/json")

        with urllib.request.urlopen(req, timeout=10) as response:
            log.info(f"Sent notification to {device_name} from {caller_name}")
            # Track recent call for targeted audio forwarding and auto-select
            recent_call["caller"] = caller_name
            recent_call["target"] = device_name
            recent_call["timestamp"] = time.time()

    except Exception as e:
        log.warning(f"Failed to send notification to {device_name}: {e}")


def is_mobile_device(room_name):
    """Check if a room name is a mobile device."""
    return any(dev["name"] == room_name for dev in MOBILE_DEVICES)


def create_tx_socket():
    """Create UDP socket for sending multicast."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    # Disable multicast loopback - prevent receiving our own packets
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 0)
    # NOTE: No IP_MULTICAST_IF — HA add-on containers route through Docker
    # bridge (172.30.x.x) even with host_network=true.  Letting the kernel
    # pick the interface via INADDR_ANY works correctly with host_network.
    return sock


def create_rx_socket():
    """Create UDP socket for receiving multicast."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    # Increase receive buffer for burst absorption (64KB holds ~325 frames)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)

    # Bind to the multicast port
    sock.bind(('', MULTICAST_PORT))

    # Join multicast group (INADDR_ANY lets the kernel pick the interface)
    mreq = struct.pack('4sl', socket.inet_aton(MULTICAST_GROUP), socket.INADDR_ANY)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    # Non-blocking for timeout handling
    sock.settimeout(0.1)

    return sock


def send_audio_packet(opus_data, target_ip=None, priority=None):
    """Send an audio packet via multicast or unicast.

    Args:
        opus_data: Opus encoded audio frame
        target_ip: If None, send multicast. Otherwise unicast to target_ip.
        priority: PRIORITY_NORMAL/HIGH/EMERGENCY — defaults to current_tx_priority.
    """
    global sequence_num, tx_socket, current_tx_priority

    if tx_socket is None:
        return

    if priority is None:
        priority = current_tx_priority

    # Header: device_id (8) + sequence (4) + priority (1) = 13 bytes
    header = DEVICE_ID + struct.pack('>IB', sequence_num, priority)
    sequence_num += 1
    packet = header + opus_data

    try:
        if target_ip:
            tx_socket.sendto(packet, (target_ip, MULTICAST_PORT))
        else:
            tx_socket.sendto(packet, (MULTICAST_GROUP, MULTICAST_PORT))
        mcast_metrics.record_tx(success=True)
        audio_capture.record("tx", DEVICE_ID_STR, sequence_num - 1, priority,
                            opus_data, target_ip=target_ip or MULTICAST_GROUP)
    except Exception as e:
        mcast_metrics.record_tx(success=False)
        mode = f"unicast to {target_ip}" if target_ip else "multicast"
        log.error(f"sending {mode}: {e}")

    mcast_metrics.maybe_log_report()


def get_target_ip():
    """Get the IP address for the current target, or None for multicast."""
    global current_target, discovered_devices

    if current_target == "All Rooms":
        return None

    # Find device by room name
    for device_id, info in discovered_devices.items():
        if info.get("room") == current_target:
            return info.get("ip")

    # Target not found, fall back to multicast
    log.warning(f"Target '{current_target}' not found, using multicast")
    return None


def _check_web_ptt_timeout():
    """Check if web PTT state is stuck and auto-reset if needed.

    Must be called WITHOUT state_lock held. Acquires state_lock internally.

    Returns True if state was auto-reset (caller should treat channel as free).
    """
    global web_ptt_active, current_state

    did_reset = False
    with state_lock:
        if web_ptt_active and last_web_ptt_frame_time > 0 and \
           time.monotonic() - last_web_ptt_frame_time > WEB_PTT_IDLE_TIMEOUT:
            log.warning(f"Web PTT idle for >{WEB_PTT_IDLE_TIMEOUT:.0f}s with no audio frames — auto-resetting to idle")
            web_ptt_active = False
            current_state = "idle"
            did_reset = True

    # Publish outside lock to avoid deadlock with MQTT/web threads
    if did_reset:
        publish_state(state="idle")

    return did_reset


def is_channel_busy(our_priority=None):
    """Check if channel is busy considering priority preemption.

    Args:
        our_priority: The TX priority we intend to use. If our priority is higher
                      than the current receiver's priority, the channel is NOT
                      considered busy (we can preempt). Pass None to use the hub's
                      current_tx_priority.

    Uses state_lock to ensure a consistent snapshot of the multiple
    shared variables (current_state, web_ptt_active, last_rx_time, last_web_ptt_frame_time)
    that are written from different threads.

    Side effect: if web_ptt_active is True but no audio frame has been
    received for WEB_PTT_IDLE_TIMEOUT seconds, auto-resets the stuck
    PTT state to idle (safety net for unclean WebSocket disconnects).
    """
    global current_tx_priority, current_rx_priority

    if our_priority is None:
        our_priority = current_tx_priority

    # Auto-reset stuck web PTT before checking state
    _check_web_ptt_timeout()

    with state_lock:
        # Channel busy if a web client is actively transmitting
        if web_ptt_active:
            return True

        # Channel busy if hub is already transmitting (TTS in progress)
        if current_state == "transmitting":
            return True

        # Channel busy if receiving audio
        if current_state == "receiving":
            # Double-check with timeout (receive_thread may not have updated yet)
            if time.time() - last_rx_time > rx_timeout:
                return False
            # Priority preemption: if our priority beats the current RX priority, not blocked
            if our_priority > current_rx_priority:
                return False
            return True

        return False


def wait_for_channel(timeout=None, our_priority=None):
    """Wait for the channel to be free before transmitting.

    Args:
        timeout: Max seconds to wait. Defaults to channel_wait_timeout.
        our_priority: Our TX priority for preemption check. Defaults to current_tx_priority.

    Returns:
        True if channel is free (or preemptable), False if timeout (will send anyway).
    """
    if timeout is None:
        timeout = channel_wait_timeout
    if our_priority is None:
        our_priority = current_tx_priority

    if not is_channel_busy(our_priority):
        return True

    log.debug("Channel busy - waiting for it to be free...")
    start = time.time()

    while time.time() - start < timeout:
        if not is_channel_busy(our_priority):
            waited = time.time() - start
            log.debug(f"Channel free after {waited:.1f}s")
            return True
        time.sleep(0.1)

    log.warning(f"Channel busy timeout ({timeout}s) - sending anyway")
    return False


def receive_thread():
    """Thread to receive multicast audio from ESP32 devices."""
    global current_state, last_rx_time, rx_socket, current_audio_sender, current_rx_priority

    log.debug("Receive thread started")

    # Create Opus decoder for forwarding to web clients
    rx_decoder = None
    if opuslib:
        try:
            rx_decoder = opuslib.Decoder(SAMPLE_RATE, CHANNELS)
            log.debug("RX decoder created for web client forwarding")
        except Exception as e:
            log.error(f"Failed to create RX decoder: {e}")

    # Sequence tracking for PLC/FEC
    rx_last_seq = None
    rx_last_sender = None

    while True:
        try:
            data, addr = rx_socket.recvfrom(1024)

            if len(data) < PACKET_HEADER_SIZE:  # 13 bytes: 8 byte ID + 4 byte seq + 1 byte priority
                continue

            # Parse packet
            sender_id = data[:8]

            # Ignore our own packets
            if sender_id == DEVICE_ID:
                continue

            sender_id_str = sender_id.hex()
            sequence = struct.unpack('>I', data[8:12])[0]

            # Track multicast metrics
            mcast_metrics.record_rx(sender_id_str, sequence)
            mcast_metrics.maybe_log_report()

            # Extract priority byte (byte 12, added in v2.5.0)
            # Old firmware (12-byte header) also handled gracefully
            if len(data) >= PACKET_HEADER_SIZE:
                incoming_priority = data[12]
                if incoming_priority > PRIORITY_EMERGENCY:
                    incoming_priority = PRIORITY_NORMAL  # Clamp unknown values
                opus_frame = data[PACKET_HEADER_SIZE:]  # 13-byte header
            else:
                incoming_priority = PRIORITY_NORMAL
                opus_frame = data[12:]  # Old 12-byte header (legacy)

            # Record per-sender RX stats (before DND filter — counts all arriving packets)
            audio_rx_stats.record(sender_id_str, sequence, incoming_priority)
            audio_capture.record("rx", sender_id_str, sequence, incoming_priority,
                                opus_frame, src_ip=addr[0])

            # Hub DND check: only EMERGENCY plays through
            if hub_dnd_enabled and incoming_priority < PRIORITY_EMERGENCY:
                continue

            # Update current RX priority (used for preemption decisions)
            current_rx_priority = incoming_priority

            # Track current sender for audio routing
            current_audio_sender = sender_id_str

            # Get target room for this sender
            sender_mqtt_id = f"intercom_{sender_id_str[-8:]}"
            target = esp32_targets.get(sender_mqtt_id)

            # Update state to receiving if we weren't already
            now = time.time()
            with state_lock:
                state_changed = (current_state != "receiving" and current_state != "transmitting")
                if state_changed:
                    current_state = "receiving"
                last_rx_time = now

            if state_changed:
                # Publish MQTT state (for HA integration) - outside lock to avoid blocking
                if mqtt_client and mqtt_client.is_connected():
                    mqtt_client.publish(STATE_TOPIC, "receiving", retain=True)

                # Notify web clients - targeted if specific target, broadcast if "all rooms"
                is_broadcast = not target or target.lower() in ("all", "all rooms", "unknown")
                if not is_broadcast:
                    # Targeted transmission - only notify the specific web client
                    notify_targeted_web_client_state(target, "receiving")
                    log.info(f"Receiving audio from {sender_mqtt_id} -> {target}")
                else:
                    # Broadcast to all rooms - notify all web clients
                    notify_web_clients_state(state="receiving", source="hub")
                    log.info(f"Receiving broadcast audio from {sender_mqtt_id}")

            # Decode and forward to web clients (with PLC/FEC for packet loss recovery)
            if rx_decoder and web_clients and len(opus_frame) > 0:
                try:
                    # Reset tracking when sender changes (new transmission)
                    if sender_id_str != rx_last_sender:
                        rx_last_sender = sender_id_str
                        rx_last_seq = None
                        # Reset decoder so stale prediction state doesn't produce
                        # ghost audio in the first frames of the new stream
                        try:
                            rx_decoder.reset_state()
                        except Exception:
                            rx_decoder = opuslib.Decoder(SAMPLE_RATE, CHANNELS)

                    # Gap detection for PLC/FEC
                    if rx_last_seq is not None:
                        gap = (sequence - rx_last_seq - 1) & 0xFFFFFFFF
                        if 0 < gap <= 4:
                            if gap == 1:
                                # FEC: recover missing frame using data embedded in next packet
                                try:
                                    fec_pcm = rx_decoder.decode(opus_frame, FRAME_SIZE, decode_fec=True)
                                    if fec_pcm:
                                        forward_audio_to_web_clients(fec_pcm)
                                except Exception:
                                    pass
                            else:
                                # PLC: generate concealment audio for each missing frame
                                for _ in range(gap):
                                    try:
                                        plc_pcm = rx_decoder.decode(None, FRAME_SIZE)
                                        if plc_pcm:
                                            forward_audio_to_web_clients(plc_pcm)
                                    except Exception:
                                        break

                    rx_last_seq = sequence

                    # Decode current frame normally
                    pcm = rx_decoder.decode(opus_frame, FRAME_SIZE)
                    if pcm:
                        forward_audio_to_web_clients(pcm, priority=incoming_priority)
                except Exception as e:
                    pass  # Ignore decode errors

        except socket.timeout:
            # Check if we should go back to idle
            with state_lock:
                should_go_idle = (current_state == "receiving" and
                                  time.time() - last_rx_time > rx_timeout)
                if should_go_idle:
                    current_state = "idle"
                    current_audio_sender = None  # Clear sender when idle
                    current_rx_priority = PRIORITY_NORMAL  # Reset for next sender
            if should_go_idle:
                publish_state(state="idle")
                log.debug("Receive ended, back to idle")
        except Exception as e:
            log.error(f"RX error: {e}")
            time.sleep(0.1)


def text_to_speech(text):
    """Convert text to PCM audio using Wyoming/Piper TTS."""
    log.info(f"TTS: {text}")

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

        # Run async function on the existing event loop (thread-safe)
        # Using run_coroutine_threadsafe avoids creating a new event loop each call
        if web_event_loop is not None and web_event_loop.is_running():
            future = asyncio.run_coroutine_threadsafe(do_tts(), web_event_loop)
            try:
                audio_data, sample_rate = future.result(timeout=30)
            except concurrent.futures.TimeoutError:
                future.cancel()
                log.warning("TTS timed out after 30 seconds")
                return None
        else:
            # Fallback if web server hasn't started yet.
            # This is safe: text_to_speech() is only called from _announce(),
            # which runs in its own daemon thread (not the MQTT thread), so
            # blocking here with asyncio.run() won't cause MQTT keepalive timeout.
            audio_data, sample_rate = asyncio.run(do_tts())

        if len(audio_data) == 0:
            log.warning("No audio data received from TTS")
            return None

        log.info(f"TTS raw: {len(audio_data)} bytes at {sample_rate}Hz ({len(audio_data)//2} samples, {len(audio_data)/2/sample_rate:.2f}s)")

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
                log.info(f"TTS resampled: {len(result.stdout)} bytes at {SAMPLE_RATE}Hz ({len(result.stdout)//2} samples, {len(result.stdout)/2/SAMPLE_RATE:.2f}s)")
                return result.stdout
            else:
                log.error(f"Resample error: {result.stderr.decode()}")
                return None  # Don't return wrong sample rate data

        return audio_data

    except ConnectionRefusedError:
        log.warning(f"TTS connection refused - is Piper running at {PIPER_HOST}:{PIPER_PORT}?")
        return None
    except Exception as e:
        log.error(f"TTS error: {e}")
        import traceback
        traceback.print_exc()
        return None


def fetch_and_convert_audio(url):
    """Fetch audio from URL and convert to 16kHz mono PCM."""
    log.info(f"Fetching audio: {url}")

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
            log.error(f"ffmpeg error: {result.stderr.decode()}")
            return None

        return result.stdout

    except subprocess.TimeoutExpired:
        log.warning("Audio fetch timeout")
        return None
    except Exception as e:
        log.error(f"fetching audio: {e}")
        return None


def get_tts_encoder():
    """Get or create the reusable Opus encoder for TTS/media broadcast.

    Lazily initializes on first call. Reuses the same encoder across calls
    to avoid the overhead of creating a new opuslib.Encoder each time.
    Caller should call encoder.reset_state() before each use to clear
    stale prediction data between broadcasts.
    """
    global tts_encoder

    if tts_encoder is None and opuslib:
        tts_encoder = opuslib.Encoder(SAMPLE_RATE, CHANNELS, opuslib.APPLICATION_VOIP)
        tts_encoder.bitrate = OPUS_BITRATE
        try:
            tts_encoder.inband_fec = 1
            tts_encoder.packet_loss_perc = 10
        except (TypeError, AttributeError):
            pass  # opuslib version may have broken ctl_set property setters
        log.info("TTS Opus encoder initialized")

    return tts_encoder


def _convert_wav_to_16k_mono_pcm(raw: bytes, nchannels: int, sampwidth: int, framerate: int) -> bytes:
    """Convert raw WAV PCM bytes to 16kHz mono 16-bit signed little-endian PCM.

    Uses only stdlib (struct) — no numpy/scipy/audioop.

    Steps:
      1. Convert sample width to 16-bit if needed.
      2. Mix down to mono if stereo.
      3. Resample to 16 kHz via linear interpolation.

    Args:
        raw: Raw PCM bytes from wave.readframes().
        nchannels: Number of audio channels in raw (1 or 2).
        sampwidth: Sample width in bytes (1=8-bit, 2=16-bit, 3=24-bit, 4=32-bit).
        framerate: Source sample rate in Hz.

    Returns:
        Raw PCM bytes: 16-bit signed little-endian, mono, 16kHz.
    """
    import struct as _struct

    n_frames = len(raw) // (nchannels * sampwidth)

    # --- Step 1: Unpack to list of 16-bit mono samples ---
    # Map sampwidth -> struct format char (all little-endian, signed)
    if sampwidth == 1:
        # 8-bit WAV is unsigned; shift to signed
        fmt_in = f"{n_frames * nchannels}B"
        raw_samples = list(_struct.unpack(fmt_in, raw))
        # Convert unsigned 8-bit [0..255] to signed 16-bit [-32768..32767]
        raw_samples = [(s - 128) << 8 for s in raw_samples]
    elif sampwidth == 2:
        fmt_in = f"<{n_frames * nchannels}h"
        raw_samples = list(_struct.unpack(fmt_in, raw))
    elif sampwidth == 3:
        # 24-bit has no direct struct format; unpack manually
        raw_samples = []
        for i in range(n_frames * nchannels):
            b = raw[i * 3:(i + 1) * 3]
            val = b[0] | (b[1] << 8) | (b[2] << 16)
            if val >= 0x800000:
                val -= 0x1000000
            raw_samples.append(val >> 8)  # Scale to 16-bit
    elif sampwidth == 4:
        fmt_in = f"<{n_frames * nchannels}i"
        raw_samples = list(_struct.unpack(fmt_in, raw))
        raw_samples = [s >> 16 for s in raw_samples]  # Scale to 16-bit
    else:
        raise ValueError(f"Unsupported sample width: {sampwidth} bytes")

    # --- Step 2: Stereo -> mono (average channels) ---
    if nchannels == 2:
        mono = []
        for i in range(0, len(raw_samples), 2):
            avg = (raw_samples[i] + raw_samples[i + 1]) // 2
            mono.append(max(-32768, min(32767, avg)))
    elif nchannels == 1:
        mono = raw_samples
    else:
        # Multi-channel: average all channels per frame
        mono = []
        for i in range(0, len(raw_samples), nchannels):
            avg = sum(raw_samples[i:i + nchannels]) // nchannels
            mono.append(max(-32768, min(32767, avg)))

    # --- Step 3: Resample to 16kHz via linear interpolation ---
    target_rate = SAMPLE_RATE  # 16000
    if framerate == target_rate:
        resampled = mono
    else:
        ratio = framerate / target_rate  # Source samples per output sample
        n_out = int(len(mono) / ratio)
        resampled = []
        for i in range(n_out):
            pos = i * ratio
            lo = int(pos)
            hi = lo + 1
            frac = pos - lo
            if hi >= len(mono):
                resampled.append(mono[lo])
            else:
                val = int(mono[lo] * (1.0 - frac) + mono[hi] * frac)
                resampled.append(max(-32768, min(32767, val)))

    # --- Pack to bytes (16-bit signed little-endian) ---
    return _struct.pack(f"<{len(resampled)}h", *resampled)


def load_chime(filepath: Path) -> list:
    """Load a WAV file and pre-encode it as a list of Opus frames.

    Converts the WAV to 16kHz mono 16-bit PCM, then encodes each 20ms
    frame (320 samples = 640 bytes) using a dedicated Opus encoder.
    Returns a list of encoded bytes objects (one per 20ms frame).

    A dedicated encoder is used (not tts_encoder) so chime state does not
    contaminate the TTS encoder's prediction history.

    Returns an empty list if opuslib is unavailable or the file is unreadable.
    """
    import wave as _wave

    if not opuslib:
        log.warning(f"opuslib not available — chime '{filepath.name}' will not be encoded")
        return []

    try:
        with _wave.open(str(filepath), 'rb') as wf:
            params = wf.getparams()
            raw = wf.readframes(params.nframes)
    except Exception as e:
        log.error(f"Failed to read chime file '{filepath}': {e}")
        return []

    log.info(
        f"Chime '{filepath.name}': {params.nchannels}ch, "
        f"{params.sampwidth*8}-bit, {params.framerate}Hz, "
        f"{params.nframes} frames ({params.nframes/params.framerate:.2f}s)"
    )

    try:
        pcm = _convert_wav_to_16k_mono_pcm(
            raw, params.nchannels, params.sampwidth, params.framerate
        )
    except Exception as e:
        log.error(f"WAV conversion failed for '{filepath.name}': {e}")
        return []

    log.info(
        f"Chime '{filepath.name}' converted: {len(pcm)} bytes PCM at 16kHz mono "
        f"({len(pcm)//(FRAME_SIZE*2)} frames)"
    )

    # Dedicated encoder — isolated from TTS encoder to avoid state pollution
    try:
        enc = opuslib.Encoder(SAMPLE_RATE, CHANNELS, opuslib.APPLICATION_VOIP)
        enc.bitrate = OPUS_BITRATE
        try:
            enc.inband_fec = 1
            enc.packet_loss_perc = 10
        except (TypeError, AttributeError):
            pass  # Some opuslib builds have broken ctl_set property setters
    except Exception as e:
        log.error(f"Failed to create Opus encoder for chime: {e}")
        return []

    frame_bytes = FRAME_SIZE * 2  # 640 bytes per 20ms frame
    frames = []
    offset = 0
    while offset < len(pcm):
        chunk = pcm[offset:offset + frame_bytes]
        if len(chunk) < frame_bytes:
            # Pad last frame to a full 20ms with silence
            chunk = chunk + b'\x00' * (frame_bytes - len(chunk))
        try:
            encoded = enc.encode(chunk, FRAME_SIZE)
            frames.append(encoded)
        except Exception as e:
            log.warning(f"Opus encode failed at frame {len(frames)}: {e}")
            break
        offset += frame_bytes

    log.info(f"Chime '{filepath.name}' encoded: {len(frames)} Opus frames")
    return frames


def _seed_persistent_chimes() -> None:
    """Copy bundled default chimes to persistent /data/chimes if not already present."""
    import shutil
    CHIMES_PATH.mkdir(parents=True, exist_ok=True)
    if not BUNDLED_CHIMES_PATH.exists():
        return
    for wav in BUNDLED_CHIMES_PATH.glob('*.wav'):
        dest = CHIMES_PATH / wav.name
        if not dest.exists():
            shutil.copy2(wav, dest)
            log.info(f"Copied bundled chime to persistent storage: {wav.name}")


def load_all_chimes() -> None:
    """Scan the chimes directory and pre-encode all WAV files into memory.

    The chime name is the filename stem (e.g., 'doorbell' for 'doorbell.wav').
    Results are stored in the module-level `loaded_chimes` dict.
    """
    global loaded_chimes

    # Ensure persistent dir exists and seed with bundled defaults
    _seed_persistent_chimes()

    if not CHIMES_PATH.exists():
        log.warning(f"Chimes directory not found: {CHIMES_PATH}")
        return

    wav_files = sorted(CHIMES_PATH.glob('*.wav'))
    if not wav_files:
        log.warning(f"No WAV files found in {CHIMES_PATH}")
        return

    for wav_path in wav_files:
        name = wav_path.stem  # e.g. "doorbell"
        frames = load_chime(wav_path)
        if frames:
            loaded_chimes[name] = frames
            log.info(f"Chime loaded: '{name}' ({len(frames)} frames)")
        else:
            log.warning(f"Chime '{name}' failed to load — skipped")

    log.info(f"Total chimes loaded: {len(loaded_chimes)} ({', '.join(loaded_chimes.keys())})")


def _stream_chime_blocking(target_ip: Optional[str], frames: list, chime_name: str) -> None:
    """Stream chime frames with precise timing (runs in a thread).

    Uses the same coarse-sleep + busy-wait pattern as encode_and_broadcast()
    for <1ms timing accuracy.  Runs via loop.run_in_executor() so it doesn't
    block the asyncio event loop.

    Acquires tx_lock and sets current_state to prevent concurrent TTS/chime
    streams from interleaving packets on the same socket.
    """
    global current_state

    # Prevent concurrent transmissions (same lock as encode_and_broadcast)
    if not tx_lock.acquire(blocking=False):
        log.warning(f"Chime '{chime_name}' skipped: transmission already in progress")
        return

    try:
        # Wait for channel to be free (chimes are PRIORITY_HIGH)
        wait_for_channel(our_priority=PRIORITY_HIGH)

        with state_lock:
            current_state = "transmitting"
        publish_state(state="transmitting")

        chime_seq = 0
        frame_interval = FRAME_DURATION_MS / 1000.0  # 0.020
        start_time = time.monotonic()
        consecutive_errors = 0
        first_frame_sent = False

        for i, opus_frame in enumerate(frames):
            packet = DEVICE_ID + struct.pack('>IB', chime_seq, PRIORITY_HIGH) + opus_frame
            chime_seq += 1

            try:
                if target_ip:
                    tx_socket.sendto(packet, (target_ip, MULTICAST_PORT))
                else:
                    tx_socket.sendto(packet, (MULTICAST_GROUP, MULTICAST_PORT))
                if not first_frame_sent:
                    first_frame_sent = True
                    log.debug(
                        f"Chime '{chime_name}' first frame sent at t+{(time.monotonic()-start_time)*1000:.1f}ms"
                    )
                mcast_metrics.record_tx(success=True)
                audio_capture.record("tx", DEVICE_ID_STR, chime_seq - 1, PRIORITY_HIGH,
                                    opus_frame, target_ip=target_ip or MULTICAST_GROUP)
                consecutive_errors = 0
            except Exception as e:
                mcast_metrics.record_tx(success=False)
                consecutive_errors += 1
                log.error(f"Chime send error at frame {i}: {e}")
                if consecutive_errors >= 5:
                    log.error("Chime aborted: 5 consecutive send errors")
                    break
                continue

            # Calculate target time for next frame
            next_frame_time = start_time + ((i + 1) * frame_interval)

            # Coarse sleep (to within 3ms of target)
            now = time.monotonic()
            sleep_time = next_frame_time - now - 0.003
            if sleep_time > 0:
                time.sleep(sleep_time)

            # Fine-grained busy-wait for precise timing
            while time.monotonic() < next_frame_time:
                pass

        elapsed = time.monotonic() - start_time
        expected = len(frames) * frame_interval
        drift_ms = (elapsed - expected) * 1000
        log.info(f"Chime '{chime_name}' complete: {chime_seq} frames in {elapsed:.2f}s (drift: {drift_ms:+.1f}ms)")

    except Exception as e:
        log.error(f"Chime '{chime_name}' error: {e}")

    finally:
        with state_lock:
            current_state = "idle"
        publish_state(state="idle")
        tx_lock.release()


async def stream_chime_to_target(target_ip: Optional[str], chime_name: str = "") -> None:
    """Stream pre-encoded chime frames to a target device (or multicast).

    Delegates to _stream_chime_blocking() in a thread for precise timing
    (coarse-sleep + busy-wait, same pattern as encode_and_broadcast).
    Uses PRIORITY_HIGH so the chime preempts ongoing NORMAL transmissions.

    Args:
        target_ip: Unicast destination IP, or None for multicast (All Rooms).
        chime_name: Which chime to play. Falls back to first available if not found.
    """
    frames = loaded_chimes.get(chime_name)
    if not frames:
        frames = next(iter(loaded_chimes.values()), None)
    if not frames:
        log.warning(f"No chime frames available (chime='{chime_name}') — skipping chime stream")
        return

    log.info(
        f"Streaming chime '{chime_name}' ({len(frames)} frames) -> "
        f"{'multicast' if target_ip is None else target_ip}"
    )

    loop = asyncio.get_running_loop()
    await loop.run_in_executor(None, _stream_chime_blocking, target_ip, frames, chime_name)


def get_chime_options() -> list:
    """Return sorted list of available chime names for HA select entity."""
    if not loaded_chimes:
        return ["doorbell"]  # Fallback label even if loading failed
    return sorted(loaded_chimes.keys())


def publish_chime_select() -> None:
    """Publish HA MQTT discovery config for the chime selector."""
    if not mqtt_client or not mqtt_client.is_connected():
        return

    device_info = {
        "identifiers": [DEVICE_ID_STR],
        "name": DEVICE_NAME,
        "model": "Intercom Hub",
        "manufacturer": "guywithacomputer",
        "sw_version": VERSION
    }

    options = get_chime_options()

    chime_config = {
        "name": f"{DEVICE_NAME} Chime",
        "unique_id": f"{UNIQUE_ID}_chime",
        "object_id": f"{UNIQUE_ID}_chime",
        "device": device_info,
        "state_topic": CHIME_STATE_TOPIC,
        "command_topic": CHIME_CMD_TOPIC,
        "availability_topic": AVAILABILITY_TOPIC,
        "options": options,
        "icon": "mdi:bell-ring"
    }

    mqtt_client.publish(
        f"homeassistant/select/{UNIQUE_ID}_chime/config",
        json.dumps(chime_config),
        retain=True
    )
    log.debug(f"Published chime select discovery: {options}")


def publish_chime() -> None:
    """Publish current chime selection to MQTT."""
    if mqtt_client and mqtt_client.is_connected():
        mqtt_client.publish(CHIME_STATE_TOPIC, current_chime, retain=True)


def encode_and_broadcast(pcm_data):
    """Encode PCM to Opus and send via multicast or unicast based on target.

    Uses two-phase approach for consistent timing:
    1. Pre-encode all frames (variable time, doesn't affect playback)
    2. Send with precise timing (busy-wait for accuracy)

    Also forwards raw PCM to web clients.
    """
    global current_state

    if len(pcm_data) == 0:
        return

    # Prepare raw PCM frames for web clients (need to send from async context)
    frame_bytes = FRAME_SIZE * 2
    pcm_frames = []
    offset = 0
    while offset + frame_bytes <= len(pcm_data):
        pcm_frames.append(pcm_data[offset:offset + frame_bytes])
        offset += frame_bytes

    # Prevent concurrent transmissions
    if not tx_lock.acquire(blocking=False):
        log.warning("Transmission already in progress, skipping")
        return

    try:
        # Wait for channel to be free (first-to-talk collision avoidance)
        wait_for_channel()

        # Mark as transmitting BEFORE encoding to prevent collisions during encode phase
        with state_lock:
            current_state = "transmitting"
        publish_state(state="transmitting")

        # Get target IP (None = multicast to all)
        target_ip = get_target_ip()
        target_desc = f"to {current_target}" if target_ip else "to all rooms"

        frame_bytes = FRAME_SIZE * 2  # 16-bit samples = 640 bytes per frame
        audio_frames = len(pcm_data) // frame_bytes
        audio_duration = audio_frames * FRAME_DURATION_MS / 1000.0
        log.info(f"Broadcasting: {audio_frames} audio frames ({audio_duration:.2f}s) + 15 lead-in + 30 trail-out = {audio_frames+45} total frames")

        # === PHASE 1: PRE-ENCODE ALL FRAMES ===
        # This separates encoding time from transmission timing
        encoder = get_tts_encoder()
        if encoder is None:
            log.error("No Opus encoder available for broadcast")
            return

        # Reset encoder state to prevent stale prediction data between broadcasts
        try:
            encoder.reset_state()
        except (AttributeError, Exception):
            log.debug("Opus encoder reset_state not available — skipping")

        encoded_frames = []
        silence_pcm = bytes(frame_bytes)

        # Lead-in silence (300ms = 15 frames) - lets ESP32 jitter buffer prime
        # Encode each frame fresh to maintain encoder state continuity
        for _ in range(15):
            silence_opus = encoder.encode(silence_pcm, FRAME_SIZE)
            encoded_frames.append(silence_opus)

        # Encode actual audio frames
        offset = 0
        while offset + frame_bytes <= len(pcm_data):
            frame = pcm_data[offset:offset + frame_bytes]
            opus_data = encoder.encode(frame, FRAME_SIZE)
            encoded_frames.append(opus_data)
            offset += frame_bytes

        # Trail-out silence (600ms = 30 frames) - flush ESP32 buffers
        # Must encode fresh to maintain decoder state continuity
        for _ in range(30):
            silence_opus = encoder.encode(silence_pcm, FRAME_SIZE)
            encoded_frames.append(silence_opus)

        # === PHASE 2: SEND WITH PRECISE TIMING ===
        log.debug(f"Sending {len(encoded_frames)} frames {target_desc}...")

        frame_interval = FRAME_DURATION_MS / 1000.0  # 0.02 seconds
        start_time = time.monotonic()

        # PCM frame index for web client forwarding (skip lead-in silence)
        pcm_frame_idx = 0

        for i, opus_data in enumerate(encoded_frames):
            # Send packet to ESP32s
            send_audio_packet(opus_data, target_ip)

            # Forward PCM to web clients (skip lead-in/trail-out silence)
            # Lead-in is first 15 frames, actual audio is next len(pcm_frames), trail-out is last 30
            if 15 <= i < 15 + len(pcm_frames) and web_clients and web_event_loop:
                pcm_frame = pcm_frames[pcm_frame_idx]
                pcm_frame_idx += 1
                try:
                    asyncio.run_coroutine_threadsafe(
                        broadcast_to_web_clients(pcm_frame),
                        web_event_loop
                    )
                except Exception:
                    pass

            # Calculate target time for next frame
            next_frame_time = start_time + ((i + 1) * frame_interval)

            # Coarse sleep (to within 3ms of target)
            now = time.monotonic()
            sleep_time = next_frame_time - now - 0.003
            if sleep_time > 0:
                time.sleep(sleep_time)

            # Fine-grained busy-wait for precise timing
            # This burns CPU but gives <1ms accuracy
            while time.monotonic() < next_frame_time:
                pass

        elapsed = time.monotonic() - start_time
        expected = len(encoded_frames) * frame_interval
        drift_ms = (elapsed - expected) * 1000
        log.info(f"Broadcast complete: {len(encoded_frames)} frames in {elapsed:.2f}s (expected {expected:.2f}s, drift: {drift_ms:+.1f}ms)")

    except Exception as e:
        log.error(f"encoding/sending audio: {e}")
        import traceback
        traceback.print_exc()

    finally:
        with state_lock:
            current_state = "idle"
        publish_state(state="idle")
        tx_lock.release()


def play_media(url):
    """Handle play_media command - fetch, convert, broadcast."""
    if not validate_url(url):
        log.warning(f"play_media URL rejected: {url[:100]}")
        return

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
        if message.startswith(("http://", "https://")):
            # It's a URL - validate before fetching
            if not validate_url(message):
                log.warning(f"Announce URL rejected by validation: {message[:100]}")
                return
            pcm_data = fetch_and_convert_audio(message)
        elif "://" in message:
            # Block non-HTTP schemes (file://, rtmp://, ftp://, etc.)
            log.warning(f"Announce blocked unsupported URL scheme: {message[:100]}")
            return
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
        "sw_version": VERSION
    }

    # Notify entity - send text (TTS) or URL to broadcast
    notify_config = {
        "name": DEVICE_NAME,
        "unique_id": f"{UNIQUE_ID}_notify",
        "default_entity_id": f"notify.{UNIQUE_ID}",
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
        "default_entity_id": f"sensor.{UNIQUE_ID}_state",
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
        "default_entity_id": f"number.{UNIQUE_ID}_volume",
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
        "default_entity_id": f"switch.{UNIQUE_ID}_mute",
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

    # Target room select - will be updated when devices are discovered
    update_target_select_options()

    # Priority select
    priority_config = {
        "name": f"{DEVICE_NAME} Priority",
        "unique_id": f"{UNIQUE_ID}_priority",
        "object_id": f"{UNIQUE_ID}_priority",
        "device": device_info,
        "state_topic": PRIORITY_STATE_TOPIC,
        "command_topic": PRIORITY_CMD_TOPIC,
        "availability_topic": AVAILABILITY_TOPIC,
        "options": ["Normal", "High", "Emergency"],
        "icon": "mdi:alert-circle-outline"
    }
    mqtt_client.publish(
        f"homeassistant/select/{UNIQUE_ID}_priority/config",
        json.dumps(priority_config),
        retain=True
    )

    # DND switch
    dnd_config = {
        "name": f"{DEVICE_NAME} Do Not Disturb",
        "unique_id": f"{UNIQUE_ID}_dnd",
        "object_id": f"{UNIQUE_ID}_dnd",
        "device": device_info,
        "state_topic": DND_STATE_TOPIC,
        "command_topic": DND_CMD_TOPIC,
        "availability_topic": AVAILABILITY_TOPIC,
        "payload_on": "ON",
        "payload_off": "OFF",
        "icon": "mdi:bell-sleep"
    }
    mqtt_client.publish(
        f"homeassistant/switch/{UNIQUE_ID}_dnd/config",
        json.dumps(dnd_config),
        retain=True
    )

    log.info("Published HA discovery configs")


def publish_state(state=None, notify_web=True, source="hub"):
    """Publish current state to MQTT and optionally web clients.

    Args:
        state: The state value to publish. If None, reads current_state (caller
               must ensure thread safety). Callers that set current_state under
               state_lock should pass the value to avoid a lockless read.
        notify_web: If True, also notify web clients. Set False when handling
                    web client notifications manually (e.g., in websocket_handler).
        source: Who triggered the state change ("hub" for TTS, "webclient" for PTT)
    """
    if state is None:
        state = current_state

    if mqtt_client and mqtt_client.is_connected():
        mqtt_client.publish(STATE_TOPIC, state, retain=True)

    # Also notify web clients (thread-safe) unless caller handles it
    if notify_web:
        notify_web_clients_state(state=state, source=source)


def notify_web_clients_state(state=None, source="hub"):
    """Notify web clients of state change (thread-safe).

    Args:
        state: The state value to broadcast. If None, reads current_state (caller
               must ensure thread safety).
        source: Who triggered the state change ("hub" or a client websocket)
                When hub transmits (TTS), clients should receive, not transmit.
    """
    global web_event_loop

    if not web_clients or web_event_loop is None:
        return

    if state is None:
        state = current_state

    # When HUB is transmitting (TTS/media), web clients should be "receiving"
    # Only when a specific web client is transmitting should THAT client be "transmitting"
    web_state = state
    if state == "transmitting" and source == "hub":
        web_state = "receiving"  # Hub TX means clients RX

    try:
        asyncio.run_coroutine_threadsafe(
            broadcast_to_web_clients({'type': 'state', 'status': web_state}),
            web_event_loop
        )
    except Exception as e:
        log.error(f"notifying web clients: {e}")


async def notify_single_web_client(target_device, state):
    """Notify a specific web client of state change."""
    if not web_clients or not target_device:
        return

    for client, client_id in list(web_client_ids.items()):
        if client_id == target_device:
            try:
                await client.send_json({'type': 'state', 'status': state})
                log.debug(f"Notified {client_id} of state: {state}")
            except Exception:
                web_clients.discard(client)
                if client in web_client_ids:
                    del web_client_ids[client]
            return


def notify_targeted_web_client_state(target_device, state):
    """Notify a specific web client of state change (thread-safe).

    Args:
        target_device: The client_id to notify (e.g., "Brians_Phone")
        state: The state to send ("receiving", "idle", etc.)
    """
    global web_event_loop

    if not web_clients or web_event_loop is None or not target_device:
        return

    try:
        asyncio.run_coroutine_threadsafe(
            notify_single_web_client(target_device, state),
            web_event_loop
        )
    except Exception as e:
        log.error(f"notifying targeted web client: {e}")


def forward_audio_to_web_clients(pcm_data, priority=None):
    """Forward audio to web clients (thread-safe).

    Args:
        pcm_data: Raw PCM bytes to forward.
        priority: PRIORITY_* constant — sent to web clients for DND/emergency handling.
    """
    global web_event_loop

    if priority is None:
        priority = PRIORITY_NORMAL

    if not web_clients or web_event_loop is None:
        return

    try:
        asyncio.run_coroutine_threadsafe(
            broadcast_audio_to_web_clients(pcm_data, priority=priority),
            web_event_loop
        )
    except Exception as e:
        log.error(f"forwarding audio to web clients: {e}")


def publish_volume():
    """Publish current volume."""
    if mqtt_client and mqtt_client.is_connected():
        mqtt_client.publish(VOLUME_STATE_TOPIC, str(current_volume), retain=True)


def publish_mute():
    """Publish mute state."""
    if mqtt_client and mqtt_client.is_connected():
        mqtt_client.publish(MUTE_STATE_TOPIC, "ON" if is_muted else "OFF", retain=True)


def publish_target():
    """Publish current target."""
    if mqtt_client and mqtt_client.is_connected():
        mqtt_client.publish(TARGET_STATE_TOPIC, current_target, retain=True)


def publish_priority():
    """Publish current hub TX priority."""
    if mqtt_client and mqtt_client.is_connected():
        names = {PRIORITY_NORMAL: "Normal", PRIORITY_HIGH: "High", PRIORITY_EMERGENCY: "Emergency"}
        mqtt_client.publish(PRIORITY_STATE_TOPIC, names.get(current_tx_priority, "Normal"), retain=True)


def publish_dnd():
    """Publish current hub DND state."""
    if mqtt_client and mqtt_client.is_connected():
        mqtt_client.publish(DND_STATE_TOPIC, "ON" if hub_dnd_enabled else "OFF", retain=True)


def get_target_options():
    """Get list of target options for the select entity."""
    options = ["All Rooms"]
    # Add discovered rooms (sorted alphabetically)
    rooms = sorted(set(d["room"] for d in discovered_devices.values()))
    options.extend(rooms)
    return options


def update_target_select_options():
    """Re-publish target select discovery with updated options."""
    if not mqtt_client or not mqtt_client.is_connected():
        return

    device_info = {
        "identifiers": [DEVICE_ID_STR],
        "name": DEVICE_NAME,
        "model": "Intercom Hub",
        "manufacturer": "guywithacomputer",
        "sw_version": VERSION
    }

    options = get_target_options()

    # Target room select
    target_config = {
        "name": "Target",
        "unique_id": f"{UNIQUE_ID}_target",
        "default_entity_id": f"select.{UNIQUE_ID}_target",
        "device": device_info,
        "state_topic": TARGET_STATE_TOPIC,
        "command_topic": TARGET_CMD_TOPIC,
        "availability_topic": AVAILABILITY_TOPIC,
        "options": options,
        "icon": "mdi:target",
        "has_entity_name": True
    }

    mqtt_client.publish(
        f"homeassistant/select/{UNIQUE_ID}_target/config",
        json.dumps(target_config),
        retain=True
    )

    # Ensure current target is still valid
    global current_target
    if current_target not in options:
        current_target = "All Rooms"
        publish_target()


def on_mqtt_connect(client, userdata, flags, reason_code, properties=None):
    """Handle MQTT connection."""
    log.info(f"Connected to MQTT broker (rc={reason_code})")

    # Subscribe to command topics
    client.subscribe(VOLUME_CMD_TOPIC)
    client.subscribe(MUTE_CMD_TOPIC)
    client.subscribe(NOTIFY_CMD_TOPIC)
    client.subscribe(TARGET_CMD_TOPIC)

    # Subscribe to device info for discovery
    client.subscribe(DEVICE_INFO_TOPIC)
    log.info(f"Subscribed to device discovery: {DEVICE_INFO_TOPIC}")

    # Subscribe to ESP32 state topics (for tracking targets)
    client.subscribe("intercom/+/state")
    log.info("Subscribed to ESP32 state topics")

    # Subscribe to device availability (LWT online/offline)
    client.subscribe("intercom/+/status")
    log.info("Subscribed to device availability: intercom/+/status")

    # Subscribe to call notifications (from ESP32s targeting mobile devices)
    client.subscribe(MOBILE_CALL_TOPIC)
    log.info(f"Subscribed to mobile call topic: {MOBILE_CALL_TOPIC}")

    # Subscribe to priority and DND commands
    client.subscribe(PRIORITY_CMD_TOPIC)
    client.subscribe(DND_CMD_TOPIC)
    log.info("Subscribed to priority and DND command topics")

    # Subscribe to chime select command
    client.subscribe(CHIME_CMD_TOPIC)
    log.info("Subscribed to chime select command topic")

    # Publish discovery
    publish_discovery()

    # Mobile devices are NOT published as selectable targets — sending audio
    # to a phone does nothing useful.  Notifications still work via automations.
    # Clear any previously retained mobile device entries from the broker,
    # including stale entries from previous runs with more devices.
    prev_max_path = Path("/data/mobile_device_max_index")
    prev_max = 0
    try:
        prev_max = int(prev_max_path.read_text().strip())
    except (FileNotFoundError, ValueError):
        pass
    clear_count = max(len(MOBILE_DEVICES), prev_max)
    for i in range(clear_count):
        device_id = f"{UNIQUE_ID}_mobile_{i}"
        client.publish(f"intercom/devices/{device_id}/info", "", retain=True)
        client.publish(f"intercom/{device_id}/status", "offline", retain=True)
    if clear_count > 0:
        log.info(f"Cleared {clear_count} mobile device entries from target list")
    # Persist current count for next restart
    try:
        prev_max_path.write_text(str(len(MOBILE_DEVICES)))
    except Exception as e:
        log.warning(f"Could not persist mobile device count: {e}")

    # Clear old "WebClients" aggregate device (replaced by individual web client tracking)
    old_web_topic = f"intercom/devices/{UNIQUE_ID}_web/info"
    old_web_status = f"intercom/{UNIQUE_ID}_web/status"
    client.publish(old_web_topic, "", retain=True)
    client.publish(old_web_status, "offline", retain=True)
    log.info("Cleared old WebClients aggregate device")

    # Clear any stale web client entries for mobile devices
    for mobile in MOBILE_DEVICES:
        safe_id = mobile["name"].replace(' ', '_').replace('/', '_').lower()
        stale_topic = f"intercom/devices/{UNIQUE_ID}_web_{safe_id}/info"
        stale_status = f"intercom/{UNIQUE_ID}_web_{safe_id}/status"
        client.publish(stale_topic, "", retain=True)
        client.publish(stale_status, "offline", retain=True)
    if MOBILE_DEVICES:
        log.info(f"Cleared stale web client entries for {len(MOBILE_DEVICES)} mobile device(s)")

    # Publish online status
    client.publish(AVAILABILITY_TOPIC, "online", retain=True)

    # Publish current state
    publish_state(state="idle")
    publish_volume()
    publish_mute()
    publish_target()
    publish_priority()
    publish_dnd()
    publish_chime_select()
    publish_chime()


def on_mqtt_message(client, userdata, msg):
    """Handle MQTT messages."""
    global current_volume, is_muted, current_target, discovered_devices
    global current_tx_priority, hub_dnd_enabled

    topic = msg.topic
    payload = msg.payload.decode('utf-8')

    # Don't log device info spam
    if not topic.startswith("intercom/devices/"):
        log.debug(f"MQTT: {topic} = {payload}")

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

    elif topic == TARGET_CMD_TOPIC:
        # Target room selection (sanitize input)
        sanitized_target = sanitize_room_name(payload)
        if sanitized_target:
            current_target = sanitized_target
            publish_target()
            log.info(f"Target set to: {current_target}")
        else:
            log.warning(f"Invalid target rejected from MQTT: {repr(payload[:20])}")

    elif topic == PRIORITY_CMD_TOPIC:
        # Priority select: "Normal", "High", "Emergency"
        priority_map = {"Normal": PRIORITY_NORMAL, "High": PRIORITY_HIGH, "Emergency": PRIORITY_EMERGENCY}
        current_tx_priority = priority_map.get(payload, PRIORITY_NORMAL)
        publish_priority()
        log.info(f"Hub TX priority set to: {payload} ({current_tx_priority})")

    elif topic == DND_CMD_TOPIC:
        hub_dnd_enabled = (payload.upper() == "ON")
        publish_dnd()
        log.info(f"Hub DND {'enabled' if hub_dnd_enabled else 'disabled'}")

    elif topic == CHIME_CMD_TOPIC:
        # Chime selection: payload is the chime name (e.g. "doorbell")
        global current_chime
        new_chime = sanitize_string(payload, 64).strip()
        if new_chime in loaded_chimes:
            current_chime = new_chime
            publish_chime()
            log.info(f"Chime set to: {current_chime}")
        elif loaded_chimes:
            # Requested chime not available — use first loaded chime
            current_chime = next(iter(loaded_chimes))
            publish_chime()
            log.warning(f"Chime '{new_chime}' not found, falling back to '{current_chime}'")

    elif topic.startswith("intercom/devices/") and topic.endswith("/info"):
        # Device info from ESP32 intercoms (validate inputs)
        try:
            data = json.loads(payload)
            if not isinstance(data, dict):
                return
            device_id = sanitize_string(data.get("id", ""), 64)
            room = sanitize_room_name(data.get("room", ""))
            ip = data.get("ip", "")

            # Validate all fields before accepting
            if device_id and room and validate_ip_address(ip):
                old_devices = set(discovered_devices.keys())
                discovered_devices[device_id] = {"room": room, "ip": ip}

                # If new device, update the select options
                if device_id not in old_devices:
                    log.info(f"Discovered device: {room} ({device_id}) at {ip}")
                    update_target_select_options()
            else:
                log.warning(f"Invalid device info rejected: id={repr(device_id[:20] if device_id else '')}")

        except json.JSONDecodeError:
            pass

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

    elif topic == MOBILE_CALL_TOPIC:
        # Call notification — stream chime audio to target, then handle mobile notification.
        # The chime is streamed from the hub so ESP32 devices no longer need local PCM data.
        try:
            data = json.loads(payload)
            if not isinstance(data, dict):
                return

            # Skip calls that originated from this hub (via WebSocket) to prevent
            # double-streaming: the WS call handler already streams the chime and
            # sends mobile notifications before publishing to MQTT.
            if data.get("source") == "hub":
                return

            target = sanitize_room_name(data.get("target", ""))
            caller = sanitize_room_name(data.get("caller", "Intercom")) or "Intercom"

            if not target:
                return

            log.info(f"Call: {caller} -> {target}")

            # Resolve target IP for unicast chime delivery.
            # 'All Rooms' / 'all' -> multicast (target_ip = None).
            target_lower = target.lower()
            if target_lower in ("all rooms", "all"):
                chime_target_ip = None
                # Send mobile notifications for all mobile devices when caller is an ESP32
                for dev_info in discovered_devices.values():
                    room = dev_info.get("room", "")
                    if room and is_mobile_device(room) and room != caller:
                        send_mobile_notification(room, caller)
            else:
                chime_target_ip = None
                for dev_info in discovered_devices.values():
                    if dev_info.get("room") == target:
                        chime_target_ip = dev_info.get("ip")
                        break
                if chime_target_ip is None:
                    log.warning(f"Chime for '{target}' skipped: target IP not found in discovered devices")
                    return  # Don't accidentally multicast a single-target call

            # Stream chime in background (async coroutine scheduled on the web event loop).
            # Capture chime_name and target_ip by value to avoid closure/late-binding issues.
            if loaded_chimes and web_event_loop is not None:
                _chime_name = current_chime   # Snapshot at dispatch time
                _target_ip = chime_target_ip  # Already a local variable
                _t_dispatch = time.monotonic()

                def _schedule_chime(_cn=_chime_name, _ip=_target_ip, _t=_t_dispatch):
                    lag_ms = (time.monotonic() - _t) * 1000
                    log.debug(f"Chime dispatch lag: {lag_ms:.1f}ms (MQTT->UDP)")
                    asyncio.run_coroutine_threadsafe(
                        stream_chime_to_target(_ip, _cn),
                        web_event_loop
                    )

                chime_thread = threading.Thread(target=_schedule_chime, daemon=True)
                chime_thread.start()
            else:
                log.debug("Chime not streamed: no chimes loaded or event loop unavailable")

            # Mobile notification (push alert) if target is a mobile device (single-room call)
            if target_lower not in ("all rooms", "all") and is_mobile_device(target):
                send_mobile_notification(target, caller)

        except Exception as e:
            if not isinstance(e, json.JSONDecodeError):
                log.warning(f"Error processing call message: {e}")

    elif topic.startswith("intercom/") and topic.endswith("/state"):
        # ESP32 state update - track target for audio routing
        # Topic format: intercom/<device_id>/state
        # Skip our own state topic
        if topic == STATE_TOPIC:
            return

        parts = topic.split("/")
        if len(parts) == 3:
            device_id = parts[1]
            try:
                data = json.loads(payload)
                if not isinstance(data, dict):
                    raise json.JSONDecodeError("not a dict", payload, 0)
                state = data.get("state", "")
                target = data.get("target", "")

                if state == "transmitting" and target:
                    # Track this ESP32's target
                    esp32_targets[device_id] = target
                    log.info(f"ESP32 {device_id} targeting: {target}")
                elif state == "idle":
                    # Clear target when idle
                    if device_id in esp32_targets:
                        del esp32_targets[device_id]
                        log.debug(f"ESP32 {device_id} target cleared")
            except json.JSONDecodeError:
                # Might be plain string like "idle", not JSON
                if payload == "idle" and device_id in esp32_targets:
                    del esp32_targets[device_id]

    elif topic.startswith("intercom/") and topic.endswith("/status"):
        # Device availability (LWT) - remove offline devices from discovery list
        # Topic: intercom/<unique_id>/status, payload: "online" or "offline"
        parts = topic.split("/")
        if len(parts) == 3:
            device_id = parts[1]
            if payload == "offline":
                if device_id in discovered_devices:
                    room = discovered_devices[device_id].get("room", device_id)
                    del discovered_devices[device_id]
                    log.info(f"Device offline, removed: {room} ({device_id})")
                    update_target_select_options()
                # Always clear retained device info to prevent zombie entries on broker.
                # Devices re-publish their info on reconnect, so clearing on offline is safe.
                client.publish(f"intercom/devices/{device_id}/info", "", retain=True, qos=1)
                log.debug(f"Cleared retained info for offline device: {device_id}")
            elif payload == "online":
                # Device came back online - info will re-populate via /info topic
                log.debug(f"Device online: {device_id}")


def on_mqtt_disconnect(client, userdata, disconnect_flags, reason_code, properties=None):
    """Handle MQTT disconnection."""
    log.warning(f"Disconnected from MQTT (rc={reason_code})")


# =============================================================================
# Web PTT Server (for ingress panel)
# =============================================================================

async def websocket_handler(request):
    """Handle WebSocket connections for web PTT."""
    global web_ptt_active, web_ptt_encoder, current_state, current_chime, last_web_ptt_frame_time

    log.debug(f"WebSocket connection request from {request.remote}")
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    log.debug(f"WebSocket prepared for {request.remote}")

    web_clients.add(ws)
    log.info(f"Web PTT client connected ({len(web_clients)} total)")

    # Note: encoder is created fresh at ptt_start, not here.
    # Creating it here caused crashes when opuslib's FEC property setter fails
    # (opuslib 3.x has a broken ctl_set implementation in some builds).

    # Check if this is a mobile app connection (for auto-naming)
    user_agent = request.headers.get('User-Agent', '')
    suggested_name = None
    if 'Home Assistant' in user_agent and MOBILE_DEVICES:
        # Use the first mobile device's friendly name
        suggested_name = MOBILE_DEVICES[0]["name"]
        log.info(f"Mobile app detected, suggesting name: {suggested_name}")

    # Send current state, version, and target list
    init_msg = {
        'type': 'init',
        'version': VERSION,
        'status': current_state
    }
    if suggested_name:
        init_msg['suggested_name'] = suggested_name
    await ws.send_json(init_msg)
    await ws.send_json({
        'type': 'targets',
        'rooms': sorted(set(d['room'] for d in discovered_devices.values() ))
    })

    # Send recent call info if within timeout (for auto-select)
    if recent_call["caller"] and time.time() - recent_call["timestamp"] < RECENT_CALL_TIMEOUT:
        await ws.send_json({
            'type': 'recent_call',
            'caller': recent_call["caller"]
        })
        log.info(f"Sent recent call info to Web PTT: {recent_call['caller']}")

    ptt_active = False
    ptt_target = None
    ptt_target_room = 'all'  # Track target room name for web client forwarding
    ptt_priority = PRIORITY_NORMAL  # Web client's chosen TX priority
    frame_count = 0
    lead_in_sent = False
    holding_lock = False  # Track if we hold web_tx_lock

    try:
        async for msg in ws:
            if msg.type == web.WSMsgType.BINARY:
                # PCM audio data from browser (640 bytes = 320 samples * 2 bytes)
                if ptt_active and web_ptt_encoder and len(msg.data) == FRAME_SIZE * 2:
                    # Send lead-in silence on first frame
                    if not lead_in_sent:
                        silence_pcm = bytes(FRAME_SIZE * 2)
                        silence_opus = web_ptt_encoder.encode(silence_pcm, FRAME_SIZE)
                        for _ in range(15):  # 300ms lead-in
                            send_audio_packet(silence_opus, ptt_target, priority=ptt_priority)
                            await asyncio.sleep(FRAME_DURATION_MS / 1000.0)
                        lead_in_sent = True

                    # Encode and send to ESP32s
                    opus_data = web_ptt_encoder.encode(msg.data, FRAME_SIZE)
                    send_audio_packet(opus_data, ptt_target, priority=ptt_priority)
                    frame_count += 1
                    last_web_ptt_frame_time = time.monotonic()

                    # Forward PCM to other web clients (respect target)
                    # Prepend priority byte so receiver can apply DND filtering
                    web_frame = bytes([ptt_priority]) + msg.data
                    is_broadcast = ptt_target_room.lower() in ('all', 'all rooms')
                    for other_client in web_clients.copy():
                        if other_client == ws:
                            continue
                        # Check if this client should receive
                        other_id = web_client_ids.get(other_client)
                        if is_broadcast or other_id == ptt_target_room:
                            try:
                                await other_client.send_bytes(web_frame)
                            except Exception:
                                pass

            elif msg.type == web.WSMsgType.TEXT:
                try:
                    data = json.loads(msg.data)
                    msg_type = data.get('type')

                    if msg_type == 'ptt_start':
                        # Read priority from web client (defaults to Normal)
                        raw_priority_str = data.get('priority', 'Normal')
                        priority_map = {'Normal': PRIORITY_NORMAL, 'High': PRIORITY_HIGH, 'Emergency': PRIORITY_EMERGENCY}
                        ptt_priority = priority_map.get(raw_priority_str, PRIORITY_NORMAL)

                        # Wait for any previous transmission to finish (including trail-out)
                        # This queues transmissions like TTS announcements do
                        if web_tx_lock:
                            await web_tx_lock.acquire()
                            holding_lock = True

                        # Check if channel is busy (receiving from ESP32)
                        if is_channel_busy(our_priority=ptt_priority):
                            if holding_lock:
                                web_tx_lock.release()
                                holding_lock = False
                            await ws.send_json({'type': 'busy'})
                        else:
                            ptt_active = True
                            with state_lock:
                                web_ptt_active = True
                            lead_in_sent = False
                            frame_count = 0

                            # Create FRESH encoder for this PTT session
                            # (critical: encoder state must not carry over between sessions)
                            if opuslib:
                                web_ptt_encoder = opuslib.Encoder(SAMPLE_RATE, CHANNELS, opuslib.APPLICATION_VOIP)
                                web_ptt_encoder.bitrate = OPUS_BITRATE
                                try:
                                    web_ptt_encoder.inband_fec = 1
                                    web_ptt_encoder.packet_loss_perc = 10
                                except (TypeError, AttributeError):
                                    pass  # opuslib version may have broken ctl_set property setters

                            # Get target IP (no automatic notifications - use Call button)
                            raw_target = data.get('target', 'all')
                            target_room = sanitize_room_name(raw_target) if raw_target != 'all' else 'all'
                            if target_room == 'all' or not target_room:
                                ptt_target = None
                                ptt_target_room = 'all'
                            else:
                                ptt_target = None
                                ptt_target_room = target_room
                                for dev in discovered_devices.values():
                                    if dev.get('room') == target_room:
                                        ptt_target = dev.get('ip')
                                        break

                            with state_lock:
                                current_state = "transmitting"
                            # Initialize frame timestamp so the idle timeout doesn't
                            # fire before the first audio frame arrives from the client.
                            last_web_ptt_frame_time = time.monotonic()
                            publish_state(state="transmitting", notify_web=False)  # MQTT only - we handle web clients below
                            log.info(f"Web PTT started -> {target_room}")

                            # Notify THIS client it's transmitting
                            await ws.send_json({'type': 'state', 'status': 'transmitting'})
                            # Notify OTHER web clients they're receiving
                            for other_client in web_clients.copy():
                                if other_client != ws:
                                    try:
                                        await other_client.send_json({'type': 'state', 'status': 'receiving'})
                                    except Exception:
                                        pass

                    elif msg_type == 'ptt_stop':
                        if ptt_active:
                            # Send trail-out silence - encode FRESH each frame
                            # (keeps codec state flowing naturally to end)
                            if web_ptt_encoder and lead_in_sent:
                                silence_pcm = bytes(FRAME_SIZE * 2)
                                for _ in range(30):  # 600ms trail-out
                                    silence_opus = web_ptt_encoder.encode(silence_pcm, FRAME_SIZE)
                                    send_audio_packet(silence_opus, ptt_target, priority=ptt_priority)
                                    await asyncio.sleep(FRAME_DURATION_MS / 1000.0)

                            ptt_active = False
                            with state_lock:
                                web_ptt_active = False
                                current_state = "idle"
                            # Reset encoder - prevents state carryover to next session
                            web_ptt_encoder = None
                            publish_state(state="idle", notify_web=False)  # MQTT only - we handle web clients below
                            log.info(f"Web PTT stopped ({frame_count} frames)")

                            # Notify web clients immediately - don't make them wait for the jitter drain gap
                            await broadcast_to_web_clients({'type': 'state', 'status': 'idle'})

                            # Gap for ESP32 jitter buffer to drain before next TX
                            await asyncio.sleep(0.75)

                            # Release lock AFTER trail-out + gap - allows queued TX to start
                            if holding_lock and web_tx_lock:
                                web_tx_lock.release()
                                holding_lock = False

                    elif msg_type == 'get_state':
                        await ws.send_json({
                            'type': 'state',
                            'status': current_state
                        })
                        # Send targets excluding self
                        my_id = web_client_ids.get(ws)
                        await ws.send_json({
                            'type': 'targets',
                            'rooms': sorted(set(d['room'] for d in discovered_devices.values() if d['room'] != my_id))
                        })

                    elif msg_type == 'set_target':
                        # Just acknowledge - actual target used at PTT start
                        pass

                    elif msg_type == 'identify':
                        # Client identifying itself (e.g., mobile device or Web_XXXX)
                        raw_client_id = data.get('client_id')
                        client_id = sanitize_client_id(raw_client_id)
                        if client_id:
                            # If this client had a different ID before, mark old one offline
                            old_id = web_client_ids.get(ws)
                            if old_id and old_id != client_id and not is_mobile_device(old_id):
                                publish_web_client_offline(old_id)

                            web_client_ids[ws] = client_id

                            publish_web_client_online(client_id)
                            if is_mobile_device(client_id):
                                log.info(f"Web client identified as mobile device: {client_id}")
                            else:
                                log.info(f"Web client identified as: {client_id}")

                            # Send updated targets list (excluding self)
                            await ws.send_json({
                                'type': 'targets',
                                'rooms': sorted(set(d['room'] for d in discovered_devices.values() if d['room'] != client_id))
                            })
                        else:
                            log.warning(f"Invalid client_id from {request.remote}")

                    elif msg_type == 'call':
                        # Call/notify a target device (or all rooms)
                        raw_target = data.get('target')
                        target = sanitize_room_name(raw_target)
                        caller_name = web_client_ids.get(ws, "Web PTT")
                        safe_caller = sanitize_string(caller_name, MAX_ROOM_NAME_LENGTH)

                        if raw_target in ('all', 'All Rooms'):
                            # Single MQTT message so all ESP32s receive it simultaneously —
                            # eliminates the per-device publish race condition where later
                            # messages arrive after the 150ms chime-detection window.
                            rooms_called = []
                            call_data = {
                                "target": "All Rooms",
                                "caller": safe_caller,
                                "source": "hub",
                                "chime": current_chime
                            }
                            _t_mqtt = time.monotonic()
                            mqtt_client.publish(MOBILE_CALL_TOPIC, json.dumps(call_data))
                            log.debug(f"Call all rooms MQTT published at t=0 (wall: {_t_mqtt:.3f})")

                            # Collect room names for logging; send mobile notifications separately
                            for dev in list(discovered_devices.values()):
                                room = dev.get('room', '')
                                if not room or room == caller_name:
                                    continue
                                if is_mobile_device(room):
                                    send_mobile_notification(room, caller_name)
                                rooms_called.append(room)

                            # Single multicast chime — all devices on the group receive it
                            if loaded_chimes and web_event_loop is not None:
                                _cn = current_chime
                                _t_chime = time.monotonic()
                                log.debug(
                                    f"Chime stream start: {(_t_chime - _t_mqtt)*1000:.1f}ms after MQTT publish"
                                )
                                asyncio.run_coroutine_threadsafe(
                                    stream_chime_to_target(None, _cn),
                                    web_event_loop
                                )
                            log.info(f"Call all rooms: {caller_name} -> {rooms_called}")
                        elif target:
                            # Send call notification via MQTT (ESP32s listen)
                            call_data = {
                                "target": target,
                                "caller": safe_caller,
                                "source": "hub",
                                "chime": current_chime
                            }
                            mqtt_client.publish(MOBILE_CALL_TOPIC, json.dumps(call_data))
                            log.info(f"Call: {caller_name} -> {target}")

                            # Stream chime to target
                            if loaded_chimes and web_event_loop is not None:
                                chime_target_ip = None
                                for dev_info in discovered_devices.values():
                                    if dev_info.get("room") == target:
                                        chime_target_ip = dev_info.get("ip")
                                        break
                                if chime_target_ip is None:
                                    log.warning(f"Chime for '{target}' skipped: target IP not found")
                                else:
                                    _cn = current_chime
                                    _ip = chime_target_ip
                                    asyncio.run_coroutine_threadsafe(
                                        stream_chime_to_target(_ip, _cn),
                                        web_event_loop
                                    )

                            # Also send mobile notification if target is mobile
                            if is_mobile_device(target):
                                send_mobile_notification(target, caller_name)
                        elif raw_target:
                            log.warning(f"Invalid call target rejected: {repr(str(raw_target)[:20])}")

                    elif msg_type == 'set_chime':
                        # Change active chime from web UI
                        new_chime = sanitize_string(data.get('chime', ''), 64).strip()
                        if new_chime in loaded_chimes:
                            current_chime = new_chime
                            publish_chime()
                            log.info(f"Chime set via web: {current_chime}")
                        else:
                            log.warning(f"Chime '{new_chime}' not found")

                except json.JSONDecodeError:
                    pass
                except Exception as msg_err:
                    # Catch send errors (e.g. connection closing mid-message) without
                    # crashing the entire WebSocket handler.
                    log.debug(f"WebSocket message handling error ({msg_type}): {msg_err}")

            elif msg.type == web.WSMsgType.ERROR:
                log.error(f"WebSocket error: {ws.exception()}")

    except Exception as e:
        log.error(f"WebSocket handler error: {e}")

    finally:
        # Clean up on disconnect
        if ptt_active:
            with state_lock:
                web_ptt_active = False
                current_state = "idle"
            publish_state(state="idle")

        # Release lock if still held (client disconnected while transmitting)
        if holding_lock and web_tx_lock:
            web_tx_lock.release()

        web_clients.discard(ws)
        # Clean up client identity and mark offline
        if ws in web_client_ids:
            client_id = web_client_ids[ws]
            del web_client_ids[ws]
            publish_web_client_offline(client_id)
        log.info(f"Web PTT client disconnected ({len(web_clients)} remaining)")

    return ws


async def broadcast_to_web_clients(message):
    """Send a message to all connected web clients."""
    if not web_clients:
        return

    for client in web_clients.copy():
        try:
            if isinstance(message, dict):
                await client.send_json(message)
            else:
                await client.send_bytes(message)
        except Exception:
            web_clients.discard(client)


async def broadcast_audio_to_web_clients(pcm_data, priority=PRIORITY_NORMAL):
    """Send audio PCM data to web clients (targeted based on sender's target).

    Prepends a 1-byte priority marker so web clients can apply DND / emergency logic.
    Binary frame format: [1 byte priority] [PCM bytes...]
    """
    if not web_clients:
        return

    # Prepend priority byte so the web client can handle it
    frame = bytes([priority]) + pcm_data

    # Determine target mobile device from the current sender
    target_device = None

    # Method 1: Check esp32_targets for the current sender
    # The MQTT unique_id format is "intercom_<last 4 bytes of device_id>"
    if current_audio_sender:
        # Try to find matching device in esp32_targets
        # Format: sender_id is full 8-byte hex, mqtt unique_id is intercom_<last 4 bytes>
        sender_suffix = current_audio_sender[-8:]  # Last 4 bytes as hex (8 chars)
        mqtt_unique_id = f"intercom_{sender_suffix}"

        if mqtt_unique_id in esp32_targets:
            target_device = esp32_targets[mqtt_unique_id]
            log.debug(f"Audio from {mqtt_unique_id} targeting: {target_device}")

    # "all", "All Rooms", or empty target means broadcast to all clients
    if not target_device or target_device.lower() in ("all", "all rooms"):
        clients_to_send = web_clients.copy()
    else:
        # Find the web client matching the target
        target_client = None
        for client, client_id in list(web_client_ids.items()):
            if client_id == target_device:
                target_client = client
                log.debug(f"Routing audio to web client: {client_id}")
                break

        # If target specified but client not connected, don't send to anyone
        if not target_client:
            log.debug(f"Target {target_device} not connected, dropping audio")
            return

        clients_to_send = [target_client]

    for client in clients_to_send:
        if client is None:
            continue
        try:
            await client.send_bytes(frame)
        except Exception:
            web_clients.discard(client)
            if client in web_client_ids:
                del web_client_ids[client]


async def index_handler(request):
    """Serve the main PTT page."""
    log.debug(f"Index request: {request.path}")
    response = web.FileResponse(WWW_PATH / 'index.html')
    response.headers['Cache-Control'] = 'no-cache, no-store, must-revalidate'
    return response


async def static_handler(request):
    """Serve static files."""
    filename = request.match_info.get('filename', 'index.html')
    filepath = WWW_PATH / filename
    log.debug(f"Static request: {request.path} -> {filepath}")

    if filepath.exists() and filepath.is_file():
        response = web.FileResponse(filepath)
        # Prevent caching for HTML/JS to ensure updates are seen
        if filename.endswith(('.html', '.js')):
            response.headers['Cache-Control'] = 'no-cache, no-store, must-revalidate'
        return response
    else:
        log.warning(f"File not found: {filepath}")
        raise web.HTTPNotFound()


# =============================================================================
# Chime Management API
# =============================================================================

MAX_CHIME_UPLOAD_SIZE = 5 * 1024 * 1024  # 5MB

# Allowed chime filename pattern: alphanumeric, dashes, underscores
SAFE_CHIME_NAME = re.compile(r'^[\w\-]+$')


async def chimes_list_handler(request):
    """GET /api/chimes — list available chimes with metadata."""
    chimes = []
    for name, frames in sorted(loaded_chimes.items()):
        duration = len(frames) * FRAME_DURATION_MS / 1000.0
        chimes.append({
            "name": name,
            "frames": len(frames),
            "duration": round(duration, 2),
        })
    return web.json_response({
        "chimes": chimes,
        "current": current_chime,
    })


async def chimes_upload_handler(request):
    """POST /api/chimes/upload — upload a WAV file as a new chime."""
    global loaded_chimes

    reader = await request.multipart()
    field = await reader.next()

    if field is None or field.name != 'file':
        return web.json_response({"error": "Missing 'file' field"}, status=400)

    filename = field.filename or ""
    if not filename.lower().endswith('.wav'):
        return web.json_response({"error": "Only .wav files are accepted"}, status=400)

    # Derive chime name from filename stem
    chime_name = Path(filename).stem.lower().replace(' ', '_')
    if not SAFE_CHIME_NAME.match(chime_name):
        return web.json_response({"error": "Invalid filename (use alphanumeric, dashes, underscores)"}, status=400)

    # Read file data with size limit
    data = bytearray()
    while True:
        chunk = await field.read_chunk(8192)
        if not chunk:
            break
        data.extend(chunk)
        if len(data) > MAX_CHIME_UPLOAD_SIZE:
            return web.json_response({"error": f"File too large (max {MAX_CHIME_UPLOAD_SIZE // (1024*1024)}MB)"}, status=400)

    if len(data) < 44:  # WAV header is at least 44 bytes
        return web.json_response({"error": "File too small to be a valid WAV"}, status=400)

    # Ensure chimes directory exists
    CHIMES_PATH.mkdir(parents=True, exist_ok=True)

    # Save the WAV file
    dest = CHIMES_PATH / f"{chime_name}.wav"
    dest.write_bytes(bytes(data))
    log.info(f"Chime uploaded: '{chime_name}' ({len(data)} bytes) -> {dest}")

    # Encode to Opus frames
    frames = load_chime(dest)
    if not frames:
        dest.unlink(missing_ok=True)
        return web.json_response({"error": "Failed to encode WAV (invalid format or codec error)"}, status=400)

    loaded_chimes[chime_name] = frames

    # Update HA select entity with new option
    publish_chime_select()

    duration = len(frames) * FRAME_DURATION_MS / 1000.0
    return web.json_response({
        "name": chime_name,
        "frames": len(frames),
        "duration": round(duration, 2),
    })


async def chimes_delete_handler(request):
    """DELETE /api/chimes/{name} — delete a chime WAV file."""
    global loaded_chimes, current_chime

    name = request.match_info.get('name', '')
    if not name or not SAFE_CHIME_NAME.match(name):
        return web.json_response({"error": "Invalid chime name"}, status=400)

    if name == "doorbell":
        return web.json_response({"error": "Cannot delete the default 'doorbell' chime"}, status=400)

    if name not in loaded_chimes:
        return web.json_response({"error": f"Chime '{name}' not found"}, status=404)

    # Remove from memory
    del loaded_chimes[name]

    # Delete WAV file
    wav_file = CHIMES_PATH / f"{name}.wav"
    wav_file.unlink(missing_ok=True)
    log.info(f"Chime deleted: '{name}'")

    # If deleted chime was active, switch to doorbell
    if current_chime == name:
        current_chime = "doorbell" if "doorbell" in loaded_chimes else next(iter(loaded_chimes), "doorbell")
        publish_chime()

    # Update HA select entity
    publish_chime_select()

    return web.json_response({"deleted": name, "current": current_chime})


# =============================================================================
# Audio RX Stats API
# =============================================================================

async def audio_stats_get_handler(request):
    """GET /api/audio_stats — query per-sender UDP receive statistics.

    Query parameters:
        window (int, default 60): Only include senders active within this
            many seconds.  Pass 0 to disable the window filter.
        sender (str, optional): Filter to a specific sender_id_hex.
        since (float, optional): Only include entries with last_rx >= since.

    Response JSON:
        {
            "current_state":  "idle"|"playing"|"receiving"|"transmitting",
            "current_sender": "<sender_id_hex>" or null,
            "senders": {
                "<sender_id_hex>": {
                    "first_rx":         <unix timestamp float>,
                    "last_rx":          <unix timestamp float>,
                    "packet_count":     <int>,
                    "seq_min":          <int>,
                    "seq_max":          <int>,
                    "priority":         <int 0|1|2>,
                    "age_seconds":      <float>,
                    "duration_seconds": <float>
                },
                ...
            }
        }
    """
    # Auto-reset stuck web PTT state so current_state is accurate
    _check_web_ptt_timeout()

    # --- Parse query parameters ---
    try:
        window = float(request.rel_url.query.get("window", "60"))
        if window < 0:
            window = 0.0
    except (ValueError, TypeError):
        return web.json_response({"error": "Invalid 'window' parameter"}, status=400)

    sender_filter = request.rel_url.query.get("sender")
    if sender_filter is not None:
        # Validate: must be a hex string (up to 16 chars for 8-byte device_id)
        sender_filter = sender_filter.strip().lower()
        if not re.fullmatch(r'[0-9a-f]{1,16}', sender_filter):
            return web.json_response({"error": "Invalid 'sender' parameter"}, status=400)

    since_filter = None
    since_raw = request.rel_url.query.get("since")
    if since_raw is not None:
        try:
            since_filter = float(since_raw)
        except (ValueError, TypeError):
            return web.json_response({"error": "Invalid 'since' parameter"}, status=400)

    # --- Build response ---
    senders = audio_rx_stats.get_stats(
        window=window,
        sender=sender_filter,
        since=since_filter,
    )

    return web.json_response({
        "current_state": current_state,
        "current_sender": current_audio_sender,
        "senders": senders,
        "tx": {
            "packets": mcast_metrics.tx_packets,
            "errors": mcast_metrics.tx_errors,
        },
    })


async def audio_stats_post_handler(request):
    """POST /api/audio_stats — clear audio RX stats.

    Request JSON (optional):
        { "older_than": <seconds float> }

    If ``older_than`` is omitted or 0, all entries are cleared.

    Response JSON:
        { "result": "ok", "cleared": <int> }
    """
    older_than: float = 0.0

    # Body is optional — ignore missing / non-JSON body and treat as clear-all.
    try:
        body = await request.json()
    except Exception:
        body = {}

    if isinstance(body, dict) and "older_than" in body:
        try:
            older_than = max(0.0, float(body["older_than"]))
        except (ValueError, TypeError):
            return web.json_response(
                {"error": "Invalid 'older_than' value — expected a number"},
                status=400,
            )

    cleared = audio_rx_stats.clear(older_than=older_than)
    # Also reset TX counters
    with mcast_metrics._lock:
        mcast_metrics.tx_packets = 0
        mcast_metrics.tx_errors = 0
    return web.json_response({"result": "ok", "cleared": cleared})


async def audio_capture_get_handler(request):
    """GET /api/audio_capture — fetch captured audio frames.

    Query params:
        direction: "rx" or "tx"
        device_id: filter by device_id hex
        since: unix timestamp float
        limit: max frames (default 500)
    """
    direction = request.rel_url.query.get("direction")
    device_id = request.rel_url.query.get("device_id")
    since = None
    since_raw = request.rel_url.query.get("since")
    if since_raw:
        try:
            since = float(since_raw)
        except (ValueError, TypeError):
            return web.json_response({"error": "Invalid 'since'"}, status=400)
    try:
        limit = int(request.rel_url.query.get("limit", "500"))
    except (ValueError, TypeError):
        limit = 500

    frames = audio_capture.get_frames(
        direction=direction, device_id=device_id, since=since, limit=limit)
    return web.json_response({
        "enabled": audio_capture.enabled,
        "frame_count": len(frames),
        "frames": frames,
    })


async def audio_capture_post_handler(request):
    """POST /api/audio_capture — control capture.

    Body: {"action": "start"|"stop"|"clear"}
    """
    try:
        body = await request.json()
    except Exception:
        return web.json_response({"error": "Invalid JSON"}, status=400)

    action = body.get("action", "").lower() if isinstance(body, dict) else ""
    if action == "start":
        audio_capture.enable()
        log.info("Audio capture enabled (QA)")
        return web.json_response({"result": "ok", "enabled": True})
    elif action == "stop":
        audio_capture.disable()
        log.info("Audio capture disabled")
        return web.json_response({"result": "ok", "enabled": False})
    elif action == "clear":
        audio_capture.clear()
        return web.json_response({"result": "ok", "cleared": True})
    else:
        return web.json_response({"error": "Unknown action. Use start/stop/clear."}, status=400)


def create_web_app():
    """Create the aiohttp web application."""
    app = web.Application(client_max_size=6 * 1024 * 1024)  # 6MB for chime uploads

    # Routes - order matters! Specific routes before wildcards
    # Use /ws instead of /api/websocket to avoid conflict with HA's WebSocket API
    app.router.add_get('/ws', websocket_handler)
    app.router.add_get('/api/chimes', chimes_list_handler)
    app.router.add_post('/api/chimes/upload', chimes_upload_handler)
    app.router.add_delete('/api/chimes/{name}', chimes_delete_handler)
    app.router.add_get('/api/audio_stats', audio_stats_get_handler)
    app.router.add_post('/api/audio_stats', audio_stats_post_handler)
    app.router.add_get('/api/audio_capture', audio_capture_get_handler)
    app.router.add_post('/api/audio_capture', audio_capture_post_handler)
    app.router.add_get('/', index_handler)
    app.router.add_static('/static', WWW_PATH)
    app.router.add_get('/{filename:.*}', static_handler)

    return app


async def run_web_server():
    """Run the web server for ingress."""
    global web_event_loop, web_tx_lock

    if web is None:
        log.warning("Web server not available (aiohttp not installed)")
        return

    # Store reference to event loop for thread-safe callbacks
    web_event_loop = asyncio.get_running_loop()

    # Create async lock for serializing web PTT transmissions
    web_tx_lock = asyncio.Lock()

    app = create_web_app()
    runner = web.AppRunner(app)
    await runner.setup()

    site = web.TCPSite(runner, '0.0.0.0', INGRESS_PORT)
    await site.start()

    log.info(f"Web PTT server running on port {INGRESS_PORT}")

    # Keep running
    while True:
        await asyncio.sleep(3600)


def run_mqtt_loop():
    """Run MQTT loop in a thread."""
    mqtt_client.loop_forever()


def main():
    global mqtt_client, tx_socket, rx_socket

    log.info("=" * 50)
    log.info(f"Intercom Hub v{VERSION}")
    log.info(f"Device ID: {DEVICE_ID_STR}")
    log.info(f"Unique ID: {UNIQUE_ID}")
    log.info(f"Log level: {LOG_LEVEL}")
    log.info("=" * 50)

    # Pre-encode chime audio files for zero-latency streaming
    global current_chime
    load_all_chimes()
    # Set initial chime to first available if the default ('doorbell') isn't loaded
    if loaded_chimes and current_chime not in loaded_chimes:
        current_chime = next(iter(loaded_chimes))
        log.info(f"Default chime set to: {current_chime}")

    # Load mobile device config
    load_mobile_devices()

    # Create multicast sockets
    tx_socket = create_tx_socket()
    log.info(f"TX socket ready: {MULTICAST_GROUP}:{MULTICAST_PORT}")

    rx_socket = create_rx_socket()
    log.info(f"RX socket ready (joined multicast group)")

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

    log.info(f"Connecting to MQTT: {MQTT_HOST}:{MQTT_PORT}")

    try:
        mqtt_client.connect(MQTT_HOST, MQTT_PORT, 60)
    except Exception as e:
        log.error(f"Failed to connect to MQTT: {e}")
        sys.exit(1)

    # Start MQTT loop in background thread
    mqtt_thread = threading.Thread(target=run_mqtt_loop, daemon=True)
    mqtt_thread.start()

    # Start mobile device refresh thread (updates every 5 minutes)
    mobile_thread = threading.Thread(target=mobile_refresh_thread, daemon=True)
    mobile_thread.start()

    # Run web server in main thread (for ingress panel)
    log.info("Intercom Hub running...")
    if web is not None:
        try:
            asyncio.run(run_web_server())
        except KeyboardInterrupt:
            log.info("Shutting down...")
    else:
        # Fallback if aiohttp not available - just run MQTT
        mqtt_client.loop_forever()


if __name__ == "__main__":
    main()
