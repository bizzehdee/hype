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
