/**
 * Acoustic Echo Cancellation (AEC) Module
 *
 * Wraps espressif/esp-sr's esp_aec API for real-time echo cancellation.
 *
 * Architecture:
 *   - Reference signal (what's playing on speaker) is pushed from audio_output_write()
 *   - Mic signal is pushed from the TX task after each I2S read
 *   - AEC processes in 512-sample (32ms) chunks internally
 *   - Cleaned audio is read back in 320-sample (20ms) Opus frames
 *
 * Frame size mismatch (AEC=512, Opus=320) is handled internally with
 * an accumulation buffer and output ring buffer — callers don't see it.
 *
 * Thread safety:
 *   - aec_push_reference() is ISR/task safe (uses FreeRTOS StreamBuffer)
 *   - aec_push_mic() and aec_pop_cleaned() must be called from the same task
 */

#ifndef AEC_H
#define AEC_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * Initialize AEC. Call after audio_output_init() and codec_init().
 * Non-fatal: if esp-sr is unavailable or allocation fails, aec_is_ready()
 * returns false and the TX path falls back to raw mic audio.
 */
esp_err_t aec_init(void);

/**
 * Returns true if AEC initialized successfully.
 */
bool aec_is_ready(void);

/**
 * Push speaker reference samples for echo cancellation.
 * Call from audio_output_write() AFTER volume/mute scaling (so reference
 * amplitude matches what the speaker actually emits).
 * Thread-safe — may be called from any task or ISR context.
 *
 * @param ref    Raw PCM mono samples (int16_t) being written to speaker
 * @param samples Number of samples
 */
void aec_push_reference(const int16_t *ref, int samples);

/**
 * Push raw mic samples and run AEC processing.
 * Call from TX task after every audio_input_read().
 * NOT thread-safe — call from a single task only.
 *
 * @param mic     Raw mic audio from audio_input_read()
 * @param samples Number of samples (typically FRAME_SIZE = 320)
 * @return Number of cleaned samples now available via aec_pop_cleaned()
 */
int aec_push_mic(const int16_t *mic, int samples);

/**
 * Read echo-cancelled audio for Opus encoding.
 * Call from TX task after aec_push_mic().
 * NOT thread-safe — call from a single task only.
 *
 * @param out        Output buffer (int16_t)
 * @param max_samples Maximum samples to return (typically FRAME_SIZE = 320)
 * @return Number of samples written to out (may be < max_samples while priming)
 */
int aec_pop_cleaned(int16_t *out, int max_samples);

/**
 * Reset AEC state (e.g. between TX sessions).
 * Clears mic accumulator and output ring but preserves ref_stream contents
 * so residual room echo from recent playback can still be cancelled.
 * Use aec_flush_reference() instead when the previous audio source was a
 * local chime or other non-acoustic-echo source.
 */
void aec_reset(void);

/**
 * Flush the AEC reference stream and re-prime it with silence.
 * Call this after playing audio that should NOT be treated as an echo
 * reference for the next TX session (e.g. local chime playback).
 * Unlike aec_reset(), this also drains any stale samples from ref_stream
 * and re-inserts the standard silence pre-delay so AEC timing stays correct.
 * NOT thread-safe with aec_push_reference() — call from main task only,
 * when the speaker is already stopped.
 */
void aec_flush_reference(void);

/**
 * Deinitialize AEC and free resources.
 */
void aec_deinit(void);

#endif // AEC_H
