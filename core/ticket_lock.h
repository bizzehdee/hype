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

#endif /* HYPE_CORE_TICKET_LOCK_H */
