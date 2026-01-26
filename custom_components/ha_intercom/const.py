"""Constants for HA Intercom integration."""

DOMAIN = "ha_intercom"

# Network configuration (must match firmware/protocol.py)
CONTROL_PORT = 5004
AUDIO_PORT = 5005
MULTICAST_GROUP = "224.0.0.100"

# Discovery
HEARTBEAT_INTERVAL = 30  # seconds
DEVICE_TIMEOUT = 90  # seconds - mark offline if no heartbeat

# Protocol
DEVICE_ID_LENGTH = 8

# Platforms
PLATFORMS = ["binary_sensor", "select", "number", "switch"]

# Attributes
ATTR_DEVICE_ID = "device_id"
ATTR_ROOM = "room"
ATTR_DEFAULT_TARGET = "default_target"
ATTR_VOLUME = "volume"
ATTR_MUTED = "muted"
ATTR_IP = "ip"
ATTR_VERSION = "version"
ATTR_CAPABILITIES = "capabilities"
ATTR_LAST_SEEN = "last_seen"

# Services
SERVICE_BROADCAST = "broadcast"
SERVICE_CALL = "call"
SERVICE_HANGUP = "hangup"

# Config
CONF_ROOMS = "rooms"
