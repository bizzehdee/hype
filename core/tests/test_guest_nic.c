#include <stdio.h>
#include <string.h>
#include "../guest_nic.h"
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

/*
 * WHAT THIS FILE TESTS IS THE CLAIM, not the plumbing.
 *
 * hype_guest_nic_ops_t exists so the forwarding plane -- proxy ARP, address learning, the on-link
 * check, NAPT, the peer mailbox -- can be written once for both frontends. That claim is only true if
 * the two behave the same at this interface, so every test here drives BOTH through the vtable and
 * asserts they agree. A test that exercised one and trusted the other would be testing the
 * forwarding for exactly half the guests hype can run.
 *
 * The frontends' own internals are covered by test_virtio_net*.c and test_e1000_dev*.c. Here the
 * subject is the equivalence.
 */

#define RING 8u

static const uint8_t MAC[6] = {0x52, 0x54, 0x00, 0xAB, 0xCD, 0x01};

static uint8_t sink_frame[HYPE_VIRTIO_NET_MAX_FRAME_LEN];
static unsigned int sink_len;
static unsigned int sink_calls;

static int sink(void *user, const uint8_t *frame, unsigned int len) {
    (void)user;
    sink_calls++;
    sink_len = len;
    if (len <= sizeof(sink_frame)) {
        memcpy(sink_frame, frame, len);
    }
    return 0;
}

/* --- a virtio-net instance, brought up as a driver would --- */
typedef struct {
    uint8_t desc[RING * 16u];
    uint8_t avail[4u + 2u * RING + 2u];
    uint8_t used[4u + 8u * RING + 2u];
    uint8_t buf[RING][2048];
    hype_virtio_net_t dev;
} vrig_t;

/* --- an e1000 instance, likewise --- */
typedef struct {
    uint8_t tx_ring[RING * HYPE_E1000_DESC_BYTES];
    uint8_t rx_ring[RING * HYPE_E1000_DESC_BYTES];
    uint8_t buf[RING][HYPE_E1000_BUF_BYTES];
    hype_e1000_dev_t dev;
} erig_t;

static uint8_t scratch[HYPE_VIRTIO_NET_MAX_FRAME_LEN];

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu); p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void vrig_up(vrig_t *r) {
    memset(r, 0, sizeof(*r));
    hype_virtio_net_reset(&r->dev, MAC);
    r->dev.driver_features = 1ull << HYPE_VIRTIO_F_VERSION_1_BIT;
    r->dev.device_status = HYPE_VIRTIO_STATUS_DRIVER_OK;
    {
        unsigned int q;
        for (q = 0; q < HYPE_VIRTIO_NET_NUM_QUEUES; q++) {
            r->dev.vq[q].size = RING;
            r->dev.vq[q].enable = 1;
            r->dev.vq[q].desc = (uint64_t)(uintptr_t)r->desc;
            r->dev.vq[q].driver = (uint64_t)(uintptr_t)r->avail;
            r->dev.vq[q].device = (uint64_t)(uintptr_t)r->used;
        }
    }
}

static void erig_up(erig_t *r) {
    memset(r, 0, sizeof(*r));
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

/* Both frontends must report the SAME MAC through the interface, because the forwarding plane
 * identifies a guest by it -- and `os_hint` changing which device a guest sees must not change who
 * the guest IS to every conntrack entry and peer rule. */
static void test_both_report_the_same_mac(void) {
    vrig_t v;
    erig_t e;
    uint8_t mv[6];
    uint8_t me[6];
    unsigned int i;

    vrig_up(&v);
    erig_up(&e);
    CHECK_HEX("virtio reports its MAC", 0, hype_guest_nic_virtio.mac(&v.dev, mv));
    CHECK_HEX("e1000 reports its MAC", 0, hype_guest_nic_e1000.mac(&e.dev, me));
    for (i = 0; i < 6u; i++) {
        CHECK_HEX("the two agree octet by octet", mv[i], me[i]);
        CHECK_HEX("...and match what was installed", MAC[i], mv[i]);
    }

    CHECK_HEX("a null device is refused (virtio)", -1, hype_guest_nic_virtio.mac(0, mv));
    CHECK_HEX("a null device is refused (e1000)", -1, hype_guest_nic_e1000.mac(0, me));
    CHECK_HEX("a null out pointer is refused (virtio)", -1,
              hype_guest_nic_virtio.mac(&v.dev, 0));
    CHECK_HEX("a null out pointer is refused (e1000)", -1, hype_guest_nic_e1000.mac(&e.dev, 0));
}

/* The names exist so a diagnostic can say WHICH NIC. A host with two kinds of guest NIC and a log
 * line that says "the NIC" is a log line that cannot be acted on. */
static void test_the_frontends_are_named_and_distinct(void) {
    CHECK_TRUE("virtio is named", hype_guest_nic_virtio.name != 0);
    CHECK_TRUE("e1000 is named", hype_guest_nic_e1000.name != 0);
    CHECK_TRUE("the names differ", strcmp(hype_guest_nic_virtio.name, hype_guest_nic_e1000.name) != 0);
}

/*
 * THE CENTRAL EQUIVALENCE: one frame in, through either frontend, and the guest gets the same bytes.
 * The rings could not be less alike -- virtio has an avail ring the driver publishes and a used ring
 * the device fills; the e1000 has one ring with a head the device owns -- and the plane above must
 * see no difference.
 */
static void test_deliver_rx_puts_the_same_bytes_in_the_guest(void) {
    vrig_t v;
    erig_t e;
    const uint8_t frame[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34};
    unsigned int i;

    /* virtio: one writable descriptor, published on the avail ring. */
    vrig_up(&v);
    put64(v.desc, (uint64_t)(uintptr_t)v.buf[0]);
    put32(v.desc + 8, 2048u);
    put16(v.desc + 12, HYPE_VIRTQ_DESC_F_WRITE);
    put16(v.avail + 4, 0u);
    put16(v.avail + 2, 1u);
    CHECK_HEX("virtio delivered", 1,
              hype_guest_nic_virtio.deliver_rx(&v.dev, 0, frame, sizeof(frame), 0));

    /* e1000: one descriptor, and RDT one ahead of RDH is what "a buffer is posted" means here. */
    erig_up(&e);
    put64(e.rx_ring, (uint64_t)(uintptr_t)e.buf[0]);
    e.dev.rdh = 0u;
    e.dev.rdt = 1u;
    CHECK_HEX("e1000 delivered", 1,
              hype_guest_nic_e1000.deliver_rx(&e.dev, 0, frame, sizeof(frame), 0));

    /*
     * The frame bytes match. They are at different OFFSETS -- virtio prefixes its 12-byte
     * virtio-net header, the e1000 does not -- and that difference is the frontends' business, not
     * the plane's. What must match is the frame.
     */
    for (i = 0; i < sizeof(frame); i++) {
        CHECK_HEX("virtio frame byte", frame[i], v.buf[0][HYPE_VIRTIO_NET_HDR_LEN_MODERN + i]);
        CHECK_HEX("e1000 frame byte", frame[i], e.buf[0][i]);
    }
    CHECK_HEX("the e1000 descriptor reports the frame length", sizeof(frame),
              get16(e.rx_ring + 8));
}

/*
 * AN EMPTY RECEIVE RING REPORTS 0, NOT AN ERROR, in both -- and this is the one that would be
 * easiest to get wrong, because the two devices signal "empty" in opposite ways. virtio: the avail
 * index has not moved past what the device consumed. e1000: head and tail are EQUAL. The plane treats
 * 0 as "drop this frame, the guest is between polls", so a frontend returning -1 here would turn
 * ordinary operation into a logged fault.
 */
static void test_an_empty_ring_reports_zero_in_both(void) {
    vrig_t v;
    erig_t e;
    const uint8_t frame[4] = {1, 2, 3, 4};

    vrig_up(&v); /* nothing published on the avail ring */
    CHECK_HEX("virtio reports no buffer", 0,
              hype_guest_nic_virtio.deliver_rx(&v.dev, 0, frame, sizeof(frame), 0));

    erig_up(&e);
    e.dev.rdh = 0u;
    e.dev.rdt = 0u; /* equal: nothing posted */
    CHECK_HEX("e1000 reports no buffer", 0,
              hype_guest_nic_e1000.deliver_rx(&e.dev, 0, frame, sizeof(frame), 0));
}

/* And a frame the guest queued comes out through either frontend as the same bytes. */
static void test_drain_tx_yields_the_same_frame(void) {
    vrig_t v;
    erig_t e;
    unsigned int i;

    /* virtio: a 12-byte header the device must SKIP, then the frame. */
    vrig_up(&v);
    for (i = 0; i < HYPE_VIRTIO_NET_HDR_LEN_MODERN; i++) {
        v.buf[0][i] = 0xEE;
    }
    for (i = 0; i < 20u; i++) {
        v.buf[0][HYPE_VIRTIO_NET_HDR_LEN_MODERN + i] = (uint8_t)(0x70u + i);
    }
    put64(v.desc, (uint64_t)(uintptr_t)v.buf[0]);
    put32(v.desc + 8, HYPE_VIRTIO_NET_HDR_LEN_MODERN + 20u);
    put16(v.desc + 12, 0u);
    put16(v.avail + 4, 0u);
    put16(v.avail + 2, 1u);
    sink_calls = 0;
    CHECK_HEX("virtio drained one", 1,
              hype_guest_nic_virtio.drain_tx(&v.dev, 0, sink, 0, scratch, sizeof(scratch), 0));
    CHECK_HEX("the header was not part of the frame", 20, sink_len);
    CHECK_HEX("first frame byte", 0x70, sink_frame[0]);

    /* e1000: no header, and the tail write is what makes the descriptor available. */
    erig_up(&e);
    for (i = 0; i < 20u; i++) {
        e.buf[0][i] = (uint8_t)(0x70u + i);
    }
    put64(e.tx_ring, (uint64_t)(uintptr_t)e.buf[0]);
    put16(e.tx_ring + 8, 20u);
    e.tx_ring[11] = HYPE_E1000_TXD_CMD_EOP | HYPE_E1000_TXD_CMD_RS;
    e.dev.tdh = 0u;
    e.dev.tdt = 1u;
    sink_calls = 0;
    CHECK_HEX("e1000 drained one", 1,
              hype_guest_nic_e1000.drain_tx(&e.dev, 0, sink, 0, scratch, sizeof(scratch), 0));
    CHECK_HEX("the same length came out", 20, sink_len);
    CHECK_HEX("...and the same first byte", 0x70, sink_frame[0]);
    for (i = 0; i < 20u; i++) {
        CHECK_HEX("every byte matches what the guest queued", 0x70u + i, sink_frame[i]);
    }
}

/* An empty TRANSMIT ring: nothing drained, no error, in both. */
static void test_an_empty_transmit_ring_drains_nothing(void) {
    vrig_t v;
    erig_t e;

    vrig_up(&v);
    CHECK_HEX("virtio has nothing to send", 0,
              hype_guest_nic_virtio.drain_tx(&v.dev, 0, sink, 0, scratch, sizeof(scratch), 0));

    erig_up(&e);
    e.dev.tdh = 0u;
    e.dev.tdt = 0u;
    CHECK_HEX("e1000 has nothing to send", 0,
              hype_guest_nic_e1000.drain_tx(&e.dev, 0, sink, 0, scratch, sizeof(scratch), 0));
}

/*
 * A frontend whose driver has not finished bringing it up must refuse, in both -- the plane relies on
 * -1 meaning "do not try", and a frontend that walked an unpublished ring would be reading whatever
 * the guest happened to have at guest-physical 0.
 */
static void test_an_unready_frontend_refuses(void) {
    vrig_t v;
    erig_t e;
    const uint8_t frame[4] = {1, 2, 3, 4};

    vrig_up(&v);
    v.dev.device_status = 0; /* DRIVER_OK withdrawn */
    CHECK_HEX("virtio refuses transmit", -1,
              hype_guest_nic_virtio.drain_tx(&v.dev, 0, sink, 0, scratch, sizeof(scratch), 0));
    CHECK_HEX("virtio refuses receive", -1,
              hype_guest_nic_virtio.deliver_rx(&v.dev, 0, frame, sizeof(frame), 0));

    erig_up(&e);
    e.dev.tctl = 0u;
    e.dev.rctl = 0u;
    CHECK_HEX("e1000 refuses transmit", -1,
              hype_guest_nic_e1000.drain_tx(&e.dev, 0, sink, 0, scratch, sizeof(scratch), 0));
    CHECK_HEX("e1000 refuses receive", -1,
              hype_guest_nic_e1000.deliver_rx(&e.dev, 0, frame, sizeof(frame), 0));
}

int main(void) {
    test_both_report_the_same_mac();
    test_the_frontends_are_named_and_distinct();
    test_deliver_rx_puts_the_same_bytes_in_the_guest();
    test_an_empty_ring_reports_zero_in_both();
    test_drain_tx_yields_the_same_frame();
    test_an_empty_transmit_ring_drains_nothing();
    test_an_unready_frontend_refuses();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
