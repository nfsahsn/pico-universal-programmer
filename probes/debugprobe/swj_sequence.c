/* Preserve PIO SWD sequences; JTAG TMS sequences use the configured SIO pins. */
#include "DAP_config.h"
#include "DAP.h"

extern void pio_swj_sequence(uint32_t count, const uint8_t *data);

void SWJ_Sequence(uint32_t count, const uint8_t *data) {
    if (DAP_Data.debug_port != DAP_PORT_JTAG) {
        pio_swj_sequence(count, data);
        return;
    }
    for (uint32_t bit = 0; bit < count; ++bit) {
        if (data[bit / 8] & (1u << (bit % 8))) PIN_SWDIO_TMS_SET();
        else PIN_SWDIO_TMS_CLR();
        PIN_SWCLK_TCK_CLR();
        PIN_DELAY_SLOW(DAP_Data.clock_delay);
        PIN_SWCLK_TCK_SET();
        PIN_DELAY_SLOW(DAP_Data.clock_delay);
    }
}
