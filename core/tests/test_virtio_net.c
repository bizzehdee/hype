#include <stdio.h>
#include <string.h>
#include "../../devices/virtio_net.h"

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
    do { \
        if (!(cond)) { \
            printf("FAIL: %s\n", (desc)); \
            failures++; \
        } \
    } while (0)

static const uint8_t GOOD_MAC[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

static uint32_t cread(const hype_virtio_net_t *dev, uint32_t offset, uint8_t size) {
    uint32_t value = 0xDEADBEEFu;
    if (hype_virtio_net_common_cfg_read(dev, offset, size, &value) != 0) {
        printf("FAIL: common read(0x%x, %u) unexpectedly rejected\n", offset, size);
        failures++;
    }
    return value;
}

static void cwrite(hype_virtio_net_t *dev, uint32_t offset, uint8_t size, uint32_t value) {
    if (hype_virtio_net_common_cfg_write(dev, offset, size, value) != 0) {
        printf("FAIL: common write(0x%x, %u) unexpectedly rejected\n", offset, size);
        failures++;
    }
}

/* Drives the negotiation a real driver performs, so the ready/queue tests start from a state a
 * guest could actually have produced rather than one hand-set field by field. */
static void bring_up_queue(hype_virtio_net_t *dev, unsigned int q, uint64_t base) {
    cwrite(dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, 4, 1u);
    cwrite(dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 4,
           1u << (HYPE_VIRTIO_F_VERSION_1_BIT - 32u));
    cwrite(dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 2, q);
    cwrite(dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO, 4, (uint32_t)(base & 0xFFFFFFFFu));
    cwrite(dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI, 4, (uint32_t)(base >> 32));
    cwrite(dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO, 4, (uint32_t)((base + 0x1000u) & 0xFFFFFFFFu));
    cwrite(dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI, 4, 0u);
    cwrite(dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO, 4, (uint32_t)((base + 0x2000u) & 0xFFFFFFFFu));
    cwrite(dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI, 4, 0u);
    cwrite(dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE, 2, 1u);
    cwrite(dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS, 1, HYPE_VIRTIO_STATUS_DRIVER_OK);
}

static void test_reset_state(void) {
    hype_virtio_net_t dev;
    unsigned int q;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    CHECK_HEX("device_status starts at 0", 0, dev.device_status);
    CHECK_HEX("bus mastering defaults on", 1, dev.bus_master);
    CHECK_HEX("the MAC came from reset", 0x52, dev.mac[0]);
    CHECK_HEX("the MAC's last byte came from reset", 0x56, dev.mac[5]);
    for (q = 0; q < HYPE_VIRTIO_NET_NUM_QUEUES; q++) {
        CHECK_HEX("queue size defaults to this device's max", HYPE_VIRTIO_NET_QUEUE_SIZE_MAX,
                  dev.vq[q].size);
        CHECK_HEX("queue starts disabled", 0, dev.vq[q].enable);
        CHECK_HEX("queue desc starts at 0", 0, dev.vq[q].desc);
    }
    hype_virtio_net_reset(0, GOOD_MAC); /* must not fault */
}

static void test_two_queues_are_advertised(void) {
    hype_virtio_net_t dev;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    CHECK_HEX("num_queues is 2 -- a NIC needs receive AND transmit",
              HYPE_VIRTIO_NET_NUM_QUEUES, cread(&dev, HYPE_VIRTIO_COMMON_CFG_NUM_QUEUES, 2));
}

/*
 * The bug this catches is the one virtio-blk's single-queue shortcut would have produced if copied:
 * queue registers that ignore queue_select. Both queues must hold their own addresses.
 */
static void test_queue_registers_are_per_queue(void) {
    hype_virtio_net_t dev;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    bring_up_queue(&dev, HYPE_VIRTIO_NET_VQ_RX, 0x11000000ull);
    bring_up_queue(&dev, HYPE_VIRTIO_NET_VQ_TX, 0x22000000ull);

    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 2, HYPE_VIRTIO_NET_VQ_RX);
    CHECK_HEX("RX keeps its own desc address", 0x11000000u,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO, 4));
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 2, HYPE_VIRTIO_NET_VQ_TX);
    CHECK_HEX("TX keeps its own desc address", 0x22000000u,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO, 4));
    CHECK_HEX("TX's driver ring address is its own", 0x22001000u,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO, 4));
    CHECK_HEX("TX's device ring address is its own", 0x22002000u,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO, 4));
}

/* Each queue must get its own doorbell, or hype cannot tell a transmit notify from a receive one. */
static void test_notify_off_is_distinct_per_queue(void) {
    hype_virtio_net_t dev;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 2, HYPE_VIRTIO_NET_VQ_RX);
    CHECK_HEX("RX notify_off", HYPE_VIRTIO_NET_VQ_RX,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_NOTIFY_OFF, 2));
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 2, HYPE_VIRTIO_NET_VQ_TX);
    CHECK_HEX("TX notify_off differs from RX's", HYPE_VIRTIO_NET_VQ_TX,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_NOTIFY_OFF, 2));
}

static void test_out_of_range_queue_select_reads_zero(void) {
    hype_virtio_net_t dev;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 2, 7u);
    CHECK_HEX("select is retained, not clamped -- the driver must be able to read it back", 7u,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 2));
    CHECK_HEX("a queue that does not exist reports size 0", 0,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2));
    CHECK_HEX("...and enable 0", 0, cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE, 2));
    CHECK_HEX("...and desc 0", 0, cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO, 4));
    CHECK_HEX("...and desc high 0", 0, cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI, 4));
    CHECK_HEX("...and driver 0", 0, cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO, 4));
    CHECK_HEX("...and driver high 0", 0, cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI, 4));
    CHECK_HEX("...and device 0", 0, cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO, 4));
    CHECK_HEX("...and device high 0", 0, cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI, 4));
    CHECK_HEX("...and notify_off 0", 0, cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_NOTIFY_OFF, 2));

    /* Writes to a non-existent queue are accepted and dropped -- they must not touch a real one. */
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO, 4, 0xAAAAAAAAu);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI, 4, 0xBBBBBBBBu);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO, 4, 0xAAAAAAAAu);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI, 4, 0xBBBBBBBBu);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO, 4, 0xAAAAAAAAu);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI, 4, 0xBBBBBBBBu);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2, 8u);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE, 2, 1u);
    CHECK_HEX("RX desc untouched by writes aimed at a phantom queue", 0, dev.vq[0].desc);
    CHECK_HEX("RX enable untouched", 0, dev.vq[0].enable);
    CHECK_HEX("TX desc untouched", 0, dev.vq[1].desc);
}

static void test_features_offered(void) {
    hype_virtio_net_t dev;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT, 4, 0u);
    CHECK_HEX("select reads back", 0, cread(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT, 4));
    CHECK_HEX("word 0 offers F_MAC and nothing else", 1u << HYPE_VIRTIO_NET_F_MAC_BIT,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE, 4));
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT, 4, 1u);
    CHECK_HEX("word 1 offers VERSION_1", 1u << (HYPE_VIRTIO_F_VERSION_1_BIT - 32u),
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE, 4));
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT, 4, 2u);
    CHECK_HEX("word 2 and beyond offer nothing", 0,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE, 4));
}

static void test_driver_features_accumulate_across_both_halves(void) {
    hype_virtio_net_t dev;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, 4, 0u);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 4, 1u << HYPE_VIRTIO_NET_F_MAC_BIT);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, 4, 1u);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 4,
           1u << (HYPE_VIRTIO_F_VERSION_1_BIT - 32u));
    /* The second write must not erase the first: a driver writes the halves one at a time. */
    CHECK_HEX("low half survived the high write", 1u << HYPE_VIRTIO_NET_F_MAC_BIT,
              (uint32_t)(dev.driver_features & 0xFFFFFFFFu));
    CHECK_HEX("high half took", 1u << (HYPE_VIRTIO_F_VERSION_1_BIT - 32u),
              (uint32_t)(dev.driver_features >> 32));
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, 4, 0u);
    CHECK_HEX("low half reads back", 1u << HYPE_VIRTIO_NET_F_MAC_BIT,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 4));
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, 4, 1u);
    CHECK_HEX("high half reads back", 1u << (HYPE_VIRTIO_F_VERSION_1_BIT - 32u),
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 4));
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, 4, 5u);
    CHECK_HEX("an out-of-range half reads 0", 0,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 4));
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 4, 0xFFFFFFFFu);
    CHECK_HEX("...and a write through it changes nothing", 1u << HYPE_VIRTIO_NET_F_MAC_BIT,
              (uint32_t)(dev.driver_features & 0xFFFFFFFFu));
}

static void test_device_reset_clears_negotiation_but_keeps_the_mac(void) {
    hype_virtio_net_t dev;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    bring_up_queue(&dev, HYPE_VIRTIO_NET_VQ_TX, 0x33000000ull);
    CHECK_TRUE("TX is ready before the reset",
               hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_TX));

    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS, 1, 0u);
    CHECK_HEX("status cleared", 0, dev.device_status);
    CHECK_HEX("negotiated features cleared", 0, dev.driver_features);
    CHECK_HEX("the queue address is cleared -- the ring is gone", 0, dev.vq[1].desc);
    CHECK_HEX("the queue is disabled", 0, dev.vq[1].enable);
    CHECK_TRUE("...so the queue is not ready",
               !hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_TX));
    /* Identity survives: a card whose MAC changed under a driver reset would look swapped. */
    CHECK_HEX("the MAC survived the driver reset", 0x52, dev.mac[0]);
    CHECK_HEX("...all of it", 0x56, dev.mac[5]);
}

static void test_mac_is_readable_a_byte_at_a_time(void) {
    hype_virtio_net_t dev;
    uint32_t v = 0;
    unsigned int i;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    for (i = 0; i < HYPE_VIRTIO_NET_MAC_BYTES; i++) {
        CHECK_HEX("MAC byte", GOOD_MAC[i],
                  (hype_virtio_net_device_cfg_read(&dev, i, 1, &v) == 0) ? v : 0xFFFFFFFFu);
    }
    CHECK_HEX("a 2-byte read returns little-endian pairs", 0x5452u,
              (hype_virtio_net_device_cfg_read(&dev, 0, 2, &v) == 0) ? v : 0u);
    CHECK_HEX("a 4-byte read at 0 is in range", 0x12005452u,
              (hype_virtio_net_device_cfg_read(&dev, 0, 4, &v) == 0) ? v : 0u);
}

/*
 * The whole access must be in range, not just its first byte. A 4-byte read at offset 4 covers
 * bytes 4..7, and only 4 and 5 exist -- returning two MAC bytes plus two bytes of something else
 * is the failure mode, and it is silent.
 */
static void test_device_cfg_rejects_a_read_that_runs_off_the_end(void) {
    hype_virtio_net_t dev;
    uint32_t v = 0;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    CHECK_HEX("4 bytes at offset 4 straddles the end and is refused", -1,
              hype_virtio_net_device_cfg_read(&dev, 4, 4, &v));
    CHECK_HEX("2 bytes at offset 5 straddles the end and is refused", -1,
              hype_virtio_net_device_cfg_read(&dev, 5, 2, &v));
    CHECK_HEX("1 byte at offset 5 is the last valid byte", 0,
              hype_virtio_net_device_cfg_read(&dev, 5, 1, &v));
    CHECK_HEX("offset 6 -- where `status` would be if F_STATUS were offered -- is refused", -1,
              hype_virtio_net_device_cfg_read(&dev, 6, 1, &v));
    CHECK_HEX("an odd access width is refused", -1,
              hype_virtio_net_device_cfg_read(&dev, 0, 3, &v));
    CHECK_HEX("a null device is refused", -1, hype_virtio_net_device_cfg_read(0, 0, 1, &v));
    CHECK_HEX("a null out pointer is refused", -1,
              hype_virtio_net_device_cfg_read(&dev, 0, 1, 0));
}

static void test_unusable_macs_are_refused(void) {
    hype_virtio_net_t dev;
    const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t multicast[6] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};

    hype_virtio_net_reset(&dev, GOOD_MAC);
    CHECK_HEX("all-zero is not a source address", -1, hype_virtio_net_set_mac(&dev, zero));
    CHECK_HEX("broadcast is not a source address", -1, hype_virtio_net_set_mac(&dev, bcast));
    CHECK_HEX("a multicast address is not a unicast sender", -1,
              hype_virtio_net_set_mac(&dev, multicast));
    CHECK_HEX("a refusal keeps the previous address", 0x52, dev.mac[0]);
    CHECK_HEX("a null address is refused", -1, hype_virtio_net_set_mac(&dev, 0));
    CHECK_HEX("a null device is refused", -1, hype_virtio_net_set_mac(0, GOOD_MAC));

    /* A reset with no MAC keeps what is there rather than zeroing it -- an all-zero address would
     * be worse than a stale one. */
    hype_virtio_net_reset(&dev, 0);
    CHECK_HEX("reset with no MAC keeps the address", 0x52, dev.mac[0]);
}

static void test_isr_is_read_to_clear(void) {
    hype_virtio_net_t dev;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    CHECK_HEX("nothing pending after reset", 0, hype_virtio_net_isr_read(&dev));
    hype_virtio_net_raise_queue_interrupt(&dev);
    CHECK_HEX("the queue-interrupt bit is set", 0x1, hype_virtio_net_isr_read(&dev));
    CHECK_HEX("reading it cleared it", 0, hype_virtio_net_isr_read(&dev));
    hype_virtio_net_raise_queue_interrupt(0); /* must not fault */
    CHECK_HEX("a null device reads 0", 0, hype_virtio_net_isr_read(0));
}

static void test_queue_ready_requires_every_step_the_driver_takes(void) {
    hype_virtio_net_t dev;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    CHECK_TRUE("nothing is ready straight after reset",
               !hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_RX));

    bring_up_queue(&dev, HYPE_VIRTIO_NET_VQ_RX, 0x44000000ull);
    CHECK_TRUE("ready once the driver has done everything",
               hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_RX));
    CHECK_TRUE("the OTHER queue is not ready -- readiness is per queue",
               !hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_TX));

    /* Each clause, removed one at a time from a known-ready state. */
    dev.bus_master = 0;
    CHECK_TRUE("bus mastering off means the device cannot reach the ring",
               !hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_RX));
    dev.bus_master = 1;

    dev.vq[HYPE_VIRTIO_NET_VQ_RX].device = 0;
    CHECK_TRUE("no used-ring address: a NIC writes it on every frame, so 0 would be a write to GPA 0",
               !hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_RX));
    dev.vq[HYPE_VIRTIO_NET_VQ_RX].device = 0x44002000ull;

    dev.vq[HYPE_VIRTIO_NET_VQ_RX].driver = 0;
    CHECK_TRUE("no avail-ring address", !hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_RX));
    dev.vq[HYPE_VIRTIO_NET_VQ_RX].driver = 0x44001000ull;

    dev.vq[HYPE_VIRTIO_NET_VQ_RX].size = 0;
    CHECK_TRUE("a zero-length ring holds nothing",
               !hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_RX));
    dev.vq[HYPE_VIRTIO_NET_VQ_RX].size = 256;

    dev.vq[HYPE_VIRTIO_NET_VQ_RX].desc = 0;
    CHECK_TRUE("no descriptor-table address", !hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_RX));
    dev.vq[HYPE_VIRTIO_NET_VQ_RX].desc = 0x44000000ull;

    dev.driver_features = 0;
    CHECK_TRUE("without VERSION_1 the ring layout is not the one this device implements",
               !hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_RX));
    dev.driver_features = 1ull << HYPE_VIRTIO_F_VERSION_1_BIT;

    dev.device_status = HYPE_VIRTIO_STATUS_FEATURES_OK; /* negotiated, but not DRIVER_OK */
    CHECK_TRUE("FEATURES_OK is not permission to start",
               !hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_RX));
    dev.device_status = HYPE_VIRTIO_STATUS_DRIVER_OK;
    CHECK_TRUE("ready again with everything restored",
               hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_RX));

    CHECK_TRUE("a queue index past the last is never ready",
               !hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_NUM_QUEUES));
    CHECK_TRUE("a null device is never ready", !hype_virtio_net_is_queue_ready(0, 0));
}

/*
 * 12 bytes, not 10. `num_buffers` is present whenever VERSION_1 is negotiated, not only under
 * MRG_RXBUF -- reading the spec the other way shifts every frame by two bytes.
 */
static void test_header_length_follows_the_negotiated_features(void) {
    hype_virtio_net_t dev;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    CHECK_HEX("before negotiation the legacy header applies", HYPE_VIRTIO_NET_HDR_LEN_LEGACY,
              hype_virtio_net_hdr_len(&dev));
    dev.driver_features = 1ull << HYPE_VIRTIO_F_VERSION_1_BIT;
    CHECK_HEX("VERSION_1 means num_buffers is present, so 12", HYPE_VIRTIO_NET_HDR_LEN_MODERN,
              hype_virtio_net_hdr_len(&dev));
    CHECK_HEX("a null device answers with the modern length", HYPE_VIRTIO_NET_HDR_LEN_MODERN,
              hype_virtio_net_hdr_len(0));
}

static void test_queue_size_clamps_but_never_grows(void) {
    hype_virtio_net_t dev;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 2, HYPE_VIRTIO_NET_VQ_TX);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2, 64u);
    CHECK_HEX("a driver may shrink the ring", 64u,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2));
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2, 4096u);
    CHECK_HEX("...and is clamped, not obeyed, when it asks for more than the device has",
              HYPE_VIRTIO_NET_QUEUE_SIZE_MAX, cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2));
}

static void test_msix_is_answered_as_absent(void) {
    hype_virtio_net_t dev;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    CHECK_HEX("config MSI-X vector reads NO_VECTOR", 0xFFFFu,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_MSIX_CONFIG, 2));
    CHECK_HEX("queue MSI-X vector reads NO_VECTOR", 0xFFFFu,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_MSIX_VECTOR, 2));
    /* A driver may write one; it is accepted and does not take, which the read side reports. */
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_MSIX_VECTOR, 2, 3u);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_MSIX_CONFIG, 2, 4u);
    CHECK_HEX("still NO_VECTOR after a write", 0xFFFFu,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_MSIX_VECTOR, 2));
}

static void test_access_widths_and_bounds_are_enforced(void) {
    hype_virtio_net_t dev;
    uint32_t v = 0;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    CHECK_HEX("a 4-byte register read 2 bytes at a time is refused", -1,
              hype_virtio_net_common_cfg_read(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE, 2, &v));
    CHECK_HEX("a 1-byte register read as 4 is refused", -1,
              hype_virtio_net_common_cfg_read(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS, 4, &v));
    CHECK_HEX("a 2-byte register read as 1 is refused", -1,
              hype_virtio_net_common_cfg_read(&dev, HYPE_VIRTIO_COMMON_CFG_NUM_QUEUES, 1, &v));
    CHECK_HEX("config_generation is 1 byte", -1,
              hype_virtio_net_common_cfg_read(&dev, HYPE_VIRTIO_COMMON_CFG_CONFIG_GENERATION, 2, &v));
    CHECK_HEX("config_generation reads 0 -- the MAC never changes", 0,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_CONFIG_GENERATION, 1));
    CHECK_HEX("past the end of common cfg is refused", -1,
              hype_virtio_net_common_cfg_read(&dev, HYPE_VIRTIO_COMMON_CFG_SIZE, 4, &v));
    CHECK_HEX("an unassigned offset inside common cfg is refused", -1,
              hype_virtio_net_common_cfg_read(&dev, 0x13u, 1, &v));
    CHECK_HEX("a null device is refused", -1, hype_virtio_net_common_cfg_read(0, 0, 4, &v));
    CHECK_HEX("a null out pointer is refused", -1,
              hype_virtio_net_common_cfg_read(&dev, 0, 4, 0));

    CHECK_HEX("writes past the end are refused", -1,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_SIZE, 4, 0));
    CHECK_HEX("a null device write is refused", -1,
              hype_virtio_net_common_cfg_write(0, 0, 4, 0));
    CHECK_HEX("a mis-sized select write is refused", -1,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 4, 0));
    CHECK_HEX("a mis-sized status write is refused", -1,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS, 2, 0));
    CHECK_HEX("a mis-sized feature-select write is refused", -1,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT, 2, 0));
    CHECK_HEX("a mis-sized driver-feature write is refused", -1,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 1, 0));
    CHECK_HEX("a mis-sized driver-feature-select write is refused", -1,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, 1, 0));
    CHECK_HEX("a mis-sized queue-size write is refused", -1,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 4, 0));
    CHECK_HEX("a mis-sized queue-enable write is refused", -1,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE, 1, 0));
    CHECK_HEX("a mis-sized desc-lo write is refused", -1,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO, 2, 0));
    CHECK_HEX("a mis-sized desc-hi write is refused", -1,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI, 2, 0));
    CHECK_HEX("a mis-sized driver-lo write is refused", -1,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO, 2, 0));
    CHECK_HEX("a mis-sized driver-hi write is refused", -1,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI, 2, 0));
    CHECK_HEX("a mis-sized device-lo write is refused", -1,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO, 2, 0));
    CHECK_HEX("a mis-sized device-hi write is refused", -1,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI, 2, 0));

    /* Read-only registers ignore a write rather than failing -- the transport has no way to report
     * an error to the driver, and the spec makes them read-only. */
    CHECK_HEX("writing device_feature is ignored, not an error", 0,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE, 4, 0xFFu));
    CHECK_HEX("writing num_queues is ignored", 0,
              hype_virtio_net_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_NUM_QUEUES, 2, 9u));
    CHECK_HEX("num_queues is unchanged", HYPE_VIRTIO_NET_NUM_QUEUES,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_NUM_QUEUES, 2));
}

static void test_bus_master_mirror(void) {
    hype_virtio_net_t dev;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    hype_virtio_net_set_bus_master(&dev, 0);
    CHECK_HEX("cleared", 0, dev.bus_master);
    hype_virtio_net_set_bus_master(&dev, 7);
    CHECK_HEX("any nonzero means enabled", 1, dev.bus_master);
    hype_virtio_net_set_bus_master(0, 1); /* must not fault */
}

/* The high halves of every 64-bit queue address must survive independently of the low ones: a ring
 * above 4 GB whose high half was dropped points somewhere entirely different. */
static void test_queue_address_high_halves_are_independent(void) {
    hype_virtio_net_t dev;

    hype_virtio_net_reset(&dev, GOOD_MAC);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 2, HYPE_VIRTIO_NET_VQ_RX);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI, 4, 0x00000002u);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO, 4, 0x40001000u);
    CHECK_HEX("desc high half kept", 0x00000002u,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI, 4));
    CHECK_HEX("desc low half kept", 0x40001000u,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO, 4));
    CHECK_HEX("the 64-bit address is the two halves joined", 0x240001000ull, dev.vq[0].desc);
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI, 4, 0x00000003u);
    CHECK_HEX("driver high half kept", 0x00000003u,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI, 4));
    cwrite(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI, 4, 0x00000004u);
    CHECK_HEX("device high half kept", 0x00000004u,
              cread(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI, 4));
}

/*
 * Table-driven, because the width rule is the same for every register and a per-register test would
 * have covered the ones I happened to think of. Each entry is read at its own width (which must
 * succeed) and at a wrong one (which must be refused). That is what caught DRIVER_FEATURE_SELECT
 * never being read back at all -- a register a driver does read.
 */
static void test_every_register_answers_at_its_own_width_and_refuses_others(void) {
    hype_virtio_net_t dev;
    uint32_t v = 0;
    unsigned int i;
    struct { uint32_t off; uint8_t width; const char *name; } regs[] = {
        {HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT, 4, "device_feature_select"},
        {HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE, 4, "device_feature"},
        {HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, 4, "driver_feature_select"},
        {HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 4, "driver_feature"},
        {HYPE_VIRTIO_COMMON_CFG_MSIX_CONFIG, 2, "msix_config"},
        {HYPE_VIRTIO_COMMON_CFG_NUM_QUEUES, 2, "num_queues"},
        {HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS, 1, "device_status"},
        {HYPE_VIRTIO_COMMON_CFG_CONFIG_GENERATION, 1, "config_generation"},
        {HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 2, "queue_select"},
        {HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2, "queue_size"},
        {HYPE_VIRTIO_COMMON_CFG_QUEUE_MSIX_VECTOR, 2, "queue_msix_vector"},
        {HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE, 2, "queue_enable"},
        {HYPE_VIRTIO_COMMON_CFG_QUEUE_NOTIFY_OFF, 2, "queue_notify_off"},
        {HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO, 4, "queue_desc_lo"},
        {HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI, 4, "queue_desc_hi"},
        {HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO, 4, "queue_driver_lo"},
        {HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI, 4, "queue_driver_hi"},
        {HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO, 4, "queue_device_lo"},
        {HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI, 4, "queue_device_hi"},
    };

    hype_virtio_net_reset(&dev, GOOD_MAC);
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint8_t wrong = (regs[i].width == 4u) ? 2u : 4u;
        if (hype_virtio_net_common_cfg_read(&dev, regs[i].off, regs[i].width, &v) != 0) {
            printf("FAIL: %s refused a read at its own width %u\n", regs[i].name, regs[i].width);
            failures++;
        }
        if (hype_virtio_net_common_cfg_read(&dev, regs[i].off, wrong, &v) != -1) {
            printf("FAIL: %s accepted a %u-byte read of a %u-byte register\n", regs[i].name, wrong,
                   regs[i].width);
            failures++;
        }
        /* Writes take the same rule. A read-only register returns 0 for a write of ANY width -- it
         * ignores the write rather than validating it, which is deliberate and worth pinning. */
        if (hype_virtio_net_common_cfg_write(&dev, regs[i].off, wrong, 0u) == 0) {
            switch (regs[i].off) {
                case HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE:
                case HYPE_VIRTIO_COMMON_CFG_NUM_QUEUES:
                case HYPE_VIRTIO_COMMON_CFG_CONFIG_GENERATION:
                case HYPE_VIRTIO_COMMON_CFG_QUEUE_NOTIFY_OFF:
                case HYPE_VIRTIO_COMMON_CFG_MSIX_CONFIG:
                case HYPE_VIRTIO_COMMON_CFG_QUEUE_MSIX_VECTOR:
                    break; /* read-only or discarded: ignoring the write is the contract */
                default:
                    printf("FAIL: %s accepted a %u-byte write\n", regs[i].name, wrong);
                    failures++;
                    break;
            }
        }
    }
}

int main(void) {
    test_reset_state();
    test_two_queues_are_advertised();
    test_queue_registers_are_per_queue();
    test_notify_off_is_distinct_per_queue();
    test_out_of_range_queue_select_reads_zero();
    test_features_offered();
    test_driver_features_accumulate_across_both_halves();
    test_device_reset_clears_negotiation_but_keeps_the_mac();
    test_mac_is_readable_a_byte_at_a_time();
    test_device_cfg_rejects_a_read_that_runs_off_the_end();
    test_unusable_macs_are_refused();
    test_isr_is_read_to_clear();
    test_queue_ready_requires_every_step_the_driver_takes();
    test_header_length_follows_the_negotiated_features();
    test_queue_size_clamps_but_never_grows();
    test_msix_is_answered_as_absent();
    test_access_widths_and_bounds_are_enforced();
    test_bus_master_mirror();
    test_queue_address_high_halves_are_independent();
    test_every_register_answers_at_its_own_width_and_refuses_others();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
