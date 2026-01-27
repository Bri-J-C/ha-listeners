/**
 * Home Assistant MQTT Integration
 *
 * MQTT client for Home Assistant auto-discovery with multiple entities:
 * - Sensor: Device state (idle/transmitting/receiving)
 * - Number: Volume control (0-100)
 * - Switch: Mute toggle
 */

#include "ha_mqtt.h"
#include "settings.h"
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
static const char *device_discovery_topic = "intercom/devices/+/info";

// Callback for settings changes from HA
static ha_mqtt_callback_t user_callback = NULL;

static void publish_discovery(void);
static void publish_all_states(void);
static void publish_target(void);
static void publish_target_discovery(void);

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
    cJSON_AddStringToObject(device, "sw_version", "1.2.0");

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
    cJSON_AddStringToObject(root, "object_id", uid);

    cJSON_AddStringToObject(root, "state_topic", state_topic);
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
    cJSON_AddStringToObject(root, "object_id", uid);

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
    cJSON_AddStringToObject(root, "object_id", uid);

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
    cJSON_AddStringToObject(root, "object_id", uid);

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
    publish_target_discovery();
    ESP_LOGI(TAG, "Published HA discovery (sensor, volume, mute, led, target)");
}

/**
 * Publish current state.
 */
static void publish_state(void)
{
    if (!mqtt_connected) return;

    const char *state_str;
    switch (current_state) {
        case HA_STATE_TRANSMITTING:
            state_str = "transmitting";
            break;
        case HA_STATE_RECEIVING:
            state_str = "receiving";
            break;
        default:
            state_str = "idle";
            break;
    }

    esp_mqtt_client_publish(mqtt_client, state_topic, state_str, 0, 0, true);
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
    publish_target();
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
    cJSON_AddStringToObject(root, "object_id", uid);

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

/**
 * Simple JSON string extractor (avoids cJSON allocation in MQTT handler).
 * Extracts value for "key":"value" pattern.
 */
static bool extract_json_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

    const char *start = strstr(json, pattern);
    if (!start) return false;

    start += strlen(pattern);
    const char *end = strchr(start, '"');
    if (!end) return false;

    size_t len = end - start;
    if (len >= out_len) len = out_len - 1;

    memcpy(out, start, len);
    out[len] = '\0';
    return true;
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

    // Simple JSON parsing without cJSON
    if (!extract_json_string(payload, "room", room, sizeof(room))) return;
    if (!extract_json_string(payload, "ip", ip, sizeof(ip))) return;
    if (!extract_json_string(payload, "id", id, sizeof(id))) return;

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
    } else if (discovered_count < MAX_DISCOVERED_DEVICES) {
        // Add new device
        strncpy(discovered_devices[discovered_count].room, room, sizeof(discovered_devices[0].room) - 1);
        strncpy(discovered_devices[discovered_count].ip, ip, sizeof(discovered_devices[0].ip) - 1);
        strncpy(discovered_devices[discovered_count].id, id, sizeof(discovered_devices[0].id) - 1);
        discovered_devices[discovered_count].active = true;
        discovered_count++;
        ESP_LOGI(TAG, "Discovered device: %s (%s) at %s", room, id, ip);

        // Defer target discovery update to main loop (avoid cJSON in MQTT handler)
        target_discovery_pending = true;
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

    // Check if this is device info (don't log these to reduce spam)
    bool is_device_info = (strncmp(topic, "intercom/devices/", 17) == 0 &&
                           strstr(topic, "/info") != NULL);

    if (!is_device_info) {
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

        // Update LED to show muted state
        if (muted) {
            button_set_led_state(LED_STATE_MUTED);
        } else {
            button_set_led_state(LED_STATE_IDLE);
        }

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
    // Target command
    else if (strcmp(topic, target_cmd_topic) == 0) {
        strncpy(current_target, data, sizeof(current_target) - 1);
        current_target[sizeof(current_target) - 1] = '\0';
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

            // Subscribe to device discovery to learn about other intercoms
            esp_mqtt_client_subscribe(mqtt_client, device_discovery_topic, 0);
            ESP_LOGI(TAG, "Subscribed to command topics and device discovery");

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

    // Build broker URI
    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", cfg->mqtt_host, cfg->mqtt_port);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.username = cfg->mqtt_user,
        .credentials.authentication.password = cfg->mqtt_password,
        .session.last_will.topic = availability_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };

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

    ESP_LOGI(TAG, "MQTT client started, connecting to %s", uri);
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
