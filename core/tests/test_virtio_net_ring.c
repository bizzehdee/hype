#include <stdio.h>
#include <string.h>
#include "../virtio_net_ring.h"

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

/*
 * map == 0 means "identity-mapped guest", so host addresses ARE guest-physical ones and the ring
 * walk is testable on the host. Same technique as test_virtio_blk.c's chain tests, and the reason
 * this code was put in core/ rather than beside process_virtio_blk_queue() in the coverage-exempt
 * SVM backend.
 */
#define QSZ 8u

typedef struct {
    uint8_t desc[QSZ * 16u];
    uint8_t avail[4u + 2u * QSZ + 2u];
    uint8_t used[4u + 8u * QSZ + 2u];
    uint8_t buf[4][2048];
    uint8_t scratch[HYPE_VIRTIO_NET_MAX_FRAME_LEN];
    hype_virtio_net_t dev;
    hype_virtio_net_ring_stats_t stats;
} rig_t;

static const uint8_t MAC[6] = {0x52, 0x54, 0x00, 0xAB, 0xCD, 0xEF};

/* What the sink saw, so a test can assert on the frame rather than only on a count. */
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

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu); p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void desc(rig_t *r, unsigned i, const void *addr, uint32_t len, uint16_t flags, uint16_t next) {
    uint8_t *d = r->desc + i * 16u;
    put64(d, (uint64_t)(uintptr_t)addr);
    put32(d + 8, len);
    put16(d + 12, flags);
    put16(d + 14, next);
}

/* Publishes descriptor `head` on the avail ring at position `slot`. */
static void avail_push(rig_t *r, unsigned slot, uint16_t head) {
    put16(r->avail + 4 + 2 * slot, head);
    put16(r->avail + 2, (uint16_t)(slot + 1u));
}

static void rig_init(rig_t *r, unsigned int queue) {
    memset(r, 0, sizeof(*r));
    sink_calls = 0;
    sink_len = 0;
    sink_result = 0;
    memset(sink_frame, 0, sizeof(sink_frame));

    hype_virtio_net_reset(&r->dev, MAC);
    r->dev.driver_features = 1ull << HYPE_VIRTIO_F_VERSION_1_BIT;
    r->dev.device_status = HYPE_VIRTIO_STATUS_DRIVER_OK;
    r->dev.vq[queue].size = QSZ;
    r->dev.vq[queue].enable = 1;
    r->dev.vq[queue].desc = (uint64_t)(uintptr_t)r->desc;
    r->dev.vq[queue].driver = (uint64_t)(uintptr_t)r->avail;
    r->dev.vq[queue].device = (uint64_t)(uintptr_t)r->used;
}

static int drain(rig_t *r) {
    return hype_virtio_net_drain_tx(&r->dev, 0, sink, r, r->scratch, sizeof(r->scratch), &r->stats);
}

/* The header is 12 bytes and NOT part of the frame. Getting that wrong shifts every packet by two
 * bytes, which the far end reports as malformed traffic rather than as a header disagreement. */
static void test_tx_one_frame_skips_the_header(void) {
    rig_t r;
    unsigned int i;

    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    for (i = 0; i < 12u; i++) {
        r.buf[0][i] = 0xEE; /* header bytes: must not reach the sink */
    }
    for (i = 0; i < 60u; i++) {
        r.buf[0][12 + i] = (uint8_t)(0x40u + i);
    }
    desc(&r, 0, r.buf[0], 12u + 60u, 0, 0);
    avail_push(&r, 0, 0);

    CHECK_HEX("one chain completed", 1, drain(&r));
    CHECK_HEX("the sink was called once", 1, sink_calls);
    CHECK_HEX("the frame is the payload only, header excluded", 60, sink_len);
    CHECK_HEX("first payload byte", 0x40, sink_frame[0]);
    CHECK_HEX("last payload byte", 0x40 + 59, sink_frame[59]);
    CHECK_HEX("the used ring advanced", 1, get16(r.used + 2));
    CHECK_HEX("the used element names the head descriptor", 0, get32(r.used + 4));
    CHECK_HEX("a completed transmit raises the queue interrupt", 0x1,
              hype_virtio_net_isr_read(&r.dev));
    CHECK_HEX("counted as a frame", 1, r.stats.tx_frames);
    CHECK_HEX("nothing dropped", 0, r.stats.tx_dropped);
}

/* The header may span descriptors, so what has been skipped has to be tracked across the chain
 * rather than assumed to be exactly the first descriptor. */
static void test_tx_header_split_across_descriptors(void) {
    rig_t r;

    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    memset(r.buf[0], 0xEE, 4);   /* 4 header bytes */
    memset(r.buf[1], 0xEE, 8);   /* the remaining 8 header bytes */
    r.buf[2][0] = 0xA1;
    r.buf[2][1] = 0xA2;
    desc(&r, 0, r.buf[0], 4u, HYPE_VIRTQ_DESC_F_NEXT, 1);
    desc(&r, 1, r.buf[1], 8u, HYPE_VIRTQ_DESC_F_NEXT, 2);
    desc(&r, 2, r.buf[2], 2u, 0, 0);
    avail_push(&r, 0, 0);

    CHECK_HEX("completed", 1, drain(&r));
    CHECK_HEX("only the payload past the 12-byte header reached the sink", 2, sink_len);
    CHECK_HEX("payload byte 0", 0xA1, sink_frame[0]);
    CHECK_HEX("payload byte 1", 0xA2, sink_frame[1]);
}

/* A descriptor that both ends the header and carries payload -- the boundary case of the split. */
static void test_tx_header_and_payload_in_one_descriptor_boundary(void) {
    rig_t r;

    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    memset(r.buf[0], 0xEE, 11);
    r.buf[0][11] = 0xEE;  /* the 12th and last header byte */
    r.buf[0][12] = 0x99;
    desc(&r, 0, r.buf[0], 13u, 0, 0);
    avail_push(&r, 0, 0);

    CHECK_HEX("completed", 1, drain(&r));
    CHECK_HEX("exactly one payload byte", 1, sink_len);
    CHECK_HEX("and it is the byte after the header", 0x99, sink_frame[0]);
}

static void test_tx_several_chains_drain_in_one_call(void) {
    rig_t r;

    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    r.buf[0][12] = 0x11;
    r.buf[1][12] = 0x22;
    r.buf[2][12] = 0x33;
    desc(&r, 0, r.buf[0], 13u, 0, 0);
    desc(&r, 1, r.buf[1], 13u, 0, 0);
    desc(&r, 2, r.buf[2], 13u, 0, 0);
    put16(r.avail + 4 + 0, 0);
    put16(r.avail + 4 + 2, 1);
    put16(r.avail + 4 + 4, 2);
    put16(r.avail + 2, 3);

    CHECK_HEX("all three chains completed", 3, drain(&r));
    CHECK_HEX("the sink saw all three", 3, sink_calls);
    CHECK_HEX("the last frame is the third", 0x33, sink_frame[0]);
    CHECK_HEX("the used index advanced three times", 3, get16(r.used + 2));
    CHECK_HEX("a second drain finds nothing new", 0, drain(&r));
}

/*
 * THE PROPERTY THAT MATTERS MOST HERE: a chain is completed even when it is unusable. A device that
 * withholds a descriptor because it disliked the contents stops the driver's transmit queue for
 * good, and the operator sees a hung network rather than a dropped packet.
 */
static void test_a_malformed_chain_is_still_completed(void) {
    rig_t r;

    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    /* A head index outside the ring. */
    avail_push(&r, 0, (uint16_t)(QSZ + 3u));
    CHECK_HEX("completed anyway", 1, drain(&r));
    CHECK_HEX("the used ring advanced", 1, get16(r.used + 2));
    CHECK_HEX("with zero bytes written", 0, get32(r.used + 8));
    CHECK_HEX("counted as a bad descriptor", 1, r.stats.tx_bad_desc);
    CHECK_HEX("and never handed to the sink", 0, sink_calls);
}

/* A `next` chain that loops must not spin forever. Every link in it is guest-controlled. */
static void test_a_looping_chain_terminates(void) {
    rig_t r;

    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    desc(&r, 0, r.buf[0], 16u, HYPE_VIRTQ_DESC_F_NEXT, 1);
    desc(&r, 1, r.buf[1], 16u, HYPE_VIRTQ_DESC_F_NEXT, 0); /* back to 0 */
    avail_push(&r, 0, 0);

    CHECK_HEX("the walk bailed out and completed the chain", 1, drain(&r));
    CHECK_HEX("counted as a bad descriptor", 1, r.stats.tx_bad_desc);
    CHECK_HEX("nothing was transmitted", 0, sink_calls);
}

static void test_a_header_only_chain_is_completed_and_is_not_a_frame(void) {
    rig_t r;

    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    desc(&r, 0, r.buf[0], 12u, 0, 0); /* exactly the header, no payload */
    avail_push(&r, 0, 0);

    CHECK_HEX("completed", 1, drain(&r));
    CHECK_HEX("the sink was not called -- there was no frame", 0, sink_calls);
    CHECK_HEX("not counted as a frame", 0, r.stats.tx_frames);
    CHECK_HEX("nor as a bad descriptor -- it was well formed and empty", 0, r.stats.tx_bad_desc);
}

/* A sink that refuses is a dropped packet, not a stalled queue. */
static void test_a_refused_frame_is_dropped_not_retried(void) {
    rig_t r;

    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    sink_result = -1;
    r.buf[0][12] = 0x77;
    desc(&r, 0, r.buf[0], 13u, 0, 0);
    avail_push(&r, 0, 0);

    CHECK_HEX("still completed", 1, drain(&r));
    CHECK_HEX("counted as dropped", 1, r.stats.tx_dropped);
    CHECK_HEX("and as a frame that was offered", 1, r.stats.tx_frames);
    CHECK_HEX("a second drain does not retry it", 0, drain(&r));
}

static void test_oversized_frame_is_dropped_rather_than_clipped(void) {
    rig_t r;

    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    /* Two descriptors whose payload together exceeds the scratch buffer. */
    desc(&r, 0, r.buf[0], 2048u, HYPE_VIRTQ_DESC_F_NEXT, 1);
    desc(&r, 1, r.buf[1], 2048u, 0, 0);
    avail_push(&r, 0, 0);

    CHECK_HEX("completed", 1, drain(&r));
    CHECK_HEX("nothing was handed on -- a clipped frame is a corrupt one", 0, sink_calls);
    CHECK_HEX("counted as a bad descriptor", 1, r.stats.tx_bad_desc);
}

static void test_tx_refusals(void) {
    rig_t r;

    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    CHECK_HEX("a null device is refused", -1,
              hype_virtio_net_drain_tx(0, 0, sink, &r, r.scratch, sizeof(r.scratch), 0));
    CHECK_HEX("a null sink is refused", -1,
              hype_virtio_net_drain_tx(&r.dev, 0, 0, &r, r.scratch, sizeof(r.scratch), 0));
    CHECK_HEX("a null scratch buffer is refused", -1,
              hype_virtio_net_drain_tx(&r.dev, 0, sink, &r, 0, 1600u, 0));
    CHECK_HEX("a scratch buffer too small to hold a frame is refused, not used", -1,
              hype_virtio_net_drain_tx(&r.dev, 0, sink, &r, r.scratch, 64u, 0));

    /* Not ready: the driver has not finished publishing the ring. */
    r.dev.device_status = 0;
    CHECK_HEX("a queue that is not ready is refused", -1, drain(&r));
    r.dev.device_status = HYPE_VIRTIO_STATUS_DRIVER_OK;

    /* Ready, but the rings do not translate. */
    r.dev.vq[HYPE_VIRTIO_NET_VQ_TX].size = 0;
    CHECK_HEX("a zero-length ring is refused", -1, drain(&r));
}

static void test_rx_delivers_a_frame_with_a_zeroed_header(void) {
    rig_t r;
    const uint8_t frame[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    unsigned int i;

    rig_init(&r, HYPE_VIRTIO_NET_VQ_RX);
    memset(r.buf[0], 0x5A, sizeof(r.buf[0])); /* poison, so written bytes are identifiable */
    desc(&r, 0, r.buf[0], 128u, HYPE_VIRTQ_DESC_F_WRITE, 0);
    avail_push(&r, 0, 0);

    CHECK_HEX("delivered", 1,
              hype_virtio_net_deliver_rx(&r.dev, 0, frame, sizeof(frame), &r.stats));
    for (i = 0; i < 10u; i++) {
        CHECK_HEX("header byte is zero -- no offloads are negotiated", 0, r.buf[0][i]);
    }
    /* num_buffers = 1. Zero would tell the driver the frame occupies no buffers. */
    CHECK_HEX("num_buffers low byte", 1, r.buf[0][10]);
    CHECK_HEX("num_buffers high byte", 0, r.buf[0][11]);
    CHECK_HEX("frame byte 0 follows the header", 0xDE, r.buf[0][12]);
    CHECK_HEX("frame byte 3", 0xEF, r.buf[0][15]);
    CHECK_HEX("the byte after the frame is untouched", 0x5A, r.buf[0][16]);
    CHECK_HEX("the used element reports header + frame", 12u + 4u, get32(r.used + 8));
    CHECK_HEX("the used ring advanced", 1, get16(r.used + 2));
    CHECK_HEX("counted as delivered", 1, r.stats.rx_delivered);
    CHECK_HEX("delivery raises the queue interrupt", 0x1, hype_virtio_net_isr_read(&r.dev));
}

/*
 * An empty receive ring is NORMAL -- a driver between polls has one -- so it must report 0 rather
 * than an error. Treating it as failure would turn ordinary operation into a logged fault.
 */
static void test_rx_with_no_posted_buffer_is_zero_not_an_error(void) {
    rig_t r;
    const uint8_t frame[4] = {1, 2, 3, 4};

    rig_init(&r, HYPE_VIRTIO_NET_VQ_RX);
    CHECK_HEX("no buffer posted reports 0", 0,
              hype_virtio_net_deliver_rx(&r.dev, 0, frame, sizeof(frame), &r.stats));
    CHECK_HEX("counted, so the operator can see drops", 1, r.stats.rx_no_buffer);
    CHECK_HEX("nothing was placed in the used ring", 0, get16(r.used + 2));
}

static void test_rx_spanning_two_descriptors(void) {
    rig_t r;
    uint8_t frame[40];
    unsigned int i;

    rig_init(&r, HYPE_VIRTIO_NET_VQ_RX);
    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(0x10u + i);
    }
    /* 16 bytes then 64: the header plus 4 frame bytes fit in the first, the rest in the second. */
    desc(&r, 0, r.buf[0], 16u, HYPE_VIRTQ_DESC_F_WRITE | HYPE_VIRTQ_DESC_F_NEXT, 1);
    desc(&r, 1, r.buf[1], 64u, HYPE_VIRTQ_DESC_F_WRITE, 0);
    avail_push(&r, 0, 0);

    CHECK_HEX("delivered across the chain", 1,
              hype_virtio_net_deliver_rx(&r.dev, 0, frame, sizeof(frame), &r.stats));
    CHECK_HEX("first frame byte after the header in descriptor 0", 0x10, r.buf[0][12]);
    CHECK_HEX("fourth frame byte fills descriptor 0", 0x13, r.buf[0][15]);
    CHECK_HEX("the fifth continues in descriptor 1", 0x14, r.buf[1][0]);
    CHECK_HEX("the last frame byte", 0x10 + 39, r.buf[1][35]);
    CHECK_HEX("the used element reports the whole write", 12u + 40u, get32(r.used + 8));
}

/* A receive descriptor the device may not write cannot hold a frame. Writing it anyway would
 * corrupt whatever the driver had put there. */
static void test_rx_refuses_a_non_writable_descriptor(void) {
    rig_t r;
    const uint8_t frame[4] = {1, 2, 3, 4};

    rig_init(&r, HYPE_VIRTIO_NET_VQ_RX);
    memset(r.buf[0], 0x5A, 32);
    desc(&r, 0, r.buf[0], 128u, 0, 0); /* no F_WRITE */
    avail_push(&r, 0, 0);

    CHECK_HEX("not delivered", 0,
              hype_virtio_net_deliver_rx(&r.dev, 0, frame, sizeof(frame), &r.stats));
    CHECK_HEX("the guest's buffer was not written", 0x5A, r.buf[0][0]);
    CHECK_HEX("the descriptor is still returned, so the ring does not stall", 1,
              get16(r.used + 2));
}

/* A buffer too small for the frame: completed (so the ring keeps moving) but counted as a drop,
 * because a partial frame is not a delivered one. */
static void test_rx_short_buffer_is_a_drop_not_a_partial_delivery(void) {
    rig_t r;
    uint8_t frame[200];

    rig_init(&r, HYPE_VIRTIO_NET_VQ_RX);
    memset(frame, 0x77, sizeof(frame));
    desc(&r, 0, r.buf[0], 20u, HYPE_VIRTQ_DESC_F_WRITE, 0); /* 12 header + 8 = too small */
    avail_push(&r, 0, 0);

    CHECK_HEX("reported as not delivered", 0,
              hype_virtio_net_deliver_rx(&r.dev, 0, frame, sizeof(frame), &r.stats));
    CHECK_HEX("counted as a drop", 1, r.stats.rx_no_buffer);
    CHECK_HEX("nothing counted as delivered", 0, r.stats.rx_delivered);
    CHECK_HEX("the descriptor came back with zero length", 0, get32(r.used + 8));
    CHECK_HEX("and the ring advanced", 1, get16(r.used + 2));
}

static void test_rx_head_out_of_range_is_completed(void) {
    rig_t r;
    const uint8_t frame[4] = {1, 2, 3, 4};

    rig_init(&r, HYPE_VIRTIO_NET_VQ_RX);
    avail_push(&r, 0, (uint16_t)(QSZ + 1u));
    CHECK_HEX("not delivered", 0,
              hype_virtio_net_deliver_rx(&r.dev, 0, frame, sizeof(frame), &r.stats));
    CHECK_HEX("but completed, so the ring keeps moving", 1, get16(r.used + 2));
}

static void test_rx_refusals(void) {
    rig_t r;
    uint8_t big[HYPE_VIRTIO_NET_MAX_FRAME_LEN + 1];
    const uint8_t frame[4] = {1, 2, 3, 4};

    rig_init(&r, HYPE_VIRTIO_NET_VQ_RX);
    memset(big, 0, sizeof(big));
    CHECK_HEX("a null device is refused", -1, hype_virtio_net_deliver_rx(0, 0, frame, 4, 0));
    CHECK_HEX("a null frame is refused", -1, hype_virtio_net_deliver_rx(&r.dev, 0, 0, 4, 0));
    CHECK_HEX("a zero-length frame is refused", -1,
              hype_virtio_net_deliver_rx(&r.dev, 0, frame, 0, 0));
    CHECK_HEX("a frame longer than the MTU allows is refused", -1,
              hype_virtio_net_deliver_rx(&r.dev, 0, big, sizeof(big), 0));

    r.dev.device_status = 0;
    CHECK_HEX("a queue that is not ready is refused", -1,
              hype_virtio_net_deliver_rx(&r.dev, 0, frame, 4, 0));
    r.dev.device_status = HYPE_VIRTIO_STATUS_DRIVER_OK;
    r.dev.vq[HYPE_VIRTIO_NET_VQ_RX].size = 0;
    CHECK_HEX("a zero-length ring is refused", -1,
              hype_virtio_net_deliver_rx(&r.dev, 0, frame, 4, 0));
}

/* The legacy 10-byte header, for a driver that never negotiated VERSION_1. Covers the header-length
 * branch in both directions. */
static void test_legacy_header_length_is_honoured(void) {
    rig_t r;
    const uint8_t frame[4] = {0xC1, 0xC2, 0xC3, 0xC4};

    rig_init(&r, HYPE_VIRTIO_NET_VQ_RX);
    /* is_queue_ready demands VERSION_1, so this exercises the length only through a device that
     * reports the legacy size -- which is what hype_virtio_net_hdr_len() is asked for. */
    CHECK_HEX("a device without VERSION_1 reports the legacy header length",
              HYPE_VIRTIO_NET_HDR_LEN_LEGACY,
              (r.dev.driver_features = 0, hype_virtio_net_hdr_len(&r.dev)));
    r.dev.driver_features = 1ull << HYPE_VIRTIO_F_VERSION_1_BIT;
    desc(&r, 0, r.buf[0], 128u, HYPE_VIRTQ_DESC_F_WRITE, 0);
    avail_push(&r, 0, 0);
    CHECK_HEX("and the modern one once negotiated", 1,
              hype_virtio_net_deliver_rx(&r.dev, 0, frame, sizeof(frame), &r.stats));
    CHECK_HEX("frame starts at 12", 0xC1, r.buf[0][12]);
}

/* A zero-length descriptor in the middle of a chain must be stepped over, not treated as an end. */
static void test_zero_length_descriptor_in_a_chain(void) {
    rig_t r;

    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    r.buf[2][0] = 0xB5;
    desc(&r, 0, r.buf[0], 12u, HYPE_VIRTQ_DESC_F_NEXT, 1);
    desc(&r, 1, r.buf[1], 0u, HYPE_VIRTQ_DESC_F_NEXT, 2);
    desc(&r, 2, r.buf[2], 1u, 0, 0);
    avail_push(&r, 0, 0);

    CHECK_HEX("completed", 1, drain(&r));
    CHECK_HEX("the payload after the empty descriptor still arrived", 1, sink_len);
    CHECK_HEX("and it is the right byte", 0xB5, sink_frame[0]);
}

/*
 * EVERYTHING ABOVE RAN WITH map == 0, the identity path. The live hypervisor ALWAYS passes a real
 * bounds-checked map, so without these the production path was the untested one -- and it is the
 * path that carries the security property: VALID-1/VALID-2 (#53/#54) exist so a guest cannot aim a
 * descriptor at hype's own memory or another VM's.
 *
 * The rig is mapped at a fake guest base, and addresses written into the rings are guest ones, so
 * the walk has to translate every single dereference to get anywhere.
 */
#define GBASE 0x40000000ull

static hype_gpa_map_t g_map;

static uint64_t gpa_of(const rig_t *r, const void *host) {
    return GBASE + (uint64_t)((const uint8_t *)host - (const uint8_t *)r);
}

static void rig_init_mapped(rig_t *r, unsigned int queue) {
    rig_init(r, queue);
    hype_gpa_map_reset(&g_map);
    if (hype_gpa_map_add(&g_map, GBASE, (uint64_t)(uintptr_t)r, sizeof(*r)) != 0) {
        printf("FAIL: could not build the test gpa map\n");
        failures++;
    }
    r->dev.vq[queue].desc = gpa_of(r, r->desc);
    r->dev.vq[queue].driver = gpa_of(r, r->avail);
    r->dev.vq[queue].device = gpa_of(r, r->used);
}

/* Descriptor whose buffer address is a GUEST address. */
static void desc_g(rig_t *r, unsigned i, const void *addr, uint32_t len, uint16_t flags,
                   uint16_t next) {
    uint8_t *d = r->desc + i * 16u;
    put64(d, gpa_of(r, addr));
    put32(d + 8, len);
    put16(d + 12, flags);
    put16(d + 14, next);
}

static void test_tx_through_a_real_bounds_checked_map(void) {
    rig_t r;

    rig_init_mapped(&r, HYPE_VIRTIO_NET_VQ_TX);
    r.buf[0][12] = 0x5C;
    desc_g(&r, 0, r.buf[0], 13u, 0, 0);
    avail_push(&r, 0, 0);

    CHECK_HEX("a chain completed through the map", 1,
              hype_virtio_net_drain_tx(&r.dev, &g_map, sink, &r, r.scratch, sizeof(r.scratch),
                                       &r.stats));
    CHECK_HEX("the frame arrived", 1, sink_len);
    CHECK_HEX("and it is the right byte", 0x5C, sink_frame[0]);
    CHECK_HEX("the used ring advanced", 1, get16(r.used + 2));
}

/*
 * The property the map exists for: a descriptor pointing OUTSIDE this VM's memory must not be
 * dereferenced. The chain is still completed, so the guest's queue keeps moving -- it just gets
 * nothing transmitted, which is the correct answer to "please read memory that is not yours".
 */
static void test_tx_descriptor_outside_the_map_is_refused_not_read(void) {
    rig_t r;
    uint8_t *d;

    rig_init_mapped(&r, HYPE_VIRTIO_NET_VQ_TX);
    d = r.desc;
    put64(d, GBASE + sizeof(r) + 0x1000ull); /* past the end of the mapping */
    put32(d + 8, 64u);
    put16(d + 12, 0);
    put16(d + 14, 0);
    avail_push(&r, 0, 0);

    CHECK_HEX("completed", 1,
              hype_virtio_net_drain_tx(&r.dev, &g_map, sink, &r, r.scratch, sizeof(r.scratch),
                                       &r.stats));
    CHECK_HEX("nothing was read or transmitted", 0, sink_calls);
    CHECK_HEX("counted as a bad descriptor", 1, r.stats.tx_bad_desc);
    CHECK_HEX("the ring still advanced", 1, get16(r.used + 2));
}

/* A descriptor TABLE that does not translate: the walk cannot even read the descriptor. */
static void test_a_descriptor_table_outside_the_map_is_refused(void) {
    rig_t r;

    rig_init_mapped(&r, HYPE_VIRTIO_NET_VQ_TX);
    avail_push(&r, 0, 0);
    r.dev.vq[HYPE_VIRTIO_NET_VQ_TX].desc = GBASE + sizeof(r) + 0x10000ull;

    CHECK_HEX("the chain is completed rather than read", 1,
              hype_virtio_net_drain_tx(&r.dev, &g_map, sink, &r, r.scratch, sizeof(r.scratch),
                                       &r.stats));
    CHECK_HEX("counted as a bad descriptor", 1, r.stats.tx_bad_desc);
    CHECK_HEX("nothing transmitted", 0, sink_calls);
}

/* Ring addresses that do not translate at all: refused before anything is consumed, so the guest is
 * never told a descriptor was taken. */
static void test_untranslatable_rings_consume_nothing(void) {
    rig_t r;
    const uint8_t frame[4] = {1, 2, 3, 4};

    rig_init_mapped(&r, HYPE_VIRTIO_NET_VQ_TX);
    r.dev.vq[HYPE_VIRTIO_NET_VQ_TX].driver = GBASE + sizeof(r) + 0x20000ull;
    CHECK_HEX("an unreachable avail ring is refused", -1,
              hype_virtio_net_drain_tx(&r.dev, &g_map, sink, &r, r.scratch, sizeof(r.scratch),
                                       &r.stats));
    CHECK_HEX("nothing was completed", 0, get16(r.used + 2));

    rig_init_mapped(&r, HYPE_VIRTIO_NET_VQ_TX);
    r.dev.vq[HYPE_VIRTIO_NET_VQ_TX].device = GBASE + sizeof(r) + 0x20000ull;
    CHECK_HEX("an unreachable used ring is refused", -1,
              hype_virtio_net_drain_tx(&r.dev, &g_map, sink, &r, r.scratch, sizeof(r.scratch),
                                       &r.stats));

    rig_init_mapped(&r, HYPE_VIRTIO_NET_VQ_RX);
    r.dev.vq[HYPE_VIRTIO_NET_VQ_RX].driver = GBASE + sizeof(r) + 0x20000ull;
    CHECK_HEX("the receive side refuses the same way", -1,
              hype_virtio_net_deliver_rx(&r.dev, &g_map, frame, sizeof(frame), &r.stats));
}

static void test_rx_through_a_real_map_and_an_unmapped_buffer(void) {
    rig_t r;
    const uint8_t frame[4] = {0x71, 0x72, 0x73, 0x74};

    rig_init_mapped(&r, HYPE_VIRTIO_NET_VQ_RX);
    desc_g(&r, 0, r.buf[0], 128u, HYPE_VIRTQ_DESC_F_WRITE, 0);
    avail_push(&r, 0, 0);
    CHECK_HEX("delivered through the map", 1,
              hype_virtio_net_deliver_rx(&r.dev, &g_map, frame, sizeof(frame), &r.stats));
    CHECK_HEX("the frame landed after the header", 0x71, r.buf[0][12]);

    /* A receive buffer outside this VM's memory must not be written. */
    rig_init_mapped(&r, HYPE_VIRTIO_NET_VQ_RX);
    {
        uint8_t *d = r.desc;
        put64(d, GBASE + sizeof(r) + 0x1000ull);
        put32(d + 8, 128u);
        put16(d + 12, HYPE_VIRTQ_DESC_F_WRITE);
        put16(d + 14, 0);
    }
    avail_push(&r, 0, 0);
    CHECK_HEX("not delivered", 0,
              hype_virtio_net_deliver_rx(&r.dev, &g_map, frame, sizeof(frame), &r.stats));
    CHECK_HEX("but the descriptor came back so the ring keeps moving", 1, get16(r.used + 2));
}

/* Both entry points must work with stats == 0: the live caller may not want counters, and a null
 * dereference there would be a crash in the packet path. */
static void test_stats_are_optional(void) {
    rig_t r;
    const uint8_t frame[4] = {1, 2, 3, 4};

    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    r.buf[0][12] = 0x31;
    desc(&r, 0, r.buf[0], 13u, 0, 0);
    avail_push(&r, 0, 0);
    CHECK_HEX("transmit works with no stats block", 1,
              hype_virtio_net_drain_tx(&r.dev, 0, sink, &r, r.scratch, sizeof(r.scratch), 0));

    /* And the failure paths inside it, which each touch stats. */
    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    avail_push(&r, 0, (uint16_t)(QSZ + 2u));
    CHECK_HEX("a bad descriptor with no stats block", 1,
              hype_virtio_net_drain_tx(&r.dev, 0, sink, &r, r.scratch, sizeof(r.scratch), 0));

    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    sink_result = -1;
    r.buf[0][12] = 0x32;
    desc(&r, 0, r.buf[0], 13u, 0, 0);
    avail_push(&r, 0, 0);
    CHECK_HEX("a refused frame with no stats block", 1,
              hype_virtio_net_drain_tx(&r.dev, 0, sink, &r, r.scratch, sizeof(r.scratch), 0));

    rig_init(&r, HYPE_VIRTIO_NET_VQ_RX);
    CHECK_HEX("an empty receive ring with no stats block", 0,
              hype_virtio_net_deliver_rx(&r.dev, 0, frame, sizeof(frame), 0));

    rig_init(&r, HYPE_VIRTIO_NET_VQ_RX);
    desc(&r, 0, r.buf[0], 128u, HYPE_VIRTQ_DESC_F_WRITE, 0);
    avail_push(&r, 0, 0);
    CHECK_HEX("a delivery with no stats block", 1,
              hype_virtio_net_deliver_rx(&r.dev, 0, frame, sizeof(frame), 0));

    rig_init(&r, HYPE_VIRTIO_NET_VQ_RX);
    desc(&r, 0, r.buf[0], 20u, HYPE_VIRTQ_DESC_F_WRITE, 0);
    avail_push(&r, 0, 0);
    {
        uint8_t big[200];
        memset(big, 0x66, sizeof(big));
        CHECK_HEX("a short receive buffer with no stats block", 0,
                  hype_virtio_net_deliver_rx(&r.dev, 0, big, sizeof(big), 0));
    }
}

/* The head index is checked before the walk, but a `next` link partway down a chain is a second,
 * separate opportunity to leave the ring -- and it is checked in a different place. */
static void test_tx_next_link_outside_the_ring(void) {
    rig_t r;

    rig_init(&r, HYPE_VIRTIO_NET_VQ_TX);
    desc(&r, 0, r.buf[0], 12u, HYPE_VIRTQ_DESC_F_NEXT, (uint16_t)(QSZ + 4u));
    avail_push(&r, 0, 0);

    CHECK_HEX("completed", 1, drain(&r));
    CHECK_HEX("counted as a bad descriptor", 1, r.stats.tx_bad_desc);
    CHECK_HEX("nothing transmitted", 0, sink_calls);
}

static void test_rx_zero_length_descriptor_is_stepped_over(void) {
    rig_t r;
    const uint8_t frame[4] = {0xF1, 0xF2, 0xF3, 0xF4};

    rig_init(&r, HYPE_VIRTIO_NET_VQ_RX);
    desc(&r, 0, r.buf[0], 0u, HYPE_VIRTQ_DESC_F_WRITE | HYPE_VIRTQ_DESC_F_NEXT, 1);
    desc(&r, 1, r.buf[1], 128u, HYPE_VIRTQ_DESC_F_WRITE, 0);
    avail_push(&r, 0, 0);

    CHECK_HEX("delivered into the descriptor after the empty one", 1,
              hype_virtio_net_deliver_rx(&r.dev, 0, frame, sizeof(frame), &r.stats));
    CHECK_HEX("the frame starts after the header in the second buffer", 0xF1, r.buf[1][12]);
}

static void test_rx_looping_chain_terminates(void) {
    rig_t r;
    uint8_t frame[600];

    rig_init(&r, HYPE_VIRTIO_NET_VQ_RX);
    memset(frame, 0x44, sizeof(frame));
    /* Two small writable buffers pointing at each other: never enough room for the frame, so the
     * walk keeps following `next` until the guard stops it. */
    desc(&r, 0, r.buf[0], 16u, HYPE_VIRTQ_DESC_F_WRITE | HYPE_VIRTQ_DESC_F_NEXT, 1);
    desc(&r, 1, r.buf[1], 16u, HYPE_VIRTQ_DESC_F_WRITE | HYPE_VIRTQ_DESC_F_NEXT, 0);
    avail_push(&r, 0, 0);

    CHECK_HEX("not delivered, and it returned rather than spinning", 0,
              hype_virtio_net_deliver_rx(&r.dev, 0, frame, sizeof(frame), &r.stats));
    CHECK_HEX("the descriptor still came back", 1, get16(r.used + 2));
    CHECK_HEX("counted as a drop", 1, r.stats.rx_no_buffer);
}

int main(void) {
    test_tx_one_frame_skips_the_header();
    test_tx_header_split_across_descriptors();
    test_tx_header_and_payload_in_one_descriptor_boundary();
    test_tx_several_chains_drain_in_one_call();
    test_a_malformed_chain_is_still_completed();
    test_a_looping_chain_terminates();
    test_a_header_only_chain_is_completed_and_is_not_a_frame();
    test_a_refused_frame_is_dropped_not_retried();
    test_oversized_frame_is_dropped_rather_than_clipped();
    test_tx_refusals();
    test_rx_delivers_a_frame_with_a_zeroed_header();
    test_rx_with_no_posted_buffer_is_zero_not_an_error();
    test_rx_spanning_two_descriptors();
    test_rx_refuses_a_non_writable_descriptor();
    test_rx_short_buffer_is_a_drop_not_a_partial_delivery();
    test_rx_head_out_of_range_is_completed();
    test_rx_refusals();
    test_legacy_header_length_is_honoured();
    test_zero_length_descriptor_in_a_chain();
    test_tx_through_a_real_bounds_checked_map();
    test_tx_descriptor_outside_the_map_is_refused_not_read();
    test_a_descriptor_table_outside_the_map_is_refused();
    test_untranslatable_rings_consume_nothing();
    test_rx_through_a_real_map_and_an_unmapped_buffer();
    test_stats_are_optional();
    test_tx_next_link_outside_the_ring();
    test_rx_zero_length_descriptor_is_stepped_over();
    test_rx_looping_chain_terminates();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
