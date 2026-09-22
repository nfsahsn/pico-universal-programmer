#ifndef TEST_DAP_H
#define TEST_DAP_H
#include <stdint.h>
#define DAP_PORT_JTAG 2
struct test_dap_data { uint32_t debug_port, clock_delay; };
extern struct test_dap_data DAP_Data;
static inline void PIN_DELAY_SLOW(uint32_t delay) { (void)delay; }
#endif
