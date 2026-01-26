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

// Callback for settings changes from HA
static ha_mqtt_callback_t user_callback = NULL;

static void publish_discovery(void);
static void publish_all_states(void);

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
    ESP_LOGI(TAG, "Published HA discovery (sensor, volume, mute, led)");
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
}

/**
 * Handle incoming MQTT commands.
 */
static void handle_mqtt_data(esp_mqtt_event_handle_t event)
{
    // Null-terminate topic and data for easier handling
    char topic[128] = {0};
    char data[64] = {0};

    size_t topic_len = event->topic_len < sizeof(topic) - 1 ? event->topic_len : sizeof(topic) - 1;
    size_t data_len = event->data_len < sizeof(data) - 1 ? event->data_len : sizeof(data) - 1;

    memcpy(topic, event->topic, topic_len);
    memcpy(data, event->data, data_len);

    ESP_LOGI(TAG, "MQTT cmd: %s = %s", topic, data);

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
            ESP_LOGI(TAG, "Subscribed to command topics");

            // Publish current states
            publish_all_states();
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
