/**
 * Settings Module
 *
 * Persistent configuration storage using NVS.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Setting limits
#define SETTINGS_SSID_MAX       32
#define SETTINGS_PASSWORD_MAX   64
#define SETTINGS_ROOM_MAX       32
#define SETTINGS_MQTT_HOST_MAX  64
#define SETTINGS_MQTT_USER_MAX  32
#define SETTINGS_MQTT_PASS_MAX  64

// Settings structure
typedef struct {
    char wifi_ssid[SETTINGS_SSID_MAX];
    char wifi_password[SETTINGS_PASSWORD_MAX];
    char room_name[SETTINGS_ROOM_MAX];
    uint8_t volume;
    bool configured;  // True if WiFi has been set up
    // MQTT settings for Home Assistant
    char mqtt_host[SETTINGS_MQTT_HOST_MAX];
    uint16_t mqtt_port;
    char mqtt_user[SETTINGS_MQTT_USER_MAX];
    char mqtt_password[SETTINGS_MQTT_PASS_MAX];
    bool mqtt_enabled;
    // Audio/LED settings
    bool muted;
    bool led_enabled;
} settings_t;

/**
 * Initialize settings module and load from NVS.
 */
esp_err_t settings_init(void);

/**
 * Get current settings (read-only).
 */
const settings_t* settings_get(void);

/**
 * Set WiFi credentials and save to NVS.
 */
esp_err_t settings_set_wifi(const char *ssid, const char *password);

/**
 * Set room name and save to NVS.
 */
esp_err_t settings_set_room(const char *room_name);

/**
 * Set volume and save to NVS.
 */
esp_err_t settings_set_volume(uint8_t volume);

/**
 * Check if device is configured (has WiFi credentials).
 */
bool settings_is_configured(void);

/**
 * Reset all settings to defaults.
 */
esp_err_t settings_reset(void);

/**
 * Set MQTT settings and save to NVS.
 */
esp_err_t settings_set_mqtt(const char *host, uint16_t port,
                            const char *user, const char *password);

/**
 * Enable/disable MQTT.
 */
esp_err_t settings_set_mqtt_enabled(bool enabled);

/**
 * Set mute state and save to NVS.
 */
esp_err_t settings_set_mute(bool muted);

/**
 * Set LED enabled state and save to NVS.
 */
esp_err_t settings_set_led_enabled(bool enabled);

#endif // SETTINGS_H
