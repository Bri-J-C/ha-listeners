/**
 * Automatic Gain Control (AGC) Module
 *
 * Peak-tracking AGC to normalize microphone volume before Opus encoding.
 * Speakers at varying distances produce consistent loudness at the receiver.
 *
 * Algorithm: sliding-window peak tracker with asymmetric attack/release gain
 * smoothing and a hard limiter to prevent clipping.
 */

#ifndef AGC_H
#define AGC_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Initialize AGC state.
 * Must be called once before agc_process().
 */
void agc_init(void);

/**
 * Process audio samples in-place.
 *
 * Applies gain normalization to bring peak amplitude toward the target
 * level (-6 dBFS). Gain ramps smoothly using attack/release coefficients
 * and is clamped to [AGC_MIN_GAIN, AGC_MAX_GAIN]. A hard limiter ensures
 * no output sample exceeds INT16_MAX / INT16_MIN.
 *
 * @param samples  Buffer of 16-bit PCM samples, modified in place.
 * @param count    Number of samples in the buffer.
 */
void agc_process(int16_t *samples, int count);

/**
 * Reset AGC state (gain returns to 1.0, history cleared).
 * Call this at TX start to avoid stale gain from previous transmission.
 */
void agc_reset(void);

#endif /* AGC_H */
