/**
 * Acoustic Echo Cancellation (AEC) Module
 *
 * Uses espressif/esp-sr's esp_aec API.
 *
 * The AEC library requires 512-sample (32ms) frames while the Opus codec
 * uses 320-sample (20ms) frames. This module bridges the gap:
 *
 *   audio_input_read()  → mic_accum[] → (when 512 samples) → aec_process()
 *                                                              ↓
 *   audio_output_write() → ref_stream (StreamBuffer)  ───────→ ref_buf
 *                                                              ↓
 *                                              out_ring[] ← cleaned
 *                                                              ↓
 *                                         aec_pop_cleaned() → codec_encode()
 *
 * After pipeline priming (~40ms / 2 mic frames), one cleaned frame
 * is available roughly every 20ms, matching the Opus encode rate.
 */

#include "aec.h"
#include "protocol.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include <string.h>

#ifdef __has_include
#  if __has_include("esp_aec.h")
#    include "esp_aec.h"
#    define AEC_AVAILABLE 1
#  else
#    define AEC_AVAILABLE 0
#  endif
#else
#  include "esp_aec.h"
#  define AEC_AVAILABLE 1
#endif

static const char *TAG = "aec";

/* AEC frame size is fixed at 512 samples (32ms @ 16kHz) by the library.    */
/* Our Opus frame is 320 samples (20ms). The accumulation and output rings   */
/* smooth this out. After the first AEC run (~40ms), throughput matches.     */
#define AEC_CHUNK_SAMPLES   512
#define MIC_ACCUM_SIZE      (AEC_CHUNK_SAMPLES * 2)  // 2 chunks max in flight
#define OUT_RING_SIZE       1024  // 64ms of cleaned audio (plenty of headroom)
#define REF_STREAM_BYTES    (AEC_CHUNK_SAMPLES * 4 * sizeof(int16_t))  // 128ms ref

/* Acoustic path delay: DMA buffering (~40ms) + speaker-to-mic propagation (~40ms).
 * Pre-filling ref_stream with this many silence samples aligns the reference
 * timeline with the actual echo arrival at the microphone.
 * Also used by aec_flush_reference() when re-priming after a chime flush. */
#define AEC_REF_DELAY_MS      80
#define AEC_REF_DELAY_SAMPLES (SAMPLE_RATE * AEC_REF_DELAY_MS / 1000)  // 1280

#if AEC_AVAILABLE

static aec_handle_t *aec_handle = NULL;
static int          aec_chunk   = 0;

/* Reference signal FIFO — written by output task, read by TX task */
static StreamBufferHandle_t ref_stream = NULL;

/* Mic accumulation buffer — TX task only, no mutex needed */
static int16_t mic_accum[MIC_ACCUM_SIZE];
static int     mic_fill = 0;

/* Output ring buffer — TX task only, no mutex needed */
static int16_t out_ring[OUT_RING_SIZE];
static int     out_write = 0;
static int     out_read  = 0;
static int     out_count = 0;

/* Temp processing buffers — allocated in PSRAM */
static int16_t *ref_buf = NULL;
static int16_t *out_buf = NULL;

/* -------------------------------------------------------------------------
 * Internal: pull reference, run AEC on mic_accum[0..aec_chunk-1],
 * push aec_chunk cleaned samples into out_ring.
 * Called from TX task only.
 * ---------------------------------------------------------------------- */
static void run_aec_chunk(void)
{
    /* Pull matching reference samples (non-blocking) */
    size_t ref_bytes = xStreamBufferReceive(ref_stream, ref_buf,
                                             aec_chunk * sizeof(int16_t), 0);
    int ref_got = (int)(ref_bytes / sizeof(int16_t));

    /* Zero-pad reference if not enough data (no audio playing = silence) */
    if (ref_got < aec_chunk) {
        memset(ref_buf + ref_got, 0, (aec_chunk - ref_got) * sizeof(int16_t));
    }

    /* AEC: mic_accum (echo'd mic) + ref_buf (speaker reference) → out_buf */
    aec_process(aec_handle, mic_accum, ref_buf, out_buf);

    /* Push cleaned samples into output ring */
    for (int i = 0; i < aec_chunk; i++) {
        if (out_count < OUT_RING_SIZE) {
            out_ring[out_write] = out_buf[i];
            out_write = (out_write + 1) % OUT_RING_SIZE;
            out_count++;
        }
        /* If ring is full, oldest samples are overwritten — shouldn't happen
         * at normal 20ms encode rate since we consume 320 every iteration */
    }
}

#endif /* AEC_AVAILABLE */

/* =========================================================================
 * Public API
 * ====================================================================== */

esp_err_t aec_init(void)
{
#if !AEC_AVAILABLE
    ESP_LOGW(TAG, "esp-sr not available — AEC disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_LOGI(TAG, "Initializing AEC (esp-sr)");

    /* filter_length=8: 128ms echo tail — recommended for separate I2S buses  */
    /* (INMP441 + MAX98357A). The extra DMA + acoustic path latency of        */
    /* separate chips needs a longer filter than integrated codecs (filter=4). */
    /* AEC_MODE_VOIP_HIGH_PERF: best quality for two-way intercom voice.      */
    aec_handle = aec_create(SAMPLE_RATE, 8, 1, AEC_MODE_VOIP_HIGH_PERF);
    if (!aec_handle) {
        ESP_LOGE(TAG, "aec_create() failed — AEC disabled");
        return ESP_FAIL;
    }

    aec_chunk = aec_get_chunksize(aec_handle);
    if (aec_chunk != AEC_CHUNK_SAMPLES) {
        ESP_LOGW(TAG, "Unexpected AEC chunk size %d (expected %d)",
                 aec_chunk, AEC_CHUNK_SAMPLES);
    }

    /* Allocate processing buffers — internal RAM preferred for cache performance.
     * These are tiny (~512-1024 bytes each) and accessed every AEC chunk. */
    size_t buf_bytes = aec_chunk * sizeof(int16_t);
    ref_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ref_buf) ref_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out_buf) out_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ref_buf || !out_buf) {
        ESP_LOGE(TAG, "AEC buffer allocation failed");
        aec_destroy(aec_handle);
        aec_handle = NULL;
        heap_caps_free(ref_buf); ref_buf = NULL;
        heap_caps_free(out_buf); out_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* StreamBuffer for reference: single writer (output task), single reader (TX task) */
    ref_stream = xStreamBufferCreate(REF_STREAM_BYTES, sizeof(int16_t));
    if (!ref_stream) {
        ESP_LOGE(TAG, "Failed to create reference StreamBuffer");
        aec_destroy(aec_handle);
        aec_handle = NULL;
        heap_caps_free(ref_buf); ref_buf = NULL;
        heap_caps_free(out_buf); out_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Pre-fill reference with AEC_REF_DELAY_MS of silence to compensate for
     * the acoustic delay between speaker DMA output and mic pickup.
     * This aligns the reference timeline with the actual echo arrival. */
    {
        int16_t silence[256];
        memset(silence, 0, sizeof(silence));
        int remaining = AEC_REF_DELAY_SAMPLES;
        while (remaining > 0) {
            int chunk = remaining < 256 ? remaining : 256;
            xStreamBufferSend(ref_stream, silence, chunk * sizeof(int16_t), 0);
            remaining -= chunk;
        }
        ESP_LOGI(TAG, "Pre-filled reference with %dms (%d samples) silence delay",
                 AEC_REF_DELAY_MS, AEC_REF_DELAY_SAMPLES);
    }

    ESP_LOGI(TAG, "AEC ready: chunk=%d samples (%dms @ %dHz), "
             "ref_buf=%s, out_buf=%s",
             aec_chunk, aec_chunk * 1000 / SAMPLE_RATE, SAMPLE_RATE,
             esp_ptr_external_ram(ref_buf) ? "PSRAM" : "internal",
             esp_ptr_external_ram(out_buf) ? "PSRAM" : "internal");

    return ESP_OK;
#endif
}

bool aec_is_ready(void)
{
#if !AEC_AVAILABLE
    return false;
#else
    return aec_handle != NULL;
#endif
}

void aec_push_reference(const int16_t *ref, int samples)
{
#if !AEC_AVAILABLE
    (void)ref; (void)samples;
    return;
#else
    if (!ref_stream || !ref || samples <= 0) return;

    size_t bytes = (size_t)samples * sizeof(int16_t);

    /* Non-blocking send: drop-newest if buffer full.
     * Never call xStreamBufferReceive here — this is the writer task.
     * StreamBuffer has a single-reader contract; draining from the writer
     * corrupts internal state. Dropping newest is safe: the AEC will
     * zero-pad a missing reference chunk (no echo = silence reference). */
    xStreamBufferSend(ref_stream, ref, bytes, 0);
#endif
}

int aec_push_mic(const int16_t *mic, int samples)
{
#if !AEC_AVAILABLE
    (void)mic; (void)samples;
    return 0;
#else
    if (!aec_handle || !mic || samples <= 0) return 0;

    /* Append incoming mic samples to accumulator, capped at buffer size */
    int to_copy = samples;
    if (mic_fill + to_copy > MIC_ACCUM_SIZE) {
        to_copy = MIC_ACCUM_SIZE - mic_fill;
    }
    if (to_copy > 0) {
        memcpy(mic_accum + mic_fill, mic, to_copy * sizeof(int16_t));
        mic_fill += to_copy;
    }

    /* Process complete AEC chunks */
    while (mic_fill >= aec_chunk) {
        run_aec_chunk();

        /* Shift any leftover samples to front of accumulator */
        int leftover = mic_fill - aec_chunk;
        if (leftover > 0) {
            memmove(mic_accum, mic_accum + aec_chunk,
                    leftover * sizeof(int16_t));
        }
        mic_fill = leftover;
    }

    return out_count;
#endif
}

int aec_pop_cleaned(int16_t *out, int max_samples)
{
#if !AEC_AVAILABLE
    (void)out; (void)max_samples;
    return 0;
#else
    int available = (out_count < max_samples) ? out_count : max_samples;
    for (int i = 0; i < available; i++) {
        out[i] = out_ring[out_read];
        out_read = (out_read + 1) % OUT_RING_SIZE;
        out_count--;
    }
    return available;
#endif
}

void aec_reset(void)
{
#if AEC_AVAILABLE
    /* Clear accumulator and output ring */
    mic_fill  = 0;
    out_write = 0;
    out_read  = 0;
    out_count = 0;

    /* Intentionally do NOT drain ref_stream here. The reference samples from
     * the last few hundred ms of playback are needed by the adaptive filter to
     * cancel any residual room echo picked up by the mic in the first frames
     * after PTT press. The speaker is stopped in on_button_event, but acoustic
     * reflections in the room can persist for 20-50ms after the speaker stops. */

    ESP_LOGD(TAG, "AEC state reset (ref preserved)");
#endif
}

void aec_flush_reference(void)
{
#if AEC_AVAILABLE
    if (!ref_stream) return;

    /* Drain ALL samples currently in ref_stream.
     * After playing a local chime, ref_stream contains chime PCM that has
     * no corresponding room echo — using it as an AEC reference would cause
     * the adaptive filter to incorrectly cancel voice as if it were chime echo.
     *
     * We read in chunks to avoid a large stack allocation. xStreamBufferReceive
     * with a zero timeout is non-blocking; we loop until it returns 0 bytes. */
    int16_t drain_buf[64];
    size_t drained = 0;
    size_t bytes;
    do {
        bytes = xStreamBufferReceive(ref_stream, drain_buf, sizeof(drain_buf), 0);
        drained += bytes / sizeof(int16_t);
    } while (bytes > 0);

    /* Also clear the mic accumulator and output ring — any partial AEC chunk
     * that was being built using the now-invalid reference should be discarded. */
    mic_fill  = 0;
    out_write = 0;
    out_read  = 0;
    out_count = 0;

    /* Re-prime reference with the standard silence pre-delay so that the AEC
     * timing alignment is correct when the next TX session starts.
     * Without this, the first ~80ms of AEC would use a zero-length reference,
     * making it unable to cancel any echo at all during the priming window. */
    int16_t silence[64];
    memset(silence, 0, sizeof(silence));
    int remaining = AEC_REF_DELAY_SAMPLES;
    while (remaining > 0) {
        int chunk = remaining < 64 ? remaining : 64;
        xStreamBufferSend(ref_stream, silence, chunk * sizeof(int16_t), 0);
        remaining -= chunk;
    }

    ESP_LOGI(TAG, "AEC reference flushed (%u stale samples drained, re-primed with %dms silence)",
             (unsigned)drained, AEC_REF_DELAY_MS);
#endif
}

void aec_deinit(void)
{
#if AEC_AVAILABLE
    if (aec_handle) {
        aec_destroy(aec_handle);
        aec_handle = NULL;
    }
    if (ref_stream) {
        vStreamBufferDelete(ref_stream);
        ref_stream = NULL;
    }
    heap_caps_free(ref_buf); ref_buf = NULL;
    heap_caps_free(out_buf); out_buf = NULL;
    mic_fill  = 0;
    out_write = 0;
    out_read  = 0;
    out_count = 0;
    ESP_LOGI(TAG, "AEC deinitialized");
#endif
}
