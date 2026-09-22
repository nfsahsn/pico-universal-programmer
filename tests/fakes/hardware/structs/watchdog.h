#ifndef TEST_WATCHDOG_STRUCT_H
#define TEST_WATCHDOG_STRUCT_H
#include <stdint.h>
typedef struct { uint32_t scratch[8]; } watchdog_hw_t;
extern watchdog_hw_t *watchdog_hw;
#endif
