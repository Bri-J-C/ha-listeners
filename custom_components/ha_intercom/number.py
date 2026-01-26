"""Number platform for HA Intercom."""

import logging

from homeassistant.components.number import NumberEntity, NumberMode
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
    """Set up number entities from a config entry."""
    coordinator: IntercomCoordinator = hass.data[DOMAIN][entry.entry_id]

    known_devices: set[str] = set()

    @callback
    def async_add_new_devices():
        """Add entities for new devices."""
        new_entities = []

        for device in coordinator.get_all_devices():
            if device.device_id not in known_devices:
                known_devices.add(device.device_id)
                new_entities.append(IntercomVolumeNumber(coordinator, device))
                _LOGGER.info("Adding volume control for %s", device.name)

        if new_entities:
            async_add_entities(new_entities)

    coordinator.register_callback(async_add_new_devices)
    async_add_new_devices()


class IntercomVolumeNumber(NumberEntity):
    """Number entity for volume control."""

    _attr_has_entity_name = True
    _attr_native_min_value = 0
    _attr_native_max_value = 100
    _attr_native_step = 5
    _attr_mode = NumberMode.SLIDER
    _attr_icon = "mdi:volume-high"

    def __init__(
        self, coordinator: IntercomCoordinator, device: IntercomDevice
    ) -> None:
        """Initialize the number."""
        self._coordinator = coordinator
        self._device = device
        self._attr_unique_id = f"{device.device_id}_volume"
        self._attr_name = "Volume"

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
    def native_value(self) -> float | None:
        """Return current value."""
        device = self._coordinator.get_device(self._device.device_id)
        return float(device.volume) if device else None

    async def async_set_native_value(self, value: float) -> None:
        """Set new value."""
        await self._coordinator.async_update_device(
            self._device.device_id, volume=int(value)
        )

    async def async_added_to_hass(self) -> None:
        """Register callbacks when entity is added."""
        self._coordinator.register_callback(self.async_write_ha_state)
