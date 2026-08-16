#include <assert.h>
#include <stdio.h>

#include "../ticket_lock.h"

static void test_idle_lock_is_claimed(void) {
    volatile unsigned int next = 9u;
    volatile unsigned int owner = 9u;

    assert(hype_ticket_lock_try_claim(&next, &owner) == 1);
    assert(next == 10u);
    assert(owner == 9u);
}

static void test_held_lock_is_not_changed(void) {
    volatile unsigned int next = 10u;
    volatile unsigned int owner = 9u;

    assert(hype_ticket_lock_try_claim(&next, &owner) == 0);
    assert(next == 10u);
    assert(owner == 9u);
}

static void test_queued_lock_is_not_changed(void) {
    volatile unsigned int next = 12u;
    volatile unsigned int owner = 9u;

    assert(hype_ticket_lock_try_claim(&next, &owner) == 0);
    assert(next == 12u);
    assert(owner == 9u);
}

static void test_failed_bsp_claim_preserves_release_order(void) {
    volatile unsigned int next = 12u;
    volatile unsigned int owner = 9u;

    /* Ticket 9 is held. Tickets 10 and 11 are queued ahead of the BSP. */
    assert(hype_ticket_lock_try_claim(&next, &owner) == 0);

    /* Ticket 9 releases. Existing waiters 10 and 11 must remain queued. */
    owner++;
    assert(owner == 10u);
    assert(next == 12u);
}

/* ---- SMP-7 (#191): acquire/release ---- */

static void test_uncontended_acquire_release_round_trips(void) {
    volatile unsigned next = 0u, owner = 0u;
    hype_ticket_lock_acquire(&next, &owner);
    assert(next == 1u);   /* a ticket was taken */
    assert(owner == 0u);  /* and it is ours -- owner still points at it */
    hype_ticket_lock_release(&owner);
    assert(owner == 1u);
    /* Re-acquirable, and the counters keep advancing rather than resetting. */
    hype_ticket_lock_acquire(&next, &owner);
    assert(next == 2u && owner == 1u);
    hype_ticket_lock_release(&owner);
    assert(owner == 2u);
}

static void test_tickets_are_served_in_order(void) {
    /*
     * FIFO is the property worth asserting: it is why a guest's BSP core, which takes this far
     * more often than its APs, cannot starve them. Single-threaded here, so the queue is
     * simulated by taking tickets before serving them.
     */
    volatile unsigned next = 0u, owner = 0u;
    unsigned a = __atomic_fetch_add(&next, 1u, __ATOMIC_ACQ_REL);
    unsigned b = __atomic_fetch_add(&next, 1u, __ATOMIC_ACQ_REL);
    unsigned c = __atomic_fetch_add(&next, 1u, __ATOMIC_ACQ_REL);
    assert(a == 0u && b == 1u && c == 2u);
    assert(owner == a);            /* the first ticket is served first */
    hype_ticket_lock_release(&owner);
    assert(owner == b);            /* then the second, not the third */
    hype_ticket_lock_release(&owner);
    assert(owner == c);
    hype_ticket_lock_release(&owner);
    assert(owner == next);         /* drained */
}

static void test_acquire_after_a_try_claim_still_orders(void) {
    /* try_claim and acquire share the same counters and must not disagree about whose turn it
     * is -- both paths are live in this codebase. */
    volatile unsigned next = 0u, owner = 0u;
    assert(hype_ticket_lock_try_claim(&next, &owner) == 1);
    assert(next == 1u && owner == 0u);
    hype_ticket_lock_release(&owner);
    hype_ticket_lock_acquire(&next, &owner);
    assert(next == 2u && owner == 1u);
    hype_ticket_lock_release(&owner);
    assert(owner == 2u);
}

int main(void) {
    test_uncontended_acquire_release_round_trips();
    test_tickets_are_served_in_order();
    test_acquire_after_a_try_claim_still_orders();
    test_idle_lock_is_claimed();
    test_held_lock_is_not_changed();
    test_queued_lock_is_not_changed();
    test_failed_bsp_claim_preserves_release_order();
    puts("ticket_lock: ok");
    return 0;
}
