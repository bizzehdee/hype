#ifndef HYPE_INT_IN_QUEUE_H
#define HYPE_INT_IN_QUEUE_H

#include <stdint.h>

/*
 * The ownership state machine for an interrupt-IN endpoint's transfers and report buffers.
 *
 * Extracted from the xHCI driver so it can be tested without an xHC. It is the code the bug
 * lived in twice: first as a single completion slot against four outstanding transfers, then
 * as a report buffer handed back to the controller while a queued completion still held it.
 * Both were invisible to the unit tests because none of it was reachable from one.
 *
 * There is no xHCI in this file. It knows about TRB addresses only as opaque identities.
 *
 * A buffer is in exactly one state, and the invariant hype_iiq_check() enforces is that the
 * three populations add up:
 *
 *     FREE  --commit-->  INFLIGHT  --deliver-->  COMPLETED  --take-->  FREE
 *
 * A buffer is DMA-visible only while INFLIGHT. That is the property the second bug broke:
 * `armed` had dropped, so the buffer looked reusable, but its report had not been read yet.
 */

#define HYPE_INT_IN_DEPTH 4u

/*
 * Mirrors xHCI completion code 1 (Success). Defined here rather than included so this module
 * stays free of the register layer; the value is fixed by the specification, not by hype.
 */
#define HYPE_IIQ_CC_SUCCESS 1u

typedef enum {
    HYPE_IIQ_FREE = 0,      /* nothing owns it */
    HYPE_IIQ_INFLIGHT = 1,  /* handed to the controller; DMA may write it at any moment */
    HYPE_IIQ_COMPLETED = 2  /* holds a report nobody has read yet -- NOT reusable */
} hype_iiq_buf_state_t;

typedef struct {
    uint64_t trb;           /* which hardware request completed */
    unsigned int buf;       /* where its data landed -- a separate identity from the TRB */
    unsigned int requested; /* TRB Transfer Length armed */
    unsigned int residue;   /* xHCI 6.4.2.1: on IN, what the device did NOT send */
    unsigned int actual;    /* requested - residue, clamped */
    unsigned int generation;/* the buffer's generation when it was armed */
    uint32_t cc;            /* completion code */
} hype_iiq_completion_t;

typedef struct {
    uint8_t state[HYPE_INT_IN_DEPTH];
    unsigned int generation[HYPE_INT_IN_DEPTH];

    /* Outstanding transfers, oldest first. An interrupt ring completes in order. */
    uint64_t pend_trb[HYPE_INT_IN_DEPTH];
    unsigned int pend_buf[HYPE_INT_IN_DEPTH];
    unsigned int pend_req[HYPE_INT_IN_DEPTH];
    unsigned int pend_gen[HYPE_INT_IN_DEPTH];
    unsigned int inflight;

    /* Completions waiting to be read, oldest first. */
    hype_iiq_completion_t done[HYPE_INT_IN_DEPTH];
    unsigned int done_n;

    /*
     * lost     a completion naming a TRB this endpoint does not hold outstanding -- stale or
     *          duplicate, correctly refused, retiring nothing.
     * skipped  a transfer retired because a LATER one completed, meaning its own event was
     *          never seen. Its report is still delivered.
     * gen_faults  a completed buffer whose generation moved while it sat in the queue, i.e.
     *          it was re-armed under a queued completion. Impossible by construction now;
     *          counted so that stays true rather than being assumed.
     */
    unsigned long long lost;
    unsigned long long skipped;
    unsigned long long gen_faults;
} hype_iiq_t;

void hype_iiq_reset(hype_iiq_t *q);

/* A buffer nothing owns, or HYPE_INT_IN_DEPTH when every one is spoken for. Reserves
 * nothing: the caller needs the index to build the TRB before it can commit. */
unsigned int hype_iiq_reserve(const hype_iiq_t *q);

/* Record a transfer just handed to the controller. `buf` must have come from
 * hype_iiq_reserve(). Returns 0 if the queue is full or the buffer is not free. */
int hype_iiq_commit(hype_iiq_t *q, unsigned int buf, uint64_t trb, unsigned int requested);

/*
 * Take a completion, retiring against it immediately.
 *
 * Matches the whole outstanding set, not just the head: an in-order match at index k proves
 * 0..k-1 completed too, so they are retired with it and their reports queued rather than
 * dropped -- a HID boot report is a state snapshot, so an intermediate one carries a
 * transition that is lost with it.
 *
 * Returns 1 if it belonged here, 0 if it matched nothing (counted in `lost`).
 */
int hype_iiq_deliver(hype_iiq_t *q, uint64_t trb, uint32_t cc, unsigned int residue);

/* Pop the oldest completion and free its buffer. Returns 1, or 0 when the queue is empty. */
int hype_iiq_take(hype_iiq_t *q, hype_iiq_completion_t *out);

/* How many buffers are in each state; any may be NULL. */
void hype_iiq_census(const hype_iiq_t *q, unsigned int *free_n, unsigned int *inflight_n,
                     unsigned int *completed_n);

/* 1 when every invariant holds. Cheap enough to assert on in a test, not on the hot path. */
int hype_iiq_check(const hype_iiq_t *q);

#endif /* HYPE_INT_IN_QUEUE_H */
