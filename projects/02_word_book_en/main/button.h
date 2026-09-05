#pragma once

#include <stdbool.h>

/* The BOOT button on GPIO 0. Active low, hardware pull-up, debounced in software. */
void button_init(void);

/* True once per press. Call from the main loop. */
bool button_pressed(void);
