/* SPDX-License-Identifier: GPL-3.0-or-later
 * Fixed original-Pico board binding for the MioLink Black Magic platform. */
#include "general.h"
#include "platform.h"
#include "version.h"

static const platform_target_pins_t target_pins = {
    .tck = 2, .tms = 3, .tms_dir = PIN_NOT_CONNECTED, .tdi = 4, .tdo = 5,
    .uart_tx = 0, .uart_rx = 1, .reset = 6, .reset_state = false,
};
static const platform_led_pins_t leds = {
    .act = PIN_NOT_CONNECTED, .ser = PIN_NOT_CONNECTED, .err = PIN_NOT_CONNECTED,
};
char board_ident[BOARD_IDENT_LENGTH];
platform_device_type_t platform_hwtype(void) { return PLATFORM_DEVICE_TYPE_PICO; }
void platform_update_hwtype(void) {}
int platform_hwversion(void) { return HWVERSION_PICO; }
const platform_target_pins_t *platform_get_target_pins(void) { return &target_pins; }
const platform_led_pins_t *platform_get_led_pins(void) { return &leds; }
const platform_vtref_info_t *platform_get_vtref_info(void) { return NULL; }
void platform_make_board_ident(void) {
    snprintf(board_ident, sizeof(board_ident), "Pico Universal Black Magic %s", FIRMWARE_VERSION);
}
const char *platform_ident(void) { return "Pico Universal"; }
