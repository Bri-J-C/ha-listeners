/**
 * Diagnostics - Logging and crash reporting
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "diagnostics.h"
#include "network.h"

static const char *TAG = "diag";

// Ring buffer for log entries
typedef struct {
    uint32_t timestamp;  // ms since boot
    char level;          // I, W, E, D
    char message[DIAG_LOG_ENTRY_SIZE];
} log_entry_t;

static log_entry_t log_buffer[DIAG_LOG_ENTRIES];
static int log_head = 0;
static int log_count = 0;
static SemaphoreHandle_t log_mutex = NULL;

// Original log function
static vprintf_like_t original_vprintf = NULL;

// Boot time tracking
static int64_t boot_time_us = 0;
static esp_reset_reason_t reset_reason;

/**
 * Custom vprintf that captures logs to ring buffer.
 */
static int diag_vprintf(const char *fmt, va_list args)
{
    // Call original first
    int ret = 0;
    if (original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = original_vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    // Capture to ring buffer
    if (log_mutex && xSemaphoreTake(log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        log_entry_t *entry = &log_buffer[log_head];
        entry->timestamp = (uint32_t)(esp_timer_get_time() / 1000);

        // Parse log level from format (ESP_LOG format: "X (%d) tag: msg")
        entry->level = 'I';
        if (fmt && strlen(fmt) > 0) {
            char first = fmt[0];
            if (first == 'E' || first == 'W' || first == 'I' || first == 'D' || first == 'V') {
                entry->level = first;
            }
        }

        // Format the message
        vsnprintf(entry->message, DIAG_LOG_ENTRY_SIZE, fmt, args);

        // Remove trailing newline
        int len = strlen(entry->message);
        if (len > 0 && entry->message[len-1] == '\n') {
            entry->message[len-1] = '\0';
        }

        log_head = (log_head + 1) % DIAG_LOG_ENTRIES;
        if (log_count < DIAG_LOG_ENTRIES) {
            log_count++;
        }

        xSemaphoreGive(log_mutex);
    }

    return ret;
}

esp_err_t diagnostics_init(void)
{
    // Record boot time and reset reason
    boot_time_us = esp_timer_get_time();
    reset_reason = esp_reset_reason();

    // Create mutex
    log_mutex = xSemaphoreCreateMutex();
    if (!log_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // Hook into ESP_LOG
    original_vprintf = esp_log_set_vprintf(diag_vprintf);

    ESP_LOGI(TAG, "Diagnostics initialized");
    ESP_LOGI(TAG, "Reset reason: %s", diagnostics_get_reset_reason());
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

    return ESP_OK;
}

const char* diagnostics_get_reset_reason(void)
{
    switch (reset_reason) {
        case ESP_RST_POWERON:   return "Power on";
        case ESP_RST_EXT:       return "External reset";
        case ESP_RST_SW:        return "Software reset";
        case ESP_RST_PANIC:     return "Crash/Panic";
        case ESP_RST_INT_WDT:   return "Interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "Task watchdog timeout";
        case ESP_RST_WDT:       return "Watchdog";
        case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
        case ESP_RST_BROWNOUT:  return "Brownout (low voltage)";
        case ESP_RST_SDIO:      return "SDIO reset";
        default:                return "Unknown";
    }
}

uint32_t diagnostics_get_uptime(void)
{
    return (uint32_t)((esp_timer_get_time() - boot_time_us) / 1000000);
}

char* diagnostics_get_logs_html(void)
{
    // Allocate buffer for HTML output
    size_t buf_size = DIAG_LOG_ENTRIES * (DIAG_LOG_ENTRY_SIZE + 100) + 512;
    char *html = malloc(buf_size);
    if (!html) return NULL;

    char *p = html;
    int remaining = buf_size;

    // Header
    int written = snprintf(p, remaining,
        "<div class='logs' id='logbox'>"
        "<style>"
        ".logs { font-family: monospace; font-size: 12px; background: #1a1a1a; color: #eee; padding: 10px; border-radius: 5px; max-height: 400px; overflow-y: auto; }"
        ".log-E { color: #ff6b6b; }"
        ".log-W { color: #feca57; }"
        ".log-I { color: #5cd85c; }"
        ".log-D { color: #48dbfb; }"
        ".log-V { color: #a0a0a0; }"
        ".log-time { color: #888; margin-right: 10px; }"
        "</style>");
    p += written;
    remaining -= written;

    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Output logs from oldest to newest
        int start = (log_count < DIAG_LOG_ENTRIES) ? 0 : log_head;
        for (int i = 0; i < log_count && remaining > 200; i++) {
            int idx = (start + i) % DIAG_LOG_ENTRIES;
            log_entry_t *entry = &log_buffer[idx];

            uint32_t secs = entry->timestamp / 1000;
            uint32_t ms = entry->timestamp % 1000;

            written = snprintf(p, remaining,
                "<div class='log-%c'>"
                "<span class='log-time'>[%3lu.%03lu]</span>"
                "%s</div>",
                entry->level, secs, ms, entry->message);
            p += written;
            remaining -= written;
        }
        xSemaphoreGive(log_mutex);
    }

    snprintf(p, remaining, "</div>");
    return html;
}

char* diagnostics_get_json(void)
{
    size_t buf_size = 640;
    char *json = malloc(buf_size);
    if (!json) return NULL;

    uint32_t uptime = diagnostics_get_uptime();
    uint32_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t min_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    uint32_t total_heap = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);

    uint32_t tx_sent = 0, tx_failed = 0;
    int tx_errno = 0;
    network_get_tx_stats(&tx_sent, &tx_failed, &tx_errno);

    snprintf(json, buf_size,
        "{"
        "\"reset_reason\":\"%s\","
        "\"uptime_seconds\":%lu,"
        "\"uptime_formatted\":\"%lud %luh %lum %lus\","
        "\"free_heap\":%lu,"
        "\"min_heap\":%lu,"
        "\"heap_usage_percent\":%.1f,"
        "\"tx_packets_sent\":%lu,"
        "\"tx_packets_failed\":%lu,"
        "\"tx_last_errno\":%d"
        "}",
        diagnostics_get_reset_reason(),
        uptime,
        uptime / 86400, (uptime % 86400) / 3600, (uptime % 3600) / 60, uptime % 60,
        heap,
        min_heap,
        total_heap > 0 ? 100.0 - (min_heap * 100.0 / total_heap) : 0.0,
        tx_sent,
        tx_failed,
        tx_errno
    );

    return json;
}

void diagnostics_log(const char* tag, const char* format, ...)
{
    va_list args;
    va_start(args, format);

    char msg[DIAG_LOG_ENTRY_SIZE];
    vsnprintf(msg, sizeof(msg), format, args);
    ESP_LOGI(tag, "%s", msg);

    va_end(args);
}
