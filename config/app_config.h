/**
 * @file app_config.h
 * @brief Hardware pin assignments and timing configurations.
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * GPIO Pin Assignments
 * ------------------------------------------------------------------------- */

/**
 * @brief Mode Selection Pushbutton.
 * Active-LOW with internal pull-up enabled. Connect pushbutton between GP15 and GND.
 */
#define PIN_MODE_BUTTON                 (15U)

/**
 * @brief Onboard Green Status LED (Raspberry Pi Pico default).
 */
#define PIN_ONBOARD_LED                 (25U)

/**
 * @brief Optional Discrete Mode Status LEDs.
 * Connect anode via 330 Ohm resistor to GPIO, cathode to GND.
 */
#define PIN_LED_MODE1_DAP               (16U) /**< Green: Mode 1 (CMSIS-DAP)  */
#define PIN_LED_MODE2_BMP               (17U) /**< Yellow: Mode 2 (Black Magic)*/
#define PIN_LED_MODE3_WCH               (18U) /**< Blue: Mode 3 (PicoRVD)     */

/* -------------------------------------------------------------------------
 * Timing & Debounce Configuration (Milliseconds)
 * ------------------------------------------------------------------------- */

#define BUTTON_DEBOUNCE_MS              (20U)    /**< 20ms debounce confirmation time */
#define BUTTON_LONG_PRESS_MS            (2000U)  /**< Long press to save default */

#define LED_BLINK_PULSE_MS              (150U)   /**< Duration of each blink     */
#define LED_BLINK_PAUSE_MS              (150U)   /**< Pause between blinks       */
#define LED_CYCLE_PAUSE_MS              (800U)   /**< Pause after complete cycle */

/* Delay before jumping to target firmware after selection (gives visual feedback) */
#define BOOT_HANDOVER_DELAY_MS          (5000U)

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
