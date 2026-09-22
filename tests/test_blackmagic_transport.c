#include "general.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static bool dtr = true, disconnect_on_wait;
static unsigned room, now, waits;
static const char *incoming = "";
static char outgoing[32];
static unsigned written;
unsigned usb_get_config(void) { return 1; }
unsigned tud_cdc_n_get_line_state(unsigned port) { (void)port; return dtr; }
unsigned tud_cdc_n_write_available(unsigned port) { (void)port; return room; }
unsigned tud_cdc_n_write(unsigned port, const void *data, unsigned count) {
    (void)port; assert(written + count < sizeof(outgoing));
    memcpy(outgoing + written, data, count); written += count; return count;
}
void tud_cdc_n_write_flush(unsigned port) { (void)port; }
unsigned tud_cdc_n_available(unsigned port) { (void)port; return strlen(incoming); }
unsigned tud_cdc_n_read(unsigned port, void *data, unsigned count) {
    (void)port; unsigned n = MIN(count, strlen(incoming));
    memcpy(data, incoming, n); incoming += n; return n;
}
void vTaskDelay(unsigned ticks) {
    now += ticks; waits++; assert(waits < 20);
    if (disconnect_on_wait) dtr = false;
}
int xTaskNotifyWait(unsigned a, unsigned b, uint32_t *value, unsigned ticks) {
    (void)a; (void)b; *value = 0; vTaskDelay(ticks); return 0;
}
void platform_timeout_set(platform_timeout_s *t, unsigned ms) { t->end = now + ms; }
bool platform_timeout_is_expired(platform_timeout_s *t) { return now >= t->end; }
unsigned platform_timeout_time_left(platform_timeout_s *t) { return t->end - now; }
int main(void) {
    disconnect_on_wait = true;
    gdb_if_putchar('a', true);
    assert(!dtr && waits == 1 && written == 0);
    dtr = true; room = 32; disconnect_on_wait = false;
    gdb_if_putchar('b', true);
    assert(written == 1 && outgoing[0] == 'b');
    incoming = "ABC";
    assert(gdb_if_getchar() == 'A');
    dtr = false;
    assert(gdb_if_getchar() == 4);
    dtr = true; incoming = "D";
    assert(gdb_if_getchar() == 'D');
    disconnect_on_wait = true;
    assert(gdb_if_getchar_to(100) == 4);
    puts("Black Magic transport tests passed: disconnect during blocked output, stale input discard, timed-read disconnect");
}
