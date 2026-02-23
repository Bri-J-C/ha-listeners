/**
 * Audio Output (Speaker) Module
 *
 * I2S interface to MAX98357A amplifier.
 * Thread-safe with mutex protection on shared buffers.
 */

#include "audio_output.h"
#include "aec.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "audio_output";

static i2s_chan_handle_t tx_handle = NULL;
static volatile bool is_active = false;  // volatile for cross-task visibility
static uint8_t current_volume = 100;
static bool is_muted = false;

// Mutex protecting I2S channel state transitions (start/stop/write).
// Prevents TOCTOU race: write() checks is_active then calls i2s_channel_write(),
// but stop() can disable the channel between those two steps, causing
// "The channel is not enabled" errors from the I2S driver.
// FreeRTOS mutex (not spinlock) because i2s_channel_write() blocks.
static SemaphoreHandle_t output_lock = NULL;

// Emergency override state
static bool emergency_override_active = false;
static uint8_t pre_emergency_volume = 100;
static bool pre_emergency_muted = false;

// Dynamically allocated stereo buffer (PSRAM if available)
static int16_t *stereo_buffer = NULL;

esp_err_t audio_output_init(void)
{
    ESP_LOGI(TAG, "Initializing I2S speaker output");

    // Create mutex for I2S channel state protection
    output_lock = xSemaphoreCreateMutex();
    if (!output_lock) {
        ESP_LOGE(TAG, "Failed to create output mutex");
        return ESP_ERR_NO_MEM;
    }

    // I2S channel configuration
    //
    // DMA buffer depth: 8 descriptors x FRAME_SIZE (320) samples = 2560 samples = 160ms at 16kHz.
    // This provides 8 Opus frames of headroom, which absorbs typical WiFi retransmission
    // latency (50-100ms) while keeping end-to-end audio latency low (~80ms average).
    // ESP-IDF default is 6x240=90ms; previous config was 12x640=480ms (excessive for voice).
    // Memory: 8 x 320 x 2ch x 2B = ~10KB DMA (vs ~30KB at old settings).
    //
    // auto_clear=true: on underrun, DMA outputs silence instead of replaying stale audio.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
#define I2S_DMA_DESC_NUM 8
    chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = FRAME_SIZE;
    chan_cfg.auto_clear = true;

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

    // Allocate stereo conversion buffer - prefer PSRAM
    size_t buf_size = FRAME_SIZE * 2 * sizeof(int16_t);
    stereo_buffer = (int16_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stereo_buffer) {
        stereo_buffer = (int16_t *)heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!stereo_buffer) {
        ESP_LOGE(TAG, "Failed to allocate stereo buffer");
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Stereo buffer: %u bytes (%s)",
             (unsigned)buf_size,
             esp_ptr_external_ram(stereo_buffer) ? "PSRAM" : "internal");

    ESP_LOGI(TAG, "I2S speaker initialized (SCK=%d, WS=%d, SD=%d)",
             I2S_SPK_SCK_PIN, I2S_SPK_WS_PIN, I2S_SPK_SD_PIN);

    return ESP_OK;
}

void audio_output_start(void)
{
    if (!tx_handle || !output_lock) return;

    xSemaphoreTake(output_lock, portMAX_DELAY);
    if (is_active) {
        ESP_LOGW(TAG, "start() called but already active — skipping");
        xSemaphoreGive(output_lock);
        return;
    }
    {
        esp_err_t ret = i2s_channel_enable(tx_handle);
        if (ret == ESP_OK) {
            is_active = true;

            /*
             * Pre-fill 2 DMA descriptors with silence to overwrite stale data
             * from a prior session (i2s_channel_disable doesn't zero buffers).
             * Only 2 needed: remaining descriptors use auto_clear=true which
             * outputs silence on underrun. Reduces start latency from ~160ms to ~40ms.
             */
            static const int16_t silence[FRAME_SIZE * 2] = {0};
            size_t written;
            for (int i = 0; i < 2; i++) {
                i2s_channel_write(tx_handle, silence, sizeof(silence),
                                  &written, pdMS_TO_TICKS(25));
            }

            ESP_LOGI(TAG, "Audio output started (vol=%d%%, muted=%d)",
                     current_volume, is_muted);
        } else {
            ESP_LOGE(TAG, "Failed to start audio output: %s", esp_err_to_name(ret));
        }
    }
    xSemaphoreGive(output_lock);
}

void audio_output_stop(void)
{
    if (!tx_handle || !output_lock) return;

    xSemaphoreTake(output_lock, portMAX_DELAY);
    if (is_active) {
        is_active = false;
        i2s_channel_disable(tx_handle);
        ESP_LOGI(TAG, "Audio output stopped");
    } else {
        ESP_LOGW(TAG, "stop() called but already inactive — skipping");
    }
    xSemaphoreGive(output_lock);
}

bool audio_output_is_active(void)
{
    return is_active;
}

int audio_output_write(const int16_t *buffer, size_t samples, uint32_t timeout_ms)
{
    // Quick check before taking mutex (avoid overhead when clearly inactive)
    if (!tx_handle || !is_active || !stereo_buffer || !output_lock) {
        return 0;
    }

    // Take mutex with timeout — if stop() is in progress, wait for it rather than
    // writing to a channel that's about to be disabled. Timeout prevents deadlock.
    if (xSemaphoreTake(output_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "write() mutex timeout (%lums) — dropping frame", (unsigned long)timeout_ms);
        return 0;
    }

    // Re-check is_active under mutex — stop() may have run between the quick check and here
    if (!is_active) {
        ESP_LOGD(TAG, "write() channel stopped while waiting for mutex — dropping frame");
        xSemaphoreGive(output_lock);
        return 0;
    }

    size_t count = samples < FRAME_SIZE ? samples : FRAME_SIZE;

    // Step 1: Apply volume scaling to mono samples, store in first half of stereo_buffer
    const int32_t volume_scale = is_muted ? 0 : (int32_t)current_volume;
    for (size_t i = 0; i < count; i++) {
        int32_t sample = (int32_t)buffer[i] * volume_scale / 100;
        if (sample > 32767) sample = 32767;
        else if (sample < -32768) sample = -32768;
        stereo_buffer[i] = (int16_t)sample;
    }

    // Step 2: Push volume-scaled mono to AEC reference.
    // The AEC needs the signal as the speaker actually outputs it (after volume)
    // so the adaptive filter amplitude matches the actual echo picked up by the mic.
    aec_push_reference(stereo_buffer, count);

    // Step 3: Expand mono to stereo in-place (back-to-front to avoid overwriting unread data)
    for (int i = (int)count - 1; i >= 0; i--) {
        stereo_buffer[i * 2]     = stereo_buffer[i];
        stereo_buffer[i * 2 + 1] = stereo_buffer[i];
    }

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(tx_handle, stereo_buffer, count * 2 * sizeof(int16_t),
                                       &bytes_written, pdMS_TO_TICKS(timeout_ms));

    xSemaphoreGive(output_lock);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_write failed: %s", esp_err_to_name(ret));
        return 0;
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

void audio_output_force_unmute_max_volume(void)
{
    if (emergency_override_active) {
        return;  // Already in emergency override — don't nest
    }

    // Save current state
    pre_emergency_volume = current_volume;
    pre_emergency_muted = is_muted;
    emergency_override_active = true;

    // Force unmute at max volume
    is_muted = false;
    current_volume = 100;
    ESP_LOGW(TAG, "Emergency override: forced unmute + max volume (was vol=%d, muted=%d)",
             pre_emergency_volume, pre_emergency_muted);
}

void audio_output_restore_volume(void)
{
    if (!emergency_override_active) {
        return;  // No active override to restore
    }

    current_volume = pre_emergency_volume;
    is_muted = pre_emergency_muted;
    emergency_override_active = false;
    ESP_LOGI(TAG, "Emergency override restored: vol=%d, muted=%d", current_volume, is_muted);
}

bool audio_output_is_emergency_override(void)
{
    return emergency_override_active;
}

void audio_output_deinit(void)
{
    if (tx_handle) {
        audio_output_stop();
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
    }
    if (stereo_buffer) {
        free(stereo_buffer);
        stereo_buffer = NULL;
    }
    if (output_lock) {
        vSemaphoreDelete(output_lock);
        output_lock = NULL;
    }
    ESP_LOGI(TAG, "Audio output deinitialized");
}
