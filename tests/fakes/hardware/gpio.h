#ifndef TEST_GPIO_H
#define TEST_GPIO_H
#include <stdbool.h>
#include <stdint.h>
#define GPIO_IN 0
#define GPIO_OUT 1
#define GPIO_OVERRIDE_NORMAL 0
#define GPIO_OVERRIDE_LOW 2
#define GPIO_OVERRIDE_HIGH 3
void gpio_set_outover(unsigned pin, unsigned value);
void gpio_set_oeover(unsigned pin, unsigned value);
void gpio_init(unsigned pin);
void gpio_set_dir(unsigned pin, bool output);
void gpio_pull_up(unsigned pin);
bool gpio_get(unsigned pin);
void gpio_put(unsigned pin, bool value);
#endif
