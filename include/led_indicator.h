/**
 * @file led_indicator.h
 * @brief Non-blocking LED sequencer supporting onboard blink codes and discrete mode LEDs.
 */

#ifndef LED_INDICATOR_H
#define LED_INDICATOR_H

#include "config/memory_map.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the onboard and optional discrete mode LEDs.
 */
void led_init(void);

/**
 * @brief Sets the active mode to display.
 * @param mode The selected probe_mode_t.
 */
void led_set_mode(probe_mode_t mode);

/**
 * @brief Non-blocking tick function to update blink sequences.
 * Must be called repeatedly in the event loop.
 */
void led_tick(void);

/**
 * @brief Flashes LEDs rapidly to confirm settings saved to Flash memory.
 */
void led_flash_save_confirmation(void);

/**
 * @brief Flashes a distinct error pattern if a target image is missing or invalid.
 */
void led_show_error(void);

#ifdef __cplusplus
}
#endif

#endif /* LED_INDICATOR_H */
