/**
 * Automatic Gain Control (AGC) Module
 *
 * Simple peak-tracking AGC for microphone normalization.
 *
 * Algorithm:
 *   1. Find the peak absolute amplitude of the current frame.
 *   2. Store it in a ring buffer covering ~200ms (10 frames x 20ms).
 *   3. Compute target gain = AGC_TARGET_LEVEL / window_peak.
 *   4. Smooth gain transitions: fast attack when gain must decrease (loud
 *      input), slow release when gain can increase (quiet input).
 *   5. Clamp gain to [AGC_MIN_GAIN, AGC_MAX_GAIN].
 *   6. Apply gain and hard-limit output to INT16 range.
 *
 * The minimum silence threshold prevents divide-by-zero and runaway gain
 * during true silence between words.
 */

#include "agc.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>

static const char *TAG = "agc";

/* ---- Tuning constants ---------------------------------------------------- */

/* Target peak level: -6 dBFS (32768 * 0.5 = 16384) */
#define AGC_TARGET_LEVEL    16384

/* Sliding window length in frames (10 x 20ms = 200ms) */
#define AGC_WINDOW_FRAMES   10

/* Gain bounds */
#define AGC_MIN_GAIN        1.0f   /* Never attenuate — keep quiet quiet */
#define AGC_MAX_GAIN        10.0f  /* +20 dB maximum boost */

/*
 * Attack / release coefficients control how fast the gain changes per frame.
 *   attack  = 0.1  → gain drops 10% of the error each frame (~200ms to halve)
 *   release = 0.01 → gain rises  1% of the error each frame (~2s  to double)
 *
 * Asymmetry prevents loud transients from sounding pumped-up while allowing
 * slow, natural recovery after quiet passages.
 */
#define AGC_ATTACK_COEFF    0.1f
#define AGC_RELEASE_COEFF   0.01f

/*
 * Minimum frame peak before AGC activates.
 * Below this level (roughly -72 dBFS) we treat audio as silence and hold
 * the current gain rather than ramping to maximum.
 */
#define AGC_SILENCE_THRESHOLD 64

/* ---- State --------------------------------------------------------------- */

typedef struct {
    float    current_gain;
    int16_t  peak_history[AGC_WINDOW_FRAMES];
    int      history_index;
    bool     initialized;
} agc_state_t;

static agc_state_t s_agc = {
    .current_gain  = 1.0f,
    .history_index = 0,
    .initialized   = false,
};

/* ---- Public API ---------------------------------------------------------- */

void agc_init(void)
{
    s_agc.current_gain  = 1.0f;
    s_agc.history_index = 0;
    for (int i = 0; i < AGC_WINDOW_FRAMES; i++) {
        s_agc.peak_history[i] = 0;
    }
    s_agc.initialized = true;
    ESP_LOGI(TAG, "AGC initialized (target=%d, window=%d frames, gain=[%.1f, %.1f])",
             AGC_TARGET_LEVEL, AGC_WINDOW_FRAMES, AGC_MIN_GAIN, AGC_MAX_GAIN);
}

void agc_reset(void)
{
    s_agc.current_gain  = 1.0f;
    s_agc.history_index = 0;
    for (int i = 0; i < AGC_WINDOW_FRAMES; i++) {
        s_agc.peak_history[i] = 0;
    }
    ESP_LOGD(TAG, "AGC state reset");
}

void agc_process(int16_t *samples, int count)
{
    if (!s_agc.initialized || samples == NULL || count <= 0) {
        return;
    }

    /* 1. Find peak absolute amplitude of this frame */
    int16_t frame_peak = 0;
    for (int i = 0; i < count; i++) {
        int16_t abs_val = samples[i] < 0 ? -samples[i] : samples[i];
        /* Guard against INT16_MIN: -(-32768) overflows int16, so clamp */
        if (samples[i] == INT16_MIN) {
            abs_val = INT16_MAX;
        }
        if (abs_val > frame_peak) {
            frame_peak = abs_val;
        }
    }

    /* 2. Update peak history ring buffer */
    s_agc.peak_history[s_agc.history_index] = frame_peak;
    s_agc.history_index = (s_agc.history_index + 1) % AGC_WINDOW_FRAMES;

    /* 3. Find maximum peak across the window */
    int16_t window_peak = 0;
    for (int i = 0; i < AGC_WINDOW_FRAMES; i++) {
        if (s_agc.peak_history[i] > window_peak) {
            window_peak = s_agc.peak_history[i];
        }
    }

    /* 4. Compute target gain — hold current gain during silence */
    float target_gain = s_agc.current_gain;
    if (window_peak >= AGC_SILENCE_THRESHOLD) {
        target_gain = (float)AGC_TARGET_LEVEL / (float)window_peak;

        /* Clamp target to valid range */
        if (target_gain < AGC_MIN_GAIN) target_gain = AGC_MIN_GAIN;
        if (target_gain > AGC_MAX_GAIN) target_gain = AGC_MAX_GAIN;
    }

    /* 5. Smooth gain transition (asymmetric attack / release) */
    float coeff;
    if (target_gain < s_agc.current_gain) {
        coeff = AGC_ATTACK_COEFF;   /* gain decreasing — fast */
    } else {
        coeff = AGC_RELEASE_COEFF;  /* gain increasing — slow */
    }
    s_agc.current_gain += coeff * (target_gain - s_agc.current_gain);

    /* Re-clamp after smoothing to absorb floating-point drift */
    if (s_agc.current_gain < AGC_MIN_GAIN) s_agc.current_gain = AGC_MIN_GAIN;
    if (s_agc.current_gain > AGC_MAX_GAIN) s_agc.current_gain = AGC_MAX_GAIN;

    /* 6. Apply gain with hard limiter */
    for (int i = 0; i < count; i++) {
        float gained = (float)samples[i] * s_agc.current_gain;

        /* Hard limit to INT16 range */
        if (gained >  32767.0f) gained =  32767.0f;
        if (gained < -32768.0f) gained = -32768.0f;

        samples[i] = (int16_t)gained;
    }
}
