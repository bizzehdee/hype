#include <stdio.h>
#include <string.h>
#include "../int_in_queue.h"

static int failures;

static void check(const char *what, unsigned long long want, unsigned long long got) {
    if (want != got) {
        printf("FAIL: %s -- want %llu, got %llu\n", what, want, got);
        failures++;
    }
}
static void check_ok(const char *what, int cond) {
    if (!cond) { printf("FAIL: %s\n", what); failures++; }
}

/* Arm `n` transfers, TRB addresses 0x1000, 0x1010, ... Returns how many actually armed. */
static unsigned int arm_n(hype_iiq_t *q, unsigned int n, unsigned int req) {
    unsigned int i, done = 0;
    for (i = 0; i < n; i++) {
        unsigned int b = hype_iiq_reserve(q);
        if (b >= HYPE_INT_IN_DEPTH) break;
        if (!hype_iiq_commit(q, b, 0x1000ull + (uint64_t)(q->generation[b] * 0x100u) + b * 0x10ull,
                             req)) {
            break;
        }
        done++;
    }
    return done;
}

/*
 * THE ORIGINAL BUG. Two completions arrive before the endpoint is polled.
 *
 * The old code retired only at poll time, so after the first completion the head was still
 * the transfer that had already finished -- and the second completion matched nothing, went
 * to an evicting shared table, and could be dropped. A dropped interrupt-IN completion is
 * not a dropped report: the transfer stays outstanding for ever and the endpoint is deaf for
 * the rest of the boot.
 */
static void test_two_completions_before_polling(void) {
    hype_iiq_t q;
    hype_iiq_completion_t c;
    uint64_t t0, t1;

    hype_iiq_reset(&q);
    check("four transfers arm", 4, arm_n(&q, 4, 8));
    check_ok("invariants hold once armed", hype_iiq_check(&q));

    t0 = q.pend_trb[0];
    t1 = q.pend_trb[1];

    check("the first completion is claimed", 1, (unsigned)hype_iiq_deliver(&q, t0, 1u, 0u));
    check("the second is claimed too, without a poll in between", 1,
          (unsigned)hype_iiq_deliver(&q, t1, 1u, 0u));
    check("both are queued", 2, q.done_n);
    check("two transfers remain outstanding", 2, q.inflight);
    check("nothing was lost", 0, q.lost);
    check("nothing was retired behind a later completion", 0, q.skipped);
    check_ok("invariants hold with two queued", hype_iiq_check(&q));

    check("the first report reads back", 1, (unsigned)hype_iiq_take(&q, &c));
    check("in order -- oldest first", t0, c.trb);
    check("the second report reads back", 1, (unsigned)hype_iiq_take(&q, &c));
    check("still in order", t1, c.trb);
    check("the queue is empty", 0, (unsigned)hype_iiq_take(&q, &c));
    check("no generation fault", 0, q.gen_faults);
}

/* The full-depth burst: every transfer completes before anything is read. */
static void test_full_depth_burst(void) {
    hype_iiq_t q;
    hype_iiq_completion_t c;
    uint64_t trb[HYPE_INT_IN_DEPTH];
    unsigned int i, f, fl, cm;

    hype_iiq_reset(&q);
    arm_n(&q, HYPE_INT_IN_DEPTH, 8);
    for (i = 0; i < HYPE_INT_IN_DEPTH; i++) trb[i] = q.pend_trb[i];

    for (i = 0; i < HYPE_INT_IN_DEPTH; i++) {
        check("each completion is claimed", 1, (unsigned)hype_iiq_deliver(&q, trb[i], 1u, 0u));
    }
    check("every completion is retained", HYPE_INT_IN_DEPTH, q.done_n);
    check("nothing is outstanding", 0, q.inflight);
    check("nothing lost", 0, q.lost);
    hype_iiq_census(&q, &f, &fl, &cm);
    check("no buffer is free while its report is unread", 0, f);
    check("no buffer is in flight", 0, fl);
    check("every buffer holds a completion", HYPE_INT_IN_DEPTH, cm);
    check_ok("invariants hold at full queue", hype_iiq_check(&q));

    /* Backpressure: with every buffer holding an unread report, nothing may be armed. */
    check("no buffer can be reserved while all reports are unread", HYPE_INT_IN_DEPTH,
          hype_iiq_reserve(&q));
    check("and so nothing arms", 0, arm_n(&q, 1, 8));

    for (i = 0; i < HYPE_INT_IN_DEPTH; i++) {
        check("each report reads back in order", 1, (unsigned)hype_iiq_take(&q, &c));
        check("oldest first", trb[i], c.trb);
    }
    check("draining frees every buffer again", 1, (unsigned)(hype_iiq_reserve(&q) < HYPE_INT_IN_DEPTH));
    check_ok("invariants hold after draining", hype_iiq_check(&q));
}

/*
 * BUFFER LIFETIME. A completed transfer frees a QUEUE slot, not its buffer -- the report has
 * not been read yet. Handing that buffer back to the controller zeroes it and DMAs over the
 * report. This is the bug review caught before it shipped.
 */
static void test_buffer_not_reused_while_report_unread(void) {
    hype_iiq_t q;
    hype_iiq_completion_t c;
    unsigned int held, b, i;

    hype_iiq_reset(&q);
    arm_n(&q, HYPE_INT_IN_DEPTH, 8);
    held = q.pend_buf[0];

    check("the head completes", 1, (unsigned)hype_iiq_deliver(&q, q.pend_trb[0], 1u, 0u));
    check("its transfer is no longer outstanding", HYPE_INT_IN_DEPTH - 1u, q.inflight);

    /* A queue slot is free, so the old code would re-arm here -- onto `held`. */
    for (i = 0; i < HYPE_INT_IN_DEPTH; i++) {
        b = hype_iiq_reserve(&q);
        if (b >= HYPE_INT_IN_DEPTH) break;
        check_ok("a reserved buffer is never one holding an unread report", b != held);
        if (!hype_iiq_commit(&q, b, 0x7000ull + i, 8)) break;
    }
    check_ok("invariants hold after re-arming", hype_iiq_check(&q));

    check("the queued report is still readable", 1, (unsigned)hype_iiq_take(&q, &c));
    check("and it is the one that completed", held, c.buf);
    check("its buffer was never handed back under it", 0, q.gen_faults);
}

/* xHCI 6.4.2.1: on an IN transfer the event's length field is the RESIDUE. */
static void test_short_packet_residue(void) {
    hype_iiq_t q;
    hype_iiq_completion_t c;

    hype_iiq_reset(&q);
    arm_n(&q, 1, 8);
    check("a short packet is claimed", 1, (unsigned)hype_iiq_deliver(&q, q.pend_trb[0], 13u, 3u));
    check("the report reads back", 1, (unsigned)hype_iiq_take(&q, &c));
    check("residue is recorded, not mistaken for a length", 3, c.residue);
    check("actual = requested - residue", 5, c.actual);
    check("the requested length is kept", 8, c.requested);

    /* A malformed event must not underflow into an enormous length. */
    hype_iiq_reset(&q);
    arm_n(&q, 1, 8);
    (void)hype_iiq_deliver(&q, q.pend_trb[0], 1u, 99u);
    check("a residue past the request is clamped", 1, (unsigned)hype_iiq_take(&q, &c));
    check("and yields zero, never a huge copy", 0, c.actual);
}

/*
 * A completion naming a TRB that is not outstanding is stale or a duplicate. It must retire
 * NOTHING -- letting one stand in for the current transfer is what drove hype's enqueue
 * pointer ten TRBs past the controller's dequeue on real hardware.
 */
static void test_stale_completion_retires_nothing(void) {
    hype_iiq_t q;

    hype_iiq_reset(&q);
    arm_n(&q, HYPE_INT_IN_DEPTH, 8);
    check("a TRB never armed is refused", 0, (unsigned)hype_iiq_deliver(&q, 0xDEADBEEFull, 1u, 0u));
    check("and counted", 1, q.lost);
    check("nothing retired", HYPE_INT_IN_DEPTH, q.inflight);
    check("nothing queued", 0, q.done_n);
    check_ok("invariants hold", hype_iiq_check(&q));

    /* Claiming the head twice: the second naming is stale. */
    (void)hype_iiq_deliver(&q, q.pend_trb[0], 1u, 0u);
    {
        hype_iiq_completion_t c;
        (void)hype_iiq_take(&q, &c);
        check("re-delivering a retired TRB is refused", 0,
              (unsigned)hype_iiq_deliver(&q, c.trb, 1u, 0u));
        check("and counted", 2, q.lost);
    }
}

/*
 * An interrupt ring completes in order, so a match at index k proves 0..k-1 completed too.
 * Their reports are DELIVERED rather than dropped: a HID boot report is a state snapshot, so
 * an intermediate one carries a key transition that is lost with it.
 */
static void test_later_completion_retires_the_ones_behind_it(void) {
    hype_iiq_t q;
    hype_iiq_completion_t c;
    uint64_t t0, t1, t2;

    hype_iiq_reset(&q);
    arm_n(&q, HYPE_INT_IN_DEPTH, 8);
    t0 = q.pend_trb[0]; t1 = q.pend_trb[1]; t2 = q.pend_trb[2];

    check("the third completion is claimed", 1, (unsigned)hype_iiq_deliver(&q, t2, 1u, 0u));
    check("the two behind it retired with it", 3, q.done_n);
    check("and are counted as never having been seen", 2, q.skipped);
    check("one transfer remains outstanding", 1, q.inflight);
    check_ok("invariants hold", hype_iiq_check(&q));

    check("report 0 is delivered, not dropped", 1, (unsigned)hype_iiq_take(&q, &c));
    check("in order", t0, c.trb);
    check("report 1 is delivered too", 1, (unsigned)hype_iiq_take(&q, &c));
    check("in order", t1, c.trb);
    check("then the one that was actually reported", 1, (unsigned)hype_iiq_take(&q, &c));
    check("in order", t2, c.trb);
}

/*
 * The guards. These paths exist so a caller mistake degrades instead of corrupting, and an
 * untested guard is just an untested branch -- the two bugs this module was extracted for
 * both lived in code no test could reach.
 */
static void test_guards_and_invariant_detection(void) {
    hype_iiq_t q;
    hype_iiq_completion_t c;
    unsigned int f, fl, cm, b;

    /* NULL is tolerated everywhere rather than trusted. */
    hype_iiq_reset((hype_iiq_t *)0);
    check("reserve on NULL yields no buffer", HYPE_INT_IN_DEPTH, hype_iiq_reserve((const hype_iiq_t *)0));
    check("commit on NULL fails", 0, (unsigned)hype_iiq_commit((hype_iiq_t *)0, 0, 1, 8));
    check("deliver on NULL fails", 0, (unsigned)hype_iiq_deliver((hype_iiq_t *)0, 1, 1u, 0u));
    check("take on NULL fails", 0, (unsigned)hype_iiq_take((hype_iiq_t *)0, &c));
    check("check on NULL is false", 0, (unsigned)hype_iiq_check((const hype_iiq_t *)0));
    hype_iiq_census((const hype_iiq_t *)0, &f, &fl, &cm);
    check("a NULL census reports nothing free", 0, f);
    hype_iiq_census((const hype_iiq_t *)0, (unsigned int *)0, (unsigned int *)0, (unsigned int *)0);

    hype_iiq_reset(&q);
    check("take from an empty queue fails", 0, (unsigned)hype_iiq_take(&q, &c));
    check("take into NULL fails", 0, (unsigned)hype_iiq_take(&q, (hype_iiq_completion_t *)0));

    /* A buffer index outside the pool, and a buffer that is not free. */
    check("committing an out-of-range buffer fails", 0,
          (unsigned)hype_iiq_commit(&q, HYPE_INT_IN_DEPTH, 0x1000ull, 8));
    b = hype_iiq_reserve(&q);
    check("the first commit succeeds", 1, (unsigned)hype_iiq_commit(&q, b, 0x1000ull, 8));
    check("committing the same buffer twice fails", 0,
          (unsigned)hype_iiq_commit(&q, b, 0x2000ull, 8));
    check_ok("invariants hold after a refused commit", hype_iiq_check(&q));

    /* Filling the queue, then one commit too many. */
    hype_iiq_reset(&q);
    check("the pool fills", HYPE_INT_IN_DEPTH, arm_n(&q, HYPE_INT_IN_DEPTH + 2u, 8));
    check("and refuses the next", HYPE_INT_IN_DEPTH, hype_iiq_reserve(&q));

    /* hype_iiq_check must actually detect corruption, or it proves nothing. */
    q.state[q.pend_buf[0]] = (uint8_t)HYPE_IIQ_FREE;
    check("a transfer holding a free buffer is caught", 0, (unsigned)hype_iiq_check(&q));
    hype_iiq_reset(&q);
    arm_n(&q, 2, 8);
    q.pend_buf[1] = q.pend_buf[0];
    check("two transfers holding one buffer is caught", 0, (unsigned)hype_iiq_check(&q));
    hype_iiq_reset(&q);
    arm_n(&q, 2, 8);
    q.inflight = HYPE_INT_IN_DEPTH; /* disagrees with the buffer states */
    check("a population mismatch is caught", 0, (unsigned)hype_iiq_check(&q));
    hype_iiq_reset(&q);
    arm_n(&q, 2, 8);
    q.pend_buf[0] = HYPE_INT_IN_DEPTH; /* out of range */
    check("an out-of-range buffer index is caught", 0, (unsigned)hype_iiq_check(&q));

    /* The same two checks on the completion side. */
    hype_iiq_reset(&q);
    arm_n(&q, 2, 8);
    (void)hype_iiq_deliver(&q, q.pend_trb[0], 1u, 0u);
    (void)hype_iiq_deliver(&q, q.pend_trb[0], 1u, 0u);
    check_ok("two queued completions are consistent", hype_iiq_check(&q));
    q.done[1].buf = q.done[0].buf;
    check("two completions holding one buffer is caught", 0, (unsigned)hype_iiq_check(&q));
    hype_iiq_reset(&q);
    arm_n(&q, 1, 8);
    (void)hype_iiq_deliver(&q, q.pend_trb[0], 1u, 0u);
    q.done[0].buf = HYPE_INT_IN_DEPTH;
    check("an out-of-range completion buffer is caught", 0, (unsigned)hype_iiq_check(&q));
    hype_iiq_reset(&q);
    arm_n(&q, 1, 8);
    (void)hype_iiq_deliver(&q, q.pend_trb[0], 1u, 0u);
    q.state[q.done[0].buf] = (uint8_t)HYPE_IIQ_INFLIGHT;
    check("a completion whose buffer is still in flight is caught", 0, (unsigned)hype_iiq_check(&q));
}

/*
 * The generation counter exists to make "a buffer was re-armed under a queued completion"
 * impossible to miss. It should never fire -- hype_iiq_reserve() cannot return a COMPLETED
 * buffer -- so the test forces the condition to prove the detector works.
 */
static void test_generation_fault_is_detected(void) {
    hype_iiq_t q;
    hype_iiq_completion_t c;
    unsigned int held;

    hype_iiq_reset(&q);
    arm_n(&q, 1, 8);
    held = q.pend_buf[0];
    (void)hype_iiq_deliver(&q, q.pend_trb[0], 1u, 0u);

    /* Force what the ownership rules prevent: hand the held buffer back to hardware. */
    q.state[held] = (uint8_t)HYPE_IIQ_FREE;
    check("the forced re-arm takes the held buffer", 1,
          (unsigned)hype_iiq_commit(&q, held, 0x9999ull, 8));
    check("reading the stale report is flagged", 1, (unsigned)hype_iiq_take(&q, &c));
    check("and counted as a generation fault", 1, q.gen_faults);
}

int main(void) {
    test_two_completions_before_polling();
    test_full_depth_burst();
    test_buffer_not_reused_while_report_unread();
    test_short_packet_residue();
    test_stale_completion_retires_nothing();
    test_later_completion_retires_the_ones_behind_it();
    test_guards_and_invariant_detection();
    test_generation_fault_is_detected();

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
