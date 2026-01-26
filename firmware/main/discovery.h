/**
 * Discovery Module
 *
 * Announce device to Home Assistant and receive configuration.
 */

#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "protocol.h"

/**
 * Callback when configuration is received from HA.
 */
typedef void (*discovery_config_callback_t)(const device_config_t *config);

/**
 * Initialize discovery.
 * @param device_name Friendly name for this device
 * @param device_id 8-byte unique device ID
 * @return ESP_OK on success
 */
esp_err_t discovery_init(const char *device_name, const uint8_t *device_id);

/**
 * Set callback for configuration updates.
 */
void discovery_set_config_callback(discovery_config_callback_t callback);

/**
 * Start discovery service (periodic announcements).
 */
esp_err_t discovery_start(void);

/**
 * Stop discovery service.
 */
void discovery_stop(void);

/**
 * Send an immediate announcement.
 */
void discovery_announce_now(void);

/**
 * Get current device configuration.
 */
const device_config_t *discovery_get_config(void);

/**
 * Deinitialize discovery.
 */
void discovery_deinit(void);

#endif // DISCOVERY_H
