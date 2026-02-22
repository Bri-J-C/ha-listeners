/**
 * Audio Output (Speaker) Module
 *
 * I2S interface to MAX98357A amplifier.
 */

#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "protocol.h"

// Pin configuration for MAX98357A
// Adjust these for your wiring
#define I2S_SPK_SCK_PIN     15  // Serial Clock (BCLK)
#define I2S_SPK_WS_PIN      16  // Word Select (LRCLK)
#define I2S_SPK_SD_PIN      17  // Serial Data (DIN)

/**
 * Initialize audio output (I2S speaker).
 * @return ESP_OK on success
 */
esp_err_t audio_output_init(void);

/**
 * Start audio output.
 */
void audio_output_start(void);

/**
 * Stop audio output.
 */
void audio_output_stop(void);

/**
 * Check if audio output is active.
 */
bool audio_output_is_active(void);

/**
 * Write audio samples to speaker.
 *
 * @param buffer Buffer containing samples (int16_t)
 * @param samples Number of samples to write
 * @param timeout_ms Timeout in milliseconds
 * @return Number of samples written, or -1 on error
 */
int audio_output_write(const int16_t *buffer, size_t samples, uint32_t timeout_ms);

/**
 * Set output volume (0-100).
 * Note: Software volume control via scaling samples.
 */
void audio_output_set_volume(uint8_t volume);

/**
 * Get current volume (0-100).
 */
uint8_t audio_output_get_volume(void);

/**
 * Set mute state.
 */
void audio_output_set_mute(bool muted);

/**
 * Get mute state.
 */
bool audio_output_is_muted(void);

/**
 * Emergency override: force unmute and set volume to 100%.
 * Saves the previous mute/volume state so it can be restored.
 * Safe to call multiple times (subsequent calls are no-ops until restore).
 */
void audio_output_force_unmute_max_volume(void);

/**
 * Restore mute state and volume saved by audio_output_force_unmute_max_volume().
 * No-op if an emergency override is not currently active.
 */
void audio_output_restore_volume(void);

/**
 * Check whether an emergency volume override is currently active.
 */
bool audio_output_is_emergency_override(void);

/**
 * Deinitialize audio output.
 */
void audio_output_deinit(void);

#endif // AUDIO_OUTPUT_H
