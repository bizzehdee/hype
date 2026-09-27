#ifndef HYPE_SCHED_H
#define HYPE_SCHED_H

#include <stdint.h>

/*
 * SMP-12 (#468) + SMP-17 (#473): the shared-tier scheduler core, as a pure module.
 *
 * One hype_sched_rq_t per shared-tier PHYSICAL CORE (plan.md §10 decision 85): a vCPU is placed
 * on one core's queue and never migrates. The core runs ONE trust group per slice (decision 40):
 * at each slice boundary the group of the vCPU at the head of the FIFO becomes the core's owner,
 * and up to `threads` runnable vCPUs of that group -- and only that group -- are made current,
 * one per hardware thread. So distrusting groups never share a core at the same time, while
 * group-mates freely share its SMT siblings. owner_changed tells the caller a flush (SMP-18) is
 * due before the first entry.
 *
 * There is no VMCB/VMCS, LAPIC or affinity here -- those are SMP-13 and SMP-14. Time is whatever
 * monotonic tick the caller passes as `now` (the TSC in hype).
 *
 * A dedicated vCPU is a one-thread queue of length one: pick-next always returns it while it is
 * runnable. All state lives in the caller's structs, so two pools share nothing.
 */

#define HYPE_SCHED_MAX_THREADS 4u

typedef enum {
    HYPE_SCHED_RUNNABLE = 0,
    HYPE_SCHED_HALTED,  /* HLT/MWAIT with no pending interrupt */
    HYPE_SCHED_BLOCKED, /* waiting on the host, e.g. a synchronous I/O */
    HYPE_SCHED_STOPPED  /* not running until explicitly started again */
} hype_sched_state_t;

typedef struct hype_sched_vcpu {
    unsigned int id;    /* the caller's handle; opaque to the scheduler */
    unsigned int group; /* trust group; the caller maps isolation_group names to numbers */
    hype_sched_state_t state;
    uint64_t run_time;       /* cumulative ticks spent current */
    uint64_t last_scheduled; /* `now` of the last time it became current; 0 = never */
    uint64_t slices;         /* how many times it became current */
    uint64_t run_start;      /* `now` when it last became current */
    uint64_t steal_time;     /* #477: cumulative ticks spent runnable in the FIFO, not running */
    uint64_t wait_max;       /* #476: the longest single FIFO wait before running -- the latency */
    uint64_t wait_start;     /* the queue clock when it last entered the FIFO */
    struct hype_sched_vcpu *next; /* runnable FIFO link */
    int queued;                   /* 1 while in the runnable FIFO */
    int member;                   /* 1 while added to a queue */
    int thread;                   /* hardware thread while current, else -1 */
} hype_sched_vcpu_t;

typedef struct {
    hype_sched_vcpu_t *head; /* runnable FIFO: head's group owns the next slice */
    hype_sched_vcpu_t *tail;
    hype_sched_vcpu_t *current[HYPE_SCHED_MAX_THREADS]; /* never in the FIFO; 0 = thread idle */
    unsigned int threads;
    uint64_t slice_start;
    uint64_t slice_len; /* ticks */
    unsigned int members;
    int has_owner;          /* 1 while a group owns the core for this slice */
    unsigned int owner;
    int has_last_owner;     /* the last group that ran here, kept across idle slices */
    unsigned int last_owner;
    int owner_changed;      /* the last pick handed the core to a different group */
    uint64_t group_switches; /* picks that set owner_changed */
    /* Thread-slices left empty by a pick. Quantised = an empty sibling of an OWNED core, which
     * no other group may use (decision 40's cost; isolation_group is the lever); nothing = the
     * whole core idled because nothing was runnable. */
    uint64_t idle_quantised;
    uint64_t idle_nothing;
    /* #477: the latest `now` any call has passed. Entries without a time of their own (add, wake)
     * start their FIFO wait from it, so steal time is charged from at most one call late. */
    uint64_t clock;
} hype_sched_rq_t;

/* #477: runnable vCPUs waiting in the FIFO (the run-queue depth, current excluded). */
unsigned int hype_sched_queued(const hype_sched_rq_t *rq);

/* threads is clamped to 1..HYPE_SCHED_MAX_THREADS. */
void hype_sched_rq_init(hype_sched_rq_t *rq, uint64_t slice_len, unsigned int threads);
void hype_sched_vcpu_init(hype_sched_vcpu_t *v, unsigned int id, unsigned int group);

/* Add a vCPU (as runnable, at the tail) or remove it. Return 0, or -1 if already a member /
 * not a member. Removing a current vCPU charges its run time up to `now` and idles its thread. */
int hype_sched_add(hype_sched_rq_t *rq, hype_sched_vcpu_t *v);
int hype_sched_remove(hype_sched_rq_t *rq, hype_sched_vcpu_t *v, uint64_t now);

/*
 * The slice boundary. Every current vCPU is charged and, if still runnable, goes to the tail in
 * thread order. The head's group then owns the core, and up to `threads` of its runnable vCPUs
 * become current in FIFO order. Returns current[0], or 0 when nothing is runnable (the core
 * idles until a wake). Every thread of the core must have left guest mode before this runs.
 */
hype_sched_vcpu_t *hype_sched_pick(hype_sched_rq_t *rq, uint64_t now);

/* A thread whose vCPU stopped running mid-slice takes the next runnable vCPU of the core's
 * owner group, if there is one. Returns it, or 0 (the thread idles until the next pick). */
hype_sched_vcpu_t *hype_sched_refill(hype_sched_rq_t *rq, unsigned int thread, uint64_t now);

/* The vCPU on `thread`, or 0. */
hype_sched_vcpu_t *hype_sched_current(const hype_sched_rq_t *rq, unsigned int thread);

/* 1 when a vCPU is current and the slice is used up at `now`. 0 when the core is idle. */
int hype_sched_slice_expired(const hype_sched_rq_t *rq, uint64_t now);

/*
 * Move a member to `state`. Leaving RUNNABLE takes it out of the FIFO; if it is current, its
 * run time is charged up to `now` and its thread idles. Entering RUNNABLE puts it at the tail.
 * Returns -1 for a non-member.
 */
int hype_sched_set_state(hype_sched_rq_t *rq, hype_sched_vcpu_t *v, hype_sched_state_t state,
                         uint64_t now);

/* An interrupt arrived: a HALTED or BLOCKED member becomes runnable. Returns 1 if it woke,
 * 0 otherwise (already runnable, or STOPPED -- a stopped vCPU needs an explicit start). */
int hype_sched_wake(hype_sched_rq_t *rq, hype_sched_vcpu_t *v);

/*
 * SMP-17 placement: assign n vCPUs (group[i] each) to ncores cores with threads[c] threads.
 * Static, since nothing migrates (decision 85). A vCPU goes to a core already holding a
 * group-mate with a free sibling thread, so a group fills a core before it spreads; otherwise
 * to the least-loaded core (vCPUs per thread), lowest index on a tie. out_core[i] receives the
 * core index. ncores == 0 writes nothing.
 */
void hype_sched_place(const unsigned int *group, unsigned int n, const unsigned int *threads,
                      unsigned int ncores, unsigned int *out_core);

/* The decision 40 invariants, for tests and debug builds: every current vCPU is runnable, not
 * queued, on its own thread, and of the owner group. Returns 0 when they hold. */
int hype_sched_check(const hype_sched_rq_t *rq);

#endif
