#include <stdio.h>
#include "../../devices/virtio_blk.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static uint32_t common_read(const hype_virtio_blk_t *dev, uint32_t offset, uint8_t size) {
    uint32_t value = 0xDEADBEEFu;
    int rc = hype_virtio_blk_common_cfg_read(dev, offset, size, &value);
    if (rc != 0) {
        printf("FAIL: common_read(0x%x, %u) unexpectedly rejected\n", offset, size);
        failures++;
    }
    return value;
}

static void common_write(hype_virtio_blk_t *dev, uint32_t offset, uint8_t size, uint32_t value) {
    int rc = hype_virtio_blk_common_cfg_write(dev, offset, size, value);
    if (rc != 0) {
        printf("FAIL: common_write(0x%x, %u) unexpectedly rejected\n", offset, size);
        failures++;
    }
}

static void test_reset_sets_capacity_and_default_queue_size(void) {
    hype_virtio_blk_t dev;

    hype_virtio_blk_reset(&dev, 204800ull);
    CHECK_HEX("capacity is set from reset's own parameter", 204800ull, dev.capacity_sectors);
    CHECK_HEX("device_status starts at 0", 0, dev.device_status);
    CHECK_HEX("queue_size defaults to this project's own max", HYPE_VIRTIO_BLK_QUEUE_SIZE_MAX,
              dev.queue_size);
    CHECK_HEX("queue_enable starts disabled", 0, dev.queue_enable);
    CHECK_HEX("queue_desc starts at 0", 0, dev.queue_desc);
}

static void test_feature_negotiation_offers_only_version_1(void) {
    hype_virtio_blk_t dev;
    uint32_t value;

    hype_virtio_blk_reset(&dev, 1);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT, 4u, 0u);
    CHECK_HEX("device_feature_select reads back", 0u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT, 4u));
    value = common_read(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE, 4u);
    CHECK_HEX("low feature word offers nothing", 0u, value);

    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT, 4u, 1u);
    CHECK_HEX("device_feature_select reads back after re-selecting", 1u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT, 4u));
    value = common_read(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE, 4u);
    CHECK_HEX("high feature word offers only VIRTIO_F_VERSION_1", 1u, value);
}

static void test_driver_feature_write_accumulates_across_both_halves(void) {
    hype_virtio_blk_t dev;

    hype_virtio_blk_reset(&dev, 1);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, 4u, 0u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 4u, 0xAAAAAAAAu);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, 4u, 1u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 4u, 0x00000001u);

    CHECK_HEX("driver_features combines both written halves",
              0x0000000100000000ull | 0xAAAAAAAAull, dev.driver_features);

    CHECK_HEX("driver_feature_select roundtrips", 1u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, 4u));
    CHECK_HEX("driver_feature high half reads back", 0x00000001u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 4u));
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, 4u, 0u);
    CHECK_HEX("driver_feature low half reads back", 0xAAAAAAAAu,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 4u));

    /* A select value beyond the two real 32-bit halves this project
     * models is a safe no-op on write and reads back 0. */
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, 4u, 2u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 4u, 0xFFFFFFFFu);
    CHECK_HEX("an out-of-range feature select is a safe no-op", 0u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 4u));
}

static void test_device_status_handshake(void) {
    hype_virtio_blk_t dev;

    hype_virtio_blk_reset(&dev, 1);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS, 1u, HYPE_VIRTIO_STATUS_ACKNOWLEDGE);
    CHECK_HEX("ACKNOWLEDGE recorded", HYPE_VIRTIO_STATUS_ACKNOWLEDGE, dev.device_status);

    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS, 1u,
                 HYPE_VIRTIO_STATUS_ACKNOWLEDGE | HYPE_VIRTIO_STATUS_DRIVER);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS, 1u,
                 HYPE_VIRTIO_STATUS_ACKNOWLEDGE | HYPE_VIRTIO_STATUS_DRIVER |
                     HYPE_VIRTIO_STATUS_FEATURES_OK);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS, 1u,
                 HYPE_VIRTIO_STATUS_ACKNOWLEDGE | HYPE_VIRTIO_STATUS_DRIVER |
                     HYPE_VIRTIO_STATUS_FEATURES_OK | HYPE_VIRTIO_STATUS_DRIVER_OK);
    CHECK_HEX("full handshake reads back", (uint32_t)(HYPE_VIRTIO_STATUS_ACKNOWLEDGE |
                                                        HYPE_VIRTIO_STATUS_DRIVER |
                                                        HYPE_VIRTIO_STATUS_FEATURES_OK |
                                                        HYPE_VIRTIO_STATUS_DRIVER_OK),
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS, 1u));

    /* Writing 0 is a full reset -- negotiation state clears, but the
     * fixed capacity property survives (it's a backing-buffer
     * property, not driver-negotiated state). */
    dev.capacity_sectors = 999;
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS, 1u, 0u);
    CHECK_HEX("writing 0 resets device_status", 0u, dev.device_status);
    CHECK_HEX("writing 0 does not touch capacity", 999ull, dev.capacity_sectors);
}

static void test_queue_registers_only_apply_to_queue_zero(void) {
    hype_virtio_blk_t dev;

    hype_virtio_blk_reset(&dev, 1);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 2u, 0u);
    CHECK_HEX("queue_select reads back", 0u, common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 2u));
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2u, 4u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE, 2u, 1u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO, 4u, 0x11110000u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI, 4u, 0x00000001u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO, 4u, 0x22220000u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI, 4u, 0x00000002u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO, 4u, 0x33330000u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI, 4u, 0x00000003u);

    CHECK_HEX("queue_size for queue 0", 4u, common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2u));
    CHECK_HEX("queue_enable for queue 0", 1u, common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE, 2u));
    CHECK_HEX("queue_desc combines both halves", 0x0000000111110000ull, dev.queue_desc);
    CHECK_HEX("queue_driver combines both halves", 0x0000000222220000ull, dev.queue_driver);
    CHECK_HEX("queue_device combines both halves", 0x0000000333330000ull, dev.queue_device);
    CHECK_HEX("queue_desc_lo reads back", 0x11110000u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO, 4u));
    CHECK_HEX("queue_desc_hi reads back", 0x00000001u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI, 4u));
    CHECK_HEX("queue_driver_lo reads back", 0x22220000u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO, 4u));
    CHECK_HEX("queue_driver_hi reads back", 0x00000002u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI, 4u));
    CHECK_HEX("queue_device_lo reads back", 0x33330000u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO, 4u));
    CHECK_HEX("queue_device_hi reads back", 0x00000003u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI, 4u));

    /* This project's own single-queue scope: selecting any queue other
     * than 0 must not touch (or expose) the one real queue's state. */
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 2u, 1u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2u, 7u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE, 2u, 1u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO, 4u, 0xFFFFFFFFu);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI, 4u, 0xFFFFFFFFu);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO, 4u, 0xFFFFFFFFu);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI, 4u, 0xFFFFFFFFu);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO, 4u, 0xFFFFFFFFu);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI, 4u, 0xFFFFFFFFu);
    CHECK_HEX("queue_size for a nonexistent queue reads 0", 0u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2u));
    CHECK_HEX("queue_enable for a nonexistent queue reads 0", 0u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE, 2u));
    CHECK_HEX("queue_desc_lo for a nonexistent queue reads 0", 0u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO, 4u));
    CHECK_HEX("queue_desc_hi for a nonexistent queue reads 0", 0u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI, 4u));
    CHECK_HEX("queue_driver_lo for a nonexistent queue reads 0", 0u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO, 4u));
    CHECK_HEX("queue_driver_hi for a nonexistent queue reads 0", 0u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI, 4u));
    CHECK_HEX("queue_device_lo for a nonexistent queue reads 0", 0u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO, 4u));
    CHECK_HEX("queue_device_hi for a nonexistent queue reads 0", 0u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI, 4u));
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 2u, 0u);
    CHECK_HEX("queue 0's own state was left untouched", 4u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2u));
}

static void test_queue_size_write_is_clamped_to_project_max(void) {
    hype_virtio_blk_t dev;

    hype_virtio_blk_reset(&dev, 1);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2u, HYPE_VIRTIO_BLK_QUEUE_SIZE_MAX + 100u);
    CHECK_HEX("an oversized queue_size write is clamped to this project's own max",
              HYPE_VIRTIO_BLK_QUEUE_SIZE_MAX, common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2u));
}

static void test_read_only_and_unmodeled_registers(void) {
    hype_virtio_blk_t dev;

    hype_virtio_blk_reset(&dev, 1);
    CHECK_HEX("num_queues is always 1", 1u, common_read(&dev, HYPE_VIRTIO_COMMON_CFG_NUM_QUEUES, 2u));
    CHECK_HEX("config_generation is always 0", 0u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_CONFIG_GENERATION, 1u));
    CHECK_HEX("queue_notify_off is always 0 (single queue)", 0u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_NOTIFY_OFF, 2u));
    CHECK_HEX("msix_config reads NO_VECTOR", 0xFFFFu,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_MSIX_CONFIG, 2u));
    CHECK_HEX("queue_msix_vector reads NO_VECTOR", 0xFFFFu,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_MSIX_VECTOR, 2u));

    /* Writes to read-only/unmodeled registers are silently ignored,
     * not rejected. */
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_NUM_QUEUES, 2u, 99u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_CONFIG_GENERATION, 1u, 99u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_NOTIFY_OFF, 2u, 99u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_MSIX_CONFIG, 2u, 1u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_MSIX_VECTOR, 2u, 1u);
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE, 4u, 0xFFFFFFFFu);
    CHECK_HEX("num_queues is unaffected by a write", 1u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_NUM_QUEUES, 2u));
    CHECK_HEX("device_feature is unaffected by a write", 0u,
              common_read(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE, 4u));
}

static void test_reserved_offset_reads_as_zero(void) {
    hype_virtio_blk_t dev;
    uint32_t value;
    int rc;

    hype_virtio_blk_reset(&dev, 1);
    /* Offset 0x36 falls after QUEUE_DEVICE_HI's own 4 bytes (0x34-0x37
     * inclusive is actually still queue_device_hi -- pick a genuinely
     * unused byte within range: there is none left in this tightly
     * packed 56-byte structure, so exercise the "in range but not
     * 4-byte-aligned to any defined register" width-mismatch path
     * instead via an odd offset that IS one of the defined register
     * starts but with the wrong width, already covered elsewhere; the
     * structure has no genuinely reserved gap, so directly hit the
     * default case by picking an offset one past a register's own
     * start that isn't itself a switch case (e.g. 0x01, the second
     * byte of device_feature_select). */
    rc = hype_virtio_blk_common_cfg_read(&dev, 0x01u, 1u, &value);
    CHECK_HEX("an in-range, non-register-start offset succeeds", 0, rc);
    CHECK_HEX("a reserved sub-byte reads as 0", 0u, value);

    common_write(&dev, 0x01u, 1u, 0xFFu);
    CHECK_HEX("a write to a reserved sub-byte is a safe no-op", 0,
              hype_virtio_blk_common_cfg_write(&dev, 0x01u, 1u, 0xFFu));
}

static void test_out_of_range_and_wrong_width_are_rejected(void) {
    hype_virtio_blk_t dev;
    uint32_t value = 0;

    hype_virtio_blk_reset(&dev, 1);
    CHECK_HEX("out-of-range common-cfg read is rejected", -1,
              hype_virtio_blk_common_cfg_read(&dev, HYPE_VIRTIO_COMMON_CFG_SIZE, 4u, &value));
    CHECK_HEX("out-of-range common-cfg write is rejected", -1,
              hype_virtio_blk_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_SIZE, 4u, 0u));
    CHECK_HEX("wrong-width device_status read is rejected", -1,
              hype_virtio_blk_common_cfg_read(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS, 4u, &value));
    CHECK_HEX("wrong-width device_status write is rejected", -1,
              hype_virtio_blk_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS, 4u, 0u));
    CHECK_HEX("wrong-width queue_select read is rejected", -1,
              hype_virtio_blk_common_cfg_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT, 4u, &value));
    CHECK_HEX("wrong-width driver_feature_select read is rejected", -1,
              hype_virtio_blk_common_cfg_read(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, 2u,
                                              &value));
    CHECK_HEX("wrong-width driver_feature write is rejected", -1,
              hype_virtio_blk_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE, 2u, 0u));
    CHECK_HEX("wrong-width queue_desc_lo write is rejected", -1,
              hype_virtio_blk_common_cfg_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO, 2u, 0u));
}

/* Every implemented register in both the read and write switches
 * guards on its own single correct access width -- sweep all of them
 * with a width (8) none of them ever accept, closing out that guard's
 * "wrong width" branch for every register in one pass rather than
 * duplicating near-identical individual tests. */
static void test_every_register_rejects_an_8_byte_access(void) {
    static const uint32_t read_offsets[] = {
        HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT, HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE,
        HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE,
        HYPE_VIRTIO_COMMON_CFG_MSIX_CONFIG,           HYPE_VIRTIO_COMMON_CFG_NUM_QUEUES,
        HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS,         HYPE_VIRTIO_COMMON_CFG_CONFIG_GENERATION,
        HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT,          HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE,
        HYPE_VIRTIO_COMMON_CFG_QUEUE_MSIX_VECTOR,     HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE,
        HYPE_VIRTIO_COMMON_CFG_QUEUE_NOTIFY_OFF,      HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO,
        HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI,         HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO,
        HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI,       HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO,
        HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI,
    };
    static const uint32_t write_offsets[] = {
        HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT, HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT,
        HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE,        HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS,
        HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT,          HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE,
        HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE,          HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO,
        HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI,         HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO,
        HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI,       HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO,
        HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI,
    };
    hype_virtio_blk_t dev;
    uint32_t value;
    unsigned int i;

    hype_virtio_blk_reset(&dev, 1);

    for (i = 0; i < sizeof(read_offsets) / sizeof(read_offsets[0]); i++) {
        int rc = hype_virtio_blk_common_cfg_read(&dev, read_offsets[i], 8u, &value);
        if (rc != -1) {
            printf("FAIL: read offset 0x%x accepted an 8-byte access\n", read_offsets[i]);
            failures++;
        }
    }
    for (i = 0; i < sizeof(write_offsets) / sizeof(write_offsets[0]); i++) {
        int rc = hype_virtio_blk_common_cfg_write(&dev, write_offsets[i], 8u, 0u);
        if (rc != -1) {
            printf("FAIL: write offset 0x%x accepted an 8-byte access\n", write_offsets[i]);
            failures++;
        }
    }
}

static void test_device_cfg_capacity_and_unmodeled_fields(void) {
    hype_virtio_blk_t dev;
    uint32_t value;

    hype_virtio_blk_reset(&dev, 0x0000000123456789ull);
    value = 0;
    hype_virtio_blk_device_cfg_read(&dev, HYPE_VIRTIO_BLK_CFG_CAPACITY_LO, 4u, &value);
    CHECK_HEX("capacity low half", 0x23456789u, value);
    value = 0;
    hype_virtio_blk_device_cfg_read(&dev, HYPE_VIRTIO_BLK_CFG_CAPACITY_HI, 4u, &value);
    CHECK_HEX("capacity high half", 0x00000001u, value);

    value = 0xFFFFFFFFu;
    hype_virtio_blk_device_cfg_read(&dev, HYPE_VIRTIO_BLK_CFG_SIZE_MAX, 4u, &value);
    CHECK_HEX("size_max is 0 -- gated behind an unoffered feature bit", 0u, value);
    value = 0xFFFFFFFFu;
    hype_virtio_blk_device_cfg_read(&dev, HYPE_VIRTIO_BLK_CFG_BLK_SIZE, 4u, &value);
    CHECK_HEX("blk_size is 0 -- gated behind an unoffered feature bit", 0u, value);

    CHECK_HEX("out-of-range device-cfg read is rejected", -1,
              hype_virtio_blk_device_cfg_read(&dev, HYPE_VIRTIO_BLK_CFG_SIZE, 4u, &value));
}

static void test_isr_read_clears_pending_status(void) {
    hype_virtio_blk_t dev;

    hype_virtio_blk_reset(&dev, 1);
    dev.isr_status = 0x01u;
    CHECK_HEX("first read reports the pending interrupt", 0x01u, hype_virtio_blk_isr_read(&dev));
    CHECK_HEX("second read reports it already cleared", 0u, hype_virtio_blk_isr_read(&dev));
}

static void test_is_queue_ready(void) {
    hype_virtio_blk_t dev;

    hype_virtio_blk_reset(&dev, 1);
    CHECK_HEX("not ready right after reset", 0, hype_virtio_blk_is_queue_ready(&dev));

    dev.device_status = HYPE_VIRTIO_STATUS_DRIVER_OK;
    CHECK_HEX("DRIVER_OK alone is not enough", 0, hype_virtio_blk_is_queue_ready(&dev));

    dev.queue_enable = 1;
    CHECK_HEX("DRIVER_OK + enable, but no queue_desc yet", 0, hype_virtio_blk_is_queue_ready(&dev));

    dev.queue_desc = 0x1000;
    CHECK_HEX("fully ready once DRIVER_OK + enable + a real queue_desc are all set", 1,
              hype_virtio_blk_is_queue_ready(&dev));

    dev.queue_size = 0;
    CHECK_HEX("a zero queue_size is not ready even with everything else set", 0,
              hype_virtio_blk_is_queue_ready(&dev));
}

static void test_virtq_decode_desc(void) {
    uint8_t raw[16] = {
        0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* addr = 0x1000 */
        0x00, 0x02, 0x00, 0x00,                         /* len = 512 */
        0x03, 0x00,                                     /* flags = NEXT|WRITE */
        0x05, 0x00,                                     /* next = 5 */
    };
    hype_virtq_desc_t desc;

    hype_virtq_decode_desc(raw, &desc);
    CHECK_HEX("addr decoded", 0x1000ull, desc.addr);
    CHECK_HEX("len decoded", 512u, desc.len);
    CHECK_HEX("flags decoded", (uint32_t)(HYPE_VIRTQ_DESC_F_NEXT | HYPE_VIRTQ_DESC_F_WRITE), desc.flags);
    CHECK_HEX("next decoded", 5u, desc.next);
}

int main(void) {
    test_reset_sets_capacity_and_default_queue_size();
    test_feature_negotiation_offers_only_version_1();
    test_driver_feature_write_accumulates_across_both_halves();
    test_device_status_handshake();
    test_queue_registers_only_apply_to_queue_zero();
    test_queue_size_write_is_clamped_to_project_max();
    test_read_only_and_unmodeled_registers();
    test_every_register_rejects_an_8_byte_access();
    test_reserved_offset_reads_as_zero();
    test_out_of_range_and_wrong_width_are_rejected();
    test_device_cfg_capacity_and_unmodeled_fields();
    test_isr_read_clears_pending_status();
    test_is_queue_ready();
    test_virtq_decode_desc();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
