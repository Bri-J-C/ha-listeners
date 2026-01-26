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
 * Check if MQTT is connected.
 */
bool ha_mqtt_is_connected(void);

#endif // HA_MQTT_H
