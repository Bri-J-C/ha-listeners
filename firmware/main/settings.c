/**
 * Settings Module
 *
 * Persistent configuration storage using NVS.
 */

#include "settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "settings";
static const char *NVS_NAMESPACE = "intercom";

// Current settings (loaded at init)
static settings_t current_settings = {
    .wifi_ssid = "",
    .wifi_password = "",
    .room_name = "Intercom",
    .volume = 100,
    .configured = false,
    .mqtt_host = "",
    .mqtt_port = 1883,
    .mqtt_user = "",
    .mqtt_password = "",
    .mqtt_enabled = false,
    .muted = false,
    .led_enabled = true,
};

static nvs_handle_t settings_nvs = 0;

// Deferred save mechanism to avoid blocking on rapid changes
static bool save_pending = false;
static uint32_t last_change_time = 0;
#define SAVE_DELAY_MS 2000  // Wait 2 seconds after last change before saving

esp_err_t settings_init(void)
{
    ESP_LOGI(TAG, "Initializing settings");

    // Initialize NVS flash (required before opening namespace)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    // Open NVS namespace
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &settings_nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    // Load WiFi SSID
    size_t len = sizeof(current_settings.wifi_ssid);
    ret = nvs_get_str(settings_nvs, "wifi_ssid", current_settings.wifi_ssid, &len);
    if (ret == ESP_OK && len > 1) {
        current_settings.configured = true;
    }

    // Load WiFi password
    len = sizeof(current_settings.wifi_password);
    nvs_get_str(settings_nvs, "wifi_pass", current_settings.wifi_password, &len);

    // Load room name
    len = sizeof(current_settings.room_name);
    ret = nvs_get_str(settings_nvs, "room_name", current_settings.room_name, &len);
    if (ret != ESP_OK) {
        strcpy(current_settings.room_name, "Intercom");
    }

    // Load volume
    uint8_t vol = 100;
    ret = nvs_get_u8(settings_nvs, "volume", &vol);
    current_settings.volume = (ret == ESP_OK) ? vol : 100;

    // Load MQTT settings
    len = sizeof(current_settings.mqtt_host);
    nvs_get_str(settings_nvs, "mqtt_host", current_settings.mqtt_host, &len);

    uint16_t port = 1883;
    nvs_get_u16(settings_nvs, "mqtt_port", &port);
    current_settings.mqtt_port = port;

    len = sizeof(current_settings.mqtt_user);
    nvs_get_str(settings_nvs, "mqtt_user", current_settings.mqtt_user, &len);

    len = sizeof(current_settings.mqtt_password);
    nvs_get_str(settings_nvs, "mqtt_pass", current_settings.mqtt_password, &len);

    uint8_t mqtt_en = 0;
    nvs_get_u8(settings_nvs, "mqtt_en", &mqtt_en);
    current_settings.mqtt_enabled = (mqtt_en == 1);

    // Load mute and LED settings
    uint8_t muted = 0;
    nvs_get_u8(settings_nvs, "muted", &muted);
    current_settings.muted = (muted == 1);

    uint8_t led_en = 1;  // Default to enabled
    nvs_get_u8(settings_nvs, "led_en", &led_en);
    current_settings.led_enabled = (led_en == 1);

    ESP_LOGI(TAG, "Settings loaded: room='%s', configured=%d, volume=%d, mqtt=%s, muted=%d, led=%d",
             current_settings.room_name, current_settings.configured,
             current_settings.volume, current_settings.mqtt_enabled ? "on" : "off",
             current_settings.muted, current_settings.led_enabled);

    return ESP_OK;
}

const settings_t* settings_get(void)
{
    return &current_settings;
}

esp_err_t settings_set_wifi(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(current_settings.wifi_ssid, ssid, SETTINGS_SSID_MAX - 1);
    current_settings.wifi_ssid[SETTINGS_SSID_MAX - 1] = '\0';

    if (password) {
        strncpy(current_settings.wifi_password, password, SETTINGS_PASSWORD_MAX - 1);
        current_settings.wifi_password[SETTINGS_PASSWORD_MAX - 1] = '\0';
    } else {
        current_settings.wifi_password[0] = '\0';
    }

    current_settings.configured = true;

    // Save to NVS
    esp_err_t ret = nvs_set_str(settings_nvs, "wifi_ssid", current_settings.wifi_ssid);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_str(settings_nvs, "wifi_pass", current_settings.wifi_password);
    if (ret != ESP_OK) return ret;

    ret = nvs_commit(settings_nvs);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials saved: SSID='%s'", current_settings.wifi_ssid);
    }

    return ret;
}

esp_err_t settings_set_room(const char *room_name)
{
    if (!room_name || strlen(room_name) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(current_settings.room_name, room_name, SETTINGS_ROOM_MAX - 1);
    current_settings.room_name[SETTINGS_ROOM_MAX - 1] = '\0';

    esp_err_t ret = nvs_set_str(settings_nvs, "room_name", current_settings.room_name);
    if (ret != ESP_OK) return ret;

    ret = nvs_commit(settings_nvs);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Room name saved: '%s'", current_settings.room_name);
    }

    return ret;
}

esp_err_t settings_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    current_settings.volume = volume;

    // Mark for deferred save (don't block on rapid volume changes)
    save_pending = true;
    last_change_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

    return ESP_OK;
}

bool settings_is_configured(void)
{
    return current_settings.configured;
}

esp_err_t settings_reset(void)
{
    ESP_LOGW(TAG, "Resetting all settings");

    memset(&current_settings, 0, sizeof(current_settings));
    strcpy(current_settings.room_name, "Intercom");
    current_settings.volume = 100;
    current_settings.configured = false;
    current_settings.mqtt_port = 1883;
    current_settings.mqtt_enabled = false;
    current_settings.muted = false;
    current_settings.led_enabled = true;

    esp_err_t ret = nvs_erase_all(settings_nvs);
    if (ret != ESP_OK) return ret;

    return nvs_commit(settings_nvs);
}

esp_err_t settings_set_mqtt(const char *host, uint16_t port,
                            const char *user, const char *password)
{
    if (host) {
        strncpy(current_settings.mqtt_host, host, SETTINGS_MQTT_HOST_MAX - 1);
        current_settings.mqtt_host[SETTINGS_MQTT_HOST_MAX - 1] = '\0';
    }

    current_settings.mqtt_port = port ? port : 1883;

    if (user) {
        strncpy(current_settings.mqtt_user, user, SETTINGS_MQTT_USER_MAX - 1);
        current_settings.mqtt_user[SETTINGS_MQTT_USER_MAX - 1] = '\0';
    }

    if (password) {
        strncpy(current_settings.mqtt_password, password, SETTINGS_MQTT_PASS_MAX - 1);
        current_settings.mqtt_password[SETTINGS_MQTT_PASS_MAX - 1] = '\0';
    }

    // Save to NVS
    esp_err_t ret = nvs_set_str(settings_nvs, "mqtt_host", current_settings.mqtt_host);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_u16(settings_nvs, "mqtt_port", current_settings.mqtt_port);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_str(settings_nvs, "mqtt_user", current_settings.mqtt_user);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_str(settings_nvs, "mqtt_pass", current_settings.mqtt_password);
    if (ret != ESP_OK) return ret;

    ret = nvs_commit(settings_nvs);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT settings saved: host='%s', port=%d",
                 current_settings.mqtt_host, current_settings.mqtt_port);
    }

    return ret;
}

esp_err_t settings_set_mqtt_enabled(bool enabled)
{
    current_settings.mqtt_enabled = enabled;

    esp_err_t ret = nvs_set_u8(settings_nvs, "mqtt_en", enabled ? 1 : 0);
    if (ret != ESP_OK) return ret;

    ret = nvs_commit(settings_nvs);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT %s", enabled ? "enabled" : "disabled");
    }

    return ret;
}

esp_err_t settings_set_mute(bool muted)
{
    current_settings.muted = muted;

    // Mark for deferred save
    save_pending = true;
    last_change_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "Mute %s", muted ? "enabled" : "disabled");

    return ESP_OK;
}

esp_err_t settings_set_led_enabled(bool enabled)
{
    current_settings.led_enabled = enabled;

    // Mark for deferred save
    save_pending = true;
    last_change_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "LED %s", enabled ? "enabled" : "disabled");

    return ESP_OK;
}

void settings_save_if_needed(void)
{
    if (!save_pending) return;

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if ((now - last_change_time) < SAVE_DELAY_MS) return;

    // Enough time has passed, save all settings
    save_pending = false;

    nvs_set_u8(settings_nvs, "volume", current_settings.volume);
    nvs_set_u8(settings_nvs, "muted", current_settings.muted ? 1 : 0);
    nvs_set_u8(settings_nvs, "led_en", current_settings.led_enabled ? 1 : 0);

    esp_err_t ret = nvs_commit(settings_nvs);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Settings saved (volume=%d, muted=%d, led=%d)",
                 current_settings.volume, current_settings.muted, current_settings.led_enabled);
    }
}
