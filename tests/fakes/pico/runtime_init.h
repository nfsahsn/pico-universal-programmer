#ifndef TEST_RUNTIME_INIT_H
#define TEST_RUNTIME_INIT_H
/* Expose startup registration without running it before the test harness. */
#define PICO_RUNTIME_INIT_FUNC(fn, order) void (*test_early_init)(void) = fn
#endif
