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

int main(void) {
    test_idle_lock_is_claimed();
    test_held_lock_is_not_changed();
    test_queued_lock_is_not_changed();
    test_failed_bsp_claim_preserves_release_order();
    puts("ticket_lock: ok");
    return 0;
}
