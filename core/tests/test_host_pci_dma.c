#include <stdio.h>
#include "../host_pci_dma.h"

static int failures;

#define CHECK(desc, cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL: %s\n", (desc));                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_INT(desc, expected, actual)                                                          \
    do {                                                                                           \
        long long e_ = (long long)(expected), a_ = (long long)(actual);                            \
        if (e_ != a_) {                                                                            \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), e_, a_);                         \
            failures++;                                                                            \
        }                                                                                           \
    } while (0)

static void test_plain_ring(void) {
    CHECK_INT("advance wraps at capacity", 0, hype_dma_ring_advance(7u, 8u));
    CHECK_INT("advance mid-ring", 4, hype_dma_ring_advance(3u, 8u));
    CHECK_INT("advance on zero-capacity ring is 0", 0, hype_dma_ring_advance(5u, 0u));

    CHECK("empty ring (head==tail) is not full", !hype_dma_ring_full(2u, 2u, 8u));
    CHECK("one slot short of wrap is full", hype_dma_ring_full(0u, 7u, 8u));
    CHECK("a zero-capacity ring is always full", hype_dma_ring_full(0u, 0u, 0u));

    CHECK_INT("used with head==tail is 0", 0, hype_dma_ring_used(3u, 3u, 8u));
    CHECK_INT("used counts the gap", 5, hype_dma_ring_used(2u, 7u, 8u));
    CHECK_INT("used wraps correctly", 3, hype_dma_ring_used(6u, 1u, 8u));
    CHECK_INT("zero-capacity ring uses nothing", 0, hype_dma_ring_used(0u, 0u, 0u));
}

static void test_cqueue_phase(void) {
    unsigned int idx = 0, phase = 1;

    hype_dma_cqueue_advance(&idx, &phase, 4u);
    CHECK_INT("index advances", 1, idx);
    CHECK_INT("phase unchanged mid-ring", 1, phase);

    idx = 3;
    hype_dma_cqueue_advance(&idx, &phase, 4u);
    CHECK_INT("index wraps to 0", 0, idx);
    CHECK_INT("phase flips on wrap", 0, phase);

    hype_dma_cqueue_advance(&idx, &phase, 4u);
    hype_dma_cqueue_advance(&idx, &phase, 4u);
    hype_dma_cqueue_advance(&idx, &phase, 4u);
    hype_dma_cqueue_advance(&idx, &phase, 4u);
    CHECK_INT("index wraps a second time", 0, idx);
    CHECK_INT("phase flips back", 1, phase);

    /* zero-capacity is a no-op, not a crash or a spurious flip */
    idx = 0;
    phase = 1;
    hype_dma_cqueue_advance(&idx, &phase, 0u);
    CHECK_INT("zero-capacity leaves index alone", 0, idx);
    CHECK_INT("zero-capacity leaves phase alone", 1, phase);
}

static void test_link_ring(void) {
    unsigned int enq = 0, cyc = 1;
    unsigned int i;

    /* capacity 4: slots 0,1,2 hold data, slot 3 is the Link TRB. */
    for (i = 0; i < 2u; i++) {
        hype_dma_link_ring_advance(&enq, &cyc, 4u);
    }
    CHECK_INT("advances through data slots", 2, enq);
    CHECK_INT("cycle unchanged before the link slot", 1, cyc);

    hype_dma_link_ring_advance(&enq, &cyc, 4u);
    CHECK_INT("wraps to slot 0 at the link slot, not slot 3", 0, enq);
    CHECK_INT("cycle flips on wrap", 0, cyc);

    /* too small to hold even one data slot plus the link slot: no-op */
    enq = 0;
    cyc = 1;
    hype_dma_link_ring_advance(&enq, &cyc, 1u);
    CHECK_INT("capacity < 2 leaves enqueue alone", 0, enq);
    CHECK_INT("capacity < 2 leaves cycle alone", 1, cyc);
}

static void test_pool(void) {
    hype_dma_pool_t pool;
    int a, b, c;

    hype_dma_pool_init(&pool, 2u);
    CHECK_INT("empty pool has 0 used", 0, hype_dma_pool_used_count(&pool));

    a = hype_dma_pool_alloc(&pool);
    b = hype_dma_pool_alloc(&pool);
    c = hype_dma_pool_alloc(&pool);
    CHECK_INT("first alloc is slot 0", 0, a);
    CHECK_INT("second alloc is slot 1", 1, b);
    CHECK_INT("third alloc fails: pool exhausted", -1, c);
    CHECK_INT("used count is 2", 2, hype_dma_pool_used_count(&pool));

    hype_dma_pool_free(&pool, a);
    CHECK("slot 0 is now free", !hype_dma_pool_is_used(&pool, 0));
    CHECK("slot 1 is still used", hype_dma_pool_is_used(&pool, 1));

    c = hype_dma_pool_alloc(&pool);
    CHECK_INT("freed slot is reused", 0, c);

    /* a double-free or an out-of-range index must not corrupt a live slot */
    hype_dma_pool_free(&pool, 0); /* already used again by c -- freeing it here is the double-free */
    hype_dma_pool_free(&pool, -1);
    hype_dma_pool_free(&pool, 99);
    CHECK("out-of-range free left slot 1 untouched", hype_dma_pool_is_used(&pool, 1));

    {
        /* slot_count above 64 is clamped: observed by exhausting it -- the
         * 65th alloc must fail, which could not happen on an unclamped 200. */
        hype_dma_pool_t big;
        int i, last = -2;
        hype_dma_pool_init(&big, 200u);
        for (i = 0; i < 65; i++) {
            last = hype_dma_pool_alloc(&big);
        }
        CHECK_INT("65th alloc fails on a clamped-to-64 pool", -1, last);
    }
}

static void test_pool_null_guards(void) {
    /* Every pool entry point must tolerate a NULL pool rather than dereference it -- a caller
     * checking "did I even initialise this yet" should get a safe, inert answer, not a crash. */
    hype_dma_pool_init(0, 4u);
    CHECK_INT("alloc on a NULL pool refuses", -1, hype_dma_pool_alloc(0));
    hype_dma_pool_free(0, 0);
    CHECK("is_used on a NULL pool is false", !hype_dma_pool_is_used(0, 0));
    CHECK_INT("used_count on a NULL pool is zero", 0, hype_dma_pool_used_count(0));
}

static void test_ring_null_guards(void) {
    unsigned int idx = 1, phase = 1, enq = 1, cyc = 1;

    /* NULL out-param guards must leave nothing to crash on and touch no state. */
    hype_dma_cqueue_advance(0, &phase, 4u);
    hype_dma_cqueue_advance(&idx, 0, 4u);
    CHECK_INT("NULL index/phase args are a no-op", 1, idx);
    CHECK_INT("phase untouched by the no-op calls", 1, phase);

    hype_dma_link_ring_advance(0, &cyc, 4u);
    hype_dma_link_ring_advance(&enq, 0, 4u);
    CHECK_INT("NULL enqueue/cycle args are a no-op", 1, enq);
    CHECK_INT("cycle untouched by the no-op calls", 1, cyc);
}

int main(void) {
    test_plain_ring();
    test_cqueue_phase();
    test_link_ring();
    test_pool();
    test_pool_null_guards();
    test_ring_null_guards();

    if (failures) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
