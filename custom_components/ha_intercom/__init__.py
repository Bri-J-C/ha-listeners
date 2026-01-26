"""HA Intercom - Home Assistant Integration.

ESP32-S3 based intercom system with multicast/unicast support.
"""

import asyncio
import json
import logging
import socket
from datetime import datetime, timedelta
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant, ServiceCall
from homeassistant.helpers import device_registry as dr
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
    PLATFORMS,
    SERVICE_BROADCAST,
    SERVICE_CALL,
    SERVICE_HANGUP,
)
from .coordinator import IntercomCoordinator

_LOGGER = logging.getLogger(__name__)


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up HA Intercom from a config entry."""
    _LOGGER.info("Setting up HA Intercom integration")

    coordinator = IntercomCoordinator(hass, entry)
    hass.data.setdefault(DOMAIN, {})[entry.entry_id] = coordinator

    # Start discovery listener
    await coordinator.async_start()

    # Set up platforms
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    # Register services
    await async_setup_services(hass, coordinator)

    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    _LOGGER.info("Unloading HA Intercom integration")

    # Unload platforms
    unload_ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)

    if unload_ok:
        coordinator: IntercomCoordinator = hass.data[DOMAIN].pop(entry.entry_id)
        await coordinator.async_stop()

    return unload_ok


async def async_setup_services(hass: HomeAssistant, coordinator: IntercomCoordinator) -> None:
    """Set up services for the integration."""

    async def handle_broadcast(call: ServiceCall) -> None:
        """Handle broadcast service call."""
        target = call.data.get("target", "all")
        message = call.data.get("message")

        _LOGGER.info("Broadcast service called: target=%s, message=%s", target, message)

        if message:
            # TTS broadcast - would integrate with Piper/TTS
            # For now, just log it
            _LOGGER.info("TTS broadcast: %s", message)
            # TODO: Integrate with HA TTS to generate audio and send

        # Send config update to devices to trigger any actions
        await coordinator.async_send_broadcast_command(target)

    async def handle_call(call: ServiceCall) -> None:
        """Handle call service call."""
        from_room = call.data.get("from_room")
        to_room = call.data.get("to_room")

        _LOGGER.info("Call service: %s -> %s", from_room, to_room)
        await coordinator.async_initiate_call(from_room, to_room)

    async def handle_hangup(call: ServiceCall) -> None:
        """Handle hangup service call."""
        room = call.data.get("room")

        _LOGGER.info("Hangup service: %s", room)
        await coordinator.async_hangup(room)

    hass.services.async_register(DOMAIN, SERVICE_BROADCAST, handle_broadcast)
    hass.services.async_register(DOMAIN, SERVICE_CALL, handle_call)
    hass.services.async_register(DOMAIN, SERVICE_HANGUP, handle_hangup)
