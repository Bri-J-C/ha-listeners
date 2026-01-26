/**
 * Network Module
 *
 * UDP multicast/unicast for audio streaming.
 */

#include "network.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/igmp.h"
#include <string.h>

static const char *TAG = "network";

static esp_netif_t *netif = NULL;
static bool wifi_connected = false;
static esp_ip4_addr_t local_ip = {0};

static int tx_socket = -1;
static int rx_socket = -1;
static TaskHandle_t rx_task_handle = NULL;
static network_rx_callback_t rx_callback = NULL;
static bool rx_running = false;

// WiFi event handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi started, connecting...");
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
                wifi_connected = false;
                esp_wifi_connect();
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        local_ip = event->ip_info.ip;
        wifi_connected = true;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&local_ip));
    }
}

esp_err_t network_init(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Initializing network");

    // NVS is already initialized by settings_init()

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    netif = esp_netif_create_default_wifi_sta();

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &wifi_event_handler, NULL, NULL));

    // Configure WiFi
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Create TX socket
    tx_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tx_socket < 0) {
        ESP_LOGE(TAG, "Failed to create TX socket");
        return ESP_FAIL;
    }

    // Set multicast TTL
    uint8_t ttl = MULTICAST_TTL;
    setsockopt(tx_socket, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    ESP_LOGI(TAG, "Network initialized");
    return ESP_OK;
}

esp_err_t network_wait_connected(uint32_t timeout_ms)
{
    uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while (!wifi_connected) {
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - start) > timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_OK;
}

bool network_is_connected(void)
{
    return wifi_connected;
}

void network_get_ip(char *ip_str)
{
    sprintf(ip_str, IPSTR, IP2STR(&local_ip));
}

void network_set_rx_callback(network_rx_callback_t callback)
{
    rx_callback = callback;
}

// RX task - receives UDP packets
static void rx_task(void *arg)
{
    uint8_t rx_buffer[MAX_PACKET_SIZE];
    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);

    ESP_LOGI(TAG, "RX task started");

    while (rx_running) {
        int len = recvfrom(rx_socket, rx_buffer, sizeof(rx_buffer), 0,
                          (struct sockaddr *)&src_addr, &addr_len);

        if (len > 0 && rx_callback && len >= HEADER_LENGTH) {
            audio_packet_t *packet = (audio_packet_t *)rx_buffer;
            rx_callback(packet, len);
        }
    }

    ESP_LOGI(TAG, "RX task stopped");
    vTaskDelete(NULL);
}

esp_err_t network_start_rx(void)
{
    if (rx_socket >= 0) {
        return ESP_OK;  // Already started
    }

    // Create RX socket
    rx_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (rx_socket < 0) {
        ESP_LOGE(TAG, "Failed to create RX socket");
        return ESP_FAIL;
    }

    // Allow address reuse
    int reuse = 1;
    setsockopt(rx_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Bind to audio port
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(AUDIO_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(rx_socket, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind RX socket");
        close(rx_socket);
        rx_socket = -1;
        return ESP_FAIL;
    }

    // Join multicast group using our local IP
    struct ip_mreq mreq = {
        .imr_multiaddr.s_addr = inet_addr(MULTICAST_GROUP),
        .imr_interface.s_addr = local_ip.addr,  // Use our actual interface
    };

    if (setsockopt(rx_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ESP_LOGW(TAG, "Failed to join multicast group, trying INADDR_ANY");
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(rx_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            ESP_LOGE(TAG, "Failed to join multicast group");
            close(rx_socket);
            rx_socket = -1;
            return ESP_FAIL;
        }
    }

    // Set receive timeout
    struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};  // 100ms
    setsockopt(rx_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Increase receive buffer size
    int rcvbuf = 32768;
    setsockopt(rx_socket, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // Start RX task - needs larger stack for audio processing
    rx_running = true;
    xTaskCreate(rx_task, "network_rx", 8192, NULL, 5, &rx_task_handle);

    ESP_LOGI(TAG, "RX started on port %d, multicast group %s", AUDIO_PORT, MULTICAST_GROUP);
    return ESP_OK;
}

void network_stop_rx(void)
{
    if (rx_socket < 0) {
        return;
    }

    rx_running = false;

    // Leave multicast group
    struct ip_mreq mreq = {
        .imr_multiaddr.s_addr = inet_addr(MULTICAST_GROUP),
        .imr_interface.s_addr = htonl(INADDR_ANY),
    };
    setsockopt(rx_socket, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));

    close(rx_socket);
    rx_socket = -1;

    // Wait for task to finish
    if (rx_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(200));
        rx_task_handle = NULL;
    }

    ESP_LOGI(TAG, "RX stopped");
}

esp_err_t network_send_multicast(const audio_packet_t *packet, size_t len)
{
    if (tx_socket < 0) {
        return ESP_FAIL;
    }

    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(AUDIO_PORT),
        .sin_addr.s_addr = inet_addr(MULTICAST_GROUP),
    };

    int sent = sendto(tx_socket, packet, len, 0,
                      (struct sockaddr *)&dest_addr, sizeof(dest_addr));

    return (sent == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t network_send_unicast(const audio_packet_t *packet, size_t len, const char *dest_ip)
{
    if (tx_socket < 0) {
        return ESP_FAIL;
    }

    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(AUDIO_PORT),
        .sin_addr.s_addr = inet_addr(dest_ip),
    };

    int sent = sendto(tx_socket, packet, len, 0,
                      (struct sockaddr *)&dest_addr, sizeof(dest_addr));

    return (sent == len) ? ESP_OK : ESP_FAIL;
}

void network_deinit(void)
{
    network_stop_rx();

    if (tx_socket >= 0) {
        close(tx_socket);
        tx_socket = -1;
    }

    esp_wifi_stop();
    esp_wifi_deinit();

    ESP_LOGI(TAG, "Network deinitialized");
}
