#include "timer.h"
#include "pic.h"

void hype_timer_isr(const hype_isr_frame_t *frame) {
    (void)frame;
    hype_timer_tick();
    hype_pic_send_eoi(HYPE_TIMER_IRQ);
}
