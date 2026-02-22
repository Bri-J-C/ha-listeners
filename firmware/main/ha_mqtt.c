/**
 * Home Assistant MQTT Integration
 *
 * MQTT client for Home Assistant auto-discovery with multiple entities:
 * - Sensor: Device state (idle/transmitting/receiving)
 * - Number: Volume control (0-100)
 * - Switch: Mute toggle
 */

#include "ha_mqtt.h"
#include "protocol.h"
#include "settings.h"
#include "agc.h"
#include "audio_output.h"
#include "button.h"
#include "network.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ha_mqtt";

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;
static char device_id_str[20] = {0};
static char unique_id[32] = {0};
static ha_state_t current_state = HA_STATE_IDLE;

// Discovered devices for room selector
#define MAX_DISCOVERED_DEVICES 10
typedef struct {
    char room[32];
    char ip[16];
    char id[32];
    bool active;
    bool available;  // Online/offline status from LWT
    bool is_mobile;  // True if this is a mobile device (phone/tablet)
} discovered_device_t;

static discovered_device_t discovered_devices[MAX_DISCOVERED_DEVICES];
static int discovered_count = 0;
static char current_target[32] = "All Rooms";  // Default target

// Topic buffers
static char base_topic[48];
static char availability_topic[64];
static char state_topic[64];
static char volume_state_topic[64];
static char volume_cmd_topic[64];
static char mute_state_topic[64];
static char mute_cmd_topic[64];
static char led_state_topic[64];
static char led_cmd_topic[64];
static char device_info_topic[64];
static char target_state_topic[64];
static char target_cmd_topic[64];
static char agc_state_topic[64];
static char agc_cmd_topic[64];
static char priority_state_topic[64];
static char priority_cmd_topic[64];
static char dnd_state_topic[64];
static char dnd_cmd_topic[64];
static const char *device_discovery_topic = "intercom/devices/+/info";
static const char *device_status_topic = "intercom/+/status";  // For availability tracking
static const char *call_topic = "intercom/call";  // Call notifications

// Incoming call state
static bool incoming_call_pending = false;
static char incoming_call_caller[32] = "";

// Callback for settings changes from HA
static ha_mqtt_callback_t user_callback = NULL;

static void publish_discovery(void);
static void publish_all_states(void);
static void publish_target(void);
static void publish_target_discovery(void);
static void publish_priority(void);
static void publish_dnd(void);

/**
 * Create device info JSON object (shared by all entities).
 */
static cJSON* create_device_info(void)
{
    const settings_t *cfg = settings_get();

    cJSON *device = cJSON_CreateObject();

    // Identifiers as array
    cJSON *ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString(device_id_str));
    cJSON_AddItemToObject(device, "identifiers", ids);

    cJSON_AddStringToObject(device, "name", cfg->room_name);
    cJSON_AddStringToObject(device, "model", "ESP32-S3 Intercom");
    cJSON_AddStringToObject(device, "manufacturer", "guywithacomputer");
    cJSON_AddStringToObject(device, "sw_version", FIRMWARE_VERSION);

    return device;
}

/**
 * Publish discovery config for state sensor.
 */
static void publish_sensor_discovery(void)
{
    const settings_t *cfg = settings_get();
    char discovery_topic[128];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "homeassistant/sensor/%s_state/config", unique_id);

    cJSON *root = cJSON_CreateObject();

    char name[64];
    snprintf(name, sizeof(name), "%s Status", cfg->room_name);
    cJSON_AddStringToObject(root, "name", name);

    char uid[48];
    snprintf(uid, sizeof(uid), "%s_state", unique_id);
    cJSON_AddStringToObject(root, "unique_id", uid);
    char entity_id[64];
    snprintf(entity_id, sizeof(entity_id), "sensor.%s", uid);
    cJSON_AddStringToObject(root, "default_entity_id", entity_id);

    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.state }}");
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    cJSON_AddStringToObject(root, "icon", "mdi:phone-classic");
    cJSON_AddItemToObject(root, "device", create_device_info());

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        esp_mqtt_client_publish(mqtt_client, discovery_topic, payload, 0, 1, true);
        free(payload);
    }
    cJSON_Delete(root);
}

/**
 * Publish discovery config for volume number.
 */
static void publish_volume_discovery(void)
{
    const settings_t *cfg = settings_get();
    char discovery_topic[128];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "homeassistant/number/%s_volume/config", unique_id);

    cJSON *root = cJSON_CreateObject();

    char name[64];
    snprintf(name, sizeof(name), "%s Volume", cfg->room_name);
    cJSON_AddStringToObject(root, "name", name);

    char uid[48];
    snprintf(uid, sizeof(uid), "%s_volume", unique_id);
    cJSON_AddStringToObject(root, "unique_id", uid);
    char entity_id[64];
    snprintf(entity_id, sizeof(entity_id), "number.%s", uid);
    cJSON_AddStringToObject(root, "default_entity_id", entity_id);

    cJSON_AddStringToObject(root, "state_topic", volume_state_topic);
    cJSON_AddStringToObject(root, "command_topic", volume_cmd_topic);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    cJSON_AddNumberToObject(root, "min", 0);
    cJSON_AddNumberToObject(root, "max", 100);
    cJSON_AddNumberToObject(root, "step", 5);
    cJSON_AddStringToObject(root, "unit_of_measurement", "%");
    cJSON_AddStringToObject(root, "icon", "mdi:volume-high");
    cJSON_AddStringToObject(root, "mode", "slider");
    cJSON_AddItemToObject(root, "device", create_device_info());

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        esp_mqtt_client_publish(mqtt_client, discovery_topic, payload, 0, 1, true);
        free(payload);
    }
    cJSON_Delete(root);
}

/**
 * Publish discovery config for mute switch.
 */
static void publish_mute_discovery(void)
{
    const settings_t *cfg = settings_get();
    char discovery_topic[128];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "homeassistant/switch/%s_mute/config", unique_id);

    cJSON *root = cJSON_CreateObject();

    char name[64];
    snprintf(name, sizeof(name), "%s Mute", cfg->room_name);
    cJSON_AddStringToObject(root, "name", name);

    char uid[48];
    snprintf(uid, sizeof(uid), "%s_mute", unique_id);
    cJSON_AddStringToObject(root, "unique_id", uid);
    char entity_id[64];
    snprintf(entity_id, sizeof(entity_id), "switch.%s", uid);
    cJSON_AddStringToObject(root, "default_entity_id", entity_id);

    cJSON_AddStringToObject(root, "state_topic", mute_state_topic);
    cJSON_AddStringToObject(root, "command_topic", mute_cmd_topic);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    cJSON_AddStringToObject(root, "payload_on", "ON");
    cJSON_AddStringToObject(root, "payload_off", "OFF");
    cJSON_AddStringToObject(root, "icon", "mdi:volume-off");
    cJSON_AddItemToObject(root, "device", create_device_info());

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        esp_mqtt_client_publish(mqtt_client, discovery_topic, payload, 0, 1, true);
        free(payload);
    }
    cJSON_Delete(root);
}

/**
 * Publish discovery config for LED switch.
 */
static void publish_led_discovery(void)
{
    const settings_t *cfg = settings_get();
    char discovery_topic[128];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "homeassistant/switch/%s_led/config", unique_id);

    cJSON *root = cJSON_CreateObject();

    char name[64];
    snprintf(name, sizeof(name), "%s LED", cfg->room_name);
    cJSON_AddStringToObject(root, "name", name);

    char uid[48];
    snprintf(uid, sizeof(uid), "%s_led", unique_id);
    cJSON_AddStringToObject(root, "unique_id", uid);
    char entity_id[64];
    snprintf(entity_id, sizeof(entity_id), "switch.%s", uid);
    cJSON_AddStringToObject(root, "default_entity_id", entity_id);

    cJSON_AddStringToObject(root, "state_topic", led_state_topic);
    cJSON_AddStringToObject(root, "command_topic", led_cmd_topic);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    cJSON_AddStringToObject(root, "payload_on", "ON");
    cJSON_AddStringToObject(root, "payload_off", "OFF");
    cJSON_AddStringToObject(root, "icon", "mdi:led-on");
    cJSON_AddItemToObject(root, "device", create_device_info());

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        esp_mqtt_client_publish(mqtt_client, discovery_topic, payload, 0, 1, true);
        free(payload);
    }
    cJSON_Delete(root);
}

/**
 * Publish discovery config for AGC switch.
 */
static void publish_agc_discovery(void)
{
    const settings_t *cfg = settings_get();
    char discovery_topic[128];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "homeassistant/switch/%s_agc/config", unique_id);

    cJSON *root = cJSON_CreateObject();

    char name[64];
    snprintf(name, sizeof(name), "%s AGC", cfg->room_name);
    cJSON_AddStringToObject(root, "name", name);

    char uid[48];
    snprintf(uid, sizeof(uid), "%s_agc", unique_id);
    cJSON_AddStringToObject(root, "unique_id", uid);
    char entity_id[64];
    snprintf(entity_id, sizeof(entity_id), "switch.%s", uid);
    cJSON_AddStringToObject(root, "default_entity_id", entity_id);

    cJSON_AddStringToObject(root, "state_topic", agc_state_topic);
    cJSON_AddStringToObject(root, "command_topic", agc_cmd_topic);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    cJSON_AddStringToObject(root, "payload_on", "ON");
    cJSON_AddStringToObject(root, "payload_off", "OFF");
    cJSON_AddStringToObject(root, "icon", "mdi:microphone-settings");
    cJSON_AddItemToObject(root, "device", create_device_info());

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        esp_mqtt_client_publish(mqtt_client, discovery_topic, payload, 0, 1, true);
        free(payload);
    }
    cJSON_Delete(root);
}

/**
 * Publish current AGC state.
 */
static void publish_agc(void)
{
    if (!mqtt_connected) return;

    const settings_t *cfg = settings_get();
    const char *state = cfg->agc_enabled ? "ON" : "OFF";
    esp_mqtt_client_publish(mqtt_client, agc_state_topic, state, 0, 0, true);
}

/**
 * Publish discovery config for priority select entity.
 */
static void publish_priority_discovery(void)
{
    const settings_t *cfg = settings_get();
    char discovery_topic[128];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "homeassistant/select/%s_priority/config", unique_id);

    cJSON *root = cJSON_CreateObject();

    char name[64];
    snprintf(name, sizeof(name), "%s Priority", cfg->room_name);
    cJSON_AddStringToObject(root, "name", name);

    char uid[48];
    snprintf(uid, sizeof(uid), "%s_priority", unique_id);
    cJSON_AddStringToObject(root, "unique_id", uid);
    char entity_id[64];
    snprintf(entity_id, sizeof(entity_id), "select.%s", uid);
    cJSON_AddStringToObject(root, "default_entity_id", entity_id);

    cJSON_AddStringToObject(root, "state_topic", priority_state_topic);
    cJSON_AddStringToObject(root, "command_topic", priority_cmd_topic);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    cJSON_AddStringToObject(root, "icon", "mdi:alert-circle-outline");

    cJSON *options = cJSON_CreateArray();
    cJSON_AddItemToArray(options, cJSON_CreateString("Normal"));
    cJSON_AddItemToArray(options, cJSON_CreateString("High"));
    cJSON_AddItemToArray(options, cJSON_CreateString("Emergency"));
    cJSON_AddItemToObject(root, "options", options);

    cJSON_AddItemToObject(root, "device", create_device_info());

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        esp_mqtt_client_publish(mqtt_client, discovery_topic, payload, 0, 1, true);
        free(payload);
    }
    cJSON_Delete(root);
}

/**
 * Publish discovery config for DND switch entity.
 */
static void publish_dnd_discovery(void)
{
    const settings_t *cfg = settings_get();
    char discovery_topic[128];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "homeassistant/switch/%s_dnd/config", unique_id);

    cJSON *root = cJSON_CreateObject();

    char name[64];
    snprintf(name, sizeof(name), "%s Do Not Disturb", cfg->room_name);
    cJSON_AddStringToObject(root, "name", name);

    char uid[48];
    snprintf(uid, sizeof(uid), "%s_dnd", unique_id);
    cJSON_AddStringToObject(root, "unique_id", uid);
    char entity_id[64];
    snprintf(entity_id, sizeof(entity_id), "switch.%s", uid);
    cJSON_AddStringToObject(root, "default_entity_id", entity_id);

    cJSON_AddStringToObject(root, "state_topic", dnd_state_topic);
    cJSON_AddStringToObject(root, "command_topic", dnd_cmd_topic);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    cJSON_AddStringToObject(root, "payload_on", "ON");
    cJSON_AddStringToObject(root, "payload_off", "OFF");
    cJSON_AddStringToObject(root, "icon", "mdi:bell-sleep");
    cJSON_AddItemToObject(root, "device", create_device_info());

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        esp_mqtt_client_publish(mqtt_client, discovery_topic, payload, 0, 1, true);
        free(payload);
    }
    cJSON_Delete(root);
}

/**
 * Publish current priority as a string.
 */
static void publish_priority(void)
{
    if (!mqtt_connected) return;

    const settings_t *cfg = settings_get();
    const char *priority_str;
    switch (cfg->priority) {
        case 1:  priority_str = "High";       break;
        case 2:  priority_str = "Emergency";  break;
        default: priority_str = "Normal";     break;
    }
    esp_mqtt_client_publish(mqtt_client, priority_state_topic, priority_str, 0, 0, true);
}

/**
 * Publish current DND state.
 */
static void publish_dnd(void)
{
    if (!mqtt_connected) return;

    const settings_t *cfg = settings_get();
    const char *state = cfg->dnd_enabled ? "ON" : "OFF";
    esp_mqtt_client_publish(mqtt_client, dnd_state_topic, state, 0, 0, true);
}

/**
 * Remove old v1.0 discovery config (cleanup).
 */
static void cleanup_old_discovery(void)
{
    char old_topic[128];
    snprintf(old_topic, sizeof(old_topic), "homeassistant/sensor/%s/config", unique_id);
    // Publish empty payload to remove old entity
    esp_mqtt_client_publish(mqtt_client, old_topic, "", 0, 1, true);
    ESP_LOGI(TAG, "Cleaned up old v1.0 discovery");
}

/**
 * Publish all discovery configs.
 */
static void publish_discovery(void)
{
    cleanup_old_discovery();
    publish_sensor_discovery();
    publish_volume_discovery();
    publish_mute_discovery();
    publish_led_discovery();
    publish_agc_discovery();
    publish_target_discovery();
    publish_priority_discovery();
    publish_dnd_discovery();
    ESP_LOGI(TAG, "Published HA discovery (sensor, volume, mute, led, agc, target, priority, dnd)");
}

/**
 * Publish current state.
 */
static void publish_state(void)
{
    if (!mqtt_connected) return;

    char payload[128];

    switch (current_state) {
        case HA_STATE_TRANSMITTING:
            // Include target when transmitting so hub knows where to forward
            snprintf(payload, sizeof(payload), "{\"state\":\"transmitting\",\"target\":\"%s\"}", current_target);
            break;
        case HA_STATE_RECEIVING:
            snprintf(payload, sizeof(payload), "{\"state\":\"receiving\"}");
            break;
        default:
            snprintf(payload, sizeof(payload), "{\"state\":\"idle\"}");
            break;
    }

    esp_mqtt_client_publish(mqtt_client, state_topic, payload, 0, 0, true);
}

/**
 * Publish current volume.
 */
static void publish_volume(void)
{
    if (!mqtt_connected) return;

    char vol_str[8];
    snprintf(vol_str, sizeof(vol_str), "%d", audio_output_get_volume());
    esp_mqtt_client_publish(mqtt_client, volume_state_topic, vol_str, 0, 0, true);
}

/**
 * Publish current mute state.
 */
static void publish_mute(void)
{
    if (!mqtt_connected) return;

    const char *state = audio_output_is_muted() ? "ON" : "OFF";
    esp_mqtt_client_publish(mqtt_client, mute_state_topic, state, 0, 0, true);
}

/**
 * Publish current LED state.
 */
static void publish_led(void)
{
    if (!mqtt_connected) return;

    const char *state = button_is_idle_led_enabled() ? "ON" : "OFF";
    esp_mqtt_client_publish(mqtt_client, led_state_topic, state, 0, 0, true);
}

/**
 * Publish all states.
 */
static void publish_all_states(void)
{
    publish_state();
    publish_volume();
    publish_mute();
    publish_led();
    publish_agc();
    publish_target();
    publish_priority();
    publish_dnd();
}

/**
 * Publish device info for hub discovery.
 * This allows the hub add-on to discover all intercom devices.
 */
static void publish_device_info(void)
{
    if (!mqtt_connected) return;

    const settings_t *cfg = settings_get();
    char ip_str[16];
    network_get_ip(ip_str);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "room", cfg->room_name);
    cJSON_AddStringToObject(root, "ip", ip_str);
    cJSON_AddStringToObject(root, "id", unique_id);

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        esp_mqtt_client_publish(mqtt_client, device_info_topic, payload, 0, 1, true);
        ESP_LOGI(TAG, "Published device info: room=%s ip=%s", cfg->room_name, ip_str);
        free(payload);
    }
    cJSON_Delete(root);
}

/**
 * Publish current target state.
 */
static void publish_target(void)
{
    if (!mqtt_connected) return;
    esp_mqtt_client_publish(mqtt_client, target_state_topic, current_target, 0, 0, true);
}

/**
 * Publish target select discovery with current options.
 */
static void publish_target_discovery(void)
{
    char discovery_topic[128];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "homeassistant/select/%s_target/config", unique_id);

    cJSON *root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "name", "Target");

    char uid[48];
    snprintf(uid, sizeof(uid), "%s_target", unique_id);
    cJSON_AddStringToObject(root, "unique_id", uid);
    char entity_id[64];
    snprintf(entity_id, sizeof(entity_id), "select.%s", uid);
    cJSON_AddStringToObject(root, "default_entity_id", entity_id);

    cJSON_AddStringToObject(root, "state_topic", target_state_topic);
    cJSON_AddStringToObject(root, "command_topic", target_cmd_topic);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    cJSON_AddStringToObject(root, "icon", "mdi:target");
    cJSON_AddBoolToObject(root, "has_entity_name", true);

    // Build options array: "All Rooms" + discovered rooms (excluding self)
    cJSON *options = cJSON_CreateArray();
    cJSON_AddItemToArray(options, cJSON_CreateString("All Rooms"));

    for (int i = 0; i < discovered_count; i++) {
        if (discovered_devices[i].active) {
            // Don't add ourselves as a target option
            if (strcmp(discovered_devices[i].id, unique_id) != 0) {
                cJSON_AddItemToArray(options, cJSON_CreateString(discovered_devices[i].room));
            }
        }
    }
    cJSON_AddItemToObject(root, "options", options);

    cJSON_AddItemToObject(root, "device", create_device_info());

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        esp_mqtt_client_publish(mqtt_client, discovery_topic, payload, 0, 1, true);
        free(payload);
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Published target select discovery (%d devices)", discovered_count);
}

// Flag for deferred target discovery update
static bool target_discovery_pending = false;
static bool availability_changed = false;  // Flag to trigger display refresh

// Cache for device status messages that arrive before device info (MQTT ordering race)
#define MAX_PENDING_STATUS 5
typedef struct {
    char id[32];
    bool is_online;
} pending_status_t;
static pending_status_t pending_statuses[MAX_PENDING_STATUS];
static int pending_status_count = 0;

/**
 * Simple JSON string extractor (avoids cJSON allocation in MQTT handler).
 * Extracts value for "key":"value" or "key": "value" pattern.
 */
static bool extract_json_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    const char *start = strstr(json, pattern);
    if (!start) return false;

    start += strlen(pattern);

    // Skip optional whitespace after colon
    while (*start == ' ' || *start == '\t') start++;

    // Expect opening quote
    if (*start != '"') return false;
    start++;

    const char *end = strchr(start, '"');
    if (!end) return false;

    size_t len = end - start;
    if (len >= out_len) len = out_len - 1;

    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

/**
 * Simple JSON boolean extractor.
 * Extracts value for "key":true or "key":false pattern.
 */
static bool extract_json_bool(const char *json, const char *key, bool *out)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    const char *start = strstr(json, pattern);
    if (!start) return false;

    start += strlen(pattern);
    // Skip whitespace
    while (*start == ' ') start++;

    if (strncmp(start, "true", 4) == 0) {
        *out = true;
        return true;
    } else if (strncmp(start, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

/**
 * Handle discovered device info from other intercoms.
 * Uses simple string parsing to avoid cJSON memory allocation.
 */
static void handle_device_info(const char *payload)
{
    char room[32] = {0};
    char ip[16] = {0};
    char id[32] = {0};
    bool is_mobile = false;

    // Simple JSON parsing without cJSON
    if (!extract_json_string(payload, "room", room, sizeof(room))) return;
    if (!extract_json_string(payload, "ip", ip, sizeof(ip))) return;
    if (!extract_json_string(payload, "id", id, sizeof(id))) return;
    extract_json_bool(payload, "is_mobile", &is_mobile);  // Optional field

    // Check if we already know this device
    int existing_idx = -1;
    for (int i = 0; i < discovered_count; i++) {
        if (strcmp(discovered_devices[i].id, id) == 0) {
            existing_idx = i;
            break;
        }
    }

    if (existing_idx >= 0) {
        // Update existing device (no need to republish discovery)
        strncpy(discovered_devices[existing_idx].room, room, sizeof(discovered_devices[0].room) - 1);
        strncpy(discovered_devices[existing_idx].ip, ip, sizeof(discovered_devices[0].ip) - 1);
        discovered_devices[existing_idx].active = true;
        /* Keep existing availability — don't override what status messages told us. */
        discovered_devices[existing_idx].is_mobile = is_mobile;
    } else if (discovered_count < MAX_DISCOVERED_DEVICES) {
        // Add new device — default available=false until an "online" status confirms it.
        // This prevents stale retained info messages (from gone devices) from polluting
        // the room list. The retained "online" status will arrive and set available=true.
        strncpy(discovered_devices[discovered_count].room, room, sizeof(discovered_devices[0].room) - 1);
        strncpy(discovered_devices[discovered_count].ip, ip, sizeof(discovered_devices[0].ip) - 1);
        strncpy(discovered_devices[discovered_count].id, id, sizeof(discovered_devices[0].id) - 1);
        discovered_devices[discovered_count].active = true;
        discovered_devices[discovered_count].available = false;  // Wait for "online" status
        discovered_devices[discovered_count].is_mobile = is_mobile;
        discovered_count++;
        ESP_LOGI(TAG, "Discovered device: %s (%s) at %s%s", room, id, ip, is_mobile ? " [mobile]" : "");

        // Apply any status that arrived before this info (retained message race condition).
        // If "online" was received while device was unknown, it was cached — apply it now.
        for (int i = 0; i < pending_status_count; i++) {
            if (strcmp(pending_statuses[i].id, id) == 0) {
                discovered_devices[discovered_count - 1].available = pending_statuses[i].is_online;
                if (pending_statuses[i].is_online) {
                    availability_changed = true;
                    ESP_LOGI(TAG, "Applied cached status: %s is online", room);
                }
                // Remove from cache (swap with last entry)
                pending_statuses[i] = pending_statuses[--pending_status_count];
                break;
            }
        }

        // Defer target discovery update to main loop (avoid cJSON in MQTT handler)
        target_discovery_pending = true;
    }
}

/**
 * Handle device status (availability) message.
 * Topic format: intercom/{unique_id}/status
 * Payload: "online" or "offline"
 */
static void handle_device_status(const char *topic, const char *payload)
{
    // Extract unique_id from topic: "intercom/{unique_id}/status"
    const char *start = topic + 9;  // Skip "intercom/"
    const char *end = strstr(start, "/status");
    if (!end) return;

    char device_id[32] = {0};
    size_t len = end - start;
    if (len >= sizeof(device_id)) len = sizeof(device_id) - 1;
    memcpy(device_id, start, len);

    bool is_online = (strcmp(payload, "online") == 0);

    // Find device and update availability
    for (int i = 0; i < discovered_count; i++) {
        if (strcmp(discovered_devices[i].id, device_id) == 0) {
            if (discovered_devices[i].available != is_online) {
                discovered_devices[i].available = is_online;
                availability_changed = true;  // Trigger display refresh
                ESP_LOGI(TAG, "Device %s is now %s", discovered_devices[i].room, is_online ? "online" : "offline");
            }
            return;
        }
    }

    // Device not yet discovered — cache status so it can be applied when info arrives.
    // MQTT retained messages can arrive in any order; status often beats info.
    for (int i = 0; i < pending_status_count; i++) {
        if (strcmp(pending_statuses[i].id, device_id) == 0) {
            pending_statuses[i].is_online = is_online;  // update existing cache entry
            return;
        }
    }
    if (pending_status_count < MAX_PENDING_STATUS) {
        strncpy(pending_statuses[pending_status_count].id, device_id,
                sizeof(pending_statuses[0].id) - 1);
        pending_statuses[pending_status_count].is_online = is_online;
        pending_status_count++;
    }
}

/**
 * Handle incoming MQTT commands.
 */
static void handle_mqtt_data(esp_mqtt_event_handle_t event)
{
    // Null-terminate topic and data for easier handling
    char topic[128] = {0};
    char data[256] = {0};  // Larger buffer for device info JSON

    size_t topic_len = event->topic_len < sizeof(topic) - 1 ? event->topic_len : sizeof(topic) - 1;
    size_t data_len = event->data_len < sizeof(data) - 1 ? event->data_len : sizeof(data) - 1;

    memcpy(topic, event->topic, topic_len);
    memcpy(data, event->data, data_len);

    // Check if this is device info or status (don't log these to reduce spam)
    bool is_device_info = (strncmp(topic, "intercom/devices/", 17) == 0 &&
                           strstr(topic, "/info") != NULL);
    bool is_device_status = (strncmp(topic, "intercom/", 9) == 0 &&
                             strstr(topic, "/status") != NULL &&
                             strncmp(topic, "intercom/devices/", 17) != 0);

    if (!is_device_info && !is_device_status) {
        ESP_LOGI(TAG, "MQTT cmd: %s = %s", topic, data);
    }

    // Volume command
    if (strcmp(topic, volume_cmd_topic) == 0) {
        int volume = atoi(data);
        if (volume >= 0 && volume <= 100) {
            audio_output_set_volume(volume);
            settings_set_volume(volume);
            publish_volume();

            if (user_callback) {
                user_callback(HA_CMD_VOLUME, volume);
            }
        }
    }
    // Mute command
    else if (strcmp(topic, mute_cmd_topic) == 0) {
        bool muted = (strcmp(data, "ON") == 0);
        audio_output_set_mute(muted);
        settings_set_mute(muted);  // Persist to NVS
        publish_mute();

        if (user_callback) {
            user_callback(HA_CMD_MUTE, muted ? 1 : 0);
        }
    }
    // LED command
    else if (strcmp(topic, led_cmd_topic) == 0) {
        bool enabled = (strcmp(data, "ON") == 0);
        button_set_idle_led_enabled(enabled);
        settings_set_led_enabled(enabled);  // Persist to NVS
        publish_led();

        if (user_callback) {
            user_callback(HA_CMD_LED, enabled ? 1 : 0);
        }
    }
    // AGC command
    else if (strcmp(topic, agc_cmd_topic) == 0) {
        bool enabled = (strcmp(data, "ON") == 0);
        settings_set_agc_enabled(enabled);
        publish_agc();

        if (user_callback) {
            user_callback(HA_CMD_AGC, enabled ? 1 : 0);
        }
    }
    // Priority command ("Normal", "High", "Emergency")
    else if (strcmp(topic, priority_cmd_topic) == 0) {
        uint8_t priority;
        if (strcmp(data, "High") == 0) {
            priority = 1;
        } else if (strcmp(data, "Emergency") == 0) {
            priority = 2;
        } else {
            priority = 0;
        }
        settings_set_priority(priority);
        publish_priority();

        if (user_callback) {
            user_callback(HA_CMD_PRIORITY, priority);
        }
    }
    // DND command
    else if (strcmp(topic, dnd_cmd_topic) == 0) {
        bool dnd = (strcmp(data, "ON") == 0);
        settings_set_dnd(dnd);
        publish_dnd();

        if (user_callback) {
            user_callback(HA_CMD_DND, dnd ? 1 : 0);
        }
    }
    // Target command
    else if (strcmp(topic, target_cmd_topic) == 0) {
        // Trim leading/trailing whitespace from HA target value (stale retained
        // messages may include trailing spaces from old room names like "INTERCOM2 ")
        char *src = data;
        while (*src == ' ' || *src == '\t') src++;
        strncpy(current_target, src, sizeof(current_target) - 1);
        current_target[sizeof(current_target) - 1] = '\0';
        int tlen = strlen(current_target);
        while (tlen > 0 && (current_target[tlen-1] == ' ' || current_target[tlen-1] == '\t')) {
            current_target[--tlen] = '\0';
        }
        publish_target();
        ESP_LOGI(TAG, "Target set to: %s", current_target);

        if (user_callback) {
            user_callback(HA_CMD_TARGET, 0);
        }
    }
    // Device info from other intercoms
    else if (is_device_info) {
        handle_device_info(data);
    }
    // Device status (availability) from other intercoms
    else if (is_device_status) {
        handle_device_status(topic, data);
    }
    // Incoming call notification
    else if (strcmp(topic, call_topic) == 0) {
        // Parse call JSON: {"target": "Room Name", "caller": "Caller Name"}
        char target[32] = {0};
        char caller[32] = {0};

        ESP_LOGI(TAG, "Call notification received: %s", data);

        if (extract_json_string(data, "target", target, sizeof(target)) &&
            extract_json_string(data, "caller", caller, sizeof(caller))) {

            const settings_t *cfg = settings_get();
            ESP_LOGI(TAG, "Call target='%s', our room='%s'", target, cfg->room_name);

            // Check if we're the target
            if (strcmp(target, cfg->room_name) == 0) {
                ESP_LOGI(TAG, "Incoming call from: %s - triggering chime", caller);
                strncpy(incoming_call_caller, caller, sizeof(incoming_call_caller) - 1);
                incoming_call_pending = true;

                if (user_callback) {
                    user_callback(HA_CMD_CALL, 0);
                }
            } else {
                ESP_LOGI(TAG, "Call not for us (target mismatch)");
            }
        } else {
            ESP_LOGW(TAG, "Failed to parse call JSON");
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            mqtt_connected = true;

            // Publish discovery configs
            publish_discovery();

            // Publish online status
            esp_mqtt_client_publish(mqtt_client, availability_topic, "online", 0, 1, true);

            // Subscribe to command topics
            esp_mqtt_client_subscribe(mqtt_client, volume_cmd_topic, 0);
            esp_mqtt_client_subscribe(mqtt_client, mute_cmd_topic, 0);
            esp_mqtt_client_subscribe(mqtt_client, led_cmd_topic, 0);
            esp_mqtt_client_subscribe(mqtt_client, target_cmd_topic, 0);
            esp_mqtt_client_subscribe(mqtt_client, agc_cmd_topic, 0);
            esp_mqtt_client_subscribe(mqtt_client, priority_cmd_topic, 0);
            esp_mqtt_client_subscribe(mqtt_client, dnd_cmd_topic, 0);

            // Subscribe to device discovery to learn about other intercoms
            esp_mqtt_client_subscribe(mqtt_client, device_discovery_topic, 0);
            esp_mqtt_client_subscribe(mqtt_client, device_status_topic, 0);

            // Subscribe to call notifications
            esp_mqtt_client_subscribe(mqtt_client, call_topic, 0);
            ESP_LOGI(TAG, "Subscribed to command topics, device discovery, status, and calls");

            // Publish current states
            publish_all_states();

            // Publish device info for hub discovery
            publish_device_info();
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            mqtt_connected = false;
            break;

        case MQTT_EVENT_DATA:
            handle_mqtt_data(event);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;

        default:
            break;
    }
}

esp_err_t ha_mqtt_init(const uint8_t *device_id)
{
    // Create device ID string
    snprintf(device_id_str, sizeof(device_id_str), "%02x%02x%02x%02x%02x%02x%02x%02x",
             device_id[0], device_id[1], device_id[2], device_id[3],
             device_id[4], device_id[5], device_id[6], device_id[7]);

    // Short unique ID for HA
    snprintf(unique_id, sizeof(unique_id), "intercom_%02x%02x%02x%02x",
             device_id[4], device_id[5], device_id[6], device_id[7]);

    // Build topic names
    snprintf(base_topic, sizeof(base_topic), "intercom/%s", unique_id);
    snprintf(availability_topic, sizeof(availability_topic), "%s/status", base_topic);
    snprintf(state_topic, sizeof(state_topic), "%s/state", base_topic);
    snprintf(volume_state_topic, sizeof(volume_state_topic), "%s/volume", base_topic);
    snprintf(volume_cmd_topic, sizeof(volume_cmd_topic), "%s/volume/set", base_topic);
    snprintf(mute_state_topic, sizeof(mute_state_topic), "%s/mute", base_topic);
    snprintf(mute_cmd_topic, sizeof(mute_cmd_topic), "%s/mute/set", base_topic);
    snprintf(led_state_topic, sizeof(led_state_topic), "%s/led", base_topic);
    snprintf(led_cmd_topic, sizeof(led_cmd_topic), "%s/led/set", base_topic);
    snprintf(device_info_topic, sizeof(device_info_topic), "intercom/devices/%s/info", unique_id);
    snprintf(target_state_topic, sizeof(target_state_topic), "%s/target", base_topic);
    snprintf(target_cmd_topic, sizeof(target_cmd_topic), "%s/target/set", base_topic);
    snprintf(agc_state_topic, sizeof(agc_state_topic), "%s/agc", base_topic);
    snprintf(agc_cmd_topic, sizeof(agc_cmd_topic), "%s/agc/set", base_topic);
    snprintf(priority_state_topic, sizeof(priority_state_topic), "%s/priority", base_topic);
    snprintf(priority_cmd_topic, sizeof(priority_cmd_topic), "%s/priority/set", base_topic);
    snprintf(dnd_state_topic, sizeof(dnd_state_topic), "%s/dnd", base_topic);
    snprintf(dnd_cmd_topic, sizeof(dnd_cmd_topic), "%s/dnd/set", base_topic);

    // Initialize current target
    strcpy(current_target, "All Rooms");

    ESP_LOGI(TAG, "HA MQTT initialized: id=%s", unique_id);
    return ESP_OK;
}

esp_err_t ha_mqtt_start(void)
{
    const settings_t *cfg = settings_get();

    if (!cfg->mqtt_enabled || strlen(cfg->mqtt_host) == 0) {
        ESP_LOGI(TAG, "MQTT disabled or not configured");
        return ESP_OK;
    }

    // Build broker URI - use mqtts:// for TLS, mqtt:// otherwise
    char uri[128];
    if (cfg->mqtt_tls_enabled) {
        snprintf(uri, sizeof(uri), "mqtts://%s:%d", cfg->mqtt_host, cfg->mqtt_port);
    } else {
        snprintf(uri, sizeof(uri), "mqtt://%s:%d", cfg->mqtt_host, cfg->mqtt_port);
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.username = cfg->mqtt_user,
        .credentials.authentication.password = cfg->mqtt_password,
        .session.last_will.topic = availability_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
        .session.keepalive = 15,  // 15 seconds - faster offline detection
    };

    // For TLS: Skip certificate verification for self-signed certs (common in home setups)
    // Production environments should use proper CA certificate validation
    if (cfg->mqtt_tls_enabled) {
        // Allow insecure connections (skip all certificate verification)
        // This is required for self-signed certificates or brokers without proper certs
        mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
        mqtt_cfg.broker.verification.crt_bundle_attach = NULL;
        mqtt_cfg.broker.verification.certificate = NULL;
        // Note: For proper security, embed CA cert and set:
        // mqtt_cfg.broker.verification.certificate = (const char *)ca_cert_pem;
        ESP_LOGI(TAG, "MQTT TLS enabled (certificate validation skipped for home use)");
    }

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!mqtt_client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    esp_err_t ret = esp_mqtt_client_start(mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client");
        return ret;
    }

    ESP_LOGI(TAG, "MQTT client started, connecting to %s (TLS: %s)", uri, cfg->mqtt_tls_enabled ? "yes" : "no");
    return ESP_OK;
}

void ha_mqtt_stop(void)
{
    if (mqtt_client) {
        if (mqtt_connected) {
            esp_mqtt_client_publish(mqtt_client, availability_topic, "offline", 0, 1, true);
        }
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        mqtt_connected = false;
        ESP_LOGI(TAG, "MQTT stopped");
    }
}

void ha_mqtt_set_state(ha_state_t state)
{
    if (state != current_state) {
        current_state = state;
        publish_state();
    }
}

void ha_mqtt_set_callback(ha_mqtt_callback_t callback)
{
    user_callback = callback;
}

void ha_mqtt_publish_volume(void)
{
    publish_volume();
}

void ha_mqtt_publish_mute(void)
{
    publish_mute();
}

void ha_mqtt_publish_led(void)
{
    publish_led();
}

void ha_mqtt_publish_agc(void)
{
    publish_agc();
}

void ha_mqtt_publish_priority(void)
{
    publish_priority();
}

void ha_mqtt_publish_dnd(void)
{
    publish_dnd();
}

bool ha_mqtt_is_connected(void)
{
    return mqtt_connected;
}

void ha_mqtt_process(void)
{
    // Handle deferred target discovery update (avoids cJSON in MQTT handler)
    if (target_discovery_pending && mqtt_connected) {
        target_discovery_pending = false;
        publish_target_discovery();
    }
}

const char* ha_mqtt_get_target_ip(void)
{
    // If target is "All Rooms", return NULL for multicast
    if (strcmp(current_target, "All Rooms") == 0) {
        return NULL;
    }

    // Find device by room name
    for (int i = 0; i < discovered_count; i++) {
        if (discovered_devices[i].active &&
            strcmp(discovered_devices[i].room, current_target) == 0) {
            // Mobile devices have hub's IP - unicast there, hub forwards to web client
            if (discovered_devices[i].is_mobile) {
                ESP_LOGI(TAG, "Target '%s' is mobile, unicast to hub at %s", current_target, discovered_devices[i].ip);
            }
            return discovered_devices[i].ip;
        }
    }

    // Target not found, fall back to multicast
    ESP_LOGW(TAG, "Target '%s' not found, using multicast", current_target);
    return NULL;
}

const char* ha_mqtt_get_target_name(void)
{
    return current_target;
}

int ha_mqtt_get_device_count(void)
{
    return discovered_count;
}

bool ha_mqtt_get_device(int index, char *room, char *ip)
{
    if (index < 0 || index >= discovered_count) return false;
    if (!discovered_devices[index].active) return false;

    if (room) {
        strncpy(room, discovered_devices[index].room, 31);
        room[31] = '\0';
    }
    if (ip) {
        strncpy(ip, discovered_devices[index].ip, 15);
        ip[15] = '\0';
    }
    return true;
}

bool ha_mqtt_is_self(int index)
{
    if (index < 0 || index >= discovered_count) return false;
    return strcmp(discovered_devices[index].id, unique_id) == 0;
}

bool ha_mqtt_is_available(int index)
{
    if (index < 0 || index >= discovered_count) return false;
    return discovered_devices[index].available;
}

bool ha_mqtt_is_device_mobile(int index)
{
    if (index < 0 || index >= discovered_count) return false;
    return discovered_devices[index].is_mobile;
}

bool ha_mqtt_availability_changed(void)
{
    if (availability_changed) {
        availability_changed = false;
        return true;
    }
    return false;
}

void ha_mqtt_set_target(const char *room_name)
{
    if (!room_name) return;

    // Trim leading/trailing whitespace
    const char *src = room_name;
    while (*src == ' ' || *src == '\t') src++;
    strncpy(current_target, src, sizeof(current_target) - 1);
    current_target[sizeof(current_target) - 1] = '\0';
    int tlen = strlen(current_target);
    while (tlen > 0 && (current_target[tlen-1] == ' ' || current_target[tlen-1] == '\t')) {
        current_target[--tlen] = '\0';
    }
    publish_target();
    ESP_LOGI(TAG, "Target set to: %s", current_target);
}

bool ha_mqtt_is_target_mobile(void)
{
    // Check if current target is a mobile device
    for (int i = 0; i < discovered_count; i++) {
        if (discovered_devices[i].active &&
            strcmp(discovered_devices[i].room, current_target) == 0) {
            return discovered_devices[i].is_mobile;
        }
    }
    return false;
}

void ha_mqtt_notify_mobile_call(void)
{
    if (!mqtt_connected || !mqtt_client) return;

    const settings_t *cfg = settings_get();

    // Find the mobile device's notify service
    char notify_service[64] = "";
    for (int i = 0; i < discovered_count; i++) {
        if (discovered_devices[i].active &&
            strcmp(discovered_devices[i].room, current_target) == 0 &&
            discovered_devices[i].is_mobile) {
            // Extract notify service from device id (e.g., "intercom_xxx_mobile_0")
            // The hub will handle the actual service lookup
            snprintf(notify_service, sizeof(notify_service), "mobile_app_%s",
                     discovered_devices[i].room);
            break;
        }
    }

    // Publish call notification
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "target", current_target);
    cJSON_AddStringToObject(call, "caller", cfg->room_name);
    cJSON_AddNumberToObject(call, "timestamp", (double)esp_log_timestamp() / 1000.0);

    char *json_str = cJSON_PrintUnformatted(call);
    if (json_str) {
        esp_mqtt_client_publish(mqtt_client, "intercom/call", json_str, 0, 0, 0);
        ESP_LOGI(TAG, "Sent mobile call notification to %s", current_target);
        free(json_str);
    }
    cJSON_Delete(call);
}

void ha_mqtt_send_call(const char *target_room)
{
    if (!mqtt_connected || !mqtt_client || !target_room) return;
    if (strcmp(target_room, "All Rooms") == 0) return;  // Use ha_mqtt_send_call_all_rooms() instead

    const settings_t *cfg = settings_get();

    // Publish call notification
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "target", target_room);
    cJSON_AddStringToObject(call, "caller", cfg->room_name);

    char *json_str = cJSON_PrintUnformatted(call);
    if (json_str) {
        esp_mqtt_client_publish(mqtt_client, "intercom/call", json_str, 0, 0, 0);
        ESP_LOGI(TAG, "Sent call to %s", target_room);
        free(json_str);
    }
    cJSON_Delete(call);
}

int ha_mqtt_send_call_all_rooms(void)
{
    if (!mqtt_connected || !mqtt_client) return 0;

    int call_count = 0;
    for (int i = 0; i < discovered_count; i++) {
        if (!discovered_devices[i].active) continue;
        if (!discovered_devices[i].available) continue;

        // Skip self — don't ring our own chime
        if (strcmp(discovered_devices[i].id, unique_id) == 0) continue;

        ha_mqtt_send_call(discovered_devices[i].room);
        call_count++;
    }

    ESP_LOGI(TAG, "Sent call to all rooms (%d devices)", call_count);
    return call_count;
}

bool ha_mqtt_check_incoming_call(char *caller_name)
{
    if (!incoming_call_pending) return false;

    // Copy caller name if buffer provided
    if (caller_name) {
        strncpy(caller_name, incoming_call_caller, 31);
        caller_name[31] = '\0';
    }

    // Clear pending state
    incoming_call_pending = false;
    incoming_call_caller[0] = '\0';

    return true;
}
