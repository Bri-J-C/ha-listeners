/**
 * Button and LED Module
 *
 * PTT button handling and LED status feedback.
 * Uses WS2812 RGB LED on GPIO48 for ESP32-S3-DevKitC.
 */

#include "button.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "led_strip.h"

static const char *TAG = "button";

// WS2812 LED on GPIO48
#define WS2812_PIN 48
static led_strip_handle_t led_strip = NULL;

static button_callback_t button_callback = NULL;
static led_state_t current_led_state = LED_STATE_OFF;
static bool button_pressed = false;
static int64_t press_start_time = 0;
static bool long_press_fired = false;
static TaskHandle_t button_task_handle = NULL;
static TimerHandle_t led_blink_timer = NULL;
static bool led_blink_state = false;
static bool button_running = false;
static bool idle_led_enabled = true;  // Show LED when idle by default

// WS2812 LED colors
static void set_led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (led_strip) {
        led_strip_set_pixel(led_strip, 0, r, g, b);
        led_strip_refresh(led_strip);
    }
}

// LED control - disabled
static void set_led(bool on)
{
    (void)on;
    // Disabled due to RMT conflict
}

// LED blink timer callback
static void led_blink_callback(TimerHandle_t timer)
{
    led_blink_state = !led_blink_state;
    set_led(led_blink_state);
}

// Button polling task
static void button_task(void *arg)
{
    bool last_state = true;  // Pulled high, active low

    ESP_LOGI(TAG, "Button task started");

    while (button_running) {
        bool current_state = gpio_get_level(BUTTON_PIN);
        int64_t now = esp_timer_get_time();

        // Button pressed (falling edge)
        if (last_state && !current_state) {
            button_pressed = true;
            press_start_time = now;
            long_press_fired = false;

            if (button_callback) {
                button_callback(BUTTON_EVENT_PRESSED, false);
            }
            ESP_LOGI(TAG, "Button pressed");
        }

        // Button held - check for long press
        if (!current_state && button_pressed && !long_press_fired) {
            int64_t held_ms = (now - press_start_time) / 1000;
            if (held_ms >= LONG_PRESS_MS) {
                long_press_fired = true;
                if (button_callback) {
                    button_callback(BUTTON_EVENT_LONG_PRESS, true);
                }
                ESP_LOGI(TAG, "Long press detected (broadcast mode)");
            }
        }

        // Button released (rising edge)
        if (!last_state && current_state && button_pressed) {
            button_pressed = false;

            if (button_callback) {
                button_callback(BUTTON_EVENT_RELEASED, long_press_fired);
            }
            ESP_LOGI(TAG, "Button released");
        }

        last_state = current_state;
        vTaskDelay(pdMS_TO_TICKS(20));  // 20ms debounce/polling
    }

    ESP_LOGI(TAG, "Button task stopped");
    vTaskDelete(NULL);
}

esp_err_t button_init(void)
{
    ESP_LOGI(TAG, "Initializing button and LED");

    // Configure button GPIO (input with pull-up)
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);

    // Initialize WS2812 RGB LED on GPIO48
    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2812_PIN,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init WS2812 LED: %s", esp_err_to_name(ret));
        led_strip = NULL;
    } else {
        set_led_rgb(32, 32, 32);  // White = ready
        ESP_LOGI(TAG, "WS2812 LED initialized on GPIO%d", WS2812_PIN);
    }

    // Create blink timer
    led_blink_timer = xTimerCreate("led_blink", pdMS_TO_TICKS(200), pdTRUE,
                                    NULL, led_blink_callback);

    // Start button task - 4096 stack for ESP_LOGI calls
    button_running = true;
    xTaskCreate(button_task, "button", 4096, NULL, 6, &button_task_handle);

    ESP_LOGI(TAG, "Button initialized (GPIO%d)", BUTTON_PIN);
    return ESP_OK;
}

void button_set_callback(button_callback_t callback)
{
    button_callback = callback;
}

bool button_is_pressed(void)
{
    return button_pressed;
}

void button_set_led_state(led_state_t state)
{
    if (state == current_led_state) {
        return;
    }

    current_led_state = state;

    // Stop any blinking
    if (led_blink_timer) {
        xTimerStop(led_blink_timer, 0);
    }

    switch (state) {
        case LED_STATE_OFF:
            set_led_rgb(0, 0, 0);      // Off
            break;

        case LED_STATE_IDLE:
            if (idle_led_enabled) {
                set_led_rgb(32, 32, 32);   // Dim white when idle
            } else {
                set_led_rgb(0, 0, 0);      // Off if idle LED disabled
            }
            break;

        case LED_STATE_TRANSMITTING:
            set_led_rgb(0, 64, 0);     // Green when transmitting
            break;

        case LED_STATE_RECEIVING:
            set_led_rgb(0, 0, 64);     // Blue when receiving
            break;

        case LED_STATE_MUTED:
            set_led_rgb(64, 0, 0);     // Red when muted
            break;

        case LED_STATE_ERROR:
            set_led_rgb(64, 0, 0);     // Red for error
            xTimerChangePeriod(led_blink_timer, pdMS_TO_TICKS(100), 0);
            xTimerStart(led_blink_timer, 0);
            break;
    }

    ESP_LOGI(TAG, "LED state: %d", state);
}

led_state_t button_get_led_state(void)
{
    return current_led_state;
}

void button_set_idle_led_enabled(bool enabled)
{
    idle_led_enabled = enabled;
    ESP_LOGI(TAG, "Idle LED %s", enabled ? "enabled" : "disabled");

    // Update LED if currently idle
    if (current_led_state == LED_STATE_IDLE) {
        if (enabled) {
            set_led_rgb(32, 32, 32);   // Dim white
        } else {
            set_led_rgb(0, 0, 0);      // Off
        }
    }
}

bool button_is_idle_led_enabled(void)
{
    return idle_led_enabled;
}

void button_deinit(void)
{
    button_running = false;

    if (button_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(100));
        button_task_handle = NULL;
    }

    if (led_blink_timer) {
        xTimerDelete(led_blink_timer, 0);
        led_blink_timer = NULL;
    }

    set_led(false);

    ESP_LOGI(TAG, "Button deinitialized");
}
