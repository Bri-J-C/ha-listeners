"""Switch platform for HA Intercom."""

import logging
from typing import Any

from homeassistant.components.switch import SwitchEntity
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
    """Set up switch entities from a config entry."""
    coordinator: IntercomCoordinator = hass.data[DOMAIN][entry.entry_id]

    known_devices: set[str] = set()

    @callback
    def async_add_new_devices():
        """Add entities for new devices."""
        new_entities = []

        for device in coordinator.get_all_devices():
            if device.device_id not in known_devices:
                known_devices.add(device.device_id)
                new_entities.append(IntercomMuteSwitch(coordinator, device))
                _LOGGER.info("Adding mute switch for %s", device.name)

        if new_entities:
            async_add_entities(new_entities)

    coordinator.register_callback(async_add_new_devices)
    async_add_new_devices()


class IntercomMuteSwitch(SwitchEntity):
    """Switch entity for mute control."""

    _attr_has_entity_name = True
    _attr_icon = "mdi:volume-off"

    def __init__(
        self, coordinator: IntercomCoordinator, device: IntercomDevice
    ) -> None:
        """Initialize the switch."""
        self._coordinator = coordinator
        self._device = device
        self._attr_unique_id = f"{device.device_id}_mute"
        self._attr_name = "Mute"

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
    def is_on(self) -> bool:
        """Return true if muted."""
        device = self._coordinator.get_device(self._device.device_id)
        return device.muted if device else False

    async def async_turn_on(self, **kwargs: Any) -> None:
        """Turn mute on."""
        await self._coordinator.async_update_device(
            self._device.device_id, muted=True
        )

    async def async_turn_off(self, **kwargs: Any) -> None:
        """Turn mute off."""
        await self._coordinator.async_update_device(
            self._device.device_id, muted=False
        )

    async def async_added_to_hass(self) -> None:
        """Register callbacks when entity is added."""
        self._coordinator.register_callback(self.async_write_ha_state)
