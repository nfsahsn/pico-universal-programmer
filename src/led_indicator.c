/**
 * @file led_indicator.c
 * @brief Non-blocking LED sequencer implementation.
 */

#include "include/led_indicator.h"
#include "config/app_config.h"
#include "hardware/gpio.h"
#include "pico/time.h"

typedef enum {
    BLINK_STATE_IDLE = 0,
    BLINK_STATE_PULSE_ON,
    BLINK_STATE_PULSE_OFF,
    BLINK_STATE_CYCLE_PAUSE
} blink_state_t;

static probe_mode_t s_active_mode = MODE_CMSIS_DAP;
static blink_state_t s_blink_state = BLINK_STATE_IDLE;
static uint32_t s_last_transition_time = 0;
static uint8_t s_blink_count = 0;
static uint8_t s_target_blinks = 1;

static void update_discrete_leds(probe_mode_t mode) {
    gpio_put(PIN_LED_MODE1_DAP, (mode == MODE_CMSIS_DAP));
    gpio_put(PIN_LED_MODE2_BMP, (mode == MODE_BLACKMAGIC));
    gpio_put(PIN_LED_MODE3_WCH, (mode == MODE_PICORVD));
}

void led_init(void) {
    gpio_init(PIN_ONBOARD_LED);
    gpio_set_dir(PIN_ONBOARD_LED, GPIO_OUT);
    gpio_put(PIN_ONBOARD_LED, 0);

    gpio_init(PIN_LED_MODE1_DAP);
    gpio_set_dir(PIN_LED_MODE1_DAP, GPIO_OUT);

    gpio_init(PIN_LED_MODE2_BMP);
    gpio_set_dir(PIN_LED_MODE2_BMP, GPIO_OUT);

    gpio_init(PIN_LED_MODE3_WCH);
    gpio_set_dir(PIN_LED_MODE3_WCH, GPIO_OUT);

    led_set_mode(MODE_CMSIS_DAP);
}

void led_set_mode(probe_mode_t mode) {
    if (mode >= MODE_COUNT) {
        mode = MODE_CMSIS_DAP;
    }
    s_active_mode = mode;
    s_target_blinks = (uint8_t)mode + 1; /* Mode 0 -> 1 blink, Mode 1 -> 2 blinks, Mode 2 -> 3 blinks */
    s_blink_count = 0;
    s_blink_state = BLINK_STATE_PULSE_ON;
    s_last_transition_time = to_ms_since_boot(get_absolute_time());

    gpio_put(PIN_ONBOARD_LED, 1);
    update_discrete_leds(mode);
}

void led_tick(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    switch (s_blink_state) {
        case BLINK_STATE_PULSE_ON:
            if ((now - s_last_transition_time) >= LED_BLINK_PULSE_MS) {
                gpio_put(PIN_ONBOARD_LED, 0);
                s_blink_count++;
                s_last_transition_time = now;
                s_blink_state = BLINK_STATE_PULSE_OFF;
            }
            break;

        case BLINK_STATE_PULSE_OFF:
            if ((now - s_last_transition_time) >= LED_BLINK_PAUSE_MS) {
                s_last_transition_time = now;
                if (s_blink_count < s_target_blinks) {
                    gpio_put(PIN_ONBOARD_LED, 1);
                    s_blink_state = BLINK_STATE_PULSE_ON;
                } else {
                    s_blink_state = BLINK_STATE_CYCLE_PAUSE;
                }
            }
            break;

        case BLINK_STATE_CYCLE_PAUSE:
            if ((now - s_last_transition_time) >= LED_CYCLE_PAUSE_MS) {
                s_blink_count = 0;
                s_last_transition_time = now;
                gpio_put(PIN_ONBOARD_LED, 1);
                s_blink_state = BLINK_STATE_PULSE_ON;
            }
            break;

        case BLINK_STATE_IDLE:
        default:
            break;
    }
}

void led_flash_save_confirmation(void) {
    /* 5 rapid blinks to confirm write to flash */
    for (int i = 0; i < 5; i++) {
        gpio_put(PIN_ONBOARD_LED, 1);
        gpio_put(PIN_LED_MODE1_DAP, 1);
        gpio_put(PIN_LED_MODE2_BMP, 1);
        gpio_put(PIN_LED_MODE3_WCH, 1);
        sleep_ms(60);
        gpio_put(PIN_ONBOARD_LED, 0);
        gpio_put(PIN_LED_MODE1_DAP, 0);
        gpio_put(PIN_LED_MODE2_BMP, 0);
        gpio_put(PIN_LED_MODE3_WCH, 0);
        sleep_ms(60);
    }
    update_discrete_leds(s_active_mode);
}

void led_show_error(void) {
    /* 1-second alternating error pattern (easy to see) */
    while (1) {
        gpio_put(PIN_ONBOARD_LED, 1);
        gpio_put(PIN_LED_MODE1_DAP, 1);
        gpio_put(PIN_LED_MODE2_BMP, 0);
        gpio_put(PIN_LED_MODE3_WCH, 0);
        sleep_ms(1000);
        gpio_put(PIN_ONBOARD_LED, 0);
        gpio_put(PIN_LED_MODE1_DAP, 0);
        gpio_put(PIN_LED_MODE2_BMP, 1);
        gpio_put(PIN_LED_MODE3_WCH, 1);
        sleep_ms(1000);
    }
}
