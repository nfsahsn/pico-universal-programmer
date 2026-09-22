#ifdef NDEBUG
#error Test assertions must remain enabled
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "DAP_config.h"
#include "DAP.h"

struct test_dap_data DAP_Data;
volatile uint32_t cached_delay;
static bool levels[30], outputs[30], latches[30], pullups[30];
static unsigned init_count, deinit_count, pio_sequences;
static uint8_t sampled_tms[256];
static unsigned clocks;
static bool capture;
static bool swd_active;
static unsigned idle_waits, output_override[30], enable_override[30];
bool probe_swd_is_active(void) { return swd_active; }
void probe_wait_idle(void) { idle_waits++; }
void gpio_set_outover(unsigned pin, unsigned value) { output_override[pin] = value; }
void gpio_set_oeover(unsigned pin, unsigned value) { enable_override[pin] = value; }

void gpio_init(unsigned pin) { outputs[pin] = false; latches[pin] = false; }
void gpio_set_dir(unsigned pin, bool output) {
    outputs[pin] = output;
    levels[pin] = output ? latches[pin] : pullups[pin];
}
void gpio_pull_up(unsigned pin) {
    pullups[pin] = true;
    if (!outputs[pin]) levels[pin] = true;
}
bool gpio_get(unsigned pin) { return levels[pin]; }
void gpio_put(unsigned pin, bool value) {
    latches[pin] = value;
    if (!outputs[pin]) return;
    if (capture && pin == PROBE_PIN_SWCLK && value && !levels[pin])
        sampled_tms[clocks++] = levels[PROBE_PIN_SWDIO];
    levels[pin] = value;
}
void probe_init(void) { init_count++; swd_active = true; }
void probe_deinit(void) { deinit_count++; probe_pin_release(); swd_active = false; }
void probe_read_mode(void) {}
void probe_write_mode(void) {}
int probe_reset_level(void) { return levels[PROBE_PIN_RESET]; }
void probe_assert_reset(bool state) { outputs[PROBE_PIN_RESET] = !state; }
uint32_t time_us_32(void) { return 0; }
void pio_swj_sequence(uint32_t count, const uint8_t *data) {
    assert(count == 13 && data[0] == 0xa5);
    pio_sequences++;
}
extern void SWJ_Sequence(uint32_t count, const uint8_t *data);

int main(void) {
    assert(DAP_JTAG == 1 && DAP_SWD == 1);
    assert(PROBE_UART_TX == 0 && PROBE_UART_RX == 1);
    assert(PROBE_PIN_TDI == 4 && PROBE_PIN_TDO == 5);
    assert(PROBE_PIN_RESET == 6 && PROBE_PIN_TRST == 7);
    PORT_JTAG_SETUP();
    assert(deinit_count == 1); /* PIO released before SIO drives JTAG */
    assert(outputs[2] && outputs[3] && outputs[4] && !outputs[5]);
    assert(!outputs[6] && !outputs[7]);
    PIN_nTRST_OUT(0);
    assert(outputs[7] && !levels[7]);
    PIN_nTRST_OUT(1);
    assert(!outputs[7]);
    PIN_TDI_OUT(2);
    assert(!levels[4]);
    PIN_TDI_OUT(3);
    assert(levels[4]);
    levels[5] = true;
    assert(PIN_TDO_IN() == 1);

    const uint8_t sequence[] = {0xa5, 0x13};
    capture = true;
    DAP_Data.debug_port = DAP_PORT_JTAG;
    SWJ_Sequence(13, sequence);
    assert(clocks == 13 && pio_sequences == 0);
    for (unsigned bit = 0; bit < 13; ++bit)
        assert(sampled_tms[bit] == ((sequence[bit / 8] >> (bit % 8)) & 1));
    capture = false;
    PORT_SWD_SETUP();
    assert(init_count == 1 && !outputs[4] && !outputs[5] && !outputs[7]);
    PIN_SWCLK_TCK_CLR();
    PIN_SWDIO_TMS_SET();
    assert(idle_waits == 2);
    assert(output_override[2] == GPIO_OVERRIDE_LOW && enable_override[2] == GPIO_OVERRIDE_HIGH);
    assert(output_override[3] == GPIO_OVERRIDE_HIGH && enable_override[3] == GPIO_OVERRIDE_HIGH);
    probe_pin_release();
    assert(output_override[2] == GPIO_OVERRIDE_NORMAL && enable_override[2] == GPIO_OVERRIDE_NORMAL);
    assert(output_override[3] == GPIO_OVERRIDE_NORMAL && enable_override[3] == GPIO_OVERRIDE_NORMAL);
    DAP_Data.debug_port = 1;
    SWJ_Sequence(13, sequence);
    assert(pio_sequences == 1 && clocks == 13);
    PORT_OFF();
    for (unsigned pin = 2; pin <= 7; ++pin) assert(!outputs[pin]);
    puts("CMSIS-DAP integration tests passed: pin ownership, JTAG TMS clock sequence, SWD routing, open-drain reset");
}
