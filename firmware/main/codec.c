/**
 * Opus Codec Module
 *
 * Encode/decode audio using Opus codec with optimized settings for
 * real-time voice on ESP32-S3.
 *
 * Optimizations applied:
 * - Inband FEC for packet loss recovery
 * - Tuned complexity for ESP32-S3 performance
 * - Packet loss percentage hint for adaptive FEC
 *
 * Requires libopus component: idf.py add-dependency "espressif/libopus"
 */

#include "codec.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <opus.h>
#include <string.h>

static const char *TAG = "codec";

static OpusEncoder *encoder = NULL;
static OpusDecoder *decoder = NULL;

esp_err_t codec_init(void)
{
    int err;

    ESP_LOGI(TAG, "Initializing Opus codec");

    // Encoder must be in internal RAM — PSRAM cache misses stall opus_encode()
    // beyond the 20ms real-time budget, hanging the TX task before it can log.
    encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || encoder == NULL) {
        ESP_LOGE(TAG, "Failed to create Opus encoder: %s", opus_strerror(err));
        return ESP_FAIL;
    }

    // Configure encoder for clear voice quality
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(encoder, OPUS_SET_DTX(0));
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));          // Embed FEC in each packet
    opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10));   // Assume 10% loss, triggers FEC

    // Decoder can use PSRAM — RX is not timing-critical (has jitter buffer)
    size_t dec_size = opus_decoder_get_size(CHANNELS);
    decoder = (OpusDecoder *)heap_caps_malloc(dec_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (decoder) {
        err = opus_decoder_init(decoder, SAMPLE_RATE, CHANNELS);
        if (err != OPUS_OK) {
            free(decoder);
            decoder = NULL;
        }
    }
    if (!decoder) {
        decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &err);
    }
    if (err != OPUS_OK || decoder == NULL) {
        ESP_LOGE(TAG, "Failed to create Opus decoder: %s", opus_strerror(err));
        codec_deinit();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Opus codec initialized (rate=%d, enc=%uB internal, dec=%uB %s)",
             SAMPLE_RATE,
             (unsigned)opus_encoder_get_size(CHANNELS),
             (unsigned)dec_size,
             esp_ptr_external_ram(decoder) ? "PSRAM" : "internal");

    return ESP_OK;
}

int codec_encode(const int16_t *pcm_in, uint8_t *opus_out, size_t max_opus_len)
{
    if (encoder == NULL) {
        return -1;
    }

    int bytes = opus_encode(encoder, pcm_in, FRAME_SIZE, opus_out, max_opus_len);
    if (bytes < 0) {
        ESP_LOGE(TAG, "Opus encode error: %s", opus_strerror(bytes));
        return -1;
    }

    return bytes;
}

int codec_decode(const uint8_t *opus_in, size_t opus_len, int16_t *pcm_out)
{
    if (decoder == NULL) {
        return -1;
    }

    int samples = opus_decode(decoder, opus_in, opus_len, pcm_out, FRAME_SIZE, 0);
    if (samples < 0) {
        ESP_LOGE(TAG, "Opus decode error: %s", opus_strerror(samples));
        return -1;
    }

    return samples;
}

void codec_set_bitrate(int bitrate)
{
    if (encoder) {
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate));
        ESP_LOGI(TAG, "Bitrate set to %d", bitrate);
    }
}

int codec_decode_plc(int16_t *pcm_out)
{
    if (decoder == NULL) {
        return -1;
    }

    // Pass NULL to trigger Packet Loss Concealment
    // Opus will generate interpolated audio based on previous state
    int samples = opus_decode(decoder, NULL, 0, pcm_out, FRAME_SIZE, 0);

    if (samples < 0) {
        ESP_LOGE(TAG, "Opus PLC error: %s", opus_strerror(samples));
        return -1;
    }

    return samples;
}

int codec_decode_fec(const uint8_t *opus_in, size_t opus_len, int16_t *pcm_out)
{
    if (decoder == NULL) {
        return -1;
    }

    // Uses FEC data from the NEXT packet to recover the CURRENT packet
    // decode_fec=1 tells decoder to use FEC data to reconstruct lost frame
    int samples = opus_decode(decoder, opus_in, opus_len, pcm_out, FRAME_SIZE, 1);

    if (samples < 0) {
        ESP_LOGE(TAG, "Opus FEC decode error: %s", opus_strerror(samples));
        return -1;
    }

    return samples;
}

void codec_set_packet_loss(int loss_percent)
{
    if (encoder) {
        opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(loss_percent));
        ESP_LOGD(TAG, "Packet loss hint set to %d%%", loss_percent);
    }
}

void codec_reset_encoder(void)
{
    if (encoder) {
        opus_encoder_ctl(encoder, OPUS_RESET_STATE);
        ESP_LOGI(TAG, "Encoder state reset");
    }
}

void codec_reset_decoder(void)
{
    if (decoder) {
        opus_decoder_ctl(decoder, OPUS_RESET_STATE);
        ESP_LOGI(TAG, "Decoder state reset");
    }
}

void codec_deinit(void)
{
    if (encoder) {
        opus_encoder_destroy(encoder);  // pairs with opus_encoder_create()
        encoder = NULL;
    }
    if (decoder) {
        free(decoder);  // works for both heap_caps_malloc and opus_decoder_create()
        decoder = NULL;
    }
    ESP_LOGI(TAG, "Opus codec deinitialized");
}
