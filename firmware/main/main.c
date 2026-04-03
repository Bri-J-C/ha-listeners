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
#include "aec.h"
#include "network.h"
#include "discovery.h"
#include "button.h"
#include "settings.h"
#include "webserver.h"
#include "ha_mqtt.h"
#include "diagnostics.h"
#include "display.h"
#include "mdns.h"
#include "agc.h"
#include "voice_assist.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"

static const char *TAG = "main";

// Fallback WiFi credentials (used if no settings saved)
#define DEFAULT_WIFI_SSID       "your_wifi_ssid"
#define DEFAULT_WIFI_PASSWORD   "your_wifi_password"

// Application state (non-static: accessed by webserver.c via extern)
uint8_t device_id[DEVICE_ID_LENGTH];
volatile bool transmitting = false;
static volatile bool broadcast_mode = false;
static uint32_t tx_sequence = 0;

// Audio buffers (separate per task to prevent race conditions)
static uint8_t tx_packet_buffer[MAX_PACKET_SIZE];

// RX audio queue: decouples network receive from play.
// Each item holds one raw PCM packet (~672 bytes).
typedef struct {
    uint8_t data[MAX_PACKET_SIZE];
    size_t  len;
} rx_queue_item_t;

#define RX_QUEUE_DEPTH  15  // ~300ms at 20ms/frame
static QueueHandle_t rx_audio_queue = NULL;

// Task handles
static TaskHandle_t tx_task_handle = NULL;
static bool tx_task_running = false;
static TaskHandle_t play_task_handle = NULL;
static bool play_task_running = false;

// Task stacks allocated dynamically from PSRAM heap (not EXT_RAM_BSS_ATTR).
// EXT_RAM_BSS_ATTR uses fixed .ext_ram.bss addresses that conflict with
// esp_partition_mmap() used by WakeNet model loading.
#define TX_TASK_STACK_SIZE   8192
#define PLAY_TASK_STACK_SIZE 16384
static StackType_t *tx_task_stack = NULL;
static StaticTask_t tx_task_tcb;
static StackType_t *play_task_stack = NULL;
static StaticTask_t play_task_tcb;

// Audio playback state
static volatile uint32_t last_audio_rx_time = 0;
volatile bool audio_playing = false;  // non-static: accessed by webserver.c via extern
uint32_t rx_packet_count = 0;  // non-static: exposed to webserver.c via extern

// QA observability counters (logging only — no functional effect)
// Single-word counters: atomic on ESP32-S3 Xtensa, used for logging only
static uint32_t rx_log_counter = 0;       // Downsample RX packet logs (every 50th)
static uint32_t tx_log_counter = 0;       // Downsample TX packet logs (every 50th)
static uint32_t rx_drop_total = 0;        // Accumulated RX queue drop count
static uint32_t tx_frame_count = 0;       // Total frames sent in current PTT session
static uint32_t tx_start_tick = 0;        // Tick when PTT started (for duration calc)

// Cumulative counters since boot (non-static: exposed to webserver.c via extern)
uint32_t tx_frame_total = 0;              // Total Opus frames sent since boot (never resets)

// Sustained TX state (non-static: accessed by webserver.c via extern)
volatile bool sustained_tx_active = false; // True while API-initiated sustained_tx is running

// Sequence tracking for PLC/FEC
static volatile uint32_t last_sequence = 0;
static volatile bool sequence_initialized = false;

// First-to-talk: track current sender to ignore others
static uint8_t current_sender[DEVICE_ID_LENGTH] = {0};
static volatile bool has_current_sender = false;
static volatile uint8_t current_rx_priority = 0;  // Priority of current sender (PRIORITY_NORMAL = 0)
#define SENDER_TIMEOUT_MS 500  // Release channel after 500ms silence

/**
 * Get current RX audio queue depth (items waiting to be decoded/played).
 * Returns 0 if queue not yet created.
 */
UBaseType_t get_rx_queue_depth(void)
{
    return rx_audio_queue ? uxQueueMessagesWaiting(rx_audio_queue) : 0;
}

// Display room list tracking
static int last_device_count = -1;

// PTT lockout after sending a call - prevents mic from picking up chime decay
static uint32_t last_call_sent_time = 0;
#define CALL_TX_LOCKOUT_MS 2000  // Suppress PTT for 2s after sending a call

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
 * Check if channel is busy (someone else is transmitting).
 */
static bool is_channel_busy(void)
{
    if (voice_assist_is_active()) return true;

    if (!has_current_sender) return false;

    // Check if sender timed out
    uint32_t now = xTaskGetTickCount();
    uint32_t elapsed = (now - last_audio_rx_time) * portTICK_PERIOD_MS;
    if (elapsed > SENDER_TIMEOUT_MS) {
        has_current_sender = false;
        return false;
    }
    return true;
}

/**
 * Network RX callback — lightweight enqueue only.
 *
 * Runs in the network_rx task. Must NOT block (no Opus decode, no I2S write).
 * Performs only cheap checks (own packet, transmitting, DND) then copies the
 * raw packet into the audio queue for the play task to handle.
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

    // Skip if voice assistant is active (private session)
    if (voice_assist_is_active()) return;

    // Extract priority for DND check
    uint8_t incoming_priority = packet->priority;
    if (incoming_priority > 2) incoming_priority = 0;

    // DND check: discard NORMAL and HIGH, let EMERGENCY through
    const settings_t *cfg = settings_get();
    if (cfg->dnd_enabled && incoming_priority < 2) {
        ESP_LOGD(TAG, "DND active, ignoring audio (priority=%d)", incoming_priority);
        return;
    }

    // Enqueue raw packet for the audio play task (non-blocking)
    if (rx_audio_queue) {
        rx_queue_item_t item;
        size_t copy_len = (total_len <= MAX_PACKET_SIZE) ? total_len : MAX_PACKET_SIZE;
        memcpy(item.data, packet, copy_len);
        item.len = copy_len;

        UBaseType_t q_depth = uxQueueMessagesWaiting(rx_audio_queue);

        if (xQueueSend(rx_audio_queue, &item, 0) != pdTRUE) {
            rx_drop_total++;
            uint32_t seq = ntohl(packet->sequence);
            if ((rx_drop_total % 50) == 1) {
                ESP_LOGW(TAG, "[RX] queue_full: dropped seq=%lu, total_drops=%lu",
                         (unsigned long)seq, (unsigned long)rx_drop_total);
            }
        } else {
            // Log every 50th accepted packet (~1/sec during audio)
            rx_log_counter++;
            if ((rx_log_counter % 50) == 1) {
                uint32_t seq = ntohl(packet->sequence);
                size_t opus_len = (total_len > HEADER_LENGTH) ? total_len - HEADER_LENGTH : 0;
                ESP_LOGD(TAG, "[RX] src=%02x%02x%02x%02x seq=%lu pri=%d opus_len=%u q_depth=%u",
                         packet->device_id[0], packet->device_id[1],
                         packet->device_id[2], packet->device_id[3],
                         (unsigned long)seq, incoming_priority,
                         (unsigned)opus_len, (unsigned)(q_depth + 1));
            }
        }
    }
}

/**
 * Process a received audio packet — extract raw PCM and play.
 *
 * Runs in the audio_play_task. Handles first-to-talk, priority preemption,
 * emergency override, and I2S output.
 */
static void process_rx_packet(const audio_packet_t *packet, size_t total_len)
{
    uint8_t incoming_priority = packet->priority;
    if (incoming_priority > 2) {
        incoming_priority = 0;
    }

    // Extract PCM payload into aligned buffer.
    // packet->pcm_data is at byte offset 13 in the packed struct (odd address),
    // so casting directly to int16_t* produces misaligned reads. memcpy to an
    // aligned buffer first.
    size_t pcm_len = (total_len > HEADER_LENGTH) ? total_len - HEADER_LENGTH : 0;
    if (pcm_len == 0 || pcm_len > PCM_FRAME_BYTES) {
        return;  // Malformed packet
    }

    static int16_t rx_pcm_aligned[FRAME_SIZE];
    memcpy(rx_pcm_aligned, packet->pcm_data, pcm_len);

    size_t samples = pcm_len / sizeof(int16_t);
    const int16_t *pcm = rx_pcm_aligned;
    bool is_silence_frame = true;
    for (size_t i = 0; i < samples; i++) {
        if (pcm[i] != 0) {
            is_silence_frame = false;
            break;
        }
    }

    // Silence from unknown sender: don't acquire channel
    if (is_silence_frame && !has_current_sender) return;

    // First-to-talk with priority-based preemption
    if (has_current_sender) {
        bool same_sender = (memcmp(packet->device_id, current_sender, DEVICE_ID_LENGTH) == 0);
        if (!same_sender) {
            if (is_silence_frame) return;

            if (incoming_priority > current_rx_priority) {
                ESP_LOGI(TAG, "Priority preemption: incoming=%d > current=%d",
                         incoming_priority, current_rx_priority);
                if (audio_playing) {
                    audio_output_stop();
                    audio_playing = false;
                }
                if (audio_output_is_emergency_override()) {
                    audio_output_restore_volume();
                }
                memcpy(current_sender, packet->device_id, DEVICE_ID_LENGTH);
                current_rx_priority = incoming_priority;
                sequence_initialized = false;
                ESP_LOGI(TAG, "Channel preempted by %02x%02x%02x%02x (priority=%d)",
                         current_sender[0], current_sender[1], current_sender[2],
                         current_sender[3], current_rx_priority);
            } else {
                return;
            }
        }
    } else {
        memcpy(current_sender, packet->device_id, DEVICE_ID_LENGTH);
        current_rx_priority = incoming_priority;
        has_current_sender = true;
        sequence_initialized = false;
        ESP_LOGI(TAG, "Channel acquired by %02x%02x%02x%02x (priority=%d)",
                 current_sender[0], current_sender[1], current_sender[2],
                 current_sender[3], current_rx_priority);
    }

    // Emergency override
    if (incoming_priority == 2 && !audio_playing) {
        audio_output_force_unmute_max_volume();
        button_set_led_state(LED_STATE_BUSY);
        ESP_LOGW(TAG, "EMERGENCY audio incoming - forced unmute + max volume");
    }

    last_audio_rx_time = xTaskGetTickCount();

    // Start audio output if not already playing
    if (!audio_playing && !is_silence_frame) {
        ESP_LOGI(TAG, "RX audio starting (pcm_len=%u, seq=%lu, pri=%d)",
                 (unsigned)pcm_len, (unsigned long)ntohl(packet->sequence), incoming_priority);
        audio_output_start();
        audio_playing = true;
        button_set_led_state(LED_STATE_RECEIVING);
        display_set_state(DISPLAY_STATE_RECEIVING);
        ha_mqtt_set_state(HA_STATE_RECEIVING);
    }

    uint32_t seq = ntohl(packet->sequence);

    // Sequence tracking (log gaps, no PLC/FEC needed with raw PCM)
    if (sequence_initialized) {
        int32_t gap = (int32_t)(seq - last_sequence - 1);
        if (gap > 4) {
            ESP_LOGW(TAG, "RX sequence jump: last=%lu cur=%lu gap=%ld",
                     (unsigned long)last_sequence, (unsigned long)seq, (long)gap);
        }
    }
    last_sequence = seq;
    sequence_initialized = true;

    // Play PCM directly (100ms timeout — DMA drain is exactly 20ms per frame,
    // so 20ms timeout causes spurious failures; 100ms gives 5 frames of headroom)
    if (!is_silence_frame && audio_playing) {
        int written = audio_output_write(pcm, samples, 100);
        if (written == 0 && audio_playing) {
            ESP_LOGW(TAG, "RX write returned 0 — restarting I2S (seq=%lu)", (unsigned long)seq);
            audio_output_stop();
            audio_output_start();
            if (audio_output_write(pcm, samples, 100) == 0) {
                ESP_LOGE(TAG, "RX write still 0 after I2S restart — stopping (seq=%lu)", (unsigned long)seq);
                audio_playing = false;
                has_current_sender = false;
            }
        }
    }

    rx_packet_count++;
}

/**
 * Audio play task — dequeues packets and decodes/plays them.
 *
 * Decoupled from the network RX task so that blocking I2S writes don't
 * cause incoming UDP packets to pile up in the socket buffer.
 */
static void audio_play_task(void *arg)
{
    ESP_LOGI(TAG, "Audio play task started");
    rx_queue_item_t item;

    while (play_task_running) {
        if (xQueueReceive(rx_audio_queue, &item, pdMS_TO_TICKS(50)) == pdTRUE) {
            process_rx_packet((audio_packet_t *)item.data, item.len);
        }
    }

    ESP_LOGI(TAG, "Audio play task stopped");
    vTaskDelete(NULL);
}

/**
 * Audio transmit task - requires 32KB stack for Opus encoding.
 * Research: ESP32 Opus encoding needs 30KB+ stack.
 *
 * Sends lead-in silence (300ms) before mic audio and trail-out silence
 * (600ms) after to ensure clean playback on receivers.
 */
static void audio_tx_task(void *arg)
{
    ESP_LOGI(TAG, "Audio TX task started");

    int16_t tx_pcm_buffer[FRAME_SIZE];

    audio_packet_t *packet = (audio_packet_t *)tx_packet_buffer;
    memcpy(packet->device_id, device_id, DEVICE_ID_LENGTH);
    packet->priority = settings_get()->priority;

    // Silence frame: 640 bytes of zeros
    static const int16_t silence_pcm[FRAME_SIZE] = {0};

    bool was_transmitting = false;

    // AEC cleaned output accumulator
    static int16_t aec_cleaned[FRAME_SIZE];
    static int aec_cleaned_fill = 0;

    while (tx_task_running) {
        packet->priority = settings_get()->priority;

        // Start of transmission — send lead-in silence
        if (transmitting && !was_transmitting) {
            was_transmitting = true;
            agc_reset();
            aec_reset();
            aec_cleaned_fill = 0;

            ESP_LOGI(TAG, "TX started - sending lead-in silence");

            for (int i = 0; i < 15; i++) {
                packet->sequence = htonl(tx_sequence++);
                memcpy(packet->pcm_data, silence_pcm, PCM_FRAME_BYTES);

                const char *target_ip = ha_mqtt_get_target_ip();
                if (target_ip) {
                    network_send_unicast(packet, HEADER_LENGTH + PCM_FRAME_BYTES, target_ip);
                } else {
                    network_send_multicast(packet, HEADER_LENGTH + PCM_FRAME_BYTES);
                }
                vTaskDelay(pdMS_TO_TICKS(FRAME_DURATION_MS));
            }
        }

        // End of transmission — send trail-out silence
        if (!transmitting && was_transmitting) {
            ESP_LOGI(TAG, "TX ended - sending trail-out silence");

            for (int i = 0; i < 10; i++) {
                packet->sequence = htonl(tx_sequence++);
                memcpy(packet->pcm_data, silence_pcm, PCM_FRAME_BYTES);

                const char *target_ip = ha_mqtt_get_target_ip();
                if (target_ip) {
                    network_send_unicast(packet, HEADER_LENGTH + PCM_FRAME_BYTES, target_ip);
                } else {
                    network_send_multicast(packet, HEADER_LENGTH + PCM_FRAME_BYTES);
                }
                vTaskDelay(pdMS_TO_TICKS(FRAME_DURATION_MS));
            }

            was_transmitting = false;
            continue;
        }

        if (!transmitting) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Read audio from microphone
        int samples = audio_input_read(tx_pcm_buffer, FRAME_SIZE, 50);
        if (samples != FRAME_SIZE) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // AGC
        if (settings_get()->agc_enabled) {
            agc_process(tx_pcm_buffer, samples);
        }

        // AEC
        const int16_t *send_buf = tx_pcm_buffer;
        if (aec_is_ready()) {
            aec_push_mic(tx_pcm_buffer, samples);
            int got = aec_pop_cleaned(aec_cleaned + aec_cleaned_fill,
                                      FRAME_SIZE - aec_cleaned_fill);
            aec_cleaned_fill += got;
            if (aec_cleaned_fill >= FRAME_SIZE) {
                send_buf = aec_cleaned;
                aec_cleaned_fill = 0;
            }
        }

        // Pack raw PCM into packet and send
        packet->sequence = htonl(tx_sequence++);
        memcpy(packet->pcm_data, send_buf, PCM_FRAME_BYTES);

        const char *target_ip = ha_mqtt_get_target_ip();
        if (target_ip) {
            network_send_unicast(packet, HEADER_LENGTH + PCM_FRAME_BYTES, target_ip);
        } else {
            network_send_multicast(packet, HEADER_LENGTH + PCM_FRAME_BYTES);
        }

        tx_frame_count++;
        tx_frame_total++;
        tx_log_counter++;
        if ((tx_log_counter % 50) == 1) {
            ESP_LOGD(TAG, "[TX] seq=%lu frames=%lu",
                     (unsigned long)(tx_sequence - 1), (unsigned long)tx_frame_count);
        }

        vTaskDelay(1);  // Yield for watchdog
    }

    ESP_LOGI(TAG, "Audio TX task stopped");
    vTaskDelete(NULL);
}

/**
 * Get the appropriate idle LED state (DND > muted > normal idle).
 */
static led_state_t get_idle_led_state(void)
{
    const settings_t *cfg = settings_get();
    if (cfg->dnd_enabled) {
        return LED_STATE_DND;    // Purple - DND active
    }
    return audio_output_is_muted() ? LED_STATE_MUTED : LED_STATE_IDLE;
}

/**
 * Sustained TX stop task — sleeps for the requested duration, then stops TX.
 *
 * Spawned by the /api/test sustained_tx action. The arg is a heap-allocated
 * uint32_t holding the duration in milliseconds. This task frees the arg
 * before deleting itself.
 *
 * If PTT button is pressed/released during the sustained TX, the button
 * handler clears sustained_tx_active and transmitting. This task detects
 * that on wakeup and exits without double-clearing.
 */
void sustained_tx_stop_task(void *arg)
{
    uint32_t duration_ms = *(uint32_t *)arg;
    free(arg);

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    if (sustained_tx_active) {
        ESP_LOGI(TAG, "[sustained_tx] duration elapsed (%lums), stopping TX",
                 (unsigned long)duration_ms);
        transmitting = false;
        sustained_tx_active = false;
        button_set_led_state(get_idle_led_state());
        display_set_state(DISPLAY_STATE_IDLE);
        ha_mqtt_set_state(HA_STATE_IDLE);
    } else {
        ESP_LOGI(TAG, "[sustained_tx] already stopped (PTT override or external stop)");
    }

    vTaskDelete(NULL);
}

/**
 * Generate a short fallback beep (800 Hz, ~500ms) for call notification.
 *
 * The hub streams the real chime over UDP/Opus. This beep is played
 * immediately on call arrival so the user gets instant feedback even
 * if the hub chime audio hasn't arrived yet (e.g., hub unreachable).
 * It is intentionally short to not overlap with incoming hub audio.
 *
 * The beep is 200ms (10 × 20ms frames) at 800 Hz, 50% amplitude.
 * Keeping it brief minimises AEC reference contamination.
 */
void play_fallback_beep(void)
{
    if (transmitting) {
        ESP_LOGW(TAG, "Beep: skipped — currently transmitting");
        return;
    }

    ESP_LOGI(TAG, "Beep: muted=%d, playing beep (caller force-unmutes)", audio_output_is_muted());

    uint32_t beep_start = xTaskGetTickCount();

    // Flush RX queue so play task doesn't wake up with stale packets during beep
    UBaseType_t queued = rx_audio_queue ? uxQueueMessagesWaiting(rx_audio_queue) : 0;
    if (rx_audio_queue) { xQueueReset(rx_audio_queue); }
    ESP_LOGI(TAG, "Beep: flushed RX queue (%u packets discarded)", (unsigned)queued);

    // Stop any RX audio to free the output path
    if (audio_playing) {
        ESP_LOGI(TAG, "Beep: stopping active RX audio (has_sender=%d, seq_init=%d)",
                 has_current_sender, sequence_initialized);
        audio_output_stop();
        audio_playing = false;
        has_current_sender = false;
        sequence_initialized = false;  // Prevent stale sequence triggering false PLC
    } else {
        ESP_LOGI(TAG, "Beep: no active RX audio to stop");
    }

    // Generate one 20ms sine-wave frame and repeat it
    static int16_t beep_frame[FRAME_SIZE];
    for (int i = 0; i < FRAME_SIZE; i++) {
        // 800 Hz at 50% amplitude (16384 peak, well below 32767 clipping)
        beep_frame[i] = (int16_t)(16384.0f * sinf(2.0f * M_PI * 800.0f * i / (float)SAMPLE_RATE));
    }

    ESP_LOGI(TAG, "Beep: starting I2S output");
    audio_output_start();
    button_set_led_state(LED_STATE_RECEIVING);

    // Play 10 frames = 200ms of beep
    int frames_written = 0;
    for (int frame = 0; frame < 10; frame++) {
        int written = audio_output_write(beep_frame, FRAME_SIZE, 50);
        if (written > 0) frames_written++;
    }

    ESP_LOGI(TAG, "Beep: stopping I2S output (%d/10 frames written)", frames_written);
    audio_output_stop();

    // Flush AEC reference: local beep audio must not contaminate the next TX's
    // AEC reference queue, which would cause voice cancellation artefacts.
    // See detailed comment in aec.h / the old play_incoming_call_chime().
    aec_flush_reference();

    button_set_led_state(get_idle_led_state());

    uint32_t beep_elapsed = (xTaskGetTickCount() - beep_start) * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "Beep: complete in %lums (hub chime incoming via UDP)", (unsigned long)beep_elapsed);
}

/**
 * Handle incoming call notification.
 *
 * The hub streams the real chime audio over UDP immediately after publishing
 * the MQTT call message.  The ESP32 plays the hub chime through the normal
 * on_audio_received() path (priority HIGH, so it preempts normal RX).
 *
 * Sets LED/display for visual feedback and forces full volume so the chime
 * is heard regardless of the device volume setting.  Volume is restored
 * automatically in the idle timeout when the chime audio ends.
 */
static void play_incoming_call_chime(void)
{
    if (transmitting) {
        ESP_LOGW(TAG, "Call chime: skipped — currently transmitting");
        return;
    }

    const settings_t *cfg = settings_get();
    if (cfg->dnd_enabled) {
        ESP_LOGI(TAG, "Call chime: skipped — DND active");
        return;
    }

    // Set LED/display immediately for instant visual feedback.
    button_set_led_state(LED_STATE_RECEIVING);
    display_set_state(DISPLAY_STATE_RECEIVING);

    // Force full volume for incoming call — must be heard regardless of setting.
    // Uses the emergency override mechanism (save/restore). Restored automatically
    // in the idle timeout when the chime audio ends.
    audio_output_force_unmute_max_volume();

    ESP_LOGI(TAG, "Call chime: LED set, volume forced — hub chime arrives via UDP");
}

/**
 * Apply a settings change made on the OLED display.
 *
 * Called from display.c (cycle_button_task) via the settings callback.
 * Applies the value to the audio/LED subsystem, persists it via settings_set_xxx(),
 * and publishes it to Home Assistant over MQTT.
 *
 * NOTE: runs in the cycle_button_task context (priority 5, 4KB stack).
 * Keep it lightweight — no blocking I/O.
 */
static void on_display_setting_changed(int index, int value)
{
    switch (index) {
        case SETTINGS_ITEM_DND: {
            bool dnd = (value != 0);
            settings_set_dnd(dnd);
            if (!transmitting && !audio_playing) {
                button_set_led_state(get_idle_led_state());
            }
            ha_mqtt_publish_dnd();
            ESP_LOGI(TAG, "[STATE] dnd=%d (via display)", dnd);
            break;
        }
        case SETTINGS_ITEM_PRIORITY: {
            uint8_t pri = (uint8_t)value;
            settings_set_priority(pri);
            ha_mqtt_publish_priority();
            ESP_LOGI(TAG, "Display: Priority -> %d", pri);
            break;
        }
        case SETTINGS_ITEM_MUTE: {
            bool muted = (value != 0);
            settings_set_mute(muted);
            audio_output_set_mute(muted);
            if (!transmitting && !audio_playing) {
                button_set_led_state(get_idle_led_state());
            }
            ha_mqtt_publish_mute();
            ESP_LOGI(TAG, "[STATE] mute=%d (via display)", muted);
            break;
        }
        case SETTINGS_ITEM_VOLUME: {
            uint8_t vol = (uint8_t)value;
            settings_set_volume(vol);
            audio_output_set_volume(vol);
            ha_mqtt_publish_volume();
            ESP_LOGI(TAG, "[STATE] volume=%d (via display)", vol);
            break;
        }
        case SETTINGS_ITEM_AGC: {
            bool agc = (value != 0);
            settings_set_agc_enabled(agc);
            ha_mqtt_publish_agc();
            ESP_LOGI(TAG, "Display: AGC -> %s", agc ? "ON" : "OFF");
            break;
        }
        case SETTINGS_ITEM_LED: {
            bool led = (value != 0);
            settings_set_led_enabled(led);
            button_set_idle_led_enabled(led);
            ha_mqtt_publish_led();
            ESP_LOGI(TAG, "Display: LED -> %s", led ? "ON" : "OFF");
            break;
        }
        default:
            ESP_LOGW(TAG, "Unknown display setting index: %d", index);
            break;
    }
}

/**
 * Handle MQTT commands from Home Assistant (priority, DND, etc.)
 */
static void on_ha_command(ha_cmd_t cmd, int value)
{
    switch (cmd) {
        case HA_CMD_DND: {
            bool dnd = (value != 0);
            // Update LED to reflect DND state immediately
            if (!transmitting && !audio_playing) {
                button_set_led_state(get_idle_led_state());
            }
            // Sync display settings page if open
            display_sync_settings();
            ESP_LOGI(TAG, "[STATE] dnd=%d (via HA)", dnd);
            break;
        }
        case HA_CMD_MUTE:
            // Update LED to reflect mute/DND state
            if (!transmitting && !audio_playing) {
                button_set_led_state(get_idle_led_state());
            }
            display_sync_settings();
            ESP_LOGI(TAG, "[STATE] mute=%d (via HA)", value);
            break;
        case HA_CMD_PRIORITY:
            display_sync_settings();
            ESP_LOGI(TAG, "Priority set to %d via HA", value);
            break;
        case HA_CMD_VOLUME:
            display_sync_settings();
            ESP_LOGI(TAG, "[STATE] volume=%d (via HA)", value);
            break;
        case HA_CMD_AGC:
            display_sync_settings();
            break;
        case HA_CMD_LED:
            display_sync_settings();
            break;
        default:
            break;
    }
}

/**
 * Handle button events.
 */
static void on_button_event(button_event_t event, bool is_broadcast)
{
    switch (event) {
        case BUTTON_EVENT_PRESSED: {
            // Cancel voice assist if active — PTT always wins
            if (voice_assist_is_active()) {
                ESP_LOGI(TAG, "PTT cancelling voice assist");
                voice_assist_cancel();
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            const settings_t *cfg = settings_get();
            uint8_t my_priority = cfg->priority;

            // Check if channel is busy (someone else talking)
            if (is_channel_busy()) {
                // Check if we have sufficient priority to preempt
                if (my_priority > current_rx_priority) {
                    // Preempt: stop current RX, start TX
                    ESP_LOGW(TAG, "PTT preempting channel (our=%d > rx=%d)",
                             my_priority, current_rx_priority);
                    if (audio_playing) {
                        audio_output_stop();
                        audio_playing = false;
                        has_current_sender = false;
                    }
                    // Restore emergency override if active from the preempted stream
                    if (audio_output_is_emergency_override()) {
                        audio_output_restore_volume();
                    }
                    has_current_sender = false;  // Release channel
                    current_rx_priority = 0;     // Reset priority
                    // Flush RX queue so play task doesn't re-acquire channel from stale packets
                    if (rx_audio_queue) { xQueueReset(rx_audio_queue); }
                    button_set_led_state(LED_STATE_TRANSMITTING);
                    display_set_state(DISPLAY_STATE_TRANSMITTING);
                    transmitting = true;
                    tx_frame_count = 0;
                    tx_start_tick = xTaskGetTickCount();
                    ha_mqtt_set_state(HA_STATE_TRANSMITTING);
                    {
                        const char *ptt_target = ha_mqtt_get_target_name();
                        const char *ptt_ip = ha_mqtt_get_target_ip();
                        ESP_LOGI(TAG, "[PTT] start: target_room=%s target_ip=%s mode=%s (preempt)",
                                 ptt_target, ptt_ip ? ptt_ip : MULTICAST_GROUP,
                                 ptt_ip ? "unicast" : "multicast");
                    }
                } else {
                    button_set_led_state(LED_STATE_BUSY);  // Orange - channel busy
                    display_set_state(DISPLAY_STATE_ERROR);
                    display_show_message("Channel Busy", 1000);
                    ESP_LOGW(TAG, "Channel busy - cannot transmit (our=%d, rx=%d)",
                             my_priority, current_rx_priority);
                }
            } else {
                // Suppress PTT for CALL_TX_LOCKOUT_MS after sending a call.
                // The caller's mic is physically near the speaker, so the chime
                // acoustic tail would be encoded into the first PTT broadcast.
                if (last_call_sent_time != 0) {
                    uint32_t now_ticks = xTaskGetTickCount();
                    uint32_t since_call_ms = (now_ticks - last_call_sent_time) * portTICK_PERIOD_MS;
                    if (since_call_ms < CALL_TX_LOCKOUT_MS) {
                        ESP_LOGW(TAG, "PTT suppressed: %lums since call (lockout %dms)",
                                 (unsigned long)since_call_ms, CALL_TX_LOCKOUT_MS);
                        break;
                    }
                }

                // Stop any audio currently playing — flushes DMA immediately so
                // the speaker doesn't leak 160ms of residual audio into the mic.
                if (audio_playing) {
                    audio_output_stop();
                    audio_playing = false;
                    has_current_sender = false;
                }
                // Flush RX queue so play task doesn't re-acquire channel from stale packets
                if (rx_audio_queue) { xQueueReset(rx_audio_queue); }
                // Normal broadcast - works for all targets including mobile
                // (Mobile users will hear audio when they open Web PTT)
                button_set_led_state(LED_STATE_TRANSMITTING);  // Cyan
                display_set_state(DISPLAY_STATE_TRANSMITTING);
                transmitting = true;
                tx_frame_count = 0;
                tx_start_tick = xTaskGetTickCount();
                ha_mqtt_set_state(HA_STATE_TRANSMITTING);
                {
                    const char *ptt_target = ha_mqtt_get_target_name();
                    const char *ptt_ip = ha_mqtt_get_target_ip();
                    ESP_LOGI(TAG, "[PTT] start: target_room=%s target_ip=%s mode=%s",
                             ptt_target, ptt_ip ? ptt_ip : MULTICAST_GROUP,
                             ptt_ip ? "unicast" : "multicast");
                }
            }
            break;
        }

        case BUTTON_EVENT_LONG_PRESS:
            break;

        case BUTTON_EVENT_RELEASED: {
            // Only log PTT-end stats when a real session occurred; tx_frame_count is 0
            // when PTT was suppressed (channel busy, call lockout) and duration/frames
            // would be meaningless garbage.
            if (tx_frame_count > 0) {
                uint32_t duration_ms = (xTaskGetTickCount() - tx_start_tick) * portTICK_PERIOD_MS;
                ESP_LOGI(TAG, "[PTT] end: total_frames=%lu duration_ms=%lu",
                         (unsigned long)tx_frame_count, (unsigned long)duration_ms);
            }
            button_set_led_state(get_idle_led_state());  // White, red, or purple if DND
            display_set_state(DISPLAY_STATE_IDLE);
            transmitting = false;
            sustained_tx_active = false;  // Cancel any API-initiated sustained_tx
            ha_mqtt_set_state(HA_STATE_IDLE);
            break;
        }

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
 * Handle long press on cycle button - send call notification to target.
 * When "All Rooms" is selected, calls every discovered device simultaneously.
 */
static void on_cycle_long_press(void)
{
    const char *target = ha_mqtt_get_target_name();

    if (strcmp(target, "All Rooms") == 0) {
        // Call all discovered rooms at once
        int count = ha_mqtt_send_call_all_rooms();
        if (count > 0) {
            last_call_sent_time = xTaskGetTickCount() | 1;  // Ensure non-zero for self-echo prevention
            display_show_message("Calling all...", 1500);
            ESP_LOGI(TAG, "Sent call to all rooms (%d devices)", count);
        } else {
            display_show_message("No devices online", 1500);
            ESP_LOGW(TAG, "Call all rooms: no devices available");
        }
        return;
    }

    // Send call to specific target (works for ESP32s and mobiles)
    ha_mqtt_send_call(target);
    last_call_sent_time = xTaskGetTickCount() | 1;  // Ensure non-zero for self-echo prevention
    display_show_message("Calling...", 1500);
    ESP_LOGI(TAG, "Sent call to %s", target);
}

/**
 * Application entry point.
 */
void app_main(void)
{
    // Initialize diagnostics first to capture all logs
    ESP_ERROR_CHECK(diagnostics_init());

    ESP_LOGI(TAG, "HA Intercom starting...");
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

    // Log PSRAM availability
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_size > 0) {
        ESP_LOGI(TAG, "PSRAM: %u KB total, %u KB free",
                 (unsigned)(psram_size / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    } else {
        ESP_LOGW(TAG, "No PSRAM detected - using internal RAM only");
    }

    // Initialize settings from NVS (must be early, before network_init uses NVS)
    ESP_ERROR_CHECK(settings_init());
    const settings_t *cfg = settings_get();

    // Generate device ID
    generate_device_id();

    // Initialize HA MQTT (does not connect yet)
    ha_mqtt_init(device_id);
    ha_mqtt_set_callback(on_ha_command);

    // Initialize button and LED
    ESP_ERROR_CHECK(button_init());
    button_set_callback(on_button_event);

    // Initialize display (optional - continues if not found)
    esp_err_t disp_ret = display_init();
    if (disp_ret == ESP_OK) {
        ESP_LOGI(TAG, "Display initialized");
        // Register long press callback for mobile notifications
        display_set_long_press_callback(on_cycle_long_press);
        // Register settings change callback for OLED settings page
        display_set_settings_callback(on_display_setting_changed);
    } else if (disp_ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No display detected - running without");
    } else {
        ESP_LOGW(TAG, "Display init failed: %s", esp_err_to_name(disp_ret));
    }

    // Initialize audio I/O
    ESP_ERROR_CHECK(audio_input_init());
    ESP_ERROR_CHECK(audio_output_init());

    // Initialize AGC
    agc_init();

    // Initialize AEC — non-fatal if esp-sr unavailable or no PSRAM
    esp_err_t aec_ret = aec_init();
    if (aec_ret == ESP_OK) {
        ESP_LOGI(TAG, "AEC enabled");
    } else {
        ESP_LOGW(TAG, "AEC unavailable (%s) — raw mic audio", esp_err_to_name(aec_ret));
    }

    // Voice assistant init deferred until after RX queue/tasks (PSRAM allocation order matters)

    // Apply saved settings
    audio_output_set_volume(cfg->volume);
    audio_output_set_mute(cfg->muted);
    button_set_idle_led_enabled(cfg->led_enabled);

    // Set initial LED state based on DND > mute > idle
    button_set_led_state(get_idle_led_state());

    // Start microphone input - must be enabled before reading
    audio_input_start();

    // Get WiFi credentials - use saved settings or defaults
    const char *wifi_ssid = cfg->configured ? cfg->wifi_ssid : DEFAULT_WIFI_SSID;
    const char *wifi_pass = cfg->configured ? cfg->wifi_password : DEFAULT_WIFI_PASSWORD;

    // Initialize network
    ESP_LOGI(TAG, "Connecting to WiFi: %s", wifi_ssid);
    ESP_ERROR_CHECK(network_init(wifi_ssid, wifi_pass));

    // Compute sanitized hostname from room name BEFORE connecting,
    // so mDNS and DHCP hostname are ready when IP is obtained
    char hostname[32];
    const char *room = cfg->room_name;
    int j = 0;
    for (int i = 0; room[i] && j < sizeof(hostname) - 1; i++) {
        char c = room[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            hostname[j++] = c;
        } else if (c >= 'A' && c <= 'Z') {
            hostname[j++] = c + 32;  // lowercase
        } else if (c == ' ' || c == '-' || c == '_') {
            if (j > 0 && hostname[j-1] != '-') hostname[j++] = '-';
        }
    }
    hostname[j] = '\0';
    if (j == 0) strcpy(hostname, "intercom");

    // Set DHCP hostname before connection completes (shown in router client list)
    network_set_hostname(hostname);

    // Start mDNS BEFORE waiting for connection so its internal event handler
    // catches IP_EVENT_STA_GOT_IP and enables the PCB automatically
    network_start_mdns(hostname);
    ESP_LOGI(TAG, "mDNS: will announce as http://%s.local/ when connected", hostname);

    esp_err_t ret = network_wait_connected(30000);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connection timeout - check AP mode");
        button_set_led_state(LED_STATE_ERROR);

        // Wait a bit for AP mode to fully start
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Start webserver in AP mode for configuration
        if (network_is_ap_mode()) {
            mdns_hostname_set("intercom-setup");
            webserver_start();
            ESP_LOGI(TAG, "AP mode active - configure at http://192.168.4.1/ or http://intercom-setup.local/");
        }
    } else {
        char ip_str[16];
        network_get_ip(ip_str);
        ESP_LOGI(TAG, "Connected! IP: %s, access at http://%s.local/", ip_str, hostname);

        // Start web server for config/OTA
        webserver_start();

        // Start MQTT for Home Assistant integration
        ha_mqtt_start();
    }

    // Create RX audio queue in PSRAM (internal RAM allocation fragments heap,
    // breaking MQTT TCP socket allocation — confirmed by isolation test)
    rx_audio_queue = xQueueCreateWithCaps(RX_QUEUE_DEPTH, sizeof(rx_queue_item_t),
                                           MALLOC_CAP_SPIRAM);
    if (!rx_audio_queue) {
        // Fallback to internal RAM if no PSRAM
        rx_audio_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_queue_item_t));
    }
    if (!rx_audio_queue) {
        ESP_LOGE(TAG, "FATAL: Failed to create RX audio queue");
    }

    // Allocate task stacks from PSRAM heap (not static BSS — avoids mmap conflict with WakeNet)
    play_task_stack = heap_caps_malloc(PLAY_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM);
    if (!play_task_stack) {
        ESP_LOGE(TAG, "FATAL: Failed to allocate play task stack in PSRAM");
    }

    play_task_running = true;
    play_task_handle = xTaskCreateStatic(audio_play_task, "audio_play",
        PLAY_TASK_STACK_SIZE / sizeof(StackType_t), NULL, 4,
        play_task_stack, &play_task_tcb);
    if (play_task_handle == NULL) {
        ESP_LOGE(TAG, "FATAL: Failed to create audio play task");
        play_task_running = false;
    }

    // Set up network callbacks and start receiving
    network_set_rx_callback(on_audio_received);
    ESP_ERROR_CHECK(network_start_rx());

    // Initialize discovery for Home Assistant (use room name from settings)
    ESP_ERROR_CHECK(discovery_init(cfg->room_name, device_id));
    discovery_set_config_callback(on_config_received);
    ESP_ERROR_CHECK(discovery_start());

    // Log heap fragmentation before task creation
    ESP_LOGI(TAG, "Heap before TX task: internal largest=%u KB, PSRAM largest=%u KB",
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));

    // Start audio TX task - stack in PSRAM to save ~32KB of internal RAM.
    // Encoder is in internal RAM (timing-critical); stack can be in PSRAM.
    tx_task_stack = heap_caps_malloc(TX_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM);
    if (!tx_task_stack) {
        ESP_LOGE(TAG, "FATAL: Failed to allocate TX task stack in PSRAM");
    }

    tx_task_running = true;
    tx_task_handle = xTaskCreateStatic(audio_tx_task, "audio_tx",
                                       TX_TASK_STACK_SIZE / sizeof(StackType_t),
                                       NULL, 5, tx_task_stack, &tx_task_tcb);
    if (tx_task_handle == NULL) {
        ESP_LOGE(TAG, "FATAL: Failed to create audio TX task - TX disabled");
        tx_task_running = false;
    }

    // Initialize voice assistant AFTER all intercom PSRAM allocations (RX queue, task stacks,
    // Opus codec). WakeNet mmap consumes PSRAM address space and must not starve intercom buffers.
    esp_err_t va_ret = voice_assist_init();
    if (va_ret == ESP_OK) {
        ESP_LOGI(TAG, "Voice assistant initialized");
        if (network_is_connected()) {
            voice_assist_start();
        }
    } else {
        ESP_LOGW(TAG, "Voice assistant unavailable (%s)", esp_err_to_name(va_ret));
    }

    ESP_LOGI(TAG, "Room: %s | Volume: %d%%", cfg->room_name, cfg->volume);
    ESP_LOGI(TAG, "Free internal: %u KB, PSRAM: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    ESP_LOGI(TAG, "Ready! Hold BOOT button to transmit");

    // Main loop - monitor state and handle audio idle timeout
    static bool was_transmitting = false;
    while (1) {
        // Log TX state changes
        if (transmitting != was_transmitting) {
            was_transmitting = transmitting;
            if (transmitting) {
                const char *target = ha_mqtt_get_target_name();
                ESP_LOGI(TAG, "TX started -> %s", target);
            } else {
                ESP_LOGI(TAG, "TX stopped");
            }
        }

        // Voice assist pause/resume: only stop wake word during PTT TX
        // (RX audio plays on separate I2S bus, doesn't conflict with mic reads)
        static bool va_was_paused = false;
        if (transmitting && !va_was_paused) {
            if (!voice_assist_is_active()) {
                voice_assist_stop();
            }
            va_was_paused = true;
        } else if (!transmitting && va_was_paused) {
            voice_assist_start();
            va_was_paused = false;
        }

        // Stop audio output after 500ms of no received packets
        if (audio_playing && !transmitting) {
            uint32_t now = xTaskGetTickCount();
            uint32_t idle_ms = (now - last_audio_rx_time) * portTICK_PERIOD_MS;
            if (idle_ms > 500) {
                UBaseType_t q_remain = rx_audio_queue ? uxQueueMessagesWaiting(rx_audio_queue) : 0;
                ESP_LOGI(TAG, "RX idle timeout: %lums since last packet (q_depth=%u, sender=%02x%02x, pri=%d)",
                         (unsigned long)idle_ms, (unsigned)q_remain,
                         current_sender[0], current_sender[1], current_rx_priority);
                audio_output_stop();
                audio_playing = false;
                has_current_sender = false;  // Release channel
                current_rx_priority = 0;     // Reset for next sender (PRIORITY_NORMAL)
                // Restore volume/mute if emergency override was active
                if (audio_output_is_emergency_override()) {
                    audio_output_restore_volume();
                    ESP_LOGI(TAG, "Emergency override restored after RX stopped");
                }
                button_set_led_state(get_idle_led_state());  // White, red, or purple
                display_set_state(DISPLAY_STATE_IDLE);
                ha_mqtt_set_state(HA_STATE_IDLE);
                ESP_LOGI(TAG, "RX audio stopped, channel released");
            }
        }

        // Clear stale sender when audio never started playing.
        // This catches the edge case where silence-only trail-out frames
        // acquired the channel (has_current_sender=true) but never set
        // audio_playing.  Without this, the device becomes permanently deaf
        // until a PTT press clears the stale sender.
        if (has_current_sender && !audio_playing && !transmitting) {
            uint32_t now = xTaskGetTickCount();
            uint32_t idle_ms = (now - last_audio_rx_time) * portTICK_PERIOD_MS;
            if (idle_ms > SENDER_TIMEOUT_MS) {
                ESP_LOGW(TAG, "Stale sender cleared: %02x%02x%02x%02x idle %lums (no audio_playing)",
                         current_sender[0], current_sender[1],
                         current_sender[2], current_sender[3],
                         (unsigned long)idle_ms);
                has_current_sender = false;
                current_rx_priority = 0;
            }
        }

        // Update display room list when new devices are discovered or availability changes
        if (display_is_available()) {
            int device_count = ha_mqtt_get_device_count();
            bool avail_changed = ha_mqtt_availability_changed();
            if (device_count != last_device_count || avail_changed) {
                last_device_count = device_count;

                // Build room list: "All Rooms" + discovered devices (excluding self)
                room_target_t room_list[MAX_ROOMS];
                int room_idx = 0;

                // First entry is always "All Rooms" (multicast)
                strncpy(room_list[room_idx].name, "All Rooms", MAX_ROOM_NAME_LEN - 1);
                strncpy(room_list[room_idx].ip, MULTICAST_GROUP, 15);
                room_list[room_idx].is_multicast = true;
                room_list[room_idx].is_mobile = false;
                room_idx++;

                // Add discovered devices (excluding self and unavailable)
                for (int i = 0; i < device_count && room_idx < MAX_ROOMS; i++) {
                    // Skip self and unavailable devices
                    if (ha_mqtt_is_self(i)) continue;
                    if (!ha_mqtt_is_available(i)) continue;

                    char room[32], ip[16];
                    if (ha_mqtt_get_device(i, room, ip)) {
                        strncpy(room_list[room_idx].name, room, MAX_ROOM_NAME_LEN - 1);
                        strncpy(room_list[room_idx].ip, ip, 15);
                        room_list[room_idx].is_multicast = false;
                        room_list[room_idx].is_mobile = ha_mqtt_is_device_mobile(i);
                        room_idx++;
                    }
                }

                display_set_rooms(room_list, room_idx);
                ESP_LOGI(TAG, "Display room list updated: %d rooms", room_idx);
            }

            // Sync display selection with MQTT target when cycle button changes it
            const room_target_t *selected = display_get_selected_room();
            if (selected) {
                const char *mqtt_target = ha_mqtt_get_target_name();
                if (strcmp(selected->name, mqtt_target) != 0) {
                    // Display selection changed - update MQTT
                    ESP_LOGI(TAG, "[ROOM] selected=%s ip=%s", selected->name, selected->ip);
                    ha_mqtt_set_target(selected->name);
                }
            }
        }

        // Save any pending settings to NVS (debounced)
        settings_save_if_needed();

        // Process deferred MQTT operations
        ha_mqtt_process();

        // Check for incoming calls
        char caller_name[32];
        if (ha_mqtt_check_incoming_call(caller_name)) {
            // Self-exclusion guard: when "All Rooms" is selected and we just sent
            // the call ourselves, our own MQTT message bounces back to us via the
            // broker.  Suppress the chime for CALL_TX_LOCKOUT_MS (2s) so we don't
            // ring our own chime.
            uint32_t now_ticks = xTaskGetTickCount();
            bool self_sent = (last_call_sent_time != 0) &&
                             ((now_ticks - last_call_sent_time) * portTICK_PERIOD_MS < CALL_TX_LOCKOUT_MS);
            if (self_sent) {
                ESP_LOGI(TAG, "Ignoring call from '%s' — self-sent within %dms lockout",
                         caller_name, CALL_TX_LOCKOUT_MS);
            } else {
                ESP_LOGI(TAG, "Incoming call from: %s", caller_name);
                play_incoming_call_chime();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
