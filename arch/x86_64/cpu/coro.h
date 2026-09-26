#ifndef HYPE_CORO_H
#define HYPE_CORO_H

#include <stdint.h>

/*
 * SMP-13 (#469): host-side stack switch between shared-tier vCPU loops. See coro.S for the
 * frame layout. Only boot/main.c uses this: the unit-test build does not assemble coro.S.
 */
void hype_coro_switch(uint64_t *save_sp, uint64_t load_sp);
void hype_coro_trampoline(void);

/*
 * Build the first frame of a new loop on the stack ending at stack_top, so that the first
 * hype_coro_switch() into the returned SP calls entry(arg) there. Interrupts start ENABLED
 * (RFLAGS = 0x202), as a dedicated core's loop starts after fw_1_ap_main's sti.
 */
static inline uint64_t hype_coro_init(uint64_t stack_top, void (*entry)(void *), void *arg) {
    uint64_t *sp = (uint64_t *)(uintptr_t)(stack_top & ~0xFull);
    unsigned int i;

    *--sp = 0;                                      /* keeps RSP 8 mod 16 after the ret */
    *--sp = (uint64_t)(uintptr_t)hype_coro_trampoline; /* return address */
    *--sp = 0x202;                                  /* RFLAGS: IF set, reserved bit 1 */
    *--sp = 0;                                      /* RBX */
    *--sp = 0;                                      /* RBP */
    *--sp = 0;                                      /* RDI */
    *--sp = 0;                                      /* RSI */
    *--sp = (uint64_t)(uintptr_t)entry;             /* R12 */
    *--sp = (uint64_t)(uintptr_t)arg;               /* R13 */
    *--sp = 0;                                      /* R14 */
    *--sp = 0;                                      /* R15 */
    for (i = 0; i < 20u; i++) *--sp = 0;            /* XMM6..XMM15 */
    return (uint64_t)(uintptr_t)sp;
}

#endif
