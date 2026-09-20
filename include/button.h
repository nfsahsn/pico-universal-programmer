/**
 * @file button.h
 * @brief Non-blocking state-machine debouncer for the mode selection button.
 */

#ifndef BUTTON_H
#define BUTTON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_SHORT_PRESS, /**< Button pressed and released (< 2s) */
    BUTTON_EVENT_LONG_PRESS   /**< Button held for >= 2s               */
} button_event_t;

/**
 * @brief Initializes the mode selection button GPIO and pull-up.
 */
void button_init(void);

/**
 * @brief Polls the button state machine. Must be called frequently in main loop.
 * @return button_event_t Detected button event.
 */
button_event_t button_poll(void);

/**
 * @brief Check if button is currently pressed.
 */
bool button_is_pressed(void);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_H */
