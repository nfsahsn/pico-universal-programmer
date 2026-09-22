#ifndef TEST_TIME_H
#define TEST_TIME_H
#include <stdint.h>
#include <stdbool.h>
typedef struct repeating_timer { int unused; } repeating_timer_t;
bool add_repeating_timer_ms(int32_t delay_ms, bool (*callback)(repeating_timer_t *),
                            void *user_data, repeating_timer_t *timer);
typedef uint64_t absolute_time_t;
absolute_time_t get_absolute_time(void);
uint32_t to_ms_since_boot(absolute_time_t time);
void sleep_ms(uint32_t ms);
#endif
