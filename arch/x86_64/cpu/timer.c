#include "timer.h"

static volatile uint64_t g_ticks = 0;

uint64_t hype_timer_get_ticks(void) {
    return g_ticks;
}

void hype_timer_tick(void) {
    g_ticks++;
}
