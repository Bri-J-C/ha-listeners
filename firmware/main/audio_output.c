/**
 * Audio Output (Speaker) Module
 *
 * I2S interface to MAX98357A amplifier.
 */

#include "audio_output.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static const char *TAG = "audio_output";

static i2s_chan_handle_t tx_handle = NULL;
static bool is_active = false;
static uint8_t current_volume = 100;  // Default 100% - MAX98357A has its own gain
static bool is_muted = false;

esp_err_t audio_output_init(void)
{
    ESP_LOGI(TAG, "Initializing I2S speaker output");

    // I2S channel configuration - larger buffers to handle network jitter
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 12;  // More buffers for jitter tolerance
    chan_cfg.dma_frame_num = FRAME_SIZE * 2;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // I2S standard mode configuration for MAX98357A
    // Use STEREO mode - mono data will be duplicated to both channels in write function
    // MAX98357A with SD pin floating outputs (L+R)/2
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SPK_SCK_PIN,
            .ws = I2S_SPK_WS_PIN,
            .dout = I2S_SPK_SD_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "I2S speaker initialized (SCK=%d, WS=%d, SD=%d)",
             I2S_SPK_SCK_PIN, I2S_SPK_WS_PIN, I2S_SPK_SD_PIN);

    return ESP_OK;
}

void audio_output_start(void)
{
    if (tx_handle && !is_active) {
        esp_err_t ret = i2s_channel_enable(tx_handle);
        if (ret == ESP_OK) {
            is_active = true;

            // Flush DMA buffers with silence to prevent replay of old audio
            static int16_t silence[FRAME_SIZE * 2] = {0};
            size_t written;
            i2s_channel_write(tx_handle, silence, sizeof(silence), &written, 10);

            ESP_LOGI(TAG, "Audio output started");
        } else {
            ESP_LOGE(TAG, "Failed to start audio output: %s", esp_err_to_name(ret));
        }
    }
}

void audio_output_stop(void)
{
    if (tx_handle && is_active) {
        // Write silence before stopping to flush any remaining audio
        static int16_t silence[FRAME_SIZE * 2] = {0};
        size_t written;
        i2s_channel_write(tx_handle, silence, sizeof(silence), &written, 10);

        i2s_channel_disable(tx_handle);
        is_active = false;
        ESP_LOGI(TAG, "Audio output stopped");
    }
}

bool audio_output_is_active(void)
{
    return is_active;
}

int audio_output_write(const int16_t *buffer, size_t samples, uint32_t timeout_ms)
{
    if (!tx_handle || !is_active) {
        return -1;
    }

    // Convert mono to stereo (duplicate each sample to L and R channels)
    // and apply volume scaling
    // Static buffer to avoid stack overflow
    static int16_t stereo_buffer[FRAME_SIZE * 2];
    int32_t volume_scale = is_muted ? 0 : (int32_t)current_volume;

    for (size_t i = 0; i < samples && i < FRAME_SIZE; i++) {
        int32_t sample = (int32_t)buffer[i] * volume_scale / 100;
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        int16_t s = (int16_t)sample;
        stereo_buffer[i * 2] = s;       // Left channel
        stereo_buffer[i * 2 + 1] = s;   // Right channel
    }

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(tx_handle, stereo_buffer, samples * 2 * sizeof(int16_t),
                                       &bytes_written, pdMS_TO_TICKS(timeout_ms));

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S write error: %s", esp_err_to_name(ret));
        return -1;
    }

    return bytes_written / (2 * sizeof(int16_t));
}

void audio_output_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    current_volume = volume;
    ESP_LOGI(TAG, "Volume set to %d%%", volume);
}

uint8_t audio_output_get_volume(void)
{
    return current_volume;
}

void audio_output_set_mute(bool muted)
{
    is_muted = muted;
    ESP_LOGI(TAG, "Mute %s", muted ? "enabled" : "disabled");
}

bool audio_output_is_muted(void)
{
    return is_muted;
}

void audio_output_deinit(void)
{
    if (tx_handle) {
        audio_output_stop();
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        ESP_LOGI(TAG, "Audio output deinitialized");
    }
}
