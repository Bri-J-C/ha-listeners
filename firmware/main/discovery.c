/**
 * Discovery Module
 *
 * Announce device to Home Assistant and receive configuration.
 */

#include "discovery.h"
#include "network.h"
#include "esp_log.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "discovery";

static char device_name[32] = "Intercom";
static uint8_t device_id[DEVICE_ID_LENGTH] = {0};
static device_config_t current_config = {
    .room = "unknown",
    .default_target = "all",
    .target_ip = "",
    .volume = 80,
    .muted = false,
};

static int control_socket = -1;
static TaskHandle_t discovery_task_handle = NULL;
static discovery_config_callback_t config_callback = NULL;
static bool discovery_running = false;

// Build JSON announcement message
static int build_announce_json(char *buffer, size_t max_len)
{
    char ip_str[16];
    network_get_ip(ip_str);

    char device_id_hex[DEVICE_ID_LENGTH * 2 + 1];
    for (int i = 0; i < DEVICE_ID_LENGTH; i++) {
        sprintf(&device_id_hex[i * 2], "%02x", device_id[i]);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "announce");
    cJSON_AddStringToObject(root, "device_id", device_id_hex);
    cJSON_AddStringToObject(root, "name", device_name);
    cJSON_AddStringToObject(root, "ip", ip_str);
    cJSON_AddStringToObject(root, "version", "1.0.0");

    cJSON *caps = cJSON_CreateArray();
    cJSON_AddItemToArray(caps, cJSON_CreateString("audio"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("ptt"));
    cJSON_AddItemToObject(root, "capabilities", caps);

    char *json_str = cJSON_PrintUnformatted(root);
    int len = strlen(json_str);
    if (len < max_len) {
        strcpy(buffer, json_str);
    } else {
        len = -1;
    }

    free(json_str);
    cJSON_Delete(root);

    return len;
}

// Parse configuration JSON from HA
static bool parse_config_json(const char *json_str, device_config_t *config)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return false;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!type || strcmp(type->valuestring, "config") != 0) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *room = cJSON_GetObjectItem(root, "room");
    if (room && room->valuestring) {
        strncpy(config->room, room->valuestring, sizeof(config->room) - 1);
    }

    cJSON *default_target = cJSON_GetObjectItem(root, "default_target");
    if (default_target && default_target->valuestring) {
        strncpy(config->default_target, default_target->valuestring, sizeof(config->default_target) - 1);
    }

    cJSON *volume = cJSON_GetObjectItem(root, "volume");
    if (volume && cJSON_IsNumber(volume)) {
        config->volume = (uint8_t)volume->valueint;
    }

    cJSON *muted = cJSON_GetObjectItem(root, "muted");
    if (muted) {
        config->muted = cJSON_IsTrue(muted);
    }

    // Get target IP from targets map
    cJSON *targets = cJSON_GetObjectItem(root, "targets");
    if (targets) {
        cJSON *target_ip = cJSON_GetObjectItem(targets, config->default_target);
        if (target_ip && target_ip->valuestring) {
            strncpy(config->target_ip, target_ip->valuestring, sizeof(config->target_ip) - 1);
        }
    }

    cJSON_Delete(root);
    return true;
}

// Discovery task - sends announcements and receives config
static void discovery_task(void *arg)
{
    char tx_buffer[512];
    char rx_buffer[512];
    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);
    TickType_t last_announce = 0;

    ESP_LOGI(TAG, "Discovery task started");

    while (discovery_running) {
        TickType_t now = xTaskGetTickCount();

        // Send periodic announcement
        if ((now - last_announce) >= pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS)) {
            int len = build_announce_json(tx_buffer, sizeof(tx_buffer));
            if (len > 0) {
                struct sockaddr_in dest_addr = {
                    .sin_family = AF_INET,
                    .sin_port = htons(CONTROL_PORT),
                    .sin_addr.s_addr = htonl(INADDR_BROADCAST),
                };

                // Enable broadcast
                int broadcast = 1;
                setsockopt(control_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

                sendto(control_socket, tx_buffer, len, 0,
                       (struct sockaddr *)&dest_addr, sizeof(dest_addr));

                ESP_LOGD(TAG, "Sent announcement");
            }
            last_announce = now;
        }

        // Check for incoming config
        int len = recvfrom(control_socket, rx_buffer, sizeof(rx_buffer) - 1, 0,
                          (struct sockaddr *)&src_addr, &addr_len);

        if (len > 0) {
            rx_buffer[len] = '\0';
            ESP_LOGD(TAG, "Received: %s", rx_buffer);

            device_config_t new_config;
            memcpy(&new_config, &current_config, sizeof(device_config_t));

            if (parse_config_json(rx_buffer, &new_config)) {
                memcpy(&current_config, &new_config, sizeof(device_config_t));
                ESP_LOGI(TAG, "Config updated: room=%s, target=%s, volume=%d",
                         current_config.room, current_config.default_target, current_config.volume);

                if (config_callback) {
                    config_callback(&current_config);
                }
            }
        }
    }

    ESP_LOGI(TAG, "Discovery task stopped");
    vTaskDelete(NULL);
}

esp_err_t discovery_init(const char *name, const uint8_t *id)
{
    strncpy(device_name, name, sizeof(device_name) - 1);
    memcpy(device_id, id, DEVICE_ID_LENGTH);

    // Create control socket
    control_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (control_socket < 0) {
        ESP_LOGE(TAG, "Failed to create control socket");
        return ESP_FAIL;
    }

    // Allow address reuse
    int reuse = 1;
    setsockopt(control_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Bind to control port
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONTROL_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(control_socket, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind control socket");
        close(control_socket);
        control_socket = -1;
        return ESP_FAIL;
    }

    // Set receive timeout
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(control_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "Discovery initialized: name=%s", device_name);
    return ESP_OK;
}

void discovery_set_config_callback(discovery_config_callback_t callback)
{
    config_callback = callback;
}

esp_err_t discovery_start(void)
{
    if (discovery_running) {
        return ESP_OK;
    }

    discovery_running = true;
    xTaskCreate(discovery_task, "discovery", 4096, NULL, 4, &discovery_task_handle);

    ESP_LOGI(TAG, "Discovery started");
    return ESP_OK;
}

void discovery_stop(void)
{
    discovery_running = false;

    if (discovery_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(1100));  // Wait for task to finish
        discovery_task_handle = NULL;
    }

    ESP_LOGI(TAG, "Discovery stopped");
}

void discovery_announce_now(void)
{
    if (control_socket < 0) {
        return;
    }

    char tx_buffer[512];
    int len = build_announce_json(tx_buffer, sizeof(tx_buffer));

    if (len > 0) {
        struct sockaddr_in dest_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(CONTROL_PORT),
            .sin_addr.s_addr = htonl(INADDR_BROADCAST),
        };

        int broadcast = 1;
        setsockopt(control_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

        sendto(control_socket, tx_buffer, len, 0,
               (struct sockaddr *)&dest_addr, sizeof(dest_addr));

        ESP_LOGI(TAG, "Sent immediate announcement");
    }
}

const device_config_t *discovery_get_config(void)
{
    return &current_config;
}

void discovery_deinit(void)
{
    discovery_stop();

    if (control_socket >= 0) {
        close(control_socket);
        control_socket = -1;
    }

    ESP_LOGI(TAG, "Discovery deinitialized");
}
