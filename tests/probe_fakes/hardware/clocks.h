#include <stdint.h>
#define clk_sys 0
static inline uint32_t clock_get_hz(int clock) { (void)clock; return 125000000; }
