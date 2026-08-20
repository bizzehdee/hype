#include <stdio.h>
#include <string.h>
#include "../e1000_dev_ring.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK_TRUE(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)

#define RING 8u

/*
 * map == 0 means "identity-mapped guest", so host addresses ARE guest-physical ones and the walk is
 * testable on the host -- the same technique test_virtio_net_ring.c uses, and the reason this code is
 * in core/ rather than in an arch backend.
 */
typedef struct {
    uint8_t tx_ring[RING * HYPE_E1000_DESC_BYTES];
    uint8_t rx_ring[RING * HYPE_E1000_DESC_BYTES];
    uint8_t buf[RING][HYPE_E1000_BUF_BYTES];
    uint8_t scratch[HYPE_VIRTIO_NET_MAX_FRAME_LEN];
    hype_e1000_dev_t dev;
    hype_virtio_net_ring_stats_t stats;
} rig_t;

static const uint8_t MAC[6] = {0x52, 0x54, 0x00, 0x77, 0x88, 0x99};

static uint8_t sink_frame[HYPE_VIRTIO_NET_MAX_FRAME_LEN];
static unsigned int sink_len;
static unsigned int sink_calls;
static int sink_result;

static int sink(void *user, const uint8_t *frame, unsigned int len) {
    (void)user;
    sink_calls++;
    sink_len = len;
    if (len <= sizeof(sink_frame)) {
        memcpy(sink_frame, frame, len);
    }
    return sink_result;
}

static void put64(uint8_t *p, uint64_t v) {
    unsigned int i;
    for (i = 0; i < 8u; i++) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void rig_init(rig_t *r) {
    memset(r, 0, sizeof(*r));
    sink_calls = 0;
    sink_len = 0;
    sink_result = 0;
    memset(sink_frame, 0, sizeof(sink_frame));

    hype_e1000_dev_reset(&r->dev, MAC);
    r->dev.tdbal = (uint32_t)(uintptr_t)r->tx_ring;
    r->dev.tdbah = (uint32_t)((uint64_t)(uintptr_t)r->tx_ring >> 32);
    r->dev.tdlen = RING * HYPE_E1000_DESC_BYTES;
    r->dev.tctl = HYPE_E1000_TCTL_EN;
    r->dev.rdbal = (uint32_t)(uintptr_t)r->rx_ring;
    r->dev.rdbah = (uint32_t)((uint64_t)(uintptr_t)r->rx_ring >> 32);
    r->dev.rdlen = RING * HYPE_E1000_DESC_BYTES;
    r->dev.rctl = HYPE_E1000_RCTL_EN;
}

/* Fills transmit descriptor `i` and points it at buffer `i`. */
static void tx_desc(rig_t *r, unsigned int i, unsigned int len, uint8_t cmd) {
    uint8_t *d = r->tx_ring + i * HYPE_E1000_DESC_BYTES;
    put64(d, (uint64_t)(uintptr_t)r->buf[i]);
    put16(d + 8, (uint16_t)len);
    d[11] = cmd;
    d[12] = 0;
}

static void rx_desc(rig_t *r, unsigned int i) {
    uint8_t *d = r->rx_ring + i * HYPE_E1000_DESC_BYTES;
    put64(d, (uint64_t)(uintptr_t)r->buf[i]);
    put16(d + 8, 0u);
    d[12] = 0;
}

static int drain(rig_t *r) {
    return hype_e1000_dev_drain_tx(&r->dev, 0, sink, r, r->scratch, sizeof(r->scratch), &r->stats);
}

/*
 * THE EMPTINESS TEST IS INVERTED relative to virtio, and this is the test for it. e1000's HEAD chases
 * TAIL: equal means the transmit ring is EMPTY. Reading it the other way produces a NIC that
 * transmits the same descriptor forever.
 */
static void test_tx_empty_when_head_equals_tail(void) {
    rig_t r;

    rig_init(&r);
    r.dev.tdh = 0u;
    r.dev.tdt = 0u;
    tx_desc(&r, 0, 100u, HYPE_E1000_TXD_CMD_EOP | HYPE_E1000_TXD_CMD_RS);

    CHECK_HEX("nothing is transmitted when head == tail", 0, drain(&r));
    CHECK_HEX("the sink was never called", 0, sink_calls);
    CHECK_HEX("head did not move", 0, r.dev.tdh);
}

static void test_tx_one_frame(void) {
    rig_t r;
    unsigned int i;

    rig_init(&r);
    for (i = 0; i < 60u; i++) {
        r.buf[0][i] = (uint8_t)(0x10u + i);
    }
    tx_desc(&r, 0, 60u, HYPE_E1000_TXD_CMD_EOP | HYPE_E1000_TXD_CMD_RS);
    r.dev.tdh = 0u;
    r.dev.tdt = 1u; /* the driver made descriptor 0 available */

    CHECK_HEX("one descriptor completed", 1, drain(&r));
    CHECK_HEX("the sink saw the frame", 1, sink_calls);
    CHECK_HEX("with the right length", 60, sink_len);
    CHECK_HEX("first byte", 0x10, sink_frame[0]);
    CHECK_HEX("last byte", 0x10 + 59, sink_frame[59]);
    CHECK_HEX("head advanced to the tail", 1, r.dev.tdh);
    CHECK_TRUE("DD was written back", (r.tx_ring[12] & HYPE_E1000_TXD_STA_DD) != 0u);
    CHECK_HEX("counted as a frame", 1, r.stats.tx_frames);
    /* A completed transmit raises TXDW. */
    CHECK_TRUE("TXDW is pending", (r.dev.icr & HYPE_E1000_ICR_TXDW) != 0u);
}

/* DD is written back only when the driver asked for it with RS -- a driver that did not ask is not
 * polling for it, and setting it anyway writes to a descriptor field it may be reusing. */
static void test_dd_only_when_rs_requested(void) {
    rig_t r;

    rig_init(&r);
    tx_desc(&r, 0, 60u, HYPE_E1000_TXD_CMD_EOP); /* no RS */
    r.dev.tdh = 0u;
    r.dev.tdt = 1u;

    CHECK_HEX("completed", 1, drain(&r));
    CHECK_HEX("the frame was still sent", 1, sink_calls);
    CHECK_HEX("but DD was not written", 0, r.tx_ring[12] & HYPE_E1000_TXD_STA_DD);
}

static void test_tx_wraps_the_ring(void) {
    rig_t r;
    unsigned int i;

    rig_init(&r);
    /* Head near the end, tail wrapped past 0: three descriptors at 6, 7, 0. */
    for (i = 0; i < RING; i++) {
        r.buf[i][0] = (uint8_t)(0xC0u + i);
        tx_desc(&r, i, 1u, HYPE_E1000_TXD_CMD_EOP | HYPE_E1000_TXD_CMD_RS);
    }
    r.dev.tdh = 6u;
    r.dev.tdt = 1u;

    CHECK_HEX("three descriptors across the wrap", 3, drain(&r));
    CHECK_HEX("the last one transmitted was descriptor 0", 0xC0, sink_frame[0]);
    CHECK_HEX("head caught the tail", 1, r.dev.tdh);
    CHECK_HEX("three frames", 3, r.stats.tx_frames);
}

/*
 * A frame split across descriptors: this model does not gather, so a non-EOP descriptor is counted as
 * unusable rather than silently transmitted as a short frame -- which would put a fragment on the
 * wire that the far end reports as a malformed packet.
 */
static void test_non_eop_descriptor_is_not_transmitted(void) {
    rig_t r;

    rig_init(&r);
    tx_desc(&r, 0, 60u, HYPE_E1000_TXD_CMD_RS); /* no EOP */
    r.dev.tdh = 0u;
    r.dev.tdt = 1u;

    CHECK_HEX("still completed, so the driver's queue keeps moving", 1, drain(&r));
    CHECK_HEX("nothing was transmitted", 0, sink_calls);
    CHECK_HEX("counted as a bad descriptor", 1, r.stats.tx_bad_desc);
    CHECK_TRUE("DD written back anyway -- withholding it would stall the queue",
               (r.tx_ring[12] & HYPE_E1000_TXD_STA_DD) != 0u);
}

static void test_tx_bad_descriptors_are_still_completed(void) {
    rig_t r;

    /* Zero length. */
    rig_init(&r);
    tx_desc(&r, 0, 0u, HYPE_E1000_TXD_CMD_EOP | HYPE_E1000_TXD_CMD_RS);
    r.dev.tdh = 0u;
    r.dev.tdt = 1u;
    CHECK_HEX("completed", 1, drain(&r));
    CHECK_HEX("nothing sent", 0, sink_calls);
    CHECK_HEX("counted", 1, r.stats.tx_bad_desc);

    /* Longer than the gather buffer. */
    rig_init(&r);
    tx_desc(&r, 0, (unsigned int)sizeof(r.scratch) + 1u,
            HYPE_E1000_TXD_CMD_EOP | HYPE_E1000_TXD_CMD_RS);
    r.dev.tdh = 0u;
    r.dev.tdt = 1u;
    CHECK_HEX("completed", 1, drain(&r));
    CHECK_HEX("dropped rather than clipped", 0, sink_calls);
    CHECK_HEX("counted", 1, r.stats.tx_bad_desc);
}

static void test_a_refused_frame_is_a_drop(void) {
    rig_t r;

    rig_init(&r);
    sink_result = -1;
    tx_desc(&r, 0, 60u, HYPE_E1000_TXD_CMD_EOP | HYPE_E1000_TXD_CMD_RS);
    r.dev.tdh = 0u;
    r.dev.tdt = 1u;
    CHECK_HEX("completed", 1, drain(&r));
    CHECK_HEX("counted as dropped", 1, r.stats.tx_dropped);
    CHECK_HEX("and as a frame that was offered", 1, r.stats.tx_frames);
    CHECK_HEX("a second drain does not retry it", 0, drain(&r));
}

/* A tail outside the ring is refused rather than masked: masking would transmit from a descriptor the
 * driver never meant. */
static void test_tail_out_of_range_is_refused(void) {
    rig_t r;

    rig_init(&r);
    r.dev.tdh = 0u;
    r.dev.tdt = RING + 3u;
    CHECK_HEX("refused", -1, drain(&r));
    CHECK_HEX("nothing consumed", 0, r.dev.tdh);

    rig_init(&r);
    r.dev.rdh = 0u;
    r.dev.rdt = RING + 1u;
    {
        const uint8_t f[4] = {1, 2, 3, 4};
        CHECK_HEX("the receive side refuses the same way", -1,
                  hype_e1000_dev_deliver_rx(&r.dev, 0, f, sizeof(f), &r.stats));
    }
}

static void test_tx_refusals(void) {
    rig_t r;

    rig_init(&r);
    CHECK_HEX("a null device", -1,
              hype_e1000_dev_drain_tx(0, 0, sink, &r, r.scratch, sizeof(r.scratch), 0));
    CHECK_HEX("a null sink", -1,
              hype_e1000_dev_drain_tx(&r.dev, 0, 0, &r, r.scratch, sizeof(r.scratch), 0));
    CHECK_HEX("a null scratch buffer", -1,
              hype_e1000_dev_drain_tx(&r.dev, 0, sink, &r, 0, 4096u, 0));
    CHECK_HEX("a scratch buffer too small to hold a frame", -1,
              hype_e1000_dev_drain_tx(&r.dev, 0, sink, &r, r.scratch, 64u, 0));

    r.dev.tctl = 0u; /* transmitter disabled */
    CHECK_HEX("a disabled transmitter", -1, drain(&r));
    r.dev.tctl = HYPE_E1000_TCTL_EN;
    r.dev.tdlen = 0u;
    CHECK_HEX("a zero-length ring", -1, drain(&r));
}

/*
 * The RECEIVE emptiness test, which is the INVERSE of transmit's on the same equality: equal pointers
 * mean the driver has posted NOTHING. Reading it as "full" would make hype claim a buffer it does not
 * have and write over whatever the descriptor happened to point at.
 */
static void test_rx_empty_when_head_equals_tail(void) {
    rig_t r;
    const uint8_t f[4] = {1, 2, 3, 4};

    rig_init(&r);
    r.dev.rdh = 0u;
    r.dev.rdt = 0u;
    rx_desc(&r, 0);
    memset(r.buf[0], 0x5A, 64);

    CHECK_HEX("reported as no buffer, not an error", 0,
              hype_e1000_dev_deliver_rx(&r.dev, 0, f, sizeof(f), &r.stats));
    CHECK_HEX("counted, so drops are visible", 1, r.stats.rx_no_buffer);
    CHECK_HEX("the guest's buffer was NOT written", 0x5A, r.buf[0][0]);
    CHECK_HEX("head did not move", 0, r.dev.rdh);
}

static void test_rx_delivers_a_frame(void) {
    rig_t r;
    uint8_t f[80];
    unsigned int i;

    rig_init(&r);
    for (i = 0; i < sizeof(f); i++) {
        f[i] = (uint8_t)(0x40u + i);
    }
    memset(r.buf[0], 0x5A, sizeof(r.buf[0]));
    rx_desc(&r, 0);
    r.dev.rdh = 0u;
    r.dev.rdt = 1u; /* one buffer posted */

    CHECK_HEX("delivered", 1, hype_e1000_dev_deliver_rx(&r.dev, 0, f, sizeof(f), &r.stats));
    CHECK_HEX("first byte", 0x40, r.buf[0][0]);
    CHECK_HEX("last byte", 0x40 + 79, r.buf[0][79]);
    CHECK_HEX("the byte after the frame is untouched", 0x5A, r.buf[0][80]);
    CHECK_HEX("the descriptor reports the length", 80, get16(r.rx_ring + 8));
    CHECK_TRUE("DD set", (r.rx_ring[12] & HYPE_E1000_RXD_STA_DD) != 0u);
    CHECK_TRUE("EOP set -- one descriptor, one whole frame",
               (r.rx_ring[12] & HYPE_E1000_RXD_STA_EOP) != 0u);
    CHECK_HEX("head advanced", 1, r.dev.rdh);
    CHECK_HEX("counted", 1, r.stats.rx_delivered);
    CHECK_TRUE("RXT0 raised", (r.dev.icr & HYPE_E1000_ICR_RXT0) != 0u);
}

static void test_rx_wraps_the_ring(void) {
    rig_t r;
    const uint8_t f[4] = {0xE1, 0xE2, 0xE3, 0xE4};
    unsigned int i;

    rig_init(&r);
    for (i = 0; i < RING; i++) {
        rx_desc(&r, i);
    }
    r.dev.rdh = (uint32_t)(RING - 1u);
    r.dev.rdt = 1u;

    CHECK_HEX("delivered into the last descriptor", 1,
              hype_e1000_dev_deliver_rx(&r.dev, 0, f, sizeof(f), &r.stats));
    CHECK_HEX("into the matching buffer", 0xE1, r.buf[RING - 1u][0]);
    CHECK_HEX("head wrapped to 0", 0, r.dev.rdh);
}

static void test_rx_refusals(void) {
    rig_t r;
    uint8_t big[HYPE_VIRTIO_NET_MAX_FRAME_LEN + 1];
    const uint8_t f[4] = {1, 2, 3, 4};

    rig_init(&r);
    memset(big, 0, sizeof(big));
    CHECK_HEX("a null device", -1, hype_e1000_dev_deliver_rx(0, 0, f, 4, 0));
    CHECK_HEX("a null frame", -1, hype_e1000_dev_deliver_rx(&r.dev, 0, 0, 4, 0));
    CHECK_HEX("a zero-length frame", -1, hype_e1000_dev_deliver_rx(&r.dev, 0, f, 0, 0));
    CHECK_HEX("a frame past the MTU", -1,
              hype_e1000_dev_deliver_rx(&r.dev, 0, big, sizeof(big), 0));
    r.dev.rctl = 0u;
    CHECK_HEX("a disabled receiver", -1, hype_e1000_dev_deliver_rx(&r.dev, 0, f, 4, 0));
    r.dev.rctl = HYPE_E1000_RCTL_EN;
    r.dev.rdlen = 0u;
    CHECK_HEX("a zero-length ring", -1, hype_e1000_dev_deliver_rx(&r.dev, 0, f, 4, 0));
}

/*
 * A REAL BOUNDS-CHECKED MAP, which is what the live hypervisor always passes -- everything above ran
 * on the identity path, so without these the production path would be the untested one. And it
 * carries the security property: VALID-1/2 exist so a guest cannot aim a descriptor at hype's memory.
 */
#define GBASE 0x50000000ull
static hype_gpa_map_t g_map;

static void rig_init_mapped(rig_t *r) {
    rig_init(r);
    hype_gpa_map_reset(&g_map);
    if (hype_gpa_map_add(&g_map, GBASE, (uint64_t)(uintptr_t)r, sizeof(*r)) != 0) {
        printf("FAIL: could not build the test gpa map\n");
        failures++;
    }
    r->dev.tdbal = (uint32_t)(GBASE + (uint64_t)((uint8_t *)r->tx_ring - (uint8_t *)r));
    r->dev.tdbah = 0u;
    r->dev.rdbal = (uint32_t)(GBASE + (uint64_t)((uint8_t *)r->rx_ring - (uint8_t *)r));
    r->dev.rdbah = 0u;
}

static uint64_t gpa_of(const rig_t *r, const void *host) {
    return GBASE + (uint64_t)((const uint8_t *)host - (const uint8_t *)r);
}

static void test_through_a_real_map(void) {
    rig_t r;
    uint8_t *d;
    const uint8_t f[4] = {0x91, 0x92, 0x93, 0x94};

    rig_init_mapped(&r);
    r.buf[0][0] = 0x77;
    d = r.tx_ring;
    put64(d, gpa_of(&r, r.buf[0]));
    put16(d + 8, 1u);
    d[11] = HYPE_E1000_TXD_CMD_EOP | HYPE_E1000_TXD_CMD_RS;
    r.dev.tdh = 0u;
    r.dev.tdt = 1u;
    CHECK_HEX("transmitted through the map", 1,
              hype_e1000_dev_drain_tx(&r.dev, &g_map, sink, &r, r.scratch, sizeof(r.scratch),
                                      &r.stats));
    CHECK_HEX("the right byte", 0x77, sink_frame[0]);

    /* A descriptor pointing OUTSIDE this VM's memory must not be dereferenced -- and the descriptor
     * is still completed, so the guest's queue keeps moving. */
    rig_init_mapped(&r);
    d = r.tx_ring;
    put64(d, GBASE + sizeof(r) + 0x1000ull);
    put16(d + 8, 64u);
    d[11] = HYPE_E1000_TXD_CMD_EOP | HYPE_E1000_TXD_CMD_RS;
    r.dev.tdh = 0u;
    r.dev.tdt = 1u;
    CHECK_HEX("completed", 1,
              hype_e1000_dev_drain_tx(&r.dev, &g_map, sink, &r, r.scratch, sizeof(r.scratch),
                                      &r.stats));
    CHECK_HEX("nothing was read or sent", 0, sink_calls);
    CHECK_HEX("counted as a bad descriptor", 1, r.stats.tx_bad_desc);

    /* A RING outside the map: refused before anything is consumed. */
    rig_init_mapped(&r);
    r.dev.tdbal = (uint32_t)(GBASE + sizeof(r) + 0x8000ull);
    r.dev.tdt = 1u;
    CHECK_HEX("an unreachable ring is refused", -1,
              hype_e1000_dev_drain_tx(&r.dev, &g_map, sink, &r, r.scratch, sizeof(r.scratch),
                                      &r.stats));

    /* Receive, through the map and into an unmapped buffer. */
    rig_init_mapped(&r);
    d = r.rx_ring;
    put64(d, gpa_of(&r, r.buf[0]));
    r.dev.rdh = 0u;
    r.dev.rdt = 1u;
    CHECK_HEX("delivered through the map", 1,
              hype_e1000_dev_deliver_rx(&r.dev, &g_map, f, sizeof(f), &r.stats));
    CHECK_HEX("the frame landed", 0x91, r.buf[0][0]);

    rig_init_mapped(&r);
    d = r.rx_ring;
    put64(d, GBASE + sizeof(r) + 0x1000ull);
    r.dev.rdh = 0u;
    r.dev.rdt = 1u;
    CHECK_HEX("an unmapped receive buffer is not written to", 0,
              hype_e1000_dev_deliver_rx(&r.dev, &g_map, f, sizeof(f), &r.stats));
    CHECK_HEX("but the descriptor came back, so the ring keeps moving", 1, r.dev.rdh);
    CHECK_TRUE("with DD set", (r.rx_ring[12] & HYPE_E1000_RXD_STA_DD) != 0u);
    CHECK_HEX("and zero length", 0, get16(r.rx_ring + 8));

    rig_init_mapped(&r);
    r.dev.rdbal = (uint32_t)(GBASE + sizeof(r) + 0x8000ull);
    r.dev.rdt = 1u;
    CHECK_HEX("an unreachable receive ring is refused", -1,
              hype_e1000_dev_deliver_rx(&r.dev, &g_map, f, sizeof(f), &r.stats));
}

/* Both entry points must work with stats == 0: the live caller may not want counters, and a null
 * dereference there would be a crash in the packet path. */
static void test_stats_are_optional(void) {
    rig_t r;
    const uint8_t f[4] = {1, 2, 3, 4};

    rig_init(&r);
    tx_desc(&r, 0, 60u, HYPE_E1000_TXD_CMD_EOP | HYPE_E1000_TXD_CMD_RS);
    r.dev.tdh = 0u;
    r.dev.tdt = 1u;
    CHECK_HEX("transmit with no stats block", 1,
              hype_e1000_dev_drain_tx(&r.dev, 0, sink, &r, r.scratch, sizeof(r.scratch), 0));

    rig_init(&r);
    tx_desc(&r, 0, 0u, HYPE_E1000_TXD_CMD_EOP | HYPE_E1000_TXD_CMD_RS);
    r.dev.tdh = 0u;
    r.dev.tdt = 1u;
    CHECK_HEX("a bad descriptor with no stats block", 1,
              hype_e1000_dev_drain_tx(&r.dev, 0, sink, &r, r.scratch, sizeof(r.scratch), 0));

    rig_init(&r);
    sink_result = -1;
    tx_desc(&r, 0, 60u, HYPE_E1000_TXD_CMD_EOP | HYPE_E1000_TXD_CMD_RS);
    r.dev.tdh = 0u;
    r.dev.tdt = 1u;
    CHECK_HEX("a refused frame with no stats block", 1,
              hype_e1000_dev_drain_tx(&r.dev, 0, sink, &r, r.scratch, sizeof(r.scratch), 0));

    rig_init(&r);
    CHECK_HEX("an empty receive ring with no stats block", 0,
              hype_e1000_dev_deliver_rx(&r.dev, 0, f, sizeof(f), 0));

    rig_init(&r);
    rx_desc(&r, 0);
    r.dev.rdh = 0u;
    r.dev.rdt = 1u;
    CHECK_HEX("a delivery with no stats block", 1,
              hype_e1000_dev_deliver_rx(&r.dev, 0, f, sizeof(f), 0));
}

int main(void) {
    test_tx_empty_when_head_equals_tail();
    test_tx_one_frame();
    test_dd_only_when_rs_requested();
    test_tx_wraps_the_ring();
    test_non_eop_descriptor_is_not_transmitted();
    test_tx_bad_descriptors_are_still_completed();
    test_a_refused_frame_is_a_drop();
    test_tail_out_of_range_is_refused();
    test_tx_refusals();
    test_rx_empty_when_head_equals_tail();
    test_rx_delivers_a_frame();
    test_rx_wraps_the_ring();
    test_rx_refusals();
    test_through_a_real_map();
    test_stats_are_optional();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
