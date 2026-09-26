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
    hype_sched_rq_init(rq, SLICE, 1);
    for (i = 0; i < n; i++) {
        hype_sched_vcpu_init(&v[i], i, 0);
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
    CHECK_INT("queue idle after current halted", 0, rq.current[0] == 0 ? 0 : 1);
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
    CHECK_INT("never idle while runnable", 1, rq.current[0] == &v[0]);
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
        if ((i % 7u) == 3u && rq.current[0] != 0) {
            hype_sched_vcpu_t *c = rq.current[0];
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
    hype_sched_vcpu_init(&v[3], 3, 0);
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
    CHECK_INT("queue idle after removing current", 1, rq.current[0] == 0);
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
    hype_sched_vcpu_init(&stranger, 9, 0);
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

/* ---- #473 (SMP-17): trust groups ---- */
static void test_473_same_group_shares_siblings(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[2];

    hype_sched_rq_init(&rq, SLICE, 2);
    hype_sched_vcpu_init(&v[0], 0, 7);
    hype_sched_vcpu_init(&v[1], 1, 7);
    hype_sched_add(&rq, &v[0]);
    hype_sched_add(&rq, &v[1]);
    hype_sched_pick(&rq, 0);
    CHECK_INT("473 group-mates both current", 1,
              hype_sched_current(&rq, 0) == &v[0] && hype_sched_current(&rq, 1) == &v[1]);
    CHECK_INT("473 invariants hold", 0, hype_sched_check(&rq));
    CHECK_INT("473 no quantisation within a group", 0, rq.idle_quantised);
    hype_sched_pick(&rq, 100);
    CHECK_INT("473 same group again: no owner change", 0, rq.owner_changed);
}

static void test_473_cross_group_never_shares_a_core(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[3];

    hype_sched_rq_init(&rq, SLICE, 2);
    hype_sched_vcpu_init(&v[0], 0, 1);
    hype_sched_vcpu_init(&v[1], 1, 2);
    hype_sched_vcpu_init(&v[2], 2, 1);
    hype_sched_add(&rq, &v[0]);
    hype_sched_add(&rq, &v[1]);
    hype_sched_add(&rq, &v[2]);
    hype_sched_pick(&rq, 0);
    CHECK_INT("473 group 1 owns the core", 1, rq.owner);
    CHECK_INT("473 both group-1 vCPUs run, skipping group 2", 1,
              hype_sched_current(&rq, 0) == &v[0] && hype_sched_current(&rq, 1) == &v[2]);
    CHECK_INT("473 first owner is not a change", 0, rq.owner_changed);
    hype_sched_pick(&rq, 100);
    CHECK_INT("473 group 2's turn", 2, rq.owner);
    CHECK_INT("473 handing the core to group 2 is a change (flush due)", 1, rq.owner_changed);
    CHECK_INT("473 group 2 alone on the core", 1,
              hype_sched_current(&rq, 0) == &v[1] && hype_sched_current(&rq, 1) == 0);
    CHECK_INT("473 its idle sibling is quantised", 1, rq.idle_quantised);
    CHECK_INT("473 invariants hold", 0, hype_sched_check(&rq));
    CHECK_INT("473 refill never takes the other group", 1, hype_sched_refill(&rq, 1, 150) == 0);
    hype_sched_pick(&rq, 200);
    CHECK_INT("473 back to group 1", 1, rq.owner_changed && rq.owner == 1u);
    CHECK_INT("473 two group switches counted", 2, rq.group_switches);
}

static void test_473_refill_takes_a_group_mate(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[3];

    hype_sched_rq_init(&rq, SLICE, 2);
    hype_sched_vcpu_init(&v[0], 0, 4);
    hype_sched_vcpu_init(&v[1], 1, 4);
    hype_sched_vcpu_init(&v[2], 2, 4);
    hype_sched_add(&rq, &v[0]);
    hype_sched_add(&rq, &v[1]);
    hype_sched_add(&rq, &v[2]);
    hype_sched_pick(&rq, 0);
    hype_sched_set_state(&rq, &v[1], HYPE_SCHED_HALTED, 40);
    CHECK_INT("473 halted vCPU left thread 1", 1, hype_sched_current(&rq, 1) == 0);
    CHECK_INT("473 refill of a busy thread does nothing", 1, hype_sched_refill(&rq, 0, 40) == 0);
    CHECK_INT("473 refill of a bad thread does nothing", 1, hype_sched_refill(&rq, 9, 40) == 0);
    CHECK_INT("473 refill takes the waiting group-mate", 1, hype_sched_refill(&rq, 1, 40) == &v[2]);
    CHECK_INT("473 invariants hold", 0, hype_sched_check(&rq));
    hype_sched_pick(&rq, 100);
    CHECK_INT("473 refilled vCPU charged from its own start", 60, v[2].run_time);
    hype_sched_set_state(&rq, &v[0], HYPE_SCHED_STOPPED, 110);
    hype_sched_set_state(&rq, &v[2], HYPE_SCHED_STOPPED, 110);
    hype_sched_pick(&rq, 200);
    CHECK_INT("473 nothing runnable: core idles", 1, rq.has_owner == 0);
    CHECK_INT("473 refill on an unowned core does nothing", 1, hype_sched_refill(&rq, 0, 200) == 0);
    CHECK_INT("473 whole-core idle is not quantisation", 1, rq.idle_nothing >= 2u);
    CHECK_INT("473 an unowned core passes the check", 0, hype_sched_check(&rq));
    CHECK_INT("473 current() out of range", 1, hype_sched_current(&rq, 9) == 0);
}

/* Every ordering the scheduler can produce: a long deterministic random walk over every
 * operation, checking decision 40's invariant after each step. */
static void test_473_invariant_under_random_orderings(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[6];
    unsigned int groups[6] = { 1, 1, 2, 3, 3, 3 };
    uint64_t now = 1;
    uint32_t x = 12345u;
    unsigned int i, bad = 0, cross = 0;

    hype_sched_rq_init(&rq, SLICE, 3);
    for (i = 0; i < 6u; i++) {
        hype_sched_vcpu_init(&v[i], i, groups[i]);
        hype_sched_add(&rq, &v[i]);
    }
    for (i = 0; i < 200000u; i++) {
        unsigned int op, k, t, a, b;
        x = x * 1103515245u + 12345u;
        op = (x >> 16) % 6u;
        k = (x >> 8) % 6u;
        now += (x >> 4) % 50u;
        switch (op) {
        case 0: hype_sched_pick(&rq, now); break;
        case 1: hype_sched_set_state(&rq, &v[k], HYPE_SCHED_HALTED, now); break;
        case 2: hype_sched_wake(&rq, &v[k]); break;
        case 3: hype_sched_refill(&rq, k % 3u, now); break;
        case 4: hype_sched_set_state(&rq, &v[k], HYPE_SCHED_RUNNABLE, now); break;
        default:
            if (v[k].member) hype_sched_remove(&rq, &v[k], now);
            else hype_sched_add(&rq, &v[k]);
            break;
        }
        if (hype_sched_check(&rq) != 0) bad++;
        for (a = 0; a < 3u; a++) {
            for (b = a + 1u; b < 3u; b++) {
                hype_sched_vcpu_t *p = rq.current[a], *q = rq.current[b];
                if (p && q && p->group != q->group) cross++;
            }
        }
        for (t = 0; t < 3u; t++) {
            if (rq.current[t] && rq.current[t]->thread != (int)t) bad++;
        }
    }
    CHECK_INT("473 invariants held at every step", 0, bad);
    CHECK_INT("473 two groups never co-resident on the core", 0, cross);
    CHECK_INT("473 groups did take turns", 1, rq.group_switches > 100u);
}

static void test_473_check_catches_a_violation(void) {
    hype_sched_rq_t rq;
    hype_sched_vcpu_t v[2];

    hype_sched_rq_init(&rq, SLICE, 2);
    hype_sched_vcpu_init(&v[0], 0, 1);
    hype_sched_vcpu_init(&v[1], 1, 2);
    hype_sched_add(&rq, &v[0]);
    hype_sched_add(&rq, &v[1]);
    hype_sched_pick(&rq, 0);
    CHECK_INT("473 clean", 0, hype_sched_check(&rq));
    rq.current[1] = &v[1]; /* forge a cross-group co-residency */
    v[1].thread = 1;
    CHECK_INT("473 forged cross-group is caught", -1, hype_sched_check(&rq));
    v[1].group = 1;
    CHECK_INT("473 forged queued current is caught", -1, hype_sched_check(&rq));
    rq.current[1] = 0;
    rq.has_owner = 0;
    CHECK_INT("473 current on an unowned core is caught", -1, hype_sched_check(&rq));
    rq.has_owner = 1;
    v[0].state = HYPE_SCHED_HALTED;
    CHECK_INT("473 non-runnable current is caught", -1, hype_sched_check(&rq));
}

static void test_473_placement(void) {
    unsigned int threads2[2] = { 2, 2 };
    unsigned int threads3[3] = { 1, 1, 1 };
    unsigned int out[8];
    unsigned int one_group[2] = { 5, 5 };
    unsigned int two_groups[2] = { 5, 6 };
    unsigned int mix[5] = { 1, 2, 1, 2, 1 };
    unsigned int uneven[1] = { 0 };

    hype_sched_place(one_group, 2, threads2, 2, out);
    CHECK_INT("473 one group fills one core's siblings", 1, out[0] == 0u && out[1] == 0u);
    hype_sched_place(two_groups, 2, threads2, 2, out);
    CHECK_INT("473 two groups spread to two cores", 1, out[0] == 0u && out[1] == 1u);
    hype_sched_place(mix, 5, threads2, 2, out);
    CHECK_INT("473 mix: g1 core0, g2 core1, g1 mate core0, g2 mate core1, g1 spreads", 1,
              out[0] == 0u && out[1] == 1u && out[2] == 0u && out[3] == 1u);
    CHECK_INT("473 mix: fifth to the least-loaded (tie -> core 0)", 0, out[4]);
    hype_sched_place(mix, 5, threads3, 3, out);
    CHECK_INT("473 single-thread cores: round-robin spread", 1,
              out[0] == 0u && out[1] == 1u && out[2] == 2u && out[3] == 0u && out[4] == 1u);
    out[0] = 99;
    hype_sched_place(uneven, 1, threads2, 0, out);
    CHECK_INT("473 no cores writes nothing", 99, out[0]);
    {
        unsigned int zero_t[1] = { 0 };
        hype_sched_place(one_group, 2, zero_t, 1, out);
        CHECK_INT("473 a 0-thread entry is read as 1", 1, out[0] == 0u && out[1] == 0u);
    }
}

static void test_473_rq_init_clamps_threads(void) {
    hype_sched_rq_t rq;
    hype_sched_rq_init(&rq, SLICE, 0);
    CHECK_INT("473 0 threads -> 1", 1, rq.threads);
    hype_sched_rq_init(&rq, SLICE, 99);
    CHECK_INT("473 too many threads -> max", HYPE_SCHED_MAX_THREADS, rq.threads);
}

int main(void) {
    test_473_same_group_shares_siblings();
    test_473_cross_group_never_shares_a_core();
    test_473_refill_takes_a_group_mate();
    test_473_invariant_under_random_orderings();
    test_473_check_catches_a_violation();
    test_473_placement();
    test_473_rq_init_clamps_threads();
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
