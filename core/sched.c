#include "sched.h"

void hype_sched_rq_init(hype_sched_rq_t *rq, uint64_t slice_len, unsigned int threads) {
    unsigned int t;
    rq->head = 0;
    rq->tail = 0;
    for (t = 0; t < HYPE_SCHED_MAX_THREADS; t++) rq->current[t] = 0;
    if (threads == 0u) threads = 1u;
    if (threads > HYPE_SCHED_MAX_THREADS) threads = HYPE_SCHED_MAX_THREADS;
    rq->threads = threads;
    rq->slice_start = 0;
    rq->slice_len = slice_len;
    rq->members = 0;
    rq->has_owner = 0;
    rq->owner = 0;
    rq->has_last_owner = 0;
    rq->last_owner = 0;
    rq->owner_changed = 0;
    rq->group_switches = 0;
    rq->idle_quantised = 0;
    rq->idle_nothing = 0;
}

void hype_sched_vcpu_init(hype_sched_vcpu_t *v, unsigned int id, unsigned int group) {
    v->id = id;
    v->group = group;
    v->state = HYPE_SCHED_RUNNABLE;
    v->run_time = 0;
    v->last_scheduled = 0;
    v->slices = 0;
    v->run_start = 0;
    v->next = 0;
    v->queued = 0;
    v->member = 0;
    v->thread = -1;
}

static void fifo_push(hype_sched_rq_t *rq, hype_sched_vcpu_t *v) {
    v->next = 0;
    if (rq->tail != 0) {
        rq->tail->next = v;
    } else {
        rq->head = v;
    }
    rq->tail = v;
    v->queued = 1;
}

/* Unlink `v`, whose predecessor is `prev` (0 when v is the head). */
static void fifo_unlink_after(hype_sched_rq_t *rq, hype_sched_vcpu_t *prev, hype_sched_vcpu_t *v) {
    if (prev != 0) {
        prev->next = v->next;
    } else {
        rq->head = v->next;
    }
    if (rq->tail == v) rq->tail = prev;
    v->next = 0;
    v->queued = 0;
}

static void fifo_unlink(hype_sched_rq_t *rq, hype_sched_vcpu_t *v) {
    hype_sched_vcpu_t *prev = 0, *it = rq->head;
    /* Only called for a queued vCPU, so the walk always finds it. */
    while (it != v) {
        prev = it;
        it = it->next;
    }
    fifo_unlink_after(rq, prev, v);
}

/* A `now` behind run_start (a caller clock glitch) charges nothing rather than wrapping. */
static void charge_current(hype_sched_rq_t *rq, hype_sched_vcpu_t *v, uint64_t now) {
    if (now > v->run_start) v->run_time += now - v->run_start;
    rq->current[v->thread] = 0;
    v->thread = -1;
}

static void make_current(hype_sched_rq_t *rq, hype_sched_vcpu_t *v, unsigned int t, uint64_t now) {
    rq->current[t] = v;
    v->thread = (int)t;
    v->run_start = now;
    v->last_scheduled = now;
    v->slices++;
}

/* Take the first runnable FIFO entry of `group` and make it current on thread t. */
static hype_sched_vcpu_t *take_group(hype_sched_rq_t *rq, unsigned int group, unsigned int t,
                                     uint64_t now) {
    hype_sched_vcpu_t *prev = 0, *it = rq->head;
    while (it != 0 && it->group != group) {
        prev = it;
        it = it->next;
    }
    if (it == 0) return 0;
    fifo_unlink_after(rq, prev, it);
    make_current(rq, it, t, now);
    return it;
}

int hype_sched_add(hype_sched_rq_t *rq, hype_sched_vcpu_t *v) {
    if (v->member) return -1;
    v->member = 1;
    v->state = HYPE_SCHED_RUNNABLE;
    fifo_push(rq, v);
    rq->members++;
    return 0;
}

int hype_sched_remove(hype_sched_rq_t *rq, hype_sched_vcpu_t *v, uint64_t now) {
    if (!v->member) return -1;
    if (v->thread >= 0) {
        charge_current(rq, v, now);
    } else if (v->queued) {
        fifo_unlink(rq, v);
    }
    v->member = 0;
    rq->members--;
    return 0;
}

hype_sched_vcpu_t *hype_sched_pick(hype_sched_rq_t *rq, uint64_t now) {
    unsigned int t, filled = 0;
    unsigned int group;

    for (t = 0; t < rq->threads; t++) {
        hype_sched_vcpu_t *v = rq->current[t];
        if (v == 0) continue;
        charge_current(rq, v, now);
        fifo_push(rq, v);
    }
    rq->owner_changed = 0;
    rq->slice_start = now;
    if (rq->head == 0) {
        rq->has_owner = 0;
        rq->idle_nothing += rq->threads;
        return 0;
    }
    group = rq->head->group;
    for (t = 0; t < rq->threads; t++) {
        if (take_group(rq, group, t, now) == 0) break;
        filled++;
    }
    /* An empty sibling of an owned core is lost to decision 40: no other group may use it. */
    rq->idle_quantised += rq->threads - filled;
    if (rq->has_last_owner && rq->last_owner != group) {
        rq->owner_changed = 1;
        rq->group_switches++;
    }
    rq->has_owner = 1;
    rq->owner = group;
    rq->has_last_owner = 1;
    rq->last_owner = group;
    return rq->current[0];
}

hype_sched_vcpu_t *hype_sched_refill(hype_sched_rq_t *rq, unsigned int thread, uint64_t now) {
    if (thread >= rq->threads || rq->current[thread] != 0 || !rq->has_owner) return 0;
    return take_group(rq, rq->owner, thread, now);
}

hype_sched_vcpu_t *hype_sched_current(const hype_sched_rq_t *rq, unsigned int thread) {
    return (thread < rq->threads) ? rq->current[thread] : 0;
}

int hype_sched_slice_expired(const hype_sched_rq_t *rq, uint64_t now) {
    unsigned int t;
    int any = 0;
    for (t = 0; t < rq->threads; t++) {
        if (rq->current[t] != 0) any = 1;
    }
    if (!any) return 0;
    return now >= rq->slice_start && now - rq->slice_start >= rq->slice_len;
}

int hype_sched_set_state(hype_sched_rq_t *rq, hype_sched_vcpu_t *v, hype_sched_state_t state,
                         uint64_t now) {
    if (!v->member) return -1;
    if (v->state == state) return 0;
    if (v->state == HYPE_SCHED_RUNNABLE) {
        if (v->thread >= 0) {
            charge_current(rq, v, now);
        } else {
            fifo_unlink(rq, v);
        }
    }
    v->state = state;
    if (state == HYPE_SCHED_RUNNABLE) fifo_push(rq, v);
    return 0;
}

int hype_sched_wake(hype_sched_rq_t *rq, hype_sched_vcpu_t *v) {
    if (!v->member) return 0;
    if (v->state != HYPE_SCHED_HALTED && v->state != HYPE_SCHED_BLOCKED) return 0;
    v->state = HYPE_SCHED_RUNNABLE;
    fifo_push(rq, v);
    return 1;
}

/* a/b < c/d without division. */
static int load_less(unsigned int a, unsigned int b, unsigned int c, unsigned int d) {
    return (unsigned long long)a * d < (unsigned long long)c * b;
}

/* How many of the first `i` placed vCPUs sit on core c, and how many of those are in `group`. */
static void core_load(const unsigned int *group, unsigned int i, const unsigned int *out_core,
                      unsigned int c, unsigned int g, unsigned int *load, unsigned int *mates) {
    unsigned int j;
    *load = 0;
    *mates = 0;
    for (j = 0; j < i; j++) {
        if (out_core[j] != c) continue;
        (*load)++;
        if (group[j] == g) (*mates)++;
    }
}

void hype_sched_place(const unsigned int *group, unsigned int n, const unsigned int *threads,
                      unsigned int ncores, unsigned int *out_core) {
    unsigned int i, c;

    for (i = 0; i < n && ncores != 0u; i++) {
        int best = -1, mate = -1;
        unsigned int best_load = 0, best_t = 1, mate_load = 0, mate_t = 1;
        for (c = 0; c < ncores; c++) {
            unsigned int load, mates, tc = threads[c] ? threads[c] : 1u;
            core_load(group, i, out_core, c, group[i], &load, &mates);
            /* A group-mate with a free sibling: fill that core before spreading. */
            if (mates % tc != 0u && (mate < 0 || load_less(load, tc, mate_load, mate_t))) {
                mate = (int)c;
                mate_load = load;
                mate_t = tc;
            }
            if (best < 0 || load_less(load, tc, best_load, best_t)) {
                best = (int)c;
                best_load = load;
                best_t = tc;
            }
        }
        out_core[i] = (unsigned int)(mate >= 0 ? mate : best);
    }
}

int hype_sched_check(const hype_sched_rq_t *rq) {
    unsigned int t;
    for (t = 0; t < rq->threads; t++) {
        const hype_sched_vcpu_t *v = rq->current[t];
        if (v == 0) continue;
        if (!rq->has_owner || v->group != rq->owner) return -1;
        if (v->queued || v->state != HYPE_SCHED_RUNNABLE || v->thread != (int)t) return -1;
    }
    return 0;
}
