#ifndef PROGRAMMER_PROBE_CONTROLS_H
#define PROGRAMMER_PROBE_CONTROLS_H
#include "config/memory_map.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Integrate once on core 0 before entering the probe's service loop. Display
 * the locked mode; button selection and flash writes belong to the supervisor. */
bool probe_controls_init(probe_mode_t mode);
#define PROBE_WATCHDOG_TOKEN(mode) (0xfa170000u | (((uint32_t)(mode) ^ 0xffu) << 8) | (uint32_t)(mode))
bool probe_controls_take_fault_request(void);
/* Compatibility with a pending save request from an older probe build. New
 * probes lock selection until reset and never produce runtime save requests. */
bool probe_controls_take_save_request(probe_mode_t *mode);
#ifdef __cplusplus
}
#endif
#endif
