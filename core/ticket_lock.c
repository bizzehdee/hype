#include "ticket_lock.h"

int hype_ticket_lock_try_claim(volatile unsigned int *next,
                               volatile unsigned int *owner) {
    unsigned int serving = __atomic_load_n(owner, __ATOMIC_ACQUIRE);
    unsigned int expected = serving;

    return __atomic_compare_exchange_n(next, &expected, serving + 1u, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)
               ? 1
               : 0;
}

void hype_ticket_lock_acquire(volatile unsigned int *next, volatile unsigned int *owner) {
    unsigned int mine = __atomic_fetch_add(next, 1u, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(owner, __ATOMIC_ACQUIRE) != mine) {
        __asm__ __volatile__("pause" ::: "memory");
    }
}

void hype_ticket_lock_release(volatile unsigned int *owner) {
    (void)__atomic_add_fetch(owner, 1u, __ATOMIC_ACQ_REL);
}
