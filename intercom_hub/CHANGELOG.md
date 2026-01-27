# Changelog

## [1.3.6] - 2026-01-26

### Added
- ESP32 device info and firmware link in documentation

## [1.3.5] - 2026-01-26

### Changed
- Replaced placeholder icons with proper intercom icon

### Added
- Icon attribution in README

## [1.3.4] - 2026-01-26

### Added
- Icon and logo for add-on store
- Translations file for config option labels/descriptions
- MIT License

## [1.3.3] - 2026-01-26

### Fixed
- Updated changelog to show all version history

## [1.3.2] - 2026-01-26

### Changed
- MQTT username and password are now required fields
- Password field is now masked in the UI

## [1.3.1] - 2026-01-26

### Added
- DOCS.md for proper HA add-on documentation tab

### Fixed
- Documentation now displays correctly in HA UI

## [1.3.0] - 2026-01-26

### Added
- Room selector (target select entity) for unicast to specific rooms
- Device discovery via MQTT (discovers ESP32 intercoms automatically)

### Changed
- Entity type changed from media_player to notify
- Use `notify.intercom_hub` service for TTS announcements

### Fixed
- Documentation accuracy (correct entity types, services, examples)

## [1.2.0] - 2026-01-26

### Added
- Target room selection for unicast vs multicast
- MQTT-based device discovery

## [1.0.0] - 2026-01-26

### Added
- Initial release
- Notify entity for Home Assistant
- UDP multicast broadcasting to ESP32 intercoms
- Opus audio encoding (16kHz mono, 12kbps)
- MQTT discovery for automatic entity creation
- Volume and mute controls
- Piper TTS integration
