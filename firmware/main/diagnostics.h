/**
 * Diagnostics - Logging and crash reporting
 *
 * Captures ESP_LOG output to a ring buffer and provides
 * reset reason reporting for debugging.
 */

#pragma once

#include "esp_err.h"

#define DIAG_LOG_ENTRIES 100
#define DIAG_LOG_ENTRY_SIZE 128

/**
 * Initialize diagnostics system.
 * Hooks into ESP_LOG to capture log messages.
 */
esp_err_t diagnostics_init(void);

/**
 * Get the reset reason as a string.
 */
const char* diagnostics_get_reset_reason(void);

/**
 * Get uptime in seconds.
 */
uint32_t diagnostics_get_uptime(void);

/**
 * Get the log buffer as HTML formatted string.
 * Caller must free the returned string.
 */
char* diagnostics_get_logs_html(void);

/**
 * Get diagnostics as JSON.
 * Caller must free the returned string.
 */
char* diagnostics_get_json(void);

/**
 * Add a manual log entry (for non-ESP_LOG messages).
 */
void diagnostics_log(const char* tag, const char* format, ...);
