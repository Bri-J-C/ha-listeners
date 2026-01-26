"""Coordinator for HA Intercom integration.

Handles device discovery, state management, and communication.
"""

import asyncio
import json
import logging
import socket
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from typing import Any, Callable

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.event import async_track_time_interval

from .const import (
    ATTR_CAPABILITIES,
    ATTR_DEFAULT_TARGET,
    ATTR_DEVICE_ID,
    ATTR_IP,
    ATTR_LAST_SEEN,
    ATTR_MUTED,
    ATTR_ROOM,
    ATTR_VERSION,
    ATTR_VOLUME,
    CONTROL_PORT,
    DEVICE_TIMEOUT,
    DOMAIN,
    MULTICAST_GROUP,
)

_LOGGER = logging.getLogger(__name__)


@dataclass
class IntercomDevice:
    """Represents an intercom device."""

    device_id: str
    name: str
    ip: str
    version: str = "1.0.0"
    capabilities: list = field(default_factory=lambda: ["audio", "ptt"])
    room: str = ""
    default_target: str = "all"
    volume: int = 80
    muted: bool = False
    last_seen: datetime = field(default_factory=datetime.now)
    online: bool = True


class IntercomCoordinator:
    """Coordinator for managing intercom devices."""

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
        """Initialize the coordinator."""
        self.hass = hass
        self.entry = entry
        self.devices: dict[str, IntercomDevice] = {}
        self._socket: socket.socket | None = None
        self._listener_task: asyncio.Task | None = None
        self._running = False
        self._callbacks: list[Callable[[], None]] = []
        self._cleanup_unsub = None

    def register_callback(self, callback: Callable[[], None]) -> Callable[[], None]:
        """Register a callback to be called when devices change."""
        self._callbacks.append(callback)

        def remove_callback():
            self._callbacks.remove(callback)

        return remove_callback

    def _notify_callbacks(self) -> None:
        """Notify all registered callbacks."""
        for callback in self._callbacks:
            callback()

    async def async_start(self) -> None:
        """Start the coordinator."""
        _LOGGER.info("Starting IntercomCoordinator")

        # Create UDP socket for discovery
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self._socket.setblocking(False)

        try:
            self._socket.bind(("", CONTROL_PORT))
        except OSError as e:
            _LOGGER.error("Failed to bind to port %d: %s", CONTROL_PORT, e)
            return

        # Start listener task
        self._running = True
        self._listener_task = asyncio.create_task(self._listen_loop())

        # Start periodic cleanup
        self._cleanup_unsub = async_track_time_interval(
            self.hass, self._cleanup_stale_devices, timedelta(seconds=30)
        )

        _LOGGER.info("IntercomCoordinator started on port %d", CONTROL_PORT)

    async def async_stop(self) -> None:
        """Stop the coordinator."""
        _LOGGER.info("Stopping IntercomCoordinator")

        self._running = False

        if self._cleanup_unsub:
            self._cleanup_unsub()
            self._cleanup_unsub = None

        if self._listener_task:
            self._listener_task.cancel()
            try:
                await self._listener_task
            except asyncio.CancelledError:
                pass
            self._listener_task = None

        if self._socket:
            self._socket.close()
            self._socket = None

    async def _listen_loop(self) -> None:
        """Listen for device announcements."""
        _LOGGER.debug("Discovery listener started")

        loop = asyncio.get_event_loop()

        while self._running:
            try:
                data, addr = await loop.sock_recvfrom(self._socket, 1024)
                await self._handle_message(data.decode(), addr[0])
            except BlockingIOError:
                await asyncio.sleep(0.1)
            except asyncio.CancelledError:
                break
            except Exception as e:
                _LOGGER.debug("Listener error: %s", e)
                await asyncio.sleep(0.5)

        _LOGGER.debug("Discovery listener stopped")

    async def _handle_message(self, message: str, source_ip: str) -> None:
        """Handle a received message."""
        try:
            data = json.loads(message)
        except json.JSONDecodeError:
            _LOGGER.debug("Invalid JSON received: %s", message)
            return

        msg_type = data.get("type")

        if msg_type == "announce":
            await self._handle_announce(data, source_ip)

    async def _handle_announce(self, data: dict, source_ip: str) -> None:
        """Handle device announcement."""
        device_id = data.get("device_id")
        if not device_id:
            return

        is_new = device_id not in self.devices

        if is_new:
            device = IntercomDevice(
                device_id=device_id,
                name=data.get("name", f"Intercom {device_id[:8]}"),
                ip=data.get("ip", source_ip),
                version=data.get("version", "1.0.0"),
                capabilities=data.get("capabilities", ["audio", "ptt"]),
            )
            self.devices[device_id] = device
            _LOGGER.info("New device discovered: %s (%s)", device.name, device_id)
        else:
            device = self.devices[device_id]
            device.ip = data.get("ip", source_ip)
            device.version = data.get("version", device.version)

        device.last_seen = datetime.now()
        device.online = True

        # Send configuration to device
        await self._send_config(device)

        if is_new:
            self._notify_callbacks()

    async def _send_config(self, device: IntercomDevice) -> None:
        """Send configuration to a device."""
        if not self._socket:
            return

        # Build targets map
        targets = {"all": MULTICAST_GROUP}
        for other_id, other_device in self.devices.items():
            if other_id != device.device_id and other_device.room:
                targets[other_device.room] = other_device.ip

        config = {
            "type": "config",
            "device_id": device.device_id,
            "room": device.room or "unknown",
            "default_target": device.default_target,
            "volume": device.volume,
            "muted": device.muted,
            "targets": targets,
        }

        message = json.dumps(config).encode()

        try:
            self._socket.sendto(message, (device.ip, CONTROL_PORT))
            _LOGGER.debug("Sent config to %s", device.name)
        except OSError as e:
            _LOGGER.warning("Failed to send config to %s: %s", device.name, e)

    @callback
    def _cleanup_stale_devices(self, now: datetime) -> None:
        """Mark devices as offline if not seen recently."""
        timeout = timedelta(seconds=DEVICE_TIMEOUT)
        changed = False

        for device in self.devices.values():
            if device.online and (now - device.last_seen) > timeout:
                device.online = False
                _LOGGER.info("Device offline: %s", device.name)
                changed = True

        if changed:
            self._notify_callbacks()

    async def async_update_device(self, device_id: str, **kwargs) -> None:
        """Update device settings and push to device."""
        device = self.devices.get(device_id)
        if not device:
            return

        for key, value in kwargs.items():
            if hasattr(device, key):
                setattr(device, key, value)

        await self._send_config(device)
        self._notify_callbacks()

    async def async_send_broadcast_command(self, target: str) -> None:
        """Send a broadcast command to all devices or specific room."""
        _LOGGER.info("Broadcast command: target=%s", target)
        # This would trigger devices to handle broadcast
        # For now, config updates handle routing

    async def async_initiate_call(self, from_room: str, to_room: str) -> None:
        """Initiate a call between two rooms."""
        _LOGGER.info("Initiating call: %s -> %s", from_room, to_room)

        # Find devices by room
        from_device = None
        to_device = None

        for device in self.devices.values():
            if device.room == from_room:
                from_device = device
            if device.room == to_room:
                to_device = device

        if not from_device or not to_device:
            _LOGGER.warning("Could not find devices for call")
            return

        # Update targets for both devices
        from_device.default_target = to_room
        to_device.default_target = from_room

        await self._send_config(from_device)
        await self._send_config(to_device)

    async def async_hangup(self, room: str) -> None:
        """End a call for a room."""
        _LOGGER.info("Hangup: %s", room)

        for device in self.devices.values():
            if device.room == room:
                device.default_target = "all"
                await self._send_config(device)
                break

    def get_device(self, device_id: str) -> IntercomDevice | None:
        """Get a device by ID."""
        return self.devices.get(device_id)

    def get_all_devices(self) -> list[IntercomDevice]:
        """Get all discovered devices."""
        return list(self.devices.values())

    def get_room_names(self) -> list[str]:
        """Get list of all room names."""
        rooms = ["all"]
        for device in self.devices.values():
            if device.room and device.room not in rooms:
                rooms.append(device.room)
        return rooms
