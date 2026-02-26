/**
 * Network Module
 *
 * UDP multicast/unicast for audio streaming.
 * AP fallback mode now uses WPA2-PSK for security.
 */

#include "network.h"
#include "settings.h"
#include "display.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "freertos/timers.h"
#include "lwip/sockets.h"
#include "lwip/igmp.h"
#include <string.h>

static const char *TAG = "network";

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;
static bool wifi_connected = false;
static bool ap_mode_active = false;
static esp_ip4_addr_t local_ip = {0};
static int connection_retries = 0;
#define MAX_RETRIES 10
#define AP_SSID_PREFIX "Intercom-"

static int tx_socket = -1;
static int rx_socket = -1;
static TaskHandle_t rx_task_handle = NULL;
static network_rx_callback_t rx_callback = NULL;
static bool rx_running = false;

// mDNS periodic re-announcement timer
static TimerHandle_t mdns_reannounce_timer = NULL;
#define MDNS_REANNOUNCE_INTERVAL_MS 60000

// TX packet statistics
static uint32_t tx_packets_sent = 0;
static uint32_t tx_packets_failed = 0;
static int tx_last_errno = 0;

// Forward declaration
static void start_ap_mode(void);

// Periodic mDNS re-announcement (safety net for missed events)
static void mdns_reannounce_timer_cb(TimerHandle_t timer)
{
    if (wifi_connected && sta_netif) {
        mdns_netif_action(sta_netif, MDNS_EVENT_ANNOUNCE_IP4);
        ESP_LOGD(TAG, "mDNS periodic re-announce");
    }
}

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
                wifi_connected = false;
                // Disable mDNS on this interface while disconnected
                if (sta_netif) {
                    mdns_netif_action(sta_netif, MDNS_EVENT_DISABLE_IP4);
                }
                connection_retries++;
                if (connection_retries >= MAX_RETRIES && !ap_mode_active) {
                    ESP_LOGW(TAG, "WiFi connection failed after %d attempts, starting AP mode", MAX_RETRIES);
                    start_ap_mode();
                } else if (!ap_mode_active) {
                    ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d...", connection_retries, MAX_RETRIES);
                    esp_wifi_connect();
                }
                break;
            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG, "Station connected to AP");
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                ESP_LOGI(TAG, "Station disconnected from AP");
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        local_ip = event->ip_info.ip;
        wifi_connected = true;
        connection_retries = 0;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&local_ip));

        // Log WiFi RSSI at connection time
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "[NET] wifi_rssi=%d", ap_info.rssi);
        }

        // Re-enable mDNS on this interface after WiFi reconnect
        // ENABLE_IP4 (not ANNOUNCE_IP4) is needed because DISABLE_IP4
        // on disconnect tears down the PCB — ANNOUNCE only works on a running PCB
        if (sta_netif) {
            mdns_netif_action(sta_netif, MDNS_EVENT_ENABLE_IP4);
            ESP_LOGI(TAG, "mDNS re-enabled after IP obtained");
        }
    }
}

// Start AP mode for configuration (now with WPA2-PSK security)
static void start_ap_mode(void)
{
    if (ap_mode_active) return;

    ESP_LOGI(TAG, "Starting AP mode for configuration...");

    // Stop STA mode
    esp_wifi_stop();

    // Create AP netif if not exists
    if (!ap_netif) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }

    // Generate AP SSID from MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);

    // Get or generate AP password
    const settings_t *s = settings_get();
    char ap_password[16];

    if (strlen(s->ap_password) >= 8) {
        // Use configured password
        strncpy(ap_password, s->ap_password, sizeof(ap_password) - 1);
        ap_password[sizeof(ap_password) - 1] = '\0';
    } else {
        // Use fixed default password for easy setup
        strncpy(ap_password, "intercom1", sizeof(ap_password) - 1);
        ap_password[sizeof(ap_password) - 1] = '\0';
    }

    wifi_config_t ap_config = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
        },
    };
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    strncpy((char *)ap_config.ap.password, ap_password, sizeof(ap_config.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ap_mode_active = true;

    // Set AP IP as local_ip so webserver works
    local_ip.addr = esp_ip4addr_aton("192.168.4.1");

    ESP_LOGI(TAG, "AP mode started: SSID='%s' (WPA2), Password='%s', IP=192.168.4.1", ap_ssid, ap_password);
    ESP_LOGI(TAG, "Connect to this network and go to http://192.168.4.1 to configure");

    // Show AP credentials on OLED for easy setup
    display_show_ap_info(ap_ssid, ap_password);
}

esp_err_t network_init(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Initializing network");

    // NVS is already initialized by settings_init()

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    sta_netif = esp_netif_create_default_wifi_sta();

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
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi power save disabled for multicast reliability");

    // Create TX socket
    tx_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tx_socket < 0) {
        ESP_LOGE(TAG, "Failed to create TX socket");
        return ESP_FAIL;
    }

    // Set multicast TTL
    uint8_t ttl = MULTICAST_TTL;
    setsockopt(tx_socket, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    // Disable multicast loopback - prevent receiving our own packets
    uint8_t loop = 0;
    setsockopt(tx_socket, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

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

bool network_is_ap_mode(void)
{
    return ap_mode_active;
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

    // Periodic RX stats (logged every 10 seconds)
    uint32_t rx_stat_packets = 0;
    uint32_t rx_stat_bytes = 0;
    char rx_stat_last_src[16] = "none";
    TickType_t last_stats_tick = xTaskGetTickCount();
    const TickType_t stats_interval = pdMS_TO_TICKS(10000);

    ESP_LOGI(TAG, "RX task started");

    while (rx_running) {
        int len = recvfrom(rx_socket, rx_buffer, sizeof(rx_buffer), 0,
                          (struct sockaddr *)&src_addr, &addr_len);

        if (len > 0 && rx_callback && len >= HEADER_LENGTH) {
            audio_packet_t *packet = (audio_packet_t *)rx_buffer;

            rx_stat_packets++;
            rx_stat_bytes += (uint32_t)len;
            inet_ntoa_r(src_addr.sin_addr, rx_stat_last_src, sizeof(rx_stat_last_src));

            rx_callback(packet, len);
        }

        // Log periodic RX stats every 10 seconds
        TickType_t now = xTaskGetTickCount();
        if ((now - last_stats_tick) >= stats_interval) {
            ESP_LOGI(TAG, "[NET] rx_stats: packets=%lu bytes=%lu last_src=%s",
                     (unsigned long)rx_stat_packets, (unsigned long)rx_stat_bytes,
                     rx_stat_last_src);
            rx_stat_packets = 0;
            rx_stat_bytes = 0;
            last_stats_tick = now;
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
    ESP_LOGI(TAG, "[NET] multicast_join: group=%s port=%d", MULTICAST_GROUP, AUDIO_PORT);

    // Boot-time TX test: send a 1-byte probe to confirm the TX socket can
    // actually send after WiFi is up.  The packet is too small to be a valid
    // audio packet (< HEADER_LENGTH=12 bytes), so receivers silently ignore it.
    {
        uint8_t probe = 0xAA;
        struct sockaddr_in probe_addr = {
            .sin_family      = AF_INET,
            .sin_port        = htons(AUDIO_PORT),
            .sin_addr.s_addr = inet_addr(MULTICAST_GROUP),
        };
        int probe_sent = sendto(tx_socket, &probe, 1, 0,
                                (struct sockaddr *)&probe_addr, sizeof(probe_addr));
        if (probe_sent == 1) {
            ESP_LOGI(TAG, "TX socket boot test OK");
        } else {
            ESP_LOGE(TAG, "TX socket boot test FAILED: sent=%d errno=%d", probe_sent, errno);
        }
    }

    // Set receive timeout
    struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};  // 100ms
    setsockopt(rx_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Increase receive buffer size
    int rcvbuf = 32768;
    setsockopt(rx_socket, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // Start RX task - needs larger stack for Opus decoding
    rx_running = true;
    xTaskCreate(rx_task, "network_rx", 16384, NULL, 5, &rx_task_handle);

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

    if (sent != (int)len) {
        tx_last_errno = errno;
        tx_packets_failed++;
        ESP_LOGW(TAG, "Multicast sendto failed: sent=%d expected=%d errno=%d", sent, (int)len, errno);
        return ESP_FAIL;
    }
    tx_packets_sent++;
    return ESP_OK;
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

    if (sent != (int)len) {
        tx_last_errno = errno;
        tx_packets_failed++;
        ESP_LOGW(TAG, "Unicast sendto %s failed: sent=%d expected=%d errno=%d", dest_ip, sent, (int)len, errno);
        return ESP_FAIL;
    }
    tx_packets_sent++;
    return ESP_OK;
}

esp_err_t network_set_hostname(const char *hostname)
{
    if (!sta_netif) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_netif_set_hostname(sta_netif, hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set DHCP hostname: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DHCP hostname set to: %s", hostname);
    }
    return err;
}

esp_err_t network_start_mdns(const char *hostname)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Set hostname (what appears before .local)
    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
        return err;
    }

    // Set instance name
    char instance[64];
    snprintf(instance, sizeof(instance), "Intercom - %s", hostname);
    mdns_instance_name_set(instance);

    // Advertise HTTP service
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    // Start periodic re-announcement timer (safety net for missed events)
    if (!mdns_reannounce_timer) {
        mdns_reannounce_timer = xTimerCreate("mdns_reann",
            pdMS_TO_TICKS(MDNS_REANNOUNCE_INTERVAL_MS),
            pdTRUE, NULL, mdns_reannounce_timer_cb);
        if (mdns_reannounce_timer) {
            xTimerStart(mdns_reannounce_timer, 0);
        }
    }

    ESP_LOGI(TAG, "mDNS started: %s.local", hostname);
    return ESP_OK;
}

void network_get_tx_stats(uint32_t *sent, uint32_t *failed, int *last_errno)
{
    if (sent)       *sent       = tx_packets_sent;
    if (failed)     *failed     = tx_packets_failed;
    if (last_errno) *last_errno = tx_last_errno;
}

void network_deinit(void)
{
    if (mdns_reannounce_timer) {
        xTimerStop(mdns_reannounce_timer, 0);
        xTimerDelete(mdns_reannounce_timer, 0);
        mdns_reannounce_timer = NULL;
    }
    mdns_free();
    network_stop_rx();

    if (tx_socket >= 0) {
        close(tx_socket);
        tx_socket = -1;
    }

    esp_wifi_stop();
    esp_wifi_deinit();

    ESP_LOGI(TAG, "Network deinitialized");
}
