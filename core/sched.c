#include "sched.h"

void hype_sched_rq_init(hype_sched_rq_t *rq, uint64_t slice_len) {
    rq->head = 0;
    rq->tail = 0;
    rq->current = 0;
    rq->slice_start = 0;
    rq->slice_len = slice_len;
    rq->members = 0;
}

void hype_sched_vcpu_init(hype_sched_vcpu_t *v, unsigned int id) {
    v->id = id;
    v->state = HYPE_SCHED_RUNNABLE;
    v->run_time = 0;
    v->last_scheduled = 0;
    v->slices = 0;
    v->next = 0;
    v->queued = 0;
    v->member = 0;
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

static hype_sched_vcpu_t *fifo_pop(hype_sched_rq_t *rq) {
    hype_sched_vcpu_t *v = rq->head;
    if (v == 0) return 0;
    rq->head = v->next;
    if (rq->head == 0) rq->tail = 0;
    v->next = 0;
    v->queued = 0;
    return v;
}

static void fifo_unlink(hype_sched_rq_t *rq, hype_sched_vcpu_t *v) {
    hype_sched_vcpu_t *prev = 0, *it = rq->head;
    /* Only called for a queued vCPU, so the walk always finds it. */
    while (it != v) {
        prev = it;
        it = it->next;
    }
    if (prev != 0) {
        prev->next = v->next;
    } else {
        rq->head = v->next;
    }
    if (rq->tail == v) rq->tail = prev;
    v->next = 0;
    v->queued = 0;
}

/* A `now` behind slice_start (a caller clock glitch) charges nothing rather than wrapping. */
static void charge_current(hype_sched_rq_t *rq, uint64_t now) {
    hype_sched_vcpu_t *cur = rq->current;
    if (now > rq->slice_start) cur->run_time += now - rq->slice_start;
    rq->current = 0;
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
    if (rq->current == v) {
        charge_current(rq, now);
    } else if (v->queued) {
        fifo_unlink(rq, v);
    }
    v->member = 0;
    rq->members--;
    return 0;
}

hype_sched_vcpu_t *hype_sched_pick(hype_sched_rq_t *rq, uint64_t now) {
    hype_sched_vcpu_t *next;

    if (rq->current != 0) {
        hype_sched_vcpu_t *prev = rq->current;
        charge_current(rq, now);
        fifo_push(rq, prev);
    }
    next = fifo_pop(rq);
    if (next == 0) return 0;
    rq->current = next;
    rq->slice_start = now;
    next->last_scheduled = now;
    next->slices++;
    return next;
}

int hype_sched_slice_expired(const hype_sched_rq_t *rq, uint64_t now) {
    if (rq->current == 0) return 0;
    return now >= rq->slice_start && now - rq->slice_start >= rq->slice_len;
}

int hype_sched_set_state(hype_sched_rq_t *rq, hype_sched_vcpu_t *v, hype_sched_state_t state,
                         uint64_t now) {
    if (!v->member) return -1;
    if (v->state == state) return 0;
    if (v->state == HYPE_SCHED_RUNNABLE) {
        if (rq->current == v) {
            charge_current(rq, now);
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
