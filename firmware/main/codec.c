/**
 * Opus Codec Module
 *
 * Encode/decode audio using Opus codec.
 * Requires libopus component: idf.py add-dependency "espressif/libopus"
 */

#include "codec.h"
#include "esp_log.h"
#include <opus.h>
#include <string.h>

static const char *TAG = "codec";

static OpusEncoder *encoder = NULL;
static OpusDecoder *decoder = NULL;

esp_err_t codec_init(void)
{
    int err;

    ESP_LOGI(TAG, "Initializing Opus codec");

    // Create encoder
    encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || encoder == NULL) {
        ESP_LOGE(TAG, "Failed to create Opus encoder: %s", opus_strerror(err));
        return ESP_FAIL;
    }

    // Configure encoder for voice on ESP32
    // Research findings:
    // - 12kbps is sufficient for wideband speech (16kHz)
    // - Complexity 2 is safe for ESP32 real-time encoding
    // - VBR improves quality at given bitrate
    // - DTX saves bandwidth during silence
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(12000));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(2));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(encoder, OPUS_SET_DTX(1));

    // Create decoder
    decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &err);
    if (err != OPUS_OK || decoder == NULL) {
        ESP_LOGE(TAG, "Failed to create Opus decoder: %s", opus_strerror(err));
        opus_encoder_destroy(encoder);
        encoder = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Opus codec initialized (rate=%d, bitrate=%d)", SAMPLE_RATE, OPUS_BITRATE);

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
        opus_encoder_destroy(encoder);
        encoder = NULL;
    }
    if (decoder) {
        opus_decoder_destroy(decoder);
        decoder = NULL;
    }
    ESP_LOGI(TAG, "Opus codec deinitialized");
}
