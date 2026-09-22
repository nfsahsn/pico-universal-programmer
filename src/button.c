/**
 * @file button.c
 * @brief Non-blocking state-machine debouncer implementation.
 */

#include "include/button.h"
#include "config/app_config.h"
#include "hardware/gpio.h"
#include "pico/time.h"

typedef enum {
    STATE_RELEASED = 0,
    STATE_DEBOUNCING_PRESS,
    STATE_PRESSED,
    STATE_DEBOUNCING_RELEASE
} debounce_state_t;

static debounce_state_t s_state = STATE_RELEASED;
static uint32_t s_press_start_time = 0;
static uint32_t s_state_entry_time = 0;
static bool s_long_press_fired = false;

void button_init(void) {
    gpio_init(PIN_MODE_BUTTON);
    gpio_set_dir(PIN_MODE_BUTTON, GPIO_IN);
    gpio_pull_up(PIN_MODE_BUTTON);

    s_state = STATE_RELEASED;
    s_long_press_fired = false;
}

bool button_is_pressed(void) {
    return !gpio_get(PIN_MODE_BUTTON);
}

bool button_is_busy(void) {
    return s_state != STATE_RELEASED || button_is_pressed();
}

button_event_t button_poll(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool raw_pressed = button_is_pressed();
    button_event_t event = BUTTON_EVENT_NONE;

    switch (s_state) {
        case STATE_RELEASED:
            if (raw_pressed) {
                s_state = STATE_DEBOUNCING_PRESS;
                s_state_entry_time = now;
            }
            break;

        case STATE_DEBOUNCING_PRESS:
            if (!raw_pressed) {
                s_state = STATE_RELEASED;
            } else if ((now - s_state_entry_time) >= BUTTON_DEBOUNCE_MS) {
                s_state = STATE_PRESSED;
                s_press_start_time = now;
                s_long_press_fired = false;
            }
            break;

        case STATE_PRESSED:
            if (!raw_pressed) {
                s_state = STATE_DEBOUNCING_RELEASE;
                s_state_entry_time = now;
            } else {
                /* Check for continuous hold long press */
                if (!s_long_press_fired && ((now - s_press_start_time) >= BUTTON_LONG_PRESS_MS)) {
                    s_long_press_fired = true;
                    event = BUTTON_EVENT_LONG_PRESS;
                }
            }
            break;

        case STATE_DEBOUNCING_RELEASE:
            if (raw_pressed) {
                s_state = STATE_PRESSED;
            } else if ((now - s_state_entry_time) >= BUTTON_DEBOUNCE_MS) {
                s_state = STATE_RELEASED;
                if (!s_long_press_fired) {
                    event = BUTTON_EVENT_SHORT_PRESS;
                }
                s_long_press_fired = false;
            }
            break;
    }

    return event;
}
