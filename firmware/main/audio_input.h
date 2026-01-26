/**
 * Audio Input (Microphone) Module
 *
 * I2S interface to INMP441 MEMS microphone.
 */

#ifndef AUDIO_INPUT_H
#define AUDIO_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "protocol.h"

// Pin configuration for INMP441
// Adjust these for your wiring
#define I2S_MIC_SCK_PIN     4   // Serial Clock (BCLK)
#define I2S_MIC_WS_PIN      5   // Word Select (LRCLK)
#define I2S_MIC_SD_PIN      6   // Serial Data (DOUT)

/**
 * Initialize audio input (I2S microphone).
 * @return ESP_OK on success
 */
esp_err_t audio_input_init(void);

/**
 * Start capturing audio.
 */
void audio_input_start(void);

/**
 * Stop capturing audio.
 */
void audio_input_stop(void);

/**
 * Check if audio input is active.
 */
bool audio_input_is_active(void);

/**
 * Read audio samples from microphone.
 * Blocks until samples are available.
 *
 * @param buffer Buffer to store samples (int16_t)
 * @param samples Number of samples to read
 * @param timeout_ms Timeout in milliseconds
 * @return Number of samples read, or -1 on error
 */
int audio_input_read(int16_t *buffer, size_t samples, uint32_t timeout_ms);

/**
 * Deinitialize audio input.
 */
void audio_input_deinit(void);

#endif // AUDIO_INPUT_H
