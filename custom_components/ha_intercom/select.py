"""Select platform for HA Intercom."""

import logging
from typing import Any

from homeassistant.components.select import SelectEntity
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
    """Set up select entities from a config entry."""
    coordinator: IntercomCoordinator = hass.data[DOMAIN][entry.entry_id]

    known_devices: set[str] = set()

    @callback
    def async_add_new_devices():
        """Add entities for new devices."""
        new_entities = []

        for device in coordinator.get_all_devices():
            if device.device_id not in known_devices:
                known_devices.add(device.device_id)
                new_entities.append(IntercomTargetSelect(coordinator, device))
                new_entities.append(IntercomRoomSelect(coordinator, device))
                _LOGGER.info("Adding select entities for %s", device.name)

        if new_entities:
            async_add_entities(new_entities)

    coordinator.register_callback(async_add_new_devices)
    async_add_new_devices()


class IntercomTargetSelect(SelectEntity):
    """Select entity for default target room."""

    _attr_has_entity_name = True

    def __init__(
        self, coordinator: IntercomCoordinator, device: IntercomDevice
    ) -> None:
        """Initialize the select."""
        self._coordinator = coordinator
        self._device = device
        self._attr_unique_id = f"{device.device_id}_target"
        self._attr_name = "Default Target"

    @property
    def device_info(self) -> DeviceInfo:
        """Return device info."""
        return DeviceInfo(
            identifiers={(DOMAIN, self._device.device_id)},
            name=self._device.name,
            manufacturer="HA Intercom",
            model="ESP32-S3 Satellite",
        )

    @property
    def options(self) -> list[str]:
        """Return available options."""
        return self._coordinator.get_room_names()

    @property
    def current_option(self) -> str | None:
        """Return current option."""
        device = self._coordinator.get_device(self._device.device_id)
        return device.default_target if device else None

    async def async_select_option(self, option: str) -> None:
        """Change the selected option."""
        await self._coordinator.async_update_device(
            self._device.device_id, default_target=option
        )

    async def async_added_to_hass(self) -> None:
        """Register callbacks when entity is added."""
        self._coordinator.register_callback(self.async_write_ha_state)


class IntercomRoomSelect(SelectEntity):
    """Select entity for room assignment."""

    _attr_has_entity_name = True

    def __init__(
        self, coordinator: IntercomCoordinator, device: IntercomDevice
    ) -> None:
        """Initialize the select."""
        self._coordinator = coordinator
        self._device = device
        self._attr_unique_id = f"{device.device_id}_room"
        self._attr_name = "Room"

    @property
    def device_info(self) -> DeviceInfo:
        """Return device info."""
        return DeviceInfo(
            identifiers={(DOMAIN, self._device.device_id)},
            name=self._device.name,
            manufacturer="HA Intercom",
            model="ESP32-S3 Satellite",
        )

    @property
    def options(self) -> list[str]:
        """Return available options (room names from HA areas)."""
        # Default rooms - in a full implementation, this would query HA areas
        return [
            "living_room",
            "kitchen",
            "bedroom",
            "bathroom",
            "office",
            "garage",
            "basement",
            "attic",
        ]

    @property
    def current_option(self) -> str | None:
        """Return current option."""
        device = self._coordinator.get_device(self._device.device_id)
        return device.room if device and device.room else None

    async def async_select_option(self, option: str) -> None:
        """Change the selected option."""
        await self._coordinator.async_update_device(
            self._device.device_id, room=option
        )

    async def async_added_to_hass(self) -> None:
        """Register callbacks when entity is added."""
        self._coordinator.register_callback(self.async_write_ha_state)
