/**
 * Voice Assistant Module
 *
 * ESP-SR WakeNet wake word detection + voice assist state management.
 *
 * State machine:
 *   VA_STATE_DISABLED  — PTT or audio RX in progress; WakeNet not running
 *   VA_STATE_IDLE      — Listening for wake word (feeds mic samples to WakeNet)
 *   VA_STATE_ACTIVE    — Streaming mic audio to hub via unicast UDP
 *   VA_STATE_TTS_PLAYBACK — Waiting for TTS response; audio RX enabled
 *
 * Flow on wake word:
 *   1. Play chime (~300ms, blocks)
 *   2. Set LED to LED_STATE_VOICE_ASSIST, OLED to DISPLAY_STATE_VOICE_ASSIST
 *   3. Publish MQTT voice_assist_start
 *   4. Enter VA_STATE_ACTIVE: encode mic → unicast UDP with PRIORITY_VOICE_ASSIST
 *   5. Stop on: silence timeout (5s), max session (30s), PTT cancel, MQTT end
 *
 * WakeNet notes:
 *   Requires CONFIG_SR_WN_WN9_ALEXA=y in sdkconfig (Task 7).
 *   Until sdkconfig is updated, this file compiles but WakeNet init is
 *   skipped — voice_assist_init() returns ESP_OK with a log warning.
 */

#include <string.h>
#include <math.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"

#include "voice_assist.h"
#include "protocol.h"
#include "audio_input.h"
#include "audio_output.h"
#include "codec.h"
#include "network.h"
#include "settings.h"
#include "button.h"
#include "display.h"
#include "ha_mqtt.h"
#include "wake_chime_data.h"

// WakeNet headers — only included when CONFIG_SR_WN_WN9_ALEXA is defined in sdkconfig.
// The guard lets this file compile cleanly before Task 7 (sdkconfig update).
#ifdef CONFIG_SR_WN_WN9_ALEXA
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
// WakeNet model name for Alexa — matches the ESP-SR Kconfig selection
#define WAKENET_MODEL_NAME "wn9_alexa"
#endif

static const char *TAG = "voice_assist";

// ─── Tuning constants ──────────────────────────────────────────────────────

#define VA_SILENCE_TIMEOUT_MS   3000   // Stop streaming after 3s of silence
#define VA_MAX_SESSION_MS       10000  // Hard cap: 10s per session
#define VA_SILENCE_RMS_THRESH   1500   // RMS below this = silence (high due to mic_gain=100 amplification)

// WakeNet chunk size — set at runtime from model's get_samp_chunksize().
// WakeNet9 Alexa uses 512 samples (32ms at 16kHz).
static int va_wakeword_chunk = 512;  // Default; updated in voice_assist_init()

// ─── State ────────────────────────────────────────────────────────────────

typedef enum {
    VA_STATE_DISABLED = 0,   // Not running (PTT/RX in progress)
    VA_STATE_IDLE,           // Listening for wake word
    VA_STATE_ACTIVE,         // Streaming to hub
    VA_STATE_TTS_PLAYBACK,   // Waiting for TTS response from HA
} va_state_t;

static volatile va_state_t s_state = VA_STATE_DISABLED;
static volatile bool s_running = false;   // Task should keep looping
static volatile bool s_cancel = false;    // PTT cancel signal

// ─── Task stack ───────────────────────────────────────────────────────────

// 16KB stack in PSRAM (mirrors pattern from main.c TX/play tasks)
#define VA_TASK_STACK_SIZE 49152
static StackType_t *s_va_task_stack = NULL;
static StaticTask_t s_va_task_tcb;
static TaskHandle_t s_va_task_handle = NULL;

// ─── Audio buffers ────────────────────────────────────────────────────────

// PCM capture buffer — declared static but lives on the task stack region;
// uses heap allocation in PSRAM to keep internal SRAM free.
static int16_t *s_pcm_buf = NULL;         // FRAME_SIZE int16_t, PSRAM
static uint8_t *s_opus_buf = NULL;        // MAX_PACKET_SIZE bytes, PSRAM
static uint8_t *s_packet_buf = NULL;      // MAX_PACKET_SIZE bytes, PSRAM

// ─── WakeNet handle ───────────────────────────────────────────────────────

#ifdef CONFIG_SR_WN_WN9_ALEXA
static const esp_wn_iface_t *s_wn_iface = NULL;
static model_iface_data_t *s_wn_handle = NULL;
#endif

// ─── Externs from main.c ─────────────────────────────────────────────────

extern uint8_t device_id[DEVICE_ID_LENGTH];
extern volatile bool transmitting;
extern volatile bool audio_playing;

// ─── Internal helpers ─────────────────────────────────────────────────────

/**
 * Compute RMS of a PCM buffer.
 * Returns 0–32768 (approximate); silence threshold is VA_SILENCE_RMS_THRESH.
 */
static uint32_t compute_rms(const int16_t *buf, size_t samples)
{
    if (samples == 0) return 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t s = buf[i];
        sum += (uint64_t)(s * s);
    }
    return (uint32_t)sqrtf((float)(sum / samples));
}

/**
 * Play the wake-word confirmation chime through the speaker.
 * Blocks for ~300ms while chime data drains through I2S.
 */
static void play_wake_chime(void)
{
    ESP_LOGI(TAG, "Playing wake chime (%d samples)", WAKE_CHIME_SAMPLE_COUNT);

    audio_output_start();

    size_t remaining = WAKE_CHIME_SAMPLE_COUNT;
    const int16_t *ptr = wake_chime_samples;

    while (remaining > 0) {
        size_t chunk = (remaining > (size_t)FRAME_SIZE) ? (size_t)FRAME_SIZE : remaining;
        int written = audio_output_write(ptr, chunk, 100);
        if (written < 0) {
            ESP_LOGW(TAG, "Chime write error");
            break;
        }
        ptr += written;
        remaining -= (size_t)written;
    }

    // Brief drain delay so last I2S frame clears the DMA buffer
    vTaskDelay(pdMS_TO_TICKS(40));
    audio_output_stop();
}

/**
 * Build and transmit one Opus-encoded voice assist audio packet via unicast.
 *
 * @param pcm     PCM samples (FRAME_SIZE int16_t)
 * @param seq     Current sequence number (incremented by caller)
 * @param dest_ip Hub IP string from settings
 * @return ESP_OK on success
 */
static esp_err_t send_va_packet(const int16_t *pcm, uint32_t seq, const char *dest_ip)
{
    // Encode PCM → Opus
    int opus_len = codec_encode(pcm, s_opus_buf, MAX_PACKET_SIZE - HEADER_LENGTH);
    if (opus_len <= 0) {
        ESP_LOGW(TAG, "Opus encode failed (%d)", opus_len);
        return ESP_FAIL;
    }

    // Build packet header
    audio_packet_t *pkt = (audio_packet_t *)s_packet_buf;
    memcpy(pkt->device_id, device_id, DEVICE_ID_LENGTH);
    pkt->sequence = htonl(seq);
    pkt->priority = PRIORITY_VOICE_ASSIST;
    memcpy(pkt->opus_data, s_opus_buf, (size_t)opus_len);

    size_t total_len = HEADER_LENGTH + (size_t)opus_len;
    return network_send_unicast(pkt, total_len, dest_ip);
}

// ─── VA task ──────────────────────────────────────────────────────────────

/**
 * Voice assist FreeRTOS task.
 *
 * Loops continuously:
 *   - VA_STATE_IDLE:   capture one FRAME_SIZE chunk and feed to WakeNet.
 *                      On detection → chime, MQTT, enter ACTIVE.
 *   - VA_STATE_ACTIVE: capture, encode, unicast.  Monitor silence + timeout.
 *   - VA_STATE_TTS_PLAYBACK: yield; audio_playing handles RX decode.
 *   - VA_STATE_DISABLED: yield 50ms.
 */
static void voice_assist_task(void *arg)
{
    ESP_LOGI(TAG, "Voice assist task started");

    uint32_t va_sequence = 0;
    uint32_t silence_start_tick = 0;
    uint32_t session_start_tick = 0;

    while (s_running) {
        va_state_t state = s_state;  // snapshot (volatile read)

        switch (state) {

        // ── DISABLED ─────────────────────────────────────────────────────
        case VA_STATE_DISABLED:
            vTaskDelay(pdMS_TO_TICKS(50));
            break;

        // ── IDLE: wake word detection ─────────────────────────────────────
        case VA_STATE_IDLE: {
#ifdef CONFIG_SR_WN_WN9_ALEXA
            if (!s_wn_handle) {
                // WakeNet unavailable — just yield
                vTaskDelay(pdMS_TO_TICKS(20));
                break;
            }

            // Read one frame from mic
            int samples = audio_input_read(s_pcm_buf, va_wakeword_chunk, 50);
            if (samples <= 0) {
                vTaskDelay(pdMS_TO_TICKS(5));
                break;
            }

            // Feed to WakeNet
            wakenet_state_t wn_state = s_wn_iface->detect(s_wn_handle, s_pcm_buf);
            if (wn_state == WAKENET_DETECTED) {
                ESP_LOGI(TAG, "Wake word detected!");

                // Transition to active — set flag before chime so PTT can cancel
                s_cancel = false;
                va_sequence = 0;
                s_state = VA_STATE_ACTIVE;
                session_start_tick = xTaskGetTickCount();
                silence_start_tick = 0;  // not in silence yet

                // Visual feedback
                button_set_led_state(LED_STATE_VOICE_ASSIST);
                display_set_state(DISPLAY_STATE_VOICE_ASSIST);
                // TODO: play_wake_chime() crashes (LoadProhibited) when called
                // from va_task on Core 1 — I2S output not safe cross-core.
                // Need to defer chime to audio_play_task or main loop.

                // Reset encoder for clean session
                codec_reset_encoder();

                // Notify HA hub
                ha_mqtt_publish_voice_assist_start();
            }
#else
            // WakeNet disabled at compile time — idle silently
            vTaskDelay(pdMS_TO_TICKS(20));
#endif
            break;
        }

        // ── ACTIVE: stream audio to hub ───────────────────────────────────
        case VA_STATE_ACTIVE: {
            // Log first frame of each session
            if (va_sequence == 0) {
                ESP_LOGI(TAG, "VA ACTIVE: streaming to %s:%d", settings_get()->mqtt_host, AUDIO_PORT);
            }

            // Check cancel flag (set by PTT press)
            if (s_cancel) {
                ESP_LOGI(TAG, "VA cancelled by PTT");
                ha_mqtt_publish_voice_assist_cancel();
                button_set_led_state(LED_STATE_IDLE);
                display_set_state(DISPLAY_STATE_IDLE);
                s_state = VA_STATE_IDLE;
                s_cancel = false;
                break;
            }

            // Max session guard
            uint32_t elapsed_ms = (xTaskGetTickCount() - session_start_tick) * portTICK_PERIOD_MS;
            if (elapsed_ms >= VA_MAX_SESSION_MS) {
                ESP_LOGI(TAG, "VA max session (%dms) reached", VA_MAX_SESSION_MS);
                ha_mqtt_publish_voice_assist_stop();
                button_set_led_state(LED_STATE_IDLE);
                display_set_state(DISPLAY_STATE_IDLE);
                s_state = VA_STATE_IDLE;
                break;
            }

            // Get hub IP from settings
            const settings_t *cfg = settings_get();
            if (cfg->mqtt_host[0] == '\0') {
                ESP_LOGW(TAG, "VA: mqtt_host not set, cannot stream");
                vTaskDelay(pdMS_TO_TICKS(20));
                break;
            }

            // Capture one frame
            int samples = audio_input_read(s_pcm_buf, FRAME_SIZE, 50);
            if (samples <= 0) {
                vTaskDelay(pdMS_TO_TICKS(5));
                break;
            }

            // Silence detection
            uint32_t rms = compute_rms(s_pcm_buf, (size_t)samples);
            uint32_t now = xTaskGetTickCount();

            if (rms < VA_SILENCE_RMS_THRESH) {
                if (silence_start_tick == 0) {
                    silence_start_tick = now;
                } else {
                    uint32_t silent_ms = (now - silence_start_tick) * portTICK_PERIOD_MS;
                    if (silent_ms >= VA_SILENCE_TIMEOUT_MS) {
                        ESP_LOGI(TAG, "VA silence timeout (%ldms, rms=%lu)",
                                 (long)silent_ms, (unsigned long)rms);
                        ha_mqtt_publish_voice_assist_stop();
                        button_set_led_state(LED_STATE_IDLE);
                        display_set_state(DISPLAY_STATE_IDLE);
                        s_state = VA_STATE_IDLE;
                        break;
                    }
                }
            } else {
                // Voice detected — reset silence timer
                silence_start_tick = 0;
            }

            // Encode and transmit
            esp_err_t err = send_va_packet(s_pcm_buf, va_sequence++, cfg->mqtt_host);
            if (err != ESP_OK) {
                if ((va_sequence % 50) == 1) {
                    ESP_LOGW(TAG, "VA send failed (seq=%lu)", (unsigned long)va_sequence);
                }
            } else if (va_sequence == 1) {
                ESP_LOGI(TAG, "VA first packet sent OK (rms=%lu)", (unsigned long)rms);
            }

            // Pace to ~50fps (20ms per frame) — yield for watchdog + rate limit
            vTaskDelay(1);
            break;
        }

        // ── TTS_PLAYBACK: waiting for HA response ─────────────────────────
        case VA_STATE_TTS_PLAYBACK:
            // Check cancel
            if (s_cancel) {
                ESP_LOGI(TAG, "VA TTS cancelled by PTT");
                ha_mqtt_publish_voice_assist_cancel();
                button_set_led_state(LED_STATE_IDLE);
                display_set_state(DISPLAY_STATE_IDLE);
                s_state = VA_STATE_IDLE;
                s_cancel = false;
            }
            // Main audio RX path handles TTS playback; just yield here
            vTaskDelay(pdMS_TO_TICKS(20));
            break;

        default:
            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        }
    }

    ESP_LOGI(TAG, "Voice assist task exiting");
    s_va_task_handle = NULL;
    vTaskDelete(NULL);
}

// ─── Public API ───────────────────────────────────────────────────────────

esp_err_t voice_assist_init(void)
{
    ESP_LOGI(TAG, "Initialising voice assist module");

    // Allocate audio buffers from PSRAM
    // Allocate for max of FRAME_SIZE (320, Opus encode) and WakeNet chunk (512)
    size_t pcm_samples = (FRAME_SIZE > 512) ? FRAME_SIZE : 512;
    s_pcm_buf    = (int16_t *)heap_caps_malloc(pcm_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_opus_buf   = (uint8_t *)heap_caps_malloc(MAX_PACKET_SIZE,               MALLOC_CAP_SPIRAM);
    s_packet_buf = (uint8_t *)heap_caps_malloc(MAX_PACKET_SIZE,               MALLOC_CAP_SPIRAM);

    if (!s_pcm_buf || !s_opus_buf || !s_packet_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffers in PSRAM");
        // Free whatever was allocated before returning
        heap_caps_free(s_pcm_buf);
        heap_caps_free(s_opus_buf);
        heap_caps_free(s_packet_buf);
        s_pcm_buf = NULL;
        s_opus_buf = NULL;
        s_packet_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

#ifdef CONFIG_SR_WN_WN9_ALEXA
    // Initialize ESP-SR models from flash partition (mmap'd packed binary format)
    ESP_LOGI(TAG, "Loading SR models from 'model' partition...");
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models && models->num > 0) {
        ESP_LOGI(TAG, "SR models loaded: %d models available", models->num);

        // Find and create the WakeNet model
        char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "alexa");
        if (!wn_name) {
            ESP_LOGW(TAG, "WakeNet 'alexa' model not found in partition");
        } else {
            ESP_LOGI(TAG, "Loading WakeNet model: %s", wn_name);
            s_wn_iface = esp_wn_handle_from_name(wn_name);
            if (s_wn_iface) {
                s_wn_handle = s_wn_iface->create(wn_name, DET_MODE_90);
                if (s_wn_handle) {
                    va_wakeword_chunk = s_wn_iface->get_samp_chunksize(s_wn_handle);
                    ESP_LOGI(TAG, "WakeNet loaded OK (chunk=%d)", va_wakeword_chunk);
                } else {
                    ESP_LOGE(TAG, "WakeNet create failed");
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "No SR models found — WakeNet disabled. "
                      "Flash model partition with: esptool.py write_flash 0x3A0000 srmodels.bin");
    }
#else
    ESP_LOGW(TAG, "WakeNet disabled (CONFIG_SR_WN_WN9_ALEXA not set) — "
                  "wake word detection inactive until Task 7 sdkconfig applied");
#endif

    // Start task in DISABLED state — voice_assist_start() activates IDLE
    s_state   = VA_STATE_DISABLED;
    s_running = true;
    s_cancel  = false;

    // Allocate task stack from PSRAM heap (not EXT_RAM_BSS_ATTR — avoids mmap conflict)
    s_va_task_stack = heap_caps_malloc(VA_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_va_task_stack) {
        ESP_LOGE(TAG, "Failed to allocate VA task stack in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    s_va_task_handle = xTaskCreateStatic(
        voice_assist_task,
        "va_task",
        VA_TASK_STACK_SIZE / sizeof(StackType_t),
        NULL,
        3,              // Priority 3: below TX(5) and RX(4)
        s_va_task_stack,
        &s_va_task_tcb
    );

    if (!s_va_task_handle) {
        ESP_LOGE(TAG, "Failed to create voice assist task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Voice assist initialised (state=DISABLED)");
    return ESP_OK;
}

void voice_assist_start(void)
{
    if (s_state == VA_STATE_DISABLED) {
        ESP_LOGI(TAG, "Voice assist: DISABLED → IDLE (wake word listening active)");
        s_cancel = false;
        s_state  = VA_STATE_IDLE;
    }
}

void voice_assist_stop(void)
{
    va_state_t prev = s_state;
    if (prev != VA_STATE_DISABLED) {
        ESP_LOGI(TAG, "Voice assist: stopping (was state=%d)", (int)prev);
        s_state = VA_STATE_DISABLED;
    }
}

bool voice_assist_is_active(void)
{
    return (s_state == VA_STATE_ACTIVE);
}

void voice_assist_cancel(void)
{
    if (s_state == VA_STATE_ACTIVE || s_state == VA_STATE_TTS_PLAYBACK) {
        ESP_LOGI(TAG, "Voice assist cancel requested (PTT press)");
        s_cancel = true;  // Picked up by task on next loop iteration
    }
}

void voice_assist_tts_done(void)
{
    if (s_state == VA_STATE_TTS_PLAYBACK) {
        ESP_LOGI(TAG, "TTS playback done — returning to IDLE");
        button_set_led_state(LED_STATE_IDLE);
        display_set_state(DISPLAY_STATE_IDLE);
        s_state = VA_STATE_IDLE;
    }
}

bool voice_assist_is_playing_tts(void)
{
    return (s_state == VA_STATE_TTS_PLAYBACK);
}
