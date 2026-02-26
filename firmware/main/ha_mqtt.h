/**
 * Home Assistant MQTT Integration
 *
 * MQTT client for Home Assistant auto-discovery with multiple entities:
 * - Sensor: Device state (idle/transmitting/receiving)
 * - Number: Volume control (0-100)
 * - Switch: Mute toggle
 */

#ifndef HA_MQTT_H
#define HA_MQTT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Device states
typedef enum {
    HA_STATE_IDLE,
    HA_STATE_TRANSMITTING,
    HA_STATE_RECEIVING,
} ha_state_t;

// Command types from Home Assistant
typedef enum {
    HA_CMD_VOLUME,
    HA_CMD_MUTE,
    HA_CMD_LED,
    HA_CMD_TARGET,
    HA_CMD_AGC,
    HA_CMD_CALL,      // Incoming call notification
    HA_CMD_PRIORITY,  // Priority changed (value = PRIORITY_NORMAL / HIGH / EMERGENCY)
    HA_CMD_DND,       // Do Not Disturb toggle (value = 0 or 1)
} ha_cmd_t;

// Callback for HA commands (cmd type, value)
typedef void (*ha_mqtt_callback_t)(ha_cmd_t cmd, int value);

/**
 * Initialize MQTT client (does not connect yet).
 */
esp_err_t ha_mqtt_init(const uint8_t *device_id);

/**
 * Start MQTT connection (call after WiFi connected).
 */
esp_err_t ha_mqtt_start(void);

/**
 * Stop MQTT connection.
 */
void ha_mqtt_stop(void);

/**
 * Update device state in Home Assistant.
 */
void ha_mqtt_set_state(ha_state_t state);

/**
 * Set callback for commands from Home Assistant.
 */
void ha_mqtt_set_callback(ha_mqtt_callback_t callback);

/**
 * Publish current volume to Home Assistant.
 * Call this after changing volume locally.
 */
void ha_mqtt_publish_volume(void);

/**
 * Publish current mute state to Home Assistant.
 * Call this after changing mute locally.
 */
void ha_mqtt_publish_mute(void);

/**
 * Publish current LED state to Home Assistant.
 * Call this after changing LED locally.
 */
void ha_mqtt_publish_led(void);

/**
 * Publish current AGC state to Home Assistant.
 * Call this after changing AGC locally.
 */
void ha_mqtt_publish_agc(void);

/**
 * Publish current priority state to Home Assistant.
 */
void ha_mqtt_publish_priority(void);

/**
 * Publish current DND state to Home Assistant.
 */
void ha_mqtt_publish_dnd(void);

/**
 * Check if MQTT is connected.
 */
bool ha_mqtt_is_connected(void);

/**
 * Get the IP address of the current target room.
 * Returns NULL if target is "All Rooms" (use multicast).
 * Returns NULL if target room not found (falls back to multicast).
 */
const char* ha_mqtt_get_target_ip(void);

/**
 * Get the current target room name.
 */
const char* ha_mqtt_get_target_name(void);

/**
 * Process deferred MQTT operations.
 * Call this periodically from main loop.
 */
void ha_mqtt_process(void);

/**
 * Get discovered device count.
 */
int ha_mqtt_get_device_count(void);

/**
 * Get discovered device info by index.
 * @param index Device index (0 to count-1)
 * @param room Buffer for room name (min 32 bytes)
 * @param ip Buffer for IP address (min 16 bytes)
 * @return true if device exists
 */
bool ha_mqtt_get_device(int index, char *room, char *ip);

/**
 * Check if discovered device at index is this device (self).
 * @param index Device index (0 to count-1)
 * @return true if device is self
 */
bool ha_mqtt_is_self(int index);

/**
 * Check if discovered device at index is available (online).
 * @param index Device index (0 to count-1)
 * @return true if device is online
 */
bool ha_mqtt_is_available(int index);

/**
 * Check if device availability has changed (and clear the flag).
 * Use this to trigger display refresh when devices go online/offline.
 * @return true if availability changed since last check
 */
bool ha_mqtt_availability_changed(void);

/**
 * Set target by room name (called from display cycle).
 * This updates both local target and publishes to MQTT.
 */
void ha_mqtt_set_target(const char *room_name);

/**
 * Check if a device at index is a mobile device.
 */
bool ha_mqtt_is_device_mobile(int index);

/**
 * Check if the current target is a mobile device.
 * Mobile devices have IP "mobile" and need notification instead of audio.
 */
bool ha_mqtt_is_target_mobile(void);

/**
 * Send call notification for mobile target.
 * Call this when PTT starts with a mobile target.
 */
void ha_mqtt_notify_mobile_call(void);

/**
 * Send call notification to any target device.
 * @param target_room Room name to call (can't be "All Rooms")
 */
void ha_mqtt_send_call(const char *target_room);

/**
 * Send call notification to ALL discovered rooms simultaneously (excluding self).
 * Iterates discovered_devices and calls ha_mqtt_send_call() for each available device.
 * @return number of devices called (0 if none available)
 */
int ha_mqtt_send_call_all_rooms(void);

/**
 * Check if there's an incoming call for this device.
 * @param caller_name Buffer for caller name (min 32 bytes), or NULL
 * @return true if there's a pending call (clears the flag)
 */
bool ha_mqtt_check_incoming_call(char *caller_name);

/**
 * Get the chime name from the most recently received call notification.
 * Returns an empty string if no chime was specified in the call payload.
 */
const char* ha_mqtt_get_incoming_chime(void);

#endif // HA_MQTT_H
