/**
 * Button and LED Module
 *
 * PTT button handling and LED status feedback.
 */

#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "protocol.h"

// Pin configuration
#define BUTTON_PIN      0   // GPIO0 (BOOT button on most dev boards)
#define LED_PIN         -1  // Disabled - GPIO48 is WS2812, not simple GPIO

// For boards with separate RGB LEDs
#define LED_RED_PIN     -1  // Set to actual pin if separate
#define LED_GREEN_PIN   -1
#define LED_BLUE_PIN    -1

// Timing
#define LONG_PRESS_MS   2000  // 2 seconds for broadcast mode

/**
 * Button event types.
 */
typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_PRESSED,      // Button pressed (PTT start)
    BUTTON_EVENT_RELEASED,     // Button released (PTT stop)
    BUTTON_EVENT_LONG_PRESS,   // Long press detected (broadcast mode)
} button_event_t;

/**
 * Callback for button events.
 */
typedef void (*button_callback_t)(button_event_t event, bool is_broadcast);

/**
 * Initialize button and LED.
 * @return ESP_OK on success
 */
esp_err_t button_init(void);

/**
 * Set callback for button events.
 */
void button_set_callback(button_callback_t callback);

/**
 * Check if button is currently pressed.
 */
bool button_is_pressed(void);

/**
 * Set LED state.
 */
void button_set_led_state(led_state_t state);

/**
 * Get current LED state.
 */
led_state_t button_get_led_state(void);

/**
 * Enable/disable idle LED.
 * When disabled, LED turns off when idle instead of showing white.
 */
void button_set_idle_led_enabled(bool enabled);

/**
 * Check if idle LED is enabled.
 */
bool button_is_idle_led_enabled(void);

/**
 * Deinitialize button and LED.
 */
void button_deinit(void);

#endif // BUTTON_H
