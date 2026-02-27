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
#define SETTINGS_WEB_PASS_MAX   32
#define SETTINGS_AP_PASS_MAX    16

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
    bool mqtt_tls_enabled;  // Enable MQTT over TLS
    // Audio/LED settings
    bool muted;
    bool led_enabled;
    // AGC settings
    bool agc_enabled;
    uint8_t mic_gain;     // Mic sensitivity 0-100, default 50 (50 = 2x, matches original gain)
    // Priority and DND
    uint8_t priority;     // TX priority: PRIORITY_NORMAL / HIGH / EMERGENCY
    bool dnd_enabled;     // Do Not Disturb: only EMERGENCY audio plays
    // Security settings
    char web_admin_password[SETTINGS_WEB_PASS_MAX];  // Web UI admin password
    char ap_password[SETTINGS_AP_PASS_MAX];          // AP mode WPA2 password
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

/**
 * Set AGC enabled state and save to NVS.
 */
esp_err_t settings_set_agc_enabled(bool enabled);

/**
 * Set mic gain (0-100) and save to NVS.
 */
esp_err_t settings_set_mic_gain(uint8_t gain);

/**
 * Set TX priority (PRIORITY_NORMAL / PRIORITY_HIGH / PRIORITY_EMERGENCY).
 */
esp_err_t settings_set_priority(uint8_t priority);

/**
 * Set Do Not Disturb enabled state and save to NVS.
 * When enabled, only EMERGENCY priority audio plays.
 */
esp_err_t settings_set_dnd(bool enabled);

/**
 * Set web admin password and save to NVS.
 */
esp_err_t settings_set_web_admin_password(const char *password);

/**
 * Set AP mode password and save to NVS.
 */
esp_err_t settings_set_ap_password(const char *password);

/**
 * Set MQTT TLS enabled and save to NVS.
 */
esp_err_t settings_set_mqtt_tls_enabled(bool enabled);

/**
 * Verify web admin password.
 * Returns true if password matches (or no password is set).
 */
bool settings_verify_web_password(const char *password);

/**
 * Check and save pending settings to NVS.
 * Call this periodically from main loop (e.g., every 100ms).
 * Settings are only written after a delay to batch rapid changes.
 */
void settings_save_if_needed(void);

#endif // SETTINGS_H
