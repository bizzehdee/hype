/*
 * #538: a micro-kernel that deliberately kills itself, so "a failing test stops its own VM and
 * nothing else" is a thing that gets RUN rather than a thing that is asserted.
 *
 * It reports first, then triple-faults. The order matters: a test that dies without saying
 * anything is indistinguishable from one that hung, and the whole point of the verdict discipline
 * is that silence is a failure rather than an absence of news.
 *
 * The fault is a #UD from an all-zero IDT: the loader leaves IDTR at base 0 limit 0xFFFF over
 * zeroed RAM, so any exception has no usable handler and the second and third faults follow
 * immediately. That is the same shutdown a genuinely broken microtest produces, which is what
 * makes this a fair stand-in for one.
 */
#include "micro.h"

#define NAME "faulter"

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    (void)zero_page_gpa;

    micro_puts("\n");
    micro_puts("micro/" NAME ": alive, about to fault on purpose\n");
    micro_fail(NAME, "faulting deliberately -- this VM must stop and no other VM may notice");

    /* UD2. Nothing in this guest can handle it. */
    __asm__ volatile("ud2");

    micro_puts("micro/" NAME ": UNREACHABLE -- ud2 did not fault\n");
    micro_halt();
}
