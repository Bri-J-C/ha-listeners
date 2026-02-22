/**
 * Display Module
 *
 * SSD1306 OLED display for room selection and status.
 * Optional feature - can be disabled at compile time.
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Feature flag - set to 0 to disable display code
#define FEATURE_DISPLAY 1

// Pin configuration for I2C OLED
#define DISPLAY_SDA_PIN     8
#define DISPLAY_SCL_PIN     9
#define DISPLAY_I2C_ADDR    0x3C  // Common SSD1306 address (some use 0x3D)

// Cycle button (avoid GPIO 0, 3, 45, 46 - strapping pins)
#define CYCLE_BUTTON_PIN    10

// Display dimensions
#define DISPLAY_WIDTH       128
#define DISPLAY_HEIGHT      64

// Maximum rooms in target list
#define MAX_ROOMS           8
#define MAX_ROOM_NAME_LEN   32

/**
 * Room target entry.
 */
typedef struct {
    char name[MAX_ROOM_NAME_LEN];
    char ip[16];  // IP address or "224.0.0.100" for multicast
    bool is_multicast;
    bool is_mobile;  // Mobile device (needs notification)
} room_target_t;

/**
 * Display state for showing status.
 */
typedef enum {
    DISPLAY_STATE_IDLE,
    DISPLAY_STATE_SELECTING,    // Showing room list
    DISPLAY_STATE_TRANSMITTING,
    DISPLAY_STATE_RECEIVING,
    DISPLAY_STATE_ERROR,
} display_state_t;

#if FEATURE_DISPLAY

/**
 * Initialize display and cycle button.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no display detected
 */
esp_err_t display_init(void);

/**
 * Check if display is available.
 */
bool display_is_available(void);

/**
 * Set the list of available room targets.
 * First entry should be "All Rooms" (multicast).
 *
 * @param rooms Array of room targets
 * @param count Number of rooms
 */
void display_set_rooms(const room_target_t *rooms, int count);

/**
 * Get current number of rooms.
 */
int display_get_room_count(void);

/**
 * Get currently selected room index.
 */
int display_get_selected_index(void);

/**
 * Get currently selected room.
 * @return Pointer to selected room, or NULL if none
 */
const room_target_t *display_get_selected_room(void);

/**
 * Cycle to next room in list.
 * Wraps around to first room after last.
 */
void display_cycle_next(void);

/**
 * Display mode (which page is shown on the OLED).
 */
typedef enum {
    DISPLAY_MODE_ROOMS,     // Normal room selector page
    DISPLAY_MODE_SETTINGS,  // Settings page
} display_mode_t;

/**
 * Settings item indices — must match the settings_items[] array in display.c.
 */
#define SETTINGS_ITEM_DND      0
#define SETTINGS_ITEM_PRIORITY 1
#define SETTINGS_ITEM_MUTE     2
#define SETTINGS_ITEM_VOLUME   3
#define SETTINGS_ITEM_AGC      4
#define SETTINGS_ITEM_LED      5
#define SETTINGS_ITEM_COUNT    6

/**
 * Callback fired when the user changes a setting on the display.
 * @param index  SETTINGS_ITEM_xxx constant
 * @param value  New numeric value (bool cast to int for toggles, raw int for others)
 */
typedef void (*display_settings_cb_t)(int index, int value);

/**
 * Set callback for settings changes made via the display.
 * Called from display.c (cycle_button_task) — runs in the cycle button task context.
 */
void display_set_settings_callback(display_settings_cb_t cb);

/**
 * Sync settings page values from the current settings_get() snapshot.
 * Call this after any external change (HA command, webserver update) so the
 * settings page reflects the latest values if it is currently open.
 */
void display_sync_settings(void);

/**
 * Callback for long press on cycle button (for mobile notifications).
 */
typedef void (*display_long_press_cb_t)(void);

/**
 * Set callback for long press on cycle button.
 */
void display_set_long_press_callback(display_long_press_cb_t cb);

/**
 * Set display state (idle, transmitting, receiving, etc.)
 */
void display_set_state(display_state_t state);

/**
 * Set the name of the remote party (for RX display).
 */
void display_set_remote_name(const char *name);

/**
 * Update the display (call periodically or after state changes).
 */
void display_update(void);

/**
 * Show a temporary message on display.
 * @param message Message to show
 * @param duration_ms How long to show (0 = until next update)
 */
void display_show_message(const char *message, uint32_t duration_ms);

/**
 * Show AP mode credentials on display for easy device setup.
 * @param ssid AP network name
 * @param password AP password
 */
void display_show_ap_info(const char *ssid, const char *password);

/**
 * Deinitialize display.
 */
void display_deinit(void);

#else // FEATURE_DISPLAY disabled - stub functions

static inline esp_err_t display_init(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline bool display_is_available(void) { return false; }
static inline void display_set_rooms(const room_target_t *rooms, int count) { (void)rooms; (void)count; }
static inline int display_get_room_count(void) { return 0; }
static inline int display_get_selected_index(void) { return 0; }
static inline const room_target_t *display_get_selected_room(void) { return NULL; }
static inline void display_cycle_next(void) {}
static inline void display_set_long_press_callback(display_long_press_cb_t cb) { (void)cb; }
static inline void display_set_settings_callback(display_settings_cb_t cb) { (void)cb; }
static inline void display_sync_settings(void) {}
static inline void display_set_state(display_state_t state) { (void)state; }
static inline void display_set_remote_name(const char *name) { (void)name; }
static inline void display_update(void) {}
static inline void display_show_message(const char *message, uint32_t duration_ms) { (void)message; (void)duration_ms; }
static inline void display_show_ap_info(const char *ssid, const char *password) { (void)ssid; (void)password; }
static inline void display_deinit(void) {}

#endif // FEATURE_DISPLAY

#endif // DISPLAY_H
