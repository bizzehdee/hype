#include "int_in_queue.h"

void hype_iiq_reset(hype_iiq_t *q) {
    unsigned int i;
    if (q == (hype_iiq_t *)0) return;
    for (i = 0; i < HYPE_INT_IN_DEPTH; i++) {
        q->state[i] = (uint8_t)HYPE_IIQ_FREE;
        q->generation[i] = 0;
        q->pend_trb[i] = 0;
        q->pend_buf[i] = 0;
        q->pend_req[i] = 0;
        q->pend_gen[i] = 0;
    }
    q->inflight = 0;
    q->done_n = 0;
    q->lost = 0;
    q->skipped = 0;
    q->gen_faults = 0;
}

unsigned int hype_iiq_reserve(const hype_iiq_t *q) {
    unsigned int b;
    if (q == (const hype_iiq_t *)0) return HYPE_INT_IN_DEPTH;
    for (b = 0; b < HYPE_INT_IN_DEPTH; b++) {
        if (q->state[b] == (uint8_t)HYPE_IIQ_FREE) return b;
    }
    return HYPE_INT_IN_DEPTH;
}

int hype_iiq_commit(hype_iiq_t *q, unsigned int buf, uint64_t trb, unsigned int requested) {
    if (q == (hype_iiq_t *)0 || buf >= HYPE_INT_IN_DEPTH) return 0;
    if (q->state[buf] != (uint8_t)HYPE_IIQ_FREE) return 0;
    if (q->inflight >= HYPE_INT_IN_DEPTH) return 0;

    /* The generation moves when the buffer is handed to hardware, so a completion carrying
     * an older one is a buffer that was re-armed while its report was still queued. */
    q->generation[buf]++;
    q->state[buf] = (uint8_t)HYPE_IIQ_INFLIGHT;
    q->pend_trb[q->inflight] = trb;
    q->pend_buf[q->inflight] = buf;
    q->pend_req[q->inflight] = requested;
    q->pend_gen[q->inflight] = q->generation[buf];
    q->inflight++;
    return 1;
}

/* Move the oldest outstanding transfer onto the completion queue. */
static void iiq_retire_head(hype_iiq_t *q, uint32_t cc, unsigned int residue) {
    unsigned int i;
    unsigned int buf = q->pend_buf[0];
    unsigned int req = q->pend_req[0];
    unsigned int res = (residue <= req) ? residue : req; /* a malformed event must not
                                                          * underflow into a huge length */

    if (q->done_n < HYPE_INT_IN_DEPTH) {
        q->done[q->done_n].trb = q->pend_trb[0];
        q->done[q->done_n].buf = buf;
        q->done[q->done_n].requested = req;
        q->done[q->done_n].residue = res;
        q->done[q->done_n].actual = req - res;
        q->done[q->done_n].generation = q->pend_gen[0];
        q->done[q->done_n].cc = cc;
        q->done_n++;
        q->state[buf] = (uint8_t)HYPE_IIQ_COMPLETED;
    } else {
        /* Unreachable: inflight + done_n <= DEPTH, so a retire always has room. */
        q->state[buf] = (uint8_t)HYPE_IIQ_FREE;
    }
    for (i = 1; i < q->inflight; i++) {
        q->pend_trb[i - 1u] = q->pend_trb[i];
        q->pend_buf[i - 1u] = q->pend_buf[i];
        q->pend_req[i - 1u] = q->pend_req[i];
        q->pend_gen[i - 1u] = q->pend_gen[i];
    }
    q->inflight--;
}

int hype_iiq_deliver(hype_iiq_t *q, uint64_t trb, uint32_t cc, unsigned int residue) {
    unsigned int k;

    if (q == (hype_iiq_t *)0) return 0;
    for (k = 0; k < q->inflight; k++) {
        if (q->pend_trb[k] == trb) break;
    }
    if (k >= q->inflight) {
        q->lost++;
        return 0;
    }
    while (k-- != 0u) {
        /* Completed, but its event was never seen. Deliver the report anyway: the residue is
         * unknown, so the full TRB length is assumed, and the buffer was zeroed at arm. */
        iiq_retire_head(q, HYPE_IIQ_CC_SUCCESS, 0u);
        q->skipped++;
    }
    iiq_retire_head(q, cc, residue);
    return 1;
}

int hype_iiq_take(hype_iiq_t *q, hype_iiq_completion_t *out) {
    unsigned int i, buf;

    if (q == (hype_iiq_t *)0 || out == (hype_iiq_completion_t *)0) return 0;
    if (q->done_n == 0u) return 0;

    *out = q->done[0];
    buf = out->buf;
    if (q->generation[buf] != out->generation) {
        q->gen_faults++; /* the buffer was re-armed under this completion */
    }
    for (i = 1; i < q->done_n; i++) q->done[i - 1u] = q->done[i];
    q->done_n--;
    if (q->state[buf] == (uint8_t)HYPE_IIQ_COMPLETED) {
        q->state[buf] = (uint8_t)HYPE_IIQ_FREE;
    }
    return 1;
}

void hype_iiq_census(const hype_iiq_t *q, unsigned int *free_n, unsigned int *inflight_n,
                     unsigned int *completed_n) {
    unsigned int b, f = 0, i = 0, c = 0;
    if (q != (const hype_iiq_t *)0) {
        for (b = 0; b < HYPE_INT_IN_DEPTH; b++) {
            if (q->state[b] == (uint8_t)HYPE_IIQ_FREE) f++;
            else if (q->state[b] == (uint8_t)HYPE_IIQ_INFLIGHT) i++;
            else c++;
        }
    }
    if (free_n) *free_n = f;
    if (inflight_n) *inflight_n = i;
    if (completed_n) *completed_n = c;
}

int hype_iiq_check(const hype_iiq_t *q) {
    unsigned int f = 0, i = 0, c = 0, k;

    if (q == (const hype_iiq_t *)0) return 0;
    hype_iiq_census(q, &f, &i, &c);
    if (f + i + c != HYPE_INT_IN_DEPTH) return 0;
    if (i != q->inflight) return 0;
    if (c != q->done_n) return 0;
    if (q->inflight + q->done_n > HYPE_INT_IN_DEPTH) return 0;

    /* Every outstanding transfer holds an INFLIGHT buffer, and no two hold the same one. */
    for (k = 0; k < q->inflight; k++) {
        unsigned int j;
        if (q->pend_buf[k] >= HYPE_INT_IN_DEPTH) return 0;
        if (q->state[q->pend_buf[k]] != (uint8_t)HYPE_IIQ_INFLIGHT) return 0;
        for (j = k + 1u; j < q->inflight; j++) {
            if (q->pend_buf[j] == q->pend_buf[k]) return 0;
        }
    }
    /* Every queued completion holds a COMPLETED buffer, and no two hold the same one. */
    for (k = 0; k < q->done_n; k++) {
        unsigned int j;
        if (q->done[k].buf >= HYPE_INT_IN_DEPTH) return 0;
        if (q->state[q->done[k].buf] != (uint8_t)HYPE_IIQ_COMPLETED) return 0;
        for (j = k + 1u; j < q->done_n; j++) {
            if (q->done[j].buf == q->done[k].buf) return 0;
        }
    }
    return 1;
}
