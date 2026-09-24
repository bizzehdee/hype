#ifndef HYPE_SCHED_H
#define HYPE_SCHED_H

#include <stdint.h>

/*
 * SMP-12 (#468): the shared-tier scheduler core, as a pure module.
 *
 * One hype_sched_rq_t per shared-tier hardware thread (plan.md §10 decision 85): a vCPU is
 * placed on one queue and never migrates. Pick-next is round-robin with a fixed slice. There is
 * no VMCB/VMCS, LAPIC or affinity here -- those are SMP-13 and SMP-14. Time is whatever
 * monotonic tick the caller passes as `now` (the TSC in hype).
 *
 * A dedicated vCPU is a queue of length one: pick-next always returns it while it is runnable.
 *
 * All state lives in the caller's rq and vcpu structs, so two pools share nothing.
 */

typedef enum {
    HYPE_SCHED_RUNNABLE = 0,
    HYPE_SCHED_HALTED,  /* HLT/MWAIT with no pending interrupt */
    HYPE_SCHED_BLOCKED, /* waiting on the host, e.g. a synchronous I/O */
    HYPE_SCHED_STOPPED  /* not running until explicitly started again */
} hype_sched_state_t;

typedef struct hype_sched_vcpu {
    unsigned int id; /* the caller's handle; opaque to the scheduler */
    hype_sched_state_t state;
    uint64_t run_time;       /* cumulative ticks spent as `current` */
    uint64_t last_scheduled; /* `now` of the last pick; 0 = never picked */
    uint64_t slices;         /* how many times it has been picked */
    struct hype_sched_vcpu *next; /* runnable FIFO link */
    int queued;                   /* 1 while in the runnable FIFO */
    int member;                   /* 1 while added to a queue */
} hype_sched_vcpu_t;

typedef struct {
    hype_sched_vcpu_t *head; /* runnable FIFO: head runs next */
    hype_sched_vcpu_t *tail;
    hype_sched_vcpu_t *current; /* running now; never in the FIFO; 0 = idle */
    uint64_t slice_start;
    uint64_t slice_len; /* ticks */
    unsigned int members;
} hype_sched_rq_t;

void hype_sched_rq_init(hype_sched_rq_t *rq, uint64_t slice_len);
void hype_sched_vcpu_init(hype_sched_vcpu_t *v, unsigned int id);

/* Add a vCPU (as runnable, at the tail) or remove it. Return 0, or -1 if already a member /
 * not a member. Removing `current` charges its run time up to `now` and leaves the queue idle. */
int hype_sched_add(hype_sched_rq_t *rq, hype_sched_vcpu_t *v);
int hype_sched_remove(hype_sched_rq_t *rq, hype_sched_vcpu_t *v, uint64_t now);

/*
 * The slice boundary, or the current vCPU stopped running. Charges `current` its run time; a
 * still-runnable `current` goes to the tail. Then the head becomes `current`. Returns it, or 0
 * when nothing is runnable (the pCPU idles until a wake).
 */
hype_sched_vcpu_t *hype_sched_pick(hype_sched_rq_t *rq, uint64_t now);

/* 1 when `current` has used its whole slice at `now`. 0 when idle. */
int hype_sched_slice_expired(const hype_sched_rq_t *rq, uint64_t now);

/*
 * Move a member to `state`. Leaving RUNNABLE takes it out of the FIFO; if it is `current`, its
 * run time is charged up to `now` and the queue goes idle until the next pick. Entering
 * RUNNABLE puts it at the tail. Returns -1 for a non-member.
 */
int hype_sched_set_state(hype_sched_rq_t *rq, hype_sched_vcpu_t *v, hype_sched_state_t state,
                         uint64_t now);

/* An interrupt arrived: a HALTED or BLOCKED member becomes runnable. Returns 1 if it woke,
 * 0 otherwise (already runnable, or STOPPED -- a stopped vCPU needs an explicit start). */
int hype_sched_wake(hype_sched_rq_t *rq, hype_sched_vcpu_t *v);

#endif
