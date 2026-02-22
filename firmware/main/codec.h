/**
 * Opus Codec Module
 *
 * Encode/decode audio using Opus codec.
 */

#ifndef CODEC_H
#define CODEC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "protocol.h"

/**
 * Initialize Opus encoder and decoder.
 * @return ESP_OK on success
 */
esp_err_t codec_init(void);

/**
 * Encode PCM audio to Opus.
 *
 * @param pcm_in Input PCM samples (int16_t, FRAME_SIZE samples)
 * @param opus_out Output buffer for Opus data
 * @param max_opus_len Maximum output buffer size
 * @return Number of bytes written to opus_out, or -1 on error
 */
int codec_encode(const int16_t *pcm_in, uint8_t *opus_out, size_t max_opus_len);

/**
 * Decode Opus to PCM audio.
 *
 * @param opus_in Input Opus data
 * @param opus_len Length of Opus data
 * @param pcm_out Output buffer for PCM samples (int16_t, FRAME_SIZE samples)
 * @return Number of samples decoded, or -1 on error
 */
int codec_decode(const uint8_t *opus_in, size_t opus_len, int16_t *pcm_out);

/**
 * Decode using Packet Loss Concealment (PLC).
 * Call this when a packet is lost to generate interpolated audio
 * based on the decoder's internal state.
 *
 * @param pcm_out Output buffer for PCM samples (int16_t, FRAME_SIZE samples)
 * @return Number of samples generated, or -1 on error
 */
int codec_decode_plc(int16_t *pcm_out);

/**
 * Decode using Forward Error Correction (FEC) data.
 * Use the FEC data embedded in the NEXT received packet to recover
 * the PREVIOUS lost packet. Call this before normal decode of next packet.
 *
 * @param opus_in Input Opus data (the packet AFTER the lost one)
 * @param opus_len Length of Opus data
 * @param pcm_out Output buffer for PCM samples (int16_t, FRAME_SIZE samples)
 * @return Number of samples decoded, or -1 on error
 */
int codec_decode_fec(const uint8_t *opus_in, size_t opus_len, int16_t *pcm_out);

/**
 * Set encoder bitrate.
 * @param bitrate Bitrate in bps (e.g., 32000 for 32kbps)
 */
void codec_set_bitrate(int bitrate);

/**
 * Set expected packet loss percentage.
 * Helps encoder adapt FEC strength. Higher values = more redundancy.
 * @param loss_percent Expected packet loss (0-100)
 */
void codec_set_packet_loss(int loss_percent);

/**
 * Reset encoder state.
 * Call this at the start of each new TX session so the encoder begins from a
 * clean prediction state. Without this, the encoder carries over internal
 * history from the previous session (e.g., silence frames, or a prior voice
 * session), which can cause the first encoded frames to be mis-predicted and
 * produce artifacts on the receiving decoder.
 */
void codec_reset_encoder(void);

/**
 * Reset decoder state.
 * Call this when starting a new audio stream to clear any residual state.
 */
void codec_reset_decoder(void);

/**
 * Deinitialize codec.
 */
void codec_deinit(void);

#endif // CODEC_H
