#include <stdio.h>
#include "../sched.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define SLICE 100u

static unsigned int picked_id(hype_sched_vcpu_t *v) { return v ? v->id : 0xFFFFu; }

static void setup(hype_sched_rq_t *rq, hype_sched_vcpu_t *v, unsigned int n) {
    unsigned int i;
    hype_sched_rq_init(rq, SLICE);
    for (i = 0; i < n; i++) {
        hype_sched_vcpu_init(&v[i], i);
        hype_sched_add(rq, &v[i]);
    }
}

static void test_fifo_round_robin_order(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[3];
    unsigned int want[7] = { 0, 1, 2, 0, 1, 2, 0 };
    unsigned int i;

    setup(&rq, v, 3);
    CHECK_INT("members", 3, rq.members);
    for (i = 0; i < 7u; i++) {
        CHECK_INT("round-robin order", want[i], picked_id(hype_sched_pick(&rq, (uint64_t)i * SLICE)));
    }
}

static void test_halted_is_skipped_and_readmitted_on_wake(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[3];

    setup(&rq, v, 3);
    CHECK_INT("first pick", 0, picked_id(hype_sched_pick(&rq, 0)));
    CHECK_INT("halt a queued vCPU", 0, hype_sched_set_state(&rq, &v[1], HYPE_SCHED_HALTED, 10));
    CHECK_INT("halted vCPU is skipped", 2, picked_id(hype_sched_pick(&rq, 100)));
    CHECK_INT("then back to 0", 0, picked_id(hype_sched_pick(&rq, 200)));
    CHECK_INT("interrupt wakes it", 1, hype_sched_wake(&rq, &v[1]));
    CHECK_INT("woken vCPU state", HYPE_SCHED_RUNNABLE, v[1].state);
    CHECK_INT("re-admitted after 2 (it went to the tail)", 2, picked_id(hype_sched_pick(&rq, 300)));
    CHECK_INT("woken vCPU runs", 1, picked_id(hype_sched_pick(&rq, 400)));
}

static void test_current_halts_mid_slice(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[2];

    setup(&rq, v, 2);
    hype_sched_pick(&rq, 1000);
    CHECK_INT("current halts", 0, hype_sched_set_state(&rq, &v[0], HYPE_SCHED_HALTED, 1030));
    CHECK_INT("charged for the part it ran", 30, v[0].run_time);
    CHECK_INT("queue idle after current halted", 0, rq.current == 0 ? 0 : 1);
    CHECK_INT("no slice while idle", 0, hype_sched_slice_expired(&rq, 5000));
    CHECK_INT("next runnable picked", 1, picked_id(hype_sched_pick(&rq, 1030)));
    CHECK_INT("halt the last runnable", 0, hype_sched_set_state(&rq, &v[1], HYPE_SCHED_BLOCKED, 1100));
    CHECK_INT("nothing runnable: idle", 0xFFFFu, picked_id(hype_sched_pick(&rq, 1100)));
    CHECK_INT("blocked vCPU wakes", 1, hype_sched_wake(&rq, &v[1]));
    CHECK_INT("idle pCPU resumes the woken vCPU", 1, picked_id(hype_sched_pick(&rq, 1200)));
}

static void test_time_accounting_across_many_slices(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[2];
    uint64_t now = 5000;
    unsigned int i;

    setup(&rq, v, 2);
    hype_sched_pick(&rq, now);
    for (i = 0; i < 1000u; i++) {
        CHECK_INT("not expired early", 0, hype_sched_slice_expired(&rq, now + SLICE - 1));
        now += SLICE;
        CHECK_INT("expired at the slice length", 1, hype_sched_slice_expired(&rq, now));
        hype_sched_pick(&rq, now);
    }
    CHECK_INT("v0 run time", 500u * SLICE, v[0].run_time);
    CHECK_INT("v1 run time", 500u * SLICE, v[1].run_time);
    CHECK_INT("v0 slices (it is current again)", 501, v[0].slices);
    CHECK_INT("v1 slices", 500, v[1].slices);
    CHECK_INT("last_scheduled is the last pick", now, v[0].last_scheduled);
    CHECK_INT("v1 last_scheduled one slice earlier", now - SLICE, v[1].last_scheduled);
}

static void test_clock_behind_slice_start_charges_nothing(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[1];

    setup(&rq, v, 1);
    hype_sched_pick(&rq, 1000);
    CHECK_INT("a clock behind the slice start is not expired", 0, hype_sched_slice_expired(&rq, 900));
    hype_sched_pick(&rq, 900);
    CHECK_INT("a clock behind the slice start charges nothing", 0, v[0].run_time);
}

/* Decision 39/85: a dedicated vCPU is a queue of length one -- the same primitive. */
static void test_single_entry_queue_is_the_dedicated_path(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[1];
    uint64_t now = 0;
    unsigned int i;

    setup(&rq, v, 1);
    for (i = 0; i < 100u; i++) {
        CHECK_INT("the only vCPU is always picked", 0, picked_id(hype_sched_pick(&rq, now)));
        now += SLICE;
    }
    CHECK_INT("it was charged every tick up to the last pick", 99u * SLICE, v[0].run_time);
    CHECK_INT("never idle while runnable", 1, rq.current == &v[0]);
    hype_sched_set_state(&rq, &v[0], HYPE_SCHED_HALTED, now);
    CHECK_INT("halted: idle, like a dedicated core in HLT", 0xFFFFu,
              picked_id(hype_sched_pick(&rq, now)));
    hype_sched_wake(&rq, &v[0]);
    CHECK_INT("woken: it runs again", 0, picked_id(hype_sched_pick(&rq, now + 1)));
}

static void test_fairness_over_a_long_synthetic_run(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[5];
    uint64_t now = 0, lo, hi;
    unsigned int i, k;

    setup(&rq, v, 5);
    hype_sched_pick(&rq, now);
    /* Uneven slice use: some slices end early because the vCPU halts and is woken at once. */
    for (i = 0; i < 20000u; i++) {
        if ((i % 7u) == 3u && rq.current != 0) {
            hype_sched_vcpu_t *c = rq.current;
            now += SLICE / 2u;
            hype_sched_set_state(&rq, c, HYPE_SCHED_HALTED, now);
            hype_sched_wake(&rq, c);
        } else {
            now += SLICE;
        }
        hype_sched_pick(&rq, now);
    }
    lo = hi = v[0].run_time;
    for (k = 1; k < 5u; k++) {
        if (v[k].run_time < lo) lo = v[k].run_time;
        if (v[k].run_time > hi) hi = v[k].run_time;
    }
    CHECK_INT("run time spread is within one slice", 1, (hi - lo) <= SLICE);
}

static void test_add_and_remove_mid_run(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[4];

    setup(&rq, v, 3);
    CHECK_INT("pick 0", 0, picked_id(hype_sched_pick(&rq, 0)));
    hype_sched_vcpu_init(&v[3], 3);
    CHECK_INT("add mid-run", 0, hype_sched_add(&rq, &v[3]));
    CHECK_INT("add twice is refused", -1, hype_sched_add(&rq, &v[3]));
    CHECK_INT("remove a queued vCPU", 0, hype_sched_remove(&rq, &v[1], 50));
    CHECK_INT("remove twice is refused", -1, hype_sched_remove(&rq, &v[1], 50));
    CHECK_INT("members after add+remove", 3, rq.members);
    CHECK_INT("removed vCPU is skipped", 2, picked_id(hype_sched_pick(&rq, 100)));
    CHECK_INT("added vCPU runs after the old tail", 3, picked_id(hype_sched_pick(&rq, 200)));
    CHECK_INT("remove the tail", 0, hype_sched_remove(&rq, &v[0], 250));
    CHECK_INT("remove current", 0, hype_sched_remove(&rq, &v[3], 260));
    CHECK_INT("current charged on removal", 60, v[3].run_time);
    CHECK_INT("queue idle after removing current", 1, rq.current == 0);
    CHECK_INT("the survivor runs", 2, picked_id(hype_sched_pick(&rq, 300)));
    CHECK_INT("and keeps running alone", 2, picked_id(hype_sched_pick(&rq, 400)));
    hype_sched_set_state(&rq, &v[2], HYPE_SCHED_STOPPED, 450);
    CHECK_INT("remove a stopped (unqueued) vCPU", 0, hype_sched_remove(&rq, &v[2], 460));
    CHECK_INT("empty queue", 0, rq.members);
    CHECK_INT("empty queue idles", 0xFFFFu, picked_id(hype_sched_pick(&rq, 500)));
    CHECK_INT("re-add a removed vCPU", 0, hype_sched_add(&rq, &v[1]));
    CHECK_INT("it runs", 1, picked_id(hype_sched_pick(&rq, 600)));
}

static void test_state_transitions_and_refusals(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[2], stranger;

    setup(&rq, v, 2);
    hype_sched_vcpu_init(&stranger, 9);
    CHECK_INT("set_state on a non-member", -1,
              hype_sched_set_state(&rq, &stranger, HYPE_SCHED_HALTED, 0));
    CHECK_INT("wake a non-member", 0, hype_sched_wake(&rq, &stranger));
    CHECK_INT("wake a runnable vCPU is a no-op", 0, hype_sched_wake(&rq, &v[0]));
    CHECK_INT("same state is a no-op", 0, hype_sched_set_state(&rq, &v[0], HYPE_SCHED_RUNNABLE, 0));
    CHECK_INT("stop", 0, hype_sched_set_state(&rq, &v[0], HYPE_SCHED_STOPPED, 0));
    CHECK_INT("an interrupt does not start a stopped vCPU", 0, hype_sched_wake(&rq, &v[0]));
    CHECK_INT("stopped vCPU is skipped", 1, picked_id(hype_sched_pick(&rq, 0)));
    CHECK_INT("stopped -> halted stays off the queue", 0,
              hype_sched_set_state(&rq, &v[0], HYPE_SCHED_HALTED, 10));
    CHECK_INT("still only v1", 1, picked_id(hype_sched_pick(&rq, 100)));
    CHECK_INT("explicit start", 0, hype_sched_set_state(&rq, &v[0], HYPE_SCHED_RUNNABLE, 150));
    CHECK_INT("started vCPU runs", 0, picked_id(hype_sched_pick(&rq, 200)));
    CHECK_INT("a never-picked vCPU has last_scheduled 0", 0, stranger.last_scheduled);
}

/* No globals: two pools run independently of each other. */
static void test_two_queues_share_nothing(void) {
    hype_sched_rq_t a, b;
    hype_sched_vcpu_t va[2], vb[1];

    setup(&a, va, 2);
    setup(&b, vb, 1);
    CHECK_INT("a picks 0", 0, picked_id(hype_sched_pick(&a, 0)));
    CHECK_INT("b picks its own 0", 1, hype_sched_pick(&b, 0) == &vb[0]);
    CHECK_INT("a picks 1", 1, picked_id(hype_sched_pick(&a, 100)));
    CHECK_INT("b is unaffected", 1, hype_sched_pick(&b, 100) == &vb[0]);
    CHECK_INT("a members", 2, a.members);
    CHECK_INT("b members", 1, b.members);
}

static void test_unlink_from_the_middle_and_tail(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[4];

    setup(&rq, v, 4);
    CHECK_INT("halt the middle", 0, hype_sched_set_state(&rq, &v[2], HYPE_SCHED_HALTED, 0));
    CHECK_INT("remove the tail", 0, hype_sched_remove(&rq, &v[3], 0));
    CHECK_INT("order skips both", 0, picked_id(hype_sched_pick(&rq, 0)));
    CHECK_INT("then 1", 1, picked_id(hype_sched_pick(&rq, 100)));
    CHECK_INT("then 0 again", 0, picked_id(hype_sched_pick(&rq, 200)));
    CHECK_INT("tail fixed up: a wake appends after 1", 1, hype_sched_wake(&rq, &v[2]));
    CHECK_INT("then 1", 1, picked_id(hype_sched_pick(&rq, 300)));
    CHECK_INT("woken middle runs last", 2, picked_id(hype_sched_pick(&rq, 400)));
}

int main(void) {
    test_unlink_from_the_middle_and_tail();
    test_fifo_round_robin_order();
    test_halted_is_skipped_and_readmitted_on_wake();
    test_current_halts_mid_slice();
    test_time_accounting_across_many_slices();
    test_clock_behind_slice_start_charges_nothing();
    test_single_entry_queue_is_the_dedicated_path();
    test_fairness_over_a_long_synthetic_run();
    test_add_and_remove_mid_run();
    test_state_transitions_and_refusals();
    test_two_queues_share_nothing();
    if (failures) {
        printf("test_sched: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_sched: all passed\n");
    return 0;
}
