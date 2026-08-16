#ifndef HYPE_CORE_TICKET_LOCK_H
#define HYPE_CORE_TICKET_LOCK_H

/*
 * Claim an idle ticket lock without joining its wait queue.
 *
 * Returns 1 after reserving the current owner ticket. Returns 0 when the lock
 * is held or another waiter is already queued. A failed claim changes neither
 * counter, so the caller may stop waiting without cancelling a ticket.
 */
int hype_ticket_lock_try_claim(volatile unsigned int *next,
                               volatile unsigned int *owner);

/*
 * SMP-7 (#191): a full ticket lock, for serialising access to state two cores share.
 *
 * FIFO by construction -- each waiter takes a ticket and waits its turn -- so a core cannot be
 * starved by a neighbour that keeps re-acquiring. That matters here: a guest's BSP core holds
 * this far more often than its APs do, and a test-and-set lock would let it monopolise the
 * device models indefinitely.
 *
 * Fair warning to callers: nothing may block, spin on another condition, or enter a guest
 * while holding it.
 */
void hype_ticket_lock_acquire(volatile unsigned int *next, volatile unsigned int *owner);
void hype_ticket_lock_release(volatile unsigned int *owner);

#endif /* HYPE_CORE_TICKET_LOCK_H */
