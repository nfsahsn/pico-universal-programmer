#ifndef TEST_WATCHDOG_H
#define TEST_WATCHDOG_H
#include "pico/stdlib.h"
bool watchdog_caused_reboot(void);
void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms);
#endif
