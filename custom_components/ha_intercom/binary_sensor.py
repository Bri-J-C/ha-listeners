"""Binary sensor platform for HA Intercom."""

import logging
from typing import Any

from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import DOMAIN
from .coordinator import IntercomCoordinator, IntercomDevice

_LOGGER = logging.getLogger(__name__)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up binary sensors from a config entry."""
    coordinator: IntercomCoordinator = hass.data[DOMAIN][entry.entry_id]

    # Track which devices we've created entities for
    known_devices: set[str] = set()

    @callback
    def async_add_new_devices():
        """Add entities for new devices."""
        new_entities = []

        for device in coordinator.get_all_devices():
            if device.device_id not in known_devices:
                known_devices.add(device.device_id)
                new_entities.append(IntercomOnlineSensor(coordinator, device))
                _LOGGER.info("Adding binary sensor for %s", device.name)

        if new_entities:
            async_add_entities(new_entities)

    # Register callback for new devices
    coordinator.register_callback(async_add_new_devices)

    # Add existing devices
    async_add_new_devices()


class IntercomOnlineSensor(BinarySensorEntity):
    """Binary sensor for intercom device online status."""

    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY
    _attr_has_entity_name = True

    def __init__(
        self, coordinator: IntercomCoordinator, device: IntercomDevice
    ) -> None:
        """Initialize the sensor."""
        self._coordinator = coordinator
        self._device = device
        self._attr_unique_id = f"{device.device_id}_online"
        self._attr_name = "Online"

    @property
    def device_info(self) -> DeviceInfo:
        """Return device info."""
        return DeviceInfo(
            identifiers={(DOMAIN, self._device.device_id)},
            name=self._device.name,
            manufacturer="HA Intercom",
            model="ESP32-S3 Satellite",
            sw_version=self._device.version,
        )

    @property
    def is_on(self) -> bool:
        """Return true if device is online."""
        device = self._coordinator.get_device(self._device.device_id)
        return device.online if device else False

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        """Return extra state attributes."""
        device = self._coordinator.get_device(self._device.device_id)
        if not device:
            return {}

        return {
            "ip": device.ip,
            "room": device.room,
            "last_seen": device.last_seen.isoformat(),
            "version": device.version,
            "capabilities": device.capabilities,
        }

    async def async_added_to_hass(self) -> None:
        """Register callbacks when entity is added."""
        self._coordinator.register_callback(self.async_write_ha_state)
