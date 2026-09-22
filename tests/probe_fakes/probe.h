#ifndef TEST_PROBE_H
#define TEST_PROBE_H
#include <stdbool.h>
void probe_init(void);
void probe_deinit(void);
void probe_read_mode(void);
void probe_write_mode(void);
int probe_reset_level(void);
void probe_assert_reset(bool state);
#endif
