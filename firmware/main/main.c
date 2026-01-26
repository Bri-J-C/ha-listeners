/**
 * HA Intercom - Main Application
 *
 * ESP32-S3 based intercom satellite for Home Assistant.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"

#include "protocol.h"
#include "audio_input.h"
#include "audio_output.h"
#include "codec.h"
#include "network.h"
#include "discovery.h"
#include "button.h"
#include "settings.h"
#include "webserver.h"
#include "ha_mqtt.h"

static const char *TAG = "main";

// Fallback WiFi credentials (used if no settings saved)
#define DEFAULT_WIFI_SSID       "wifi"
#define DEFAULT_WIFI_PASSWORD   "your_wifi_password"

// Application state
static uint8_t device_id[DEVICE_ID_LENGTH];
static volatile bool transmitting = false;
static volatile bool broadcast_mode = false;
static uint32_t tx_sequence = 0;

// Audio buffers
static int16_t pcm_buffer[FRAME_SIZE];
static uint8_t opus_buffer[MAX_PACKET_SIZE];
static uint8_t tx_packet_buffer[MAX_PACKET_SIZE];

// Task handles
static TaskHandle_t tx_task_handle = NULL;
static bool tx_task_running = false;

// Audio playback state
static uint32_t last_audio_rx_time = 0;
static bool audio_playing = false;
static uint32_t rx_packet_count = 0;

// Jitter buffer - accumulate frames before playback starts
#define JITTER_BUFFER_FRAMES 3
static int16_t jitter_buffer[JITTER_BUFFER_FRAMES][FRAME_SIZE];
static uint8_t jitter_write_idx = 0;
static uint8_t jitter_read_idx = 0;
static uint8_t jitter_count = 0;
static bool jitter_primed = false;

/**
 * Generate unique device ID from MAC address.
 */
static void generate_device_id(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    device_id[0] = mac[0];
    device_id[1] = mac[1];
    device_id[2] = mac[2];
    device_id[3] = mac[3];
    device_id[4] = mac[4];
    device_id[5] = mac[5];
    device_id[6] = mac[0] ^ mac[2] ^ mac[4];
    device_id[7] = mac[1] ^ mac[3] ^ mac[5];

    ESP_LOGI(TAG, "Device ID: %02x%02x%02x%02x%02x%02x%02x%02x",
             device_id[0], device_id[1], device_id[2], device_id[3],
             device_id[4], device_id[5], device_id[6], device_id[7]);
}

/**
 * Handle received audio packets with jitter buffering.
 */
static void on_audio_received(const audio_packet_t *packet, size_t total_len)
{
    rx_packet_count++;

    // Skip our own packets
    if (memcmp(packet->device_id, device_id, DEVICE_ID_LENGTH) == 0) {
        return;
    }

    // Don't play while transmitting (half-duplex)
    if (transmitting) {
        return;
    }

    size_t opus_len = total_len - HEADER_LENGTH;
    if (opus_len == 0 || opus_len > MAX_PACKET_SIZE - HEADER_LENGTH) {
        return;
    }

    // Decode Opus to PCM
    int samples = codec_decode(packet->opus_data, opus_len, pcm_buffer);
    if (samples <= 0) {
        return;
    }

    last_audio_rx_time = xTaskGetTickCount();

    // Start audio output if not already playing
    if (!audio_playing) {
        audio_output_start();
        audio_playing = true;
        jitter_primed = false;
        jitter_count = 0;
        jitter_write_idx = 0;
        jitter_read_idx = 0;
        button_set_led_state(LED_STATE_RECEIVING);  // Blue LED
        ha_mqtt_set_state(HA_STATE_RECEIVING);
        ESP_LOGI(TAG, "RX audio started (buffering)");
    }

    // Add to jitter buffer
    memcpy(jitter_buffer[jitter_write_idx], pcm_buffer, samples * sizeof(int16_t));
    jitter_write_idx = (jitter_write_idx + 1) % JITTER_BUFFER_FRAMES;
    if (jitter_count < JITTER_BUFFER_FRAMES) {
        jitter_count++;
    }

    // Once we have enough frames buffered, start playback
    if (!jitter_primed && jitter_count >= 2) {
        jitter_primed = true;
        ESP_LOGI(TAG, "Jitter buffer primed, starting playback");
    }

    // Play from jitter buffer
    if (jitter_primed && jitter_count > 0) {
        audio_output_write(jitter_buffer[jitter_read_idx], FRAME_SIZE, 50);
        jitter_read_idx = (jitter_read_idx + 1) % JITTER_BUFFER_FRAMES;
        jitter_count--;
    }
}

/**
 * Audio transmit task - requires 32KB stack for Opus encoding.
 * Research: ESP32 Opus encoding needs 30KB+ stack.
 */
static void audio_tx_task(void *arg)
{
    ESP_LOGI(TAG, "Audio TX task started");

    audio_packet_t *packet = (audio_packet_t *)tx_packet_buffer;
    memcpy(packet->device_id, device_id, DEVICE_ID_LENGTH);

    while (tx_task_running) {
        if (!transmitting) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Read audio from microphone (320 samples = 20ms at 16kHz)
        int samples = audio_input_read(pcm_buffer, FRAME_SIZE, 50);
        if (samples != FRAME_SIZE) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // Encode PCM to Opus
        int opus_len = codec_encode(pcm_buffer, opus_buffer, sizeof(opus_buffer));
        if (opus_len <= 0) {
            continue;
        }

        // Build and send packet
        packet->sequence = htonl(tx_sequence++);
        memcpy(packet->opus_data, opus_buffer, opus_len);
        network_send_multicast(packet, HEADER_LENGTH + opus_len);
    }

    ESP_LOGI(TAG, "Audio TX task stopped");
    vTaskDelete(NULL);
}

/**
 * Get the appropriate idle LED state (muted or normal idle).
 */
static led_state_t get_idle_led_state(void)
{
    return audio_output_is_muted() ? LED_STATE_MUTED : LED_STATE_IDLE;
}

/**
 * Handle button events.
 */
static void on_button_event(button_event_t event, bool is_broadcast)
{
    switch (event) {
        case BUTTON_EVENT_PRESSED:
            button_set_led_state(LED_STATE_TRANSMITTING);  // Green
            transmitting = true;
            ha_mqtt_set_state(HA_STATE_TRANSMITTING);
            break;

        case BUTTON_EVENT_LONG_PRESS:
            break;

        case BUTTON_EVENT_RELEASED:
            button_set_led_state(get_idle_led_state());  // White or red if muted
            transmitting = false;
            ha_mqtt_set_state(HA_STATE_IDLE);
            break;

        default:
            break;
    }
}

/**
 * Handle configuration updates from HA.
 */
static void on_config_received(const device_config_t *config)
{
    ESP_LOGI(TAG, "Config: room=%s, target=%s, volume=%d",
             config->room, config->default_target, config->volume);
    audio_output_set_volume(config->volume);
}

/**
 * Application entry point.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "HA Intercom starting...");
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

    // Initialize settings from NVS (must be early, before network_init uses NVS)
    ESP_ERROR_CHECK(settings_init());
    const settings_t *cfg = settings_get();

    // Generate device ID
    generate_device_id();

    // Initialize HA MQTT (does not connect yet)
    ha_mqtt_init(device_id);

    // Initialize button and LED
    ESP_ERROR_CHECK(button_init());
    button_set_callback(on_button_event);

    // Initialize audio codec and I/O
    ESP_ERROR_CHECK(codec_init());
    ESP_ERROR_CHECK(audio_input_init());
    ESP_ERROR_CHECK(audio_output_init());

    // Apply saved settings
    audio_output_set_volume(cfg->volume);
    audio_output_set_mute(cfg->muted);
    button_set_idle_led_enabled(cfg->led_enabled);

    // Set initial LED state based on mute
    if (cfg->muted) {
        button_set_led_state(LED_STATE_MUTED);
    } else {
        button_set_led_state(LED_STATE_IDLE);
    }

    // Start microphone input - must be enabled before reading
    audio_input_start();

    // Get WiFi credentials - use saved settings or defaults
    const char *wifi_ssid = cfg->configured ? cfg->wifi_ssid : DEFAULT_WIFI_SSID;
    const char *wifi_pass = cfg->configured ? cfg->wifi_password : DEFAULT_WIFI_PASSWORD;

    // Initialize network
    ESP_LOGI(TAG, "Connecting to WiFi: %s", wifi_ssid);
    ESP_ERROR_CHECK(network_init(wifi_ssid, wifi_pass));

    esp_err_t ret = network_wait_connected(30000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection timeout!");
        button_set_led_state(LED_STATE_ERROR);
    } else {
        char ip_str[16];
        network_get_ip(ip_str);
        ESP_LOGI(TAG, "Connected! IP: %s", ip_str);

        // Start web server for config/OTA
        webserver_start();
        ESP_LOGI(TAG, "Web config at http://%s/", ip_str);

        // Start MQTT for Home Assistant integration
        ha_mqtt_start();
    }

    // Set up network callbacks and start receiving
    network_set_rx_callback(on_audio_received);
    ESP_ERROR_CHECK(network_start_rx());

    // Initialize discovery for Home Assistant (use room name from settings)
    ESP_ERROR_CHECK(discovery_init(cfg->room_name, device_id));
    discovery_set_config_callback(on_config_received);
    ESP_ERROR_CHECK(discovery_start());

    // Start audio TX task with 32KB stack (required for Opus encoding)
    tx_task_running = true;
    xTaskCreate(audio_tx_task, "audio_tx", 32768, NULL, 5, &tx_task_handle);

    ESP_LOGI(TAG, "Room: %s | Volume: %d%%", cfg->room_name, cfg->volume);
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

    ESP_LOGI(TAG, "Ready! Hold BOOT button to transmit");

    // Main loop - monitor state and handle audio idle timeout
    static bool was_transmitting = false;
    while (1) {
        // Log TX state changes
        if (transmitting != was_transmitting) {
            was_transmitting = transmitting;
            if (transmitting) {
                ESP_LOGI(TAG, "TX started");
            } else {
                ESP_LOGI(TAG, "TX stopped");
            }
        }

        // Stop audio output after 500ms of no received packets
        if (audio_playing && !transmitting) {
            uint32_t now = xTaskGetTickCount();
            if ((now - last_audio_rx_time) > pdMS_TO_TICKS(500)) {
                audio_output_stop();
                audio_playing = false;
                button_set_led_state(get_idle_led_state());  // White or red if muted
                ha_mqtt_set_state(HA_STATE_IDLE);
                ESP_LOGI(TAG, "RX audio stopped (idle)");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
