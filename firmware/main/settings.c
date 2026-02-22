/**
 * Settings Module
 *
 * Persistent configuration storage using NVS.
 * Credentials are encrypted using AES-256-GCM with a device-unique key.
 */

#include "settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// Encryption constants
#define ENCRYPTION_VERSION 1
#define AES_KEY_SIZE 32      // 256 bits
#define GCM_IV_SIZE 12       // 96 bits recommended for GCM
#define GCM_TAG_SIZE 16      // 128 bits
// Encrypted blob: [1B version][12B IV][16B tag][encrypted data]
#define ENCRYPTED_OVERHEAD (1 + GCM_IV_SIZE + GCM_TAG_SIZE)

static uint8_t encryption_key[AES_KEY_SIZE];
static bool encryption_initialized = false;

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
    .mqtt_tls_enabled = false,  // TLS disabled by default (most HA setups don't have MQTT TLS)
    .muted = false,
    .led_enabled = true,
    .agc_enabled = true,
    .priority = 0,  // PRIORITY_NORMAL — defined in protocol.h
    .dnd_enabled = false,
    .web_admin_password = "",  // Empty = no password (first setup)
    .ap_password = "",         // Generated from device ID if empty
};

static nvs_handle_t settings_nvs = 0;

// Deferred save mechanism to avoid blocking on rapid changes
static bool save_pending = false;
static uint32_t last_change_time = 0;
#define SAVE_DELAY_MS 2000  // Wait 2 seconds after last change before saving

/**
 * Initialize device-unique encryption key from eFuse MAC.
 * Uses SHA-256(salt || MAC) to derive a 256-bit key.
 */
static void init_encryption_key(void)
{
    if (encryption_initialized) return;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    // Derive key using SHA-256(salt || MAC)
    // Salt is a fixed application-specific value
    const char *salt = "intercom-nvs-cred-key-v1";

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);  // 0 = SHA-256 (not SHA-224)
    mbedtls_sha256_update(&sha, (const uint8_t *)salt, strlen(salt));
    mbedtls_sha256_update(&sha, mac, sizeof(mac));
    mbedtls_sha256_finish(&sha, encryption_key);
    mbedtls_sha256_free(&sha);

    encryption_initialized = true;
    ESP_LOGI(TAG, "Encryption key derived from device ID");
}

/**
 * Encrypt a string for NVS storage.
 * Output format: [1B version][12B IV][16B tag][encrypted data]
 * Returns encrypted length, or -1 on error.
 */
static int encrypt_credential(const char *plaintext, uint8_t *output, size_t output_max)
{
    if (!encryption_initialized || !plaintext || !output) return -1;

    size_t plaintext_len = strlen(plaintext) + 1;  // Include null terminator
    size_t required = ENCRYPTED_OVERHEAD + plaintext_len;

    if (output_max < required) {
        ESP_LOGE(TAG, "Output buffer too small: need %d, have %d", required, output_max);
        return -1;
    }

    // Version byte
    output[0] = ENCRYPTION_VERSION;

    // Generate random IV
    uint8_t *iv = &output[1];
    esp_fill_random(iv, GCM_IV_SIZE);

    // Tag will be placed after IV
    uint8_t *tag = &output[1 + GCM_IV_SIZE];

    // Encrypted data after tag
    uint8_t *ciphertext = &output[ENCRYPTED_OVERHEAD];

    // Encrypt using AES-GCM
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, encryption_key, AES_KEY_SIZE * 8);
    if (ret != 0) {
        ESP_LOGE(TAG, "GCM setkey failed: %d", ret);
        mbedtls_gcm_free(&gcm);
        return -1;
    }

    ret = mbedtls_gcm_crypt_and_tag(
        &gcm, MBEDTLS_GCM_ENCRYPT,
        plaintext_len,
        iv, GCM_IV_SIZE,
        NULL, 0,  // No additional authenticated data
        (const uint8_t *)plaintext, ciphertext,
        GCM_TAG_SIZE, tag
    );

    mbedtls_gcm_free(&gcm);

    if (ret != 0) {
        ESP_LOGE(TAG, "GCM encrypt failed: %d", ret);
        return -1;
    }

    return required;
}

/**
 * Decrypt a credential from NVS storage.
 * Input format: [1B version][12B IV][16B tag][encrypted data]
 * Returns 0 on success, -1 on error (also handles legacy plaintext).
 */
static int decrypt_credential(const uint8_t *input, size_t input_len,
                               char *output, size_t output_max)
{
    if (!input || !output || input_len == 0) return -1;

    // Check if this is encrypted data (version byte check)
    if (input[0] != ENCRYPTION_VERSION) {
        // Likely legacy plaintext - just copy it
        size_t copy_len = (input_len < output_max) ? input_len : output_max - 1;
        memcpy(output, input, copy_len);
        output[copy_len] = '\0';
        ESP_LOGD(TAG, "Legacy plaintext credential loaded");
        return 0;
    }

    if (!encryption_initialized) {
        ESP_LOGE(TAG, "Encryption not initialized");
        return -1;
    }

    if (input_len < ENCRYPTED_OVERHEAD + 1) {
        ESP_LOGE(TAG, "Encrypted data too short");
        return -1;
    }

    const uint8_t *iv = &input[1];
    const uint8_t *tag = &input[1 + GCM_IV_SIZE];
    const uint8_t *ciphertext = &input[ENCRYPTED_OVERHEAD];
    size_t ciphertext_len = input_len - ENCRYPTED_OVERHEAD;

    if (ciphertext_len > output_max) {
        ESP_LOGE(TAG, "Output buffer too small");
        return -1;
    }

    // Decrypt using AES-GCM
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, encryption_key, AES_KEY_SIZE * 8);
    if (ret != 0) {
        mbedtls_gcm_free(&gcm);
        return -1;
    }

    ret = mbedtls_gcm_auth_decrypt(
        &gcm,
        ciphertext_len,
        iv, GCM_IV_SIZE,
        NULL, 0,  // No additional authenticated data
        tag, GCM_TAG_SIZE,
        ciphertext, (uint8_t *)output
    );

    mbedtls_gcm_free(&gcm);

    if (ret != 0) {
        ESP_LOGE(TAG, "GCM decrypt/verify failed: %d", ret);
        return -1;
    }

    return 0;
}

/**
 * Save encrypted credential to NVS.
 */
static esp_err_t save_encrypted_str(const char *key, const char *value)
{
    if (!encryption_initialized) {
        // Fallback to plaintext if encryption not available
        return nvs_set_str(settings_nvs, key, value);
    }

    // Max encrypted size: overhead + max password length + null
    uint8_t encrypted[ENCRYPTED_OVERHEAD + SETTINGS_PASSWORD_MAX + 1];
    int enc_len = encrypt_credential(value, encrypted, sizeof(encrypted));

    if (enc_len < 0) {
        ESP_LOGE(TAG, "Failed to encrypt %s", key);
        return ESP_FAIL;
    }

    return nvs_set_blob(settings_nvs, key, encrypted, enc_len);
}

/**
 * Load encrypted credential from NVS.
 */
static esp_err_t load_encrypted_str(const char *key, char *value, size_t value_max)
{
    // First try to load as blob (encrypted)
    uint8_t encrypted[ENCRYPTED_OVERHEAD + SETTINGS_PASSWORD_MAX + 1];
    size_t enc_len = sizeof(encrypted);

    esp_err_t ret = nvs_get_blob(settings_nvs, key, encrypted, &enc_len);
    if (ret == ESP_OK && enc_len > 0) {
        if (decrypt_credential(encrypted, enc_len, value, value_max) == 0) {
            return ESP_OK;
        }
    }

    // Fallback: try to load as plaintext string (legacy data)
    size_t str_len = value_max;
    ret = nvs_get_str(settings_nvs, key, value, &str_len);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Loaded legacy plaintext for %s", key);
        return ESP_OK;
    }

    value[0] = '\0';
    return ret;
}

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

    // Initialize encryption key from device ID
    init_encryption_key();

    // Load WiFi SSID (not encrypted - needed for display/debugging)
    size_t len = sizeof(current_settings.wifi_ssid);
    ret = nvs_get_str(settings_nvs, "wifi_ssid", current_settings.wifi_ssid, &len);
    if (ret == ESP_OK && len > 1) {
        current_settings.configured = true;
    }

    // Load WiFi password (encrypted)
    load_encrypted_str("wifi_pass", current_settings.wifi_password,
                       sizeof(current_settings.wifi_password));

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

    // Load MQTT password (encrypted)
    load_encrypted_str("mqtt_pass", current_settings.mqtt_password,
                       sizeof(current_settings.mqtt_password));

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

    // Load AGC setting (default to enabled)
    uint8_t agc_en = 1;
    nvs_get_u8(settings_nvs, "agc_en", &agc_en);
    current_settings.agc_enabled = (agc_en == 1);

    // Load priority (default: PRIORITY_NORMAL)
    uint8_t priority = 0;  // PRIORITY_NORMAL
    nvs_get_u8(settings_nvs, "priority", &priority);
    if (priority > 2) priority = 0;  // Clamp to valid range (PRIORITY_EMERGENCY = 2)
    current_settings.priority = priority;

    // Load DND (default: disabled)
    uint8_t dnd_en = 0;
    nvs_get_u8(settings_nvs, "dnd_en", &dnd_en);
    current_settings.dnd_enabled = (dnd_en == 1);

    // Load MQTT TLS setting (default to disabled for home use compatibility)
    uint8_t mqtt_tls = 0;
    nvs_get_u8(settings_nvs, "mqtt_tls", &mqtt_tls);
    current_settings.mqtt_tls_enabled = (mqtt_tls == 1);

    // Load security settings (encrypted)
    load_encrypted_str("web_pass", current_settings.web_admin_password,
                       sizeof(current_settings.web_admin_password));
    load_encrypted_str("ap_pass", current_settings.ap_password,
                       sizeof(current_settings.ap_password));

    ESP_LOGI(TAG, "Settings loaded: room='%s', configured=%d, volume=%d, mqtt=%s (tls=%d), muted=%d, led=%d, web_auth=%s",
             current_settings.room_name, current_settings.configured,
             current_settings.volume, current_settings.mqtt_enabled ? "on" : "off",
             current_settings.mqtt_tls_enabled, current_settings.muted, current_settings.led_enabled,
             strlen(current_settings.web_admin_password) > 0 ? "enabled" : "disabled");

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

    // Save to NVS (SSID plaintext, password encrypted)
    esp_err_t ret = nvs_set_str(settings_nvs, "wifi_ssid", current_settings.wifi_ssid);
    if (ret != ESP_OK) return ret;

    ret = save_encrypted_str("wifi_pass", current_settings.wifi_password);
    if (ret != ESP_OK) return ret;

    ret = nvs_commit(settings_nvs);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials saved (encrypted): SSID='%s'", current_settings.wifi_ssid);
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
    current_settings.mqtt_tls_enabled = false;  // TLS off by default for home use
    current_settings.muted = false;
    current_settings.led_enabled = true;
    current_settings.agc_enabled = true;
    current_settings.priority = 0;  // PRIORITY_NORMAL
    current_settings.dnd_enabled = false;
    current_settings.web_admin_password[0] = '\0';
    current_settings.ap_password[0] = '\0';

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

    // Save to NVS (password encrypted)
    esp_err_t ret = nvs_set_str(settings_nvs, "mqtt_host", current_settings.mqtt_host);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_u16(settings_nvs, "mqtt_port", current_settings.mqtt_port);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_str(settings_nvs, "mqtt_user", current_settings.mqtt_user);
    if (ret != ESP_OK) return ret;

    ret = save_encrypted_str("mqtt_pass", current_settings.mqtt_password);
    if (ret != ESP_OK) return ret;

    ret = nvs_commit(settings_nvs);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT settings saved (password encrypted): host='%s', port=%d",
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

esp_err_t settings_set_agc_enabled(bool enabled)
{
    current_settings.agc_enabled = enabled;

    // Mark for deferred save
    save_pending = true;
    last_change_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "AGC %s", enabled ? "enabled" : "disabled");

    return ESP_OK;
}

esp_err_t settings_set_priority(uint8_t priority)
{
    if (priority > 2) {  // PRIORITY_EMERGENCY = 2
        return ESP_ERR_INVALID_ARG;
    }

    current_settings.priority = priority;

    // Mark for deferred save
    save_pending = true;
    last_change_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

    const char *names[] = {"Normal", "High", "Emergency"};
    ESP_LOGI(TAG, "Priority set to %s (%d)", names[priority], priority);

    return ESP_OK;
}

esp_err_t settings_set_dnd(bool enabled)
{
    current_settings.dnd_enabled = enabled;

    // Mark for deferred save
    save_pending = true;
    last_change_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "DND %s", enabled ? "enabled" : "disabled");

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
    nvs_set_u8(settings_nvs, "agc_en", current_settings.agc_enabled ? 1 : 0);
    nvs_set_u8(settings_nvs, "priority", current_settings.priority);
    nvs_set_u8(settings_nvs, "dnd_en", current_settings.dnd_enabled ? 1 : 0);

    esp_err_t ret = nvs_commit(settings_nvs);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Settings saved (volume=%d, muted=%d, led=%d, agc=%d, priority=%d, dnd=%d)",
                 current_settings.volume, current_settings.muted,
                 current_settings.led_enabled, current_settings.agc_enabled,
                 current_settings.priority, current_settings.dnd_enabled);
    }
}

esp_err_t settings_set_web_admin_password(const char *password)
{
    if (password) {
        strncpy(current_settings.web_admin_password, password, SETTINGS_WEB_PASS_MAX - 1);
        current_settings.web_admin_password[SETTINGS_WEB_PASS_MAX - 1] = '\0';
    } else {
        current_settings.web_admin_password[0] = '\0';
    }

    esp_err_t ret = save_encrypted_str("web_pass", current_settings.web_admin_password);
    if (ret != ESP_OK) return ret;

    ret = nvs_commit(settings_nvs);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Web admin password %s (encrypted)",
                 strlen(current_settings.web_admin_password) > 0 ? "set" : "cleared");
    }

    return ret;
}

esp_err_t settings_set_ap_password(const char *password)
{
    if (password) {
        strncpy(current_settings.ap_password, password, SETTINGS_AP_PASS_MAX - 1);
        current_settings.ap_password[SETTINGS_AP_PASS_MAX - 1] = '\0';
    } else {
        current_settings.ap_password[0] = '\0';
    }

    esp_err_t ret = save_encrypted_str("ap_pass", current_settings.ap_password);
    if (ret != ESP_OK) return ret;

    ret = nvs_commit(settings_nvs);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "AP password %s (encrypted)",
                 strlen(current_settings.ap_password) > 0 ? "set" : "cleared");
    }

    return ret;
}

esp_err_t settings_set_mqtt_tls_enabled(bool enabled)
{
    current_settings.mqtt_tls_enabled = enabled;

    esp_err_t ret = nvs_set_u8(settings_nvs, "mqtt_tls", enabled ? 1 : 0);
    if (ret != ESP_OK) return ret;

    ret = nvs_commit(settings_nvs);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT TLS %s", enabled ? "enabled" : "disabled");
    }

    return ret;
}

bool settings_verify_web_password(const char *password)
{
    // If no password is set, always allow access (first-time setup)
    if (strlen(current_settings.web_admin_password) == 0) {
        return true;
    }

    // Compare passwords
    if (password == NULL) {
        return false;
    }

    return (strcmp(current_settings.web_admin_password, password) == 0);
}
