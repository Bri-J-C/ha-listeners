/**
 * Voice Assistant Module
 *
 * ESP-SR WakeNet wake word detection + voice assist state management.
 * Listens for "Alexa" wake word during IDLE state, then streams
 * audio to the hub with PRIORITY_VOICE_ASSIST for HA pipeline processing.
 */

#ifndef VOICE_ASSIST_H
#define VOICE_ASSIST_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * Initialize voice assistant (WakeNet model + task).
 * Call after audio_input_init() and codec_init().
 * @return ESP_OK on success
 */
esp_err_t voice_assist_init(void);

/**
 * Start voice assistant listening (wake word detection).
 * Call after WiFi connected and MQTT started.
 */
void voice_assist_start(void);

/**
 * Stop voice assistant listening.
 */
void voice_assist_stop(void);

/**
 * Check if voice assist is currently active (streaming to HA).
 * Used by button handler to show busy and by RX to reject incoming audio.
 */
bool voice_assist_is_active(void);

/**
 * Cancel an active voice assist session.
 * Called when PTT is pressed during voice assist.
 * Sends MQTT cancel event, stops streaming, returns to IDLE.
 */
void voice_assist_cancel(void);

/**
 * Notify voice assist that TTS playback has finished.
 * Called from the main loop or MQTT handler when hub signals end.
 * Returns device to IDLE state.
 */
void voice_assist_tts_done(void);

/**
 * Check if voice assist is in TTS playback phase.
 * During this phase, incoming intercom audio is still rejected.
 */
bool voice_assist_is_playing_tts(void);

#endif // VOICE_ASSIST_H
