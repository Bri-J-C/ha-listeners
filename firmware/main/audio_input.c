/**
 * Audio Input (Microphone) Module
 *
 * I2S interface to INMP441 MEMS microphone.
 * Thread-safe with mutex protection on shared buffers.
 */

#include "audio_input.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "audio_input";

static i2s_chan_handle_t rx_handle = NULL;
static bool is_active = false;
static SemaphoreHandle_t buffer_mutex = NULL;

// Dynamically allocated raw buffer (PSRAM if available)
static int32_t *raw_buffer = NULL;

esp_err_t audio_input_init(void)
{
    ESP_LOGI(TAG, "Initializing I2S microphone input");

    // Create mutex for thread-safe buffer access
    buffer_mutex = xSemaphoreCreateMutex();
    if (!buffer_mutex) {
        ESP_LOGE(TAG, "Failed to create buffer mutex");
        return ESP_FAIL;
    }

    // I2S channel configuration - optimized for low latency
    // Research: Fewer, smaller DMA buffers = lower latency but less jitter tolerance
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;           // Reduced from 8 for lower latency
    chan_cfg.dma_frame_num = FRAME_SIZE; // Match codec frame size exactly

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // I2S standard mode configuration for INMP441
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_MIC_SCK_PIN,
            .ws = I2S_MIC_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_MIC_SD_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    // INMP441 outputs 32-bit data, left-justified
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
        return ret;
    }

    // Allocate raw sample buffer - prefer PSRAM
    size_t buf_size = FRAME_SIZE * sizeof(int32_t);
    raw_buffer = (int32_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw_buffer) {
        raw_buffer = (int32_t *)heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!raw_buffer) {
        ESP_LOGE(TAG, "Failed to allocate raw buffer");
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Raw buffer: %u bytes (%s)",
             (unsigned)buf_size,
             esp_ptr_external_ram(raw_buffer) ? "PSRAM" : "internal");

    ESP_LOGI(TAG, "I2S microphone initialized (SCK=%d, WS=%d, SD=%d)",
             I2S_MIC_SCK_PIN, I2S_MIC_WS_PIN, I2S_MIC_SD_PIN);

    return ESP_OK;
}

void audio_input_start(void)
{
    if (rx_handle && !is_active) {
        esp_err_t ret = i2s_channel_enable(rx_handle);
        if (ret == ESP_OK) {
            is_active = true;
            ESP_LOGI(TAG, "Audio input started");
        } else {
            ESP_LOGE(TAG, "Failed to start audio input: %s", esp_err_to_name(ret));
        }
    }
}

void audio_input_stop(void)
{
    if (rx_handle && is_active) {
        i2s_channel_disable(rx_handle);
        is_active = false;
        ESP_LOGI(TAG, "Audio input stopped");
    }
}

bool audio_input_is_active(void)
{
    return is_active;
}

int audio_input_read(int16_t *buffer, size_t samples, uint32_t timeout_ms)
{
    if (!rx_handle || !is_active || !buffer_mutex) {
        return -1;
    }

    // Thread-safe buffer access
    if (xSemaphoreTake(buffer_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "Buffer mutex timeout");
        return -1;
    }

    if (!raw_buffer) {
        xSemaphoreGive(buffer_mutex);
        return -1;
    }

    // INMP441 outputs 32-bit samples, we need to convert to 16-bit
    size_t bytes_read = 0;

    esp_err_t ret = i2s_channel_read(rx_handle, raw_buffer, samples * sizeof(int32_t),
                                      &bytes_read, pdMS_TO_TICKS(timeout_ms));

    if (ret != ESP_OK) {
        xSemaphoreGive(buffer_mutex);
        ESP_LOGE(TAG, "I2S read error: %s", esp_err_to_name(ret));
        return -1;
    }

    size_t samples_read = bytes_read / sizeof(int32_t);

    // Convert 32-bit to 16-bit with gain boost
    // INMP441 outputs 24-bit data left-justified in 32-bit word
    // Optimized conversion: shift right 12 bits, apply 2x gain, clamp
    for (size_t i = 0; i < samples_read; i++) {
        int32_t sample = raw_buffer[i] >> 12;  // Less shift = more signal
        // Apply 2x gain and clamp to 16-bit range
        sample = sample * 2;
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        buffer[i] = (int16_t)sample;
    }

    xSemaphoreGive(buffer_mutex);
    return samples_read;
}

void audio_input_deinit(void)
{
    if (rx_handle) {
        audio_input_stop();
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
    }
    if (raw_buffer) {
        free(raw_buffer);
        raw_buffer = NULL;
    }
    if (buffer_mutex) {
        vSemaphoreDelete(buffer_mutex);
        buffer_mutex = NULL;
    }
    ESP_LOGI(TAG, "Audio input deinitialized");
}
