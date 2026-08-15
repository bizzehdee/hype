#include <stdio.h>
#include <string.h>
#include "../../devices/virtio_blk.h"
#include "../blk_backend.h"

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


/*
 * #268: descriptor-chain walking. These drive process_virtio_blk_queue() directly
 * with dma_map == 0, which means "trusted identity-mapped guest" -- so the device
 * treats these host addresses as guest-physical ones, the same path the M5-1
 * cooperating test guest uses. That makes the chain walk testable on the host even
 * though it lives in the (coverage-exempt) SVM backend file.
 */
#define TQ_QSZ 8u
#define TQ_SECTORS 16u

typedef struct {
    uint8_t desc[TQ_QSZ * 16u];
    uint8_t avail[4u + 2u * TQ_QSZ + 2u];
    uint8_t used[4u + 8u * TQ_QSZ + 2u];
    uint8_t hdr[16];
    uint8_t status;
    uint8_t img[TQ_SECTORS * 512u];
    uint8_t gbuf[8u * 512u]; /* guest data buffer the segments point into */
    hype_virtio_blk_t dev;
    hype_blk_file_t file;
    hype_blk_backend_t be;
} tq_t;

static void tq_put64(uint8_t *p, uint64_t v) {
    unsigned i;
    for (i = 0; i < 8u; i++) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

static void tq_put32(uint8_t *p, uint32_t v) {
    unsigned i;
    for (i = 0; i < 4u; i++) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

static void tq_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t tq_get16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t tq_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void tq_desc(tq_t *q, unsigned i, const void *addr, uint32_t len, uint16_t flags,
                    uint16_t next) {
    uint8_t *d = q->desc + i * 16u;
    tq_put64(d, (uint64_t)(uintptr_t)addr);
    tq_put32(d + 8, len);
    tq_put16(d + 12, flags);
    tq_put16(d + 14, next);
}


/* Capture rejection reasons so a test can assert WHY a request was refused, and
 * so the walk runs at all on the host (the default sink is hype_debug_print,
 * which reaches a real UART through port I/O and faults in a user process). */
static char tq_reject_last[128];
static unsigned tq_reject_count;

static void tq_reject_sink(const char *why) {
    unsigned i;
    tq_reject_count++;
    for (i = 0; i + 1u < sizeof(tq_reject_last) && why[i] != 0; i++) {
        tq_reject_last[i] = why[i];
    }
    tq_reject_last[i] = 0;
}

static int tq_reject_says(const char *needle) {
    return strstr(tq_reject_last, needle) != 0;
}

/* Reset the rig: an empty queue, a sector-patterned backing image, and a device
 * pointed at the three rings. Nothing is submitted yet. */
static void tq_init(tq_t *q, uint32_t req_type, uint64_t sector) {
    unsigned s;

    memset(q, 0, sizeof(*q));
    hype_virtio_blk_set_reject_sink(tq_reject_sink); /* also resets the rate limit */
    tq_reject_count = 0;
    tq_reject_last[0] = 0;
    for (s = 0; s < TQ_SECTORS; s++) {
        memset(q->img + s * 512u, (int)(0x10u + s), 512u);
    }
    q->status = 0xEE; /* poison, so "status untouched" is distinguishable from OK */

    tq_put32(q->hdr, req_type);
    tq_put64(q->hdr + 8, sector);

    hype_virtio_blk_reset(&q->dev, TQ_SECTORS);
    q->dev.queue_size = (uint16_t)TQ_QSZ;
    q->dev.queue_desc = (uint64_t)(uintptr_t)q->desc;
    q->dev.queue_driver = (uint64_t)(uintptr_t)q->avail;
    q->dev.queue_device = (uint64_t)(uintptr_t)q->used;

    hype_blk_file_init(&q->file, &q->be, q->img, sizeof(q->img));
}

/* Submit one chain whose head is descriptor `head`. */
static void tq_submit(tq_t *q, uint16_t head) {
    uint16_t idx = tq_get16(q->avail + 2);
    tq_put16(q->avail + 4 + 2u * (idx % TQ_QSZ), head);
    tq_put16(q->avail + 2, (uint16_t)(idx + 1u));
}

static int tq_run(tq_t *q) {
    return process_virtio_blk_queue(&q->dev, &q->be, 0);
}

/* Build header -> `nsegs` data segments -> status, with segment i covering
 * seg_len[i] bytes of q->gbuf laid end to end. Descriptor 0 is the header,
 * 1..nsegs the segments, nsegs+1 the status. */
static void tq_chain(tq_t *q, const uint32_t *seg_len, unsigned nsegs, uint16_t data_flags) {
    unsigned i;
    uint32_t off = 0;

    tq_desc(q, 0, q->hdr, 16u, HYPE_VIRTQ_DESC_F_NEXT, 1);
    for (i = 0; i < nsegs; i++) {
        tq_desc(q, 1u + i, q->gbuf + off, seg_len[i],
                (uint16_t)(HYPE_VIRTQ_DESC_F_NEXT | data_flags), (uint16_t)(2u + i));
        off += seg_len[i];
    }
    tq_desc(q, 1u + nsegs, &q->status, 1u, 0, 0);
}

static void test_chain_single_segment_write_still_works(void) {
    tq_t q;
    uint32_t len[1] = {512u};

    tq_init(&q, HYPE_VIRTIO_BLK_T_OUT, 3);
    memset(q.gbuf, 0xA5, 512u);
    tq_chain(&q, len, 1, 0);
    tq_submit(&q, 0);

    CHECK_HEX("single-segment write returns 0", 0, tq_run(&q));
    CHECK_HEX("single-segment write status OK", HYPE_VIRTIO_BLK_S_OK, q.status);
    CHECK_HEX("sector 3 written", 0xA5u, q.img[3u * 512u]);
    CHECK_HEX("sector 2 untouched", 0x12u, q.img[2u * 512u]);
    CHECK_HEX("sector 4 untouched", 0x14u, q.img[4u * 512u]);
    CHECK_HEX("used idx advanced", 1u, tq_get16(q.used + 2));
    CHECK_HEX("used elem id is the chain head", 0u, tq_get32(q.used + 4));
    CHECK_HEX("write used_len is just the status byte", 1u, tq_get32(q.used + 8));
    CHECK_HEX("last_avail_idx consumed the chain", 1u, q.dev.last_avail_idx);
}

/* The bug: header + 3 data + status was rejected outright, capping every request
 * at one contiguous segment. */
static void test_chain_multi_segment_write(void) {
    tq_t q;
    uint32_t len[3] = {512u, 1024u, 512u};

    tq_init(&q, HYPE_VIRTIO_BLK_T_OUT, 1);
    memset(q.gbuf + 0, 0xB1, 512u);     /* -> sector 1 */
    memset(q.gbuf + 512, 0xB2, 1024u);  /* -> sectors 2,3 */
    memset(q.gbuf + 1536, 0xB3, 512u);  /* -> sector 4 */
    tq_chain(&q, len, 3, 0);
    tq_submit(&q, 0);

    CHECK_HEX("3-segment write accepted", 0, tq_run(&q));
    CHECK_HEX("3-segment write status OK", HYPE_VIRTIO_BLK_S_OK, q.status);
    /* The scattered buffer must land as ONE contiguous run, LBA advancing by
     * each segment's sector count. */
    CHECK_HEX("segment 0 -> sector 1", 0xB1u, q.img[1u * 512u]);
    CHECK_HEX("segment 1 -> sector 2", 0xB2u, q.img[2u * 512u]);
    CHECK_HEX("segment 1 -> sector 3", 0xB2u, q.img[3u * 512u]);
    CHECK_HEX("segment 2 -> sector 4", 0xB3u, q.img[4u * 512u]);
    CHECK_HEX("sector 0 untouched", 0x10u, q.img[0]);
    CHECK_HEX("sector 5 untouched", 0x15u, q.img[5u * 512u]);
}

static void test_chain_multi_segment_read(void) {
    tq_t q;
    uint32_t len[3] = {1024u, 512u, 512u};

    tq_init(&q, HYPE_VIRTIO_BLK_T_IN, 2);
    tq_chain(&q, len, 3, HYPE_VIRTQ_DESC_F_WRITE);
    tq_submit(&q, 0);

    CHECK_HEX("3-segment read accepted", 0, tq_run(&q));
    CHECK_HEX("3-segment read status OK", HYPE_VIRTIO_BLK_S_OK, q.status);
    CHECK_HEX("gbuf[0] is sector 2", 0x12u, q.gbuf[0]);
    CHECK_HEX("gbuf[512] is sector 3", 0x13u, q.gbuf[512]);
    CHECK_HEX("gbuf[1024] is sector 4", 0x14u, q.gbuf[1024]);
    CHECK_HEX("gbuf[1536] is sector 5", 0x15u, q.gbuf[1536]);
    /* A read's used_len is every data byte written plus the status byte. */
    CHECK_HEX("read used_len covers all 4 sectors + status", 2048u + 1u, tq_get32(q.used + 8));
}

/*
 * A FLUSH chain is header -> status with NO data descriptor. The old walk did not
 * merely refuse it: returning -1 aborted the notify without advancing
 * last_avail_idx or writing a status byte, so the guest waited on it forever.
 */
static void test_chain_flush_has_no_data_descriptor(void) {
    tq_t q;

    tq_init(&q, HYPE_VIRTIO_BLK_T_FLUSH, 0);
    tq_chain(&q, 0, 0, 0);
    tq_submit(&q, 0);

    CHECK_HEX("2-descriptor FLUSH accepted", 0, tq_run(&q));
    CHECK_HEX("FLUSH acknowledged OK", HYPE_VIRTIO_BLK_S_OK, q.status);
    CHECK_HEX("FLUSH completed into the used ring", 1u, tq_get16(q.used + 2));
    CHECK_HEX("FLUSH consumed its avail entry", 1u, q.dev.last_avail_idx);
}

/*
 * A cyclic NEXT list must terminate the walk. The indices are guest-controlled,
 * so without the queue_size hop bound this spins inside hype forever and takes
 * the VM's dispatch loop with it. The assertion that matters most here is simply
 * that this test RETURNS.
 */
static void test_chain_cycle_is_rejected(void) {
    tq_t q;

    tq_init(&q, HYPE_VIRTIO_BLK_T_OUT, 1);
    tq_desc(&q, 0, q.hdr, 16u, HYPE_VIRTQ_DESC_F_NEXT, 1);
    tq_desc(&q, 1, q.gbuf, 512u, HYPE_VIRTQ_DESC_F_NEXT, 2);
    tq_desc(&q, 2, q.gbuf, 512u, HYPE_VIRTQ_DESC_F_NEXT, 1); /* 1 -> 2 -> 1 */
    tq_submit(&q, 0);

    CHECK_HEX("cyclic chain rejected", (unsigned long long)(-1), (unsigned long long)tq_run(&q));
    CHECK_HEX("cyclic chain wrote no status", 0xEEu, q.status);
    CHECK_HEX("cyclic chain posted no completion", 0u, tq_get16(q.used + 2));
    CHECK_HEX("cyclic chain was logged", 1u, tq_reject_count);
    CHECK_HEX("cycle logged as a malformed chain", 1, tq_reject_says("malformed descriptor chain"));
}

/* A malformed chain must be rejected having changed nothing -- the property the
 * old fixed-3 walk had for free and the variable-length walk has to earn. */
static void test_chain_malformed_shapes_change_nothing(void) {
    tq_t q;
    uint32_t len[1] = {512u};

    /* Head index beyond the queue. */
    tq_init(&q, HYPE_VIRTIO_BLK_T_OUT, 1);
    memset(q.gbuf, 0xC1, 512u);
    tq_chain(&q, len, 1, 0);
    tq_submit(&q, (uint16_t)(TQ_QSZ + 1u));
    CHECK_HEX("out-of-range head rejected", (unsigned long long)(-1), (unsigned long long)tq_run(&q));
    CHECK_HEX("out-of-range head wrote nothing", 0x11u, q.img[1u * 512u]);
    CHECK_HEX("out-of-range head posted no completion", 0u, tq_get16(q.used + 2));

    /* A header with no NEXT has nowhere to put the status byte. */
    tq_init(&q, HYPE_VIRTIO_BLK_T_OUT, 1);
    tq_desc(&q, 0, q.hdr, 16u, 0, 0);
    tq_submit(&q, 0);
    CHECK_HEX("header without NEXT rejected", (unsigned long long)(-1),
              (unsigned long long)tq_run(&q));
    CHECK_HEX("header without NEXT posted no completion", 0u, tq_get16(q.used + 2));

    /* A NEXT pointing past the queue, found partway along the chain. */
    tq_init(&q, HYPE_VIRTIO_BLK_T_OUT, 1);
    memset(q.gbuf, 0xC3, 512u);
    tq_desc(&q, 0, q.hdr, 16u, HYPE_VIRTQ_DESC_F_NEXT, 1);
    tq_desc(&q, 1, q.gbuf, 512u, HYPE_VIRTQ_DESC_F_NEXT, (uint16_t)TQ_QSZ);
    tq_submit(&q, 0);
    CHECK_HEX("mid-chain out-of-range NEXT rejected", (unsigned long long)(-1),
              (unsigned long long)tq_run(&q));
    CHECK_HEX("mid-chain rejection wrote no data", 0x11u, q.img[1u * 512u]);
}

/*
 * A segment that is not a whole number of sectors, a read/write carrying no data
 * segment at all, and an out-of-bounds LBA are all COMPLETED with IOERR rather
 * than left dangling. That distinction is the point: an uncompleted request is a
 * guest hang, whereas IOERR is an error the guest can see and report.
 */
static void test_chain_bad_requests_complete_with_ioerr(void) {
    tq_t q;
    uint32_t ragged[1] = {500u};
    uint32_t ok[1] = {512u};

    tq_init(&q, HYPE_VIRTIO_BLK_T_OUT, 1);
    tq_chain(&q, ragged, 1, 0);
    tq_submit(&q, 0);
    CHECK_HEX("ragged segment does not abort the notify", 0, tq_run(&q));
    CHECK_HEX("ragged segment reported IOERR", HYPE_VIRTIO_BLK_S_IOERR, q.status);
    CHECK_HEX("ragged segment was completed", 1u, tq_get16(q.used + 2));
    CHECK_HEX("ragged segment named in the log", 1, tq_reject_says("whole number of sectors"));

    /* Legal chain SHAPE (it is what FLUSH uses) but meaningless for a read. */
    tq_init(&q, HYPE_VIRTIO_BLK_T_IN, 1);
    tq_chain(&q, 0, 0, 0);
    tq_submit(&q, 0);
    CHECK_HEX("dataless read does not abort the notify", 0, tq_run(&q));
    CHECK_HEX("dataless read reported IOERR", HYPE_VIRTIO_BLK_S_IOERR, q.status);
    CHECK_HEX("dataless read was completed", 1u, tq_get16(q.used + 2));
    CHECK_HEX("dataless read named in the log", 1, tq_reject_says("no data segment"));

    /* Out-of-bounds LBA: the backend's VALID-3 gate refuses it. */
    tq_init(&q, HYPE_VIRTIO_BLK_T_OUT, TQ_SECTORS);
    tq_chain(&q, ok, 1, 0);
    tq_submit(&q, 0);
    CHECK_HEX("out-of-bounds LBA does not abort the notify", 0, tq_run(&q));
    CHECK_HEX("out-of-bounds LBA reported IOERR", HYPE_VIRTIO_BLK_S_IOERR, q.status);

    /* A multi-segment write that runs off the end mid-chain: the backend refuses
     * the offending segment, and the request reports IOERR. */
    tq_init(&q, HYPE_VIRTIO_BLK_T_OUT, TQ_SECTORS - 1u);
    {
        uint32_t two[2] = {512u, 512u};
        tq_chain(&q, two, 2, 0);
        tq_submit(&q, 0);
        CHECK_HEX("write straddling the end reported IOERR", 0, tq_run(&q));
        CHECK_HEX("straddling write status IOERR", HYPE_VIRTIO_BLK_S_IOERR, q.status);
    }
}

/*
 * VALID-2: process_virtio_blk_queue() is shared, vendor-neutral code, and every
 * guest-supplied address in it (the ring bases, each descriptor, every data
 * segment, the status byte) is already routed through guest_dma_xlate() ->
 * hype_gpa_to_host() -- the tests above all drive it with dma_map == 0
 * ("trusted identity-mapped guest"), which never exercises that bounds check at
 * all. These two prove the check is real for a genuinely non-identity guest
 * (FW-1's own case): a legitimate request through a real hype_gpa_map_t still
 * completes, and a data segment pointing outside that VM's own mapped window is
 * refused rather than translated to whatever host address the raw guest value
 * happens to collide with.
 */
#define TQM_QSZ 8u
#define TQM_SECTORS 16u
#define TQM_GUEST_BASE 0x9000000000ull /* arbitrary GPA base for this VM's one region */

typedef struct {
    uint8_t desc[TQM_QSZ * 16u];
    uint8_t avail[4u + 2u * TQM_QSZ + 2u];
    uint8_t used[4u + 8u * TQM_QSZ + 2u];
    uint8_t hdr[16];
    uint8_t status;
    uint8_t gbuf[512u]; /* the one in-bounds data segment target */
    hype_virtio_blk_t dev;
    hype_blk_file_t file;
    hype_blk_backend_t be;
    uint8_t img[TQM_SECTORS * 512u];
    hype_gpa_map_t map;
} tqm_t;

static uint64_t tqm_gpa(const tqm_t *q, const void *host_ptr) {
    return TQM_GUEST_BASE + (uint64_t)((const uint8_t *)host_ptr - (const uint8_t *)q);
}

/* Same rig as tq_init(), but addresses stored on the wire are GUEST-PHYSICAL
 * (TQM_GUEST_BASE-relative) offsets into `q`, and `q->map` maps exactly that one
 * region to `q`'s real host address -- reproducing FW-1's non-identity case. */
static void tqm_init(tqm_t *q, uint32_t req_type, uint64_t sector) {
    unsigned s;

    memset(q, 0, sizeof(*q));
    hype_virtio_blk_set_reject_sink(tq_reject_sink);
    tq_reject_count = 0;
    tq_reject_last[0] = 0;
    for (s = 0; s < TQM_SECTORS; s++) {
        memset(q->img + s * 512u, (int)(0x10u + s), 512u);
    }
    q->status = 0xEE;

    tq_put32(q->hdr, req_type);
    tq_put64(q->hdr + 8, sector);

    hype_gpa_map_reset(&q->map);
    if (hype_gpa_map_add(&q->map, TQM_GUEST_BASE, (uint64_t)(uintptr_t)q, sizeof(*q)) != 0) {
        printf("FAIL: tqm_init: could not map the test VM's one region\n");
        failures++;
    }

    hype_virtio_blk_reset(&q->dev, TQM_SECTORS);
    q->dev.queue_size = (uint16_t)TQM_QSZ;
    q->dev.queue_desc = tqm_gpa(q, q->desc);
    q->dev.queue_driver = tqm_gpa(q, q->avail);
    q->dev.queue_device = tqm_gpa(q, q->used);

    hype_blk_file_init(&q->file, &q->be, q->img, sizeof(q->img));
}

static void tqm_submit(tqm_t *q, uint16_t head) {
    uint16_t idx = tq_get16(q->avail + 2);
    tq_put16(q->avail + 4 + 2u * (idx % TQM_QSZ), head);
    tq_put16(q->avail + 2, (uint16_t)(idx + 1u));
}

/* header -> one data segment -> status, all addresses GPA offsets into `q`
 * except `data_addr`, which a caller may point outside `q` entirely to probe
 * the bounds check. */
static void tqm_desc(tqm_t *q, unsigned i, uint64_t addr, uint32_t len, uint16_t flags,
                     uint16_t next) {
    uint8_t *d = q->desc + i * 16u;
    tq_put64(d, addr);
    tq_put32(d + 8, len);
    tq_put16(d + 12, flags);
    tq_put16(d + 14, next);
}

static void tqm_chain_1seg(tqm_t *q, uint64_t data_addr, uint32_t data_len, uint16_t data_flags) {
    tqm_desc(q, 0, tqm_gpa(q, q->hdr), 16u, HYPE_VIRTQ_DESC_F_NEXT, 1);
    tqm_desc(q, 1, data_addr, data_len, (uint16_t)(HYPE_VIRTQ_DESC_F_NEXT | data_flags), 2);
    tqm_desc(q, 2, tqm_gpa(q, &q->status), 1u, 0, 0);
}

static int tqm_run(tqm_t *q) {
    return process_virtio_blk_queue(&q->dev, &q->be, &q->map);
}

static void test_chain_via_real_gpa_map_translates_and_succeeds(void) {
    tqm_t q;

    tqm_init(&q, HYPE_VIRTIO_BLK_T_OUT, 3);
    memset(q.gbuf, 0xA5, sizeof(q.gbuf));
    tqm_chain_1seg(&q, tqm_gpa(&q, q.gbuf), 512u, 0);
    tqm_submit(&q, 0);

    CHECK_HEX("a real (non-identity) gpa_map still lets a legitimate write through", 0,
              tqm_run(&q));
    CHECK_HEX("status OK through the real map", HYPE_VIRTIO_BLK_S_OK, q.status);
    CHECK_HEX("sector 3 actually written via the translated address", 0xA5u, q.img[3u * 512u]);
}

static void test_chain_data_segment_outside_mapped_region_is_rejected(void) {
    tqm_t q;
    uint8_t victim[512];

    memset(victim, 0x77, sizeof(victim));
    tqm_init(&q, HYPE_VIRTIO_BLK_T_OUT, 3);
    /*
     * The malicious segment names `victim`'s real host address as if it were a
     * guest-physical one -- exactly what a guest driver would send if it had
     * learned (or guessed) hype's own address space. `victim` is NOT part of
     * `q`'s one mapped region, so this must be refused rather than translated.
     */
    tqm_chain_1seg(&q, (uint64_t)(uintptr_t)victim, 512u, 0);
    tqm_submit(&q, 0);

    CHECK_HEX("an out-of-region data segment does not abort the notify", 0, tqm_run(&q));
    CHECK_HEX("out-of-region data segment reported IOERR", HYPE_VIRTIO_BLK_S_IOERR, q.status);
    CHECK_HEX("out-of-region data segment named in the log", 1,
              tq_reject_says("bounds check"));
    CHECK_HEX("sector 3 was never written (translation failed before any transfer)", 0x13u,
              q.img[3u * 512u]);
    CHECK_HEX("victim buffer untouched -- the guest never reached it", 0x77,
              victim[0]);
}

/* #310 --------------------------------------------------------------------------------- */

static void test_get_id_returns_the_serial_nul_padded(void) {
    /*
     * #310: FreeBSD's vtblk issues GET_ID during attach and reported
     * "error getting device identifier: 45" (ENOTSUP == S_UNSUPP) when hype refused it.
     *
     * The field is a fixed 20 bytes of ASCII, NUL-PADDED rather than NUL-terminated, and
     * used_len must count what the device wrote PLUS the status byte.
     */
    tq_t q;
    uint32_t len[1] = {HYPE_VIRTIO_BLK_ID_BYTES};
    unsigned i;
    unsigned nonzero = 0;

    tq_init(&q, HYPE_VIRTIO_BLK_T_GET_ID, 0);
    hype_virtio_blk_set_serial(&q.dev, "vm0");
    tq_chain(&q, len, 1, HYPE_VIRTQ_DESC_F_WRITE);
    tq_submit(&q, 0);

    CHECK_HEX("GET_ID does not abort the notify", 0, tq_run(&q));
    CHECK_HEX("GET_ID reported OK", HYPE_VIRTIO_BLK_S_OK, q.status);
    CHECK_HEX("GET_ID was completed", 1u, tq_get16(q.used + 2));
    CHECK_HEX("used_len is the 20 bytes written plus the status byte",
              HYPE_VIRTIO_BLK_ID_BYTES + 1u, tq_get32(q.used + 8));
    CHECK_HEX("serial byte 0", 'v', q.gbuf[0]);
    CHECK_HEX("serial byte 1", 'm', q.gbuf[1]);
    CHECK_HEX("serial byte 2", '0', q.gbuf[2]);
    /* Everything past the name must be NUL padding, not stale buffer contents. */
    for (i = 3; i < HYPE_VIRTIO_BLK_ID_BYTES; i++) {
        CHECK_HEX("padded with NUL", 0, q.gbuf[i]);
        nonzero += (q.gbuf[i] != 0) ? 1u : 0u;
    }
    CHECK_HEX("no stale bytes in the padding", 0, nonzero);
}

static void test_get_id_is_stable_across_calls(void) {
    /*
     * The ticket's own acceptance point: the identifier must be UNCHANGED across two calls. A
     * serial that varied would make a guest OS think the disk had been swapped.
     */
    tq_t q;
    uint32_t len[1] = {HYPE_VIRTIO_BLK_ID_BYTES};
    uint8_t first[HYPE_VIRTIO_BLK_ID_BYTES];
    unsigned i;

    tq_init(&q, HYPE_VIRTIO_BLK_T_GET_ID, 0);
    tq_chain(&q, len, 1, HYPE_VIRTQ_DESC_F_WRITE);
    tq_submit(&q, 0);
    (void)tq_run(&q);
    for (i = 0; i < HYPE_VIRTIO_BLK_ID_BYTES; i++) {
        first[i] = q.gbuf[i];
        q.gbuf[i] = 0xEE; /* poison, so an unwritten buffer cannot pass by looking unchanged */
    }

    tq_submit(&q, 0);
    (void)tq_run(&q);
    for (i = 0; i < HYPE_VIRTIO_BLK_ID_BYTES; i++) {
        CHECK_HEX("serial identical on the second call", first[i], q.gbuf[i]);
    }
}

static void test_get_id_default_serial_is_a_valid_20_byte_field(void) {
    /* A device nobody named still has to answer GET_ID with something sane. */
    hype_virtio_blk_t dev;
    unsigned i;
    unsigned printable = 0;

    hype_virtio_blk_reset(&dev, 128u);
    for (i = 0; i < HYPE_VIRTIO_BLK_ID_BYTES; i++) {
        if (dev.serial[i] >= 0x20u && dev.serial[i] < 0x7Fu) {
            printable++;
        }
    }
    CHECK_HEX("reset installs a full 20-character printable default", HYPE_VIRTIO_BLK_ID_BYTES,
              printable);
}

static void test_serial_survives_a_driver_reset(void) {
    /*
     * device_status = 0 is a DRIVER reset: it clears negotiation state. The serial is device
     * IDENTITY and must survive it -- a disk whose serial changed mid-boot under the driver's
     * own reset would look to the guest like the disk had been swapped.
     */
    hype_virtio_blk_t dev;
    uint8_t before[HYPE_VIRTIO_BLK_ID_BYTES];
    unsigned i;

    hype_virtio_blk_reset(&dev, 128u);
    hype_virtio_blk_set_serial(&dev, "vm1");
    for (i = 0; i < HYPE_VIRTIO_BLK_ID_BYTES; i++) {
        before[i] = dev.serial[i];
    }
    common_write(&dev, HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS, 1, 0);
    for (i = 0; i < HYPE_VIRTIO_BLK_ID_BYTES; i++) {
        CHECK_HEX("serial unchanged by a driver reset", before[i], dev.serial[i]);
    }
}

static void test_set_serial_truncates_and_keeps_the_default_for_no_name(void) {
    hype_virtio_blk_t dev;
    unsigned i;

    /* Longer than the field: truncated to 20, no overrun, no terminator. */
    hype_virtio_blk_reset(&dev, 128u);
    hype_virtio_blk_set_serial(&dev, "0123456789ABCDEFGHIJ-OVERFLOW");
    for (i = 0; i < HYPE_VIRTIO_BLK_ID_BYTES; i++) {
        CHECK_HEX("truncated to the field width", "0123456789ABCDEFGHIJ"[i], dev.serial[i]);
    }

    /* A NULL or empty name keeps the default rather than blanking the field. */
    hype_virtio_blk_reset(&dev, 128u);
    hype_virtio_blk_set_serial(&dev, 0);
    CHECK_HEX("NULL name keeps the default", 'H', dev.serial[0]);
    hype_virtio_blk_set_serial(&dev, "");
    CHECK_HEX("empty name keeps the default", 'H', dev.serial[0]);
}

static void test_get_id_short_buffer_is_not_overrun(void) {
    /*
     * The guest offered fewer than 20 bytes. hype must write only what was offered -- a short
     * buffer is the guest's business, overrunning it would be hype's.
     */
    tq_t q;
    uint32_t len[1] = {4u};

    tq_init(&q, HYPE_VIRTIO_BLK_T_GET_ID, 0);
    hype_virtio_blk_set_serial(&q.dev, "vm0");
    tq_chain(&q, len, 1, HYPE_VIRTQ_DESC_F_WRITE);
    q.gbuf[4] = 0xEE; /* sentinel immediately past the offered buffer */
    tq_submit(&q, 0);

    CHECK_HEX("short GET_ID still succeeds", 0, tq_run(&q));
    CHECK_HEX("short GET_ID reported OK", HYPE_VIRTIO_BLK_S_OK, q.status);
    CHECK_HEX("used_len counts only what was written, plus status", 5u, tq_get32(q.used + 8));
    CHECK_HEX("wrote the offered bytes", 'v', q.gbuf[0]);
    CHECK_HEX("byte past the buffer untouched", 0xEE, q.gbuf[4]);
}

static void test_get_id_with_no_data_descriptor_is_rejected_not_ignored(void) {
    /*
     * A GET_ID whose chain is header-then-status carries nowhere to put the answer. It must be
     * completed with an error rather than silently reported OK, which would tell the guest a
     * serial had been written into a buffer that does not exist.
     */
    tq_t q;

    tq_init(&q, HYPE_VIRTIO_BLK_T_GET_ID, 0);
    /* header (desc 0) -> status (desc 1), no data segment */
    tq_desc(&q, 0, q.hdr, 16u, HYPE_VIRTQ_DESC_F_NEXT, 1);
    tq_desc(&q, 1, &q.status, 1u, HYPE_VIRTQ_DESC_F_WRITE, 0);
    tq_submit(&q, 0);

    CHECK_HEX("does not abort the notify", 0, tq_run(&q));
    CHECK_HEX("reported IOERR", HYPE_VIRTIO_BLK_S_IOERR, q.status);
    CHECK_HEX("was completed", 1u, tq_get16(q.used + 2));
    CHECK_HEX("named in the log", 1, tq_reject_says("no data descriptor"));
}

static void test_chain_unsupported_type_is_completed(void) {
    tq_t q;
    uint32_t len[1] = {512u};

    tq_init(&q, 0x777u, 1);
    tq_chain(&q, len, 1, 0);
    tq_submit(&q, 0);

    CHECK_HEX("unsupported type does not abort the notify", 0, tq_run(&q));
    CHECK_HEX("unsupported type reported UNSUPP", HYPE_VIRTIO_BLK_S_UNSUPP, q.status);
    CHECK_HEX("unsupported type was completed", 1u, tq_get16(q.used + 2));
    CHECK_HEX("unsupported type named in the log", 1, tq_reject_says("unsupported request type"));
}

/* Several chains submitted before a single notify must all drain, and a second
 * notify with nothing new must be a no-op. */
static void test_chain_multiple_pending_chains_all_drain(void) {
    tq_t q;

    tq_init(&q, HYPE_VIRTIO_BLK_T_OUT, 1);
    /* chain A: desc 0,1,2 -> sector 1 ; chain B: desc 3,4,5 -> sector 6 */
    memset(q.gbuf, 0xD1, 512u);
    memset(q.gbuf + 512, 0xD2, 512u);
    tq_desc(&q, 0, q.hdr, 16u, HYPE_VIRTQ_DESC_F_NEXT, 1);
    tq_desc(&q, 1, q.gbuf, 512u, HYPE_VIRTQ_DESC_F_NEXT, 2);
    tq_desc(&q, 2, &q.status, 1u, 0, 0);

    {
        static uint8_t hdr_b[16];
        static uint8_t status_b;
        tq_put32(hdr_b, HYPE_VIRTIO_BLK_T_OUT);
        tq_put64(hdr_b + 8, 6);
        status_b = 0xEE;
        tq_desc(&q, 3, hdr_b, 16u, HYPE_VIRTQ_DESC_F_NEXT, 4);
        tq_desc(&q, 4, q.gbuf + 512, 512u, HYPE_VIRTQ_DESC_F_NEXT, 5);
        tq_desc(&q, 5, &status_b, 1u, 0, 0);

        tq_submit(&q, 0);
        tq_submit(&q, 3);

        CHECK_HEX("both chains drained", 0, tq_run(&q));
        CHECK_HEX("chain A wrote sector 1", 0xD1u, q.img[1u * 512u]);
        CHECK_HEX("chain B wrote sector 6", 0xD2u, q.img[6u * 512u]);
        CHECK_HEX("chain A status OK", HYPE_VIRTIO_BLK_S_OK, q.status);
        CHECK_HEX("chain B status OK", HYPE_VIRTIO_BLK_S_OK, status_b);
        CHECK_HEX("two completions posted", 2u, tq_get16(q.used + 2));
        CHECK_HEX("second used elem id is chain B head", 3u, tq_get32(q.used + 12));

        CHECK_HEX("re-notify with nothing new is a no-op", 0, tq_run(&q));
        CHECK_HEX("no extra completion posted", 2u, tq_get16(q.used + 2));
    }
}

/* A zero queue_size is not a usable queue. */
static void test_chain_zero_queue_size_rejected(void) {
    tq_t q;

    tq_init(&q, HYPE_VIRTIO_BLK_T_OUT, 1);
    q.dev.queue_size = 0;
    CHECK_HEX("zero queue_size rejected", (unsigned long long)(-1), (unsigned long long)tq_run(&q));
}


/* The reject log is rate-limited: one bad guest must not bury the rest of the
 * log, which is the failure mode #238 was about. */
static void test_chain_reject_log_is_rate_limited(void) {
    tq_t q;
    unsigned i;

    tq_init(&q, HYPE_VIRTIO_BLK_T_OUT, 1);
    tq_desc(&q, 0, q.hdr, 16u, 0, 0); /* header with no NEXT -- always rejected */
    for (i = 0; i < 40u; i++) {
        tq_submit(&q, 0);
        (void)tq_run(&q);
    }
    CHECK_HEX("rejection logging capped", 8u, tq_reject_count);
}


/* #265: queue-depth instrumentation. Pure, so the bucket boundaries and the
 * scaled mean are checked here rather than only ever on hardware -- where a
 * misplaced boundary would silently mis-report the one number that decides
 * which fix the write path gets. */
static void test_depth_buckets(void) {
    struct { uint32_t depth; unsigned bucket; } cases[] = {
        {0u, 0u}, {1u, 0u},
        {2u, 1u}, {3u, 1u},
        {4u, 2u}, {7u, 2u},
        {8u, 3u}, {15u, 3u},
        {16u, 4u}, {31u, 4u},
        {32u, 5u}, {1000u, 5u},
    };
    unsigned i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned got = hype_virtio_blk_depth_bucket(cases[i].depth);
        if (got != cases[i].bucket) {
            printf("FAIL: depth %u -> bucket %u, expected %u\n", cases[i].depth, got,
                   cases[i].bucket);
            failures++;
        }
    }
}

static void test_depth_record_accumulates(void) {
    hype_virtio_blk_depth_t d;

    hype_virtio_blk_depth_reset(&d);
    CHECK_HEX("no kicks recorded yet", 0u, (unsigned)d.kicks);
    CHECK_HEX("mean of no kicks is 0, not a divide by zero", 0u,
              hype_virtio_blk_depth_mean_x100(&d));

    hype_virtio_blk_depth_record(&d, 1u);
    hype_virtio_blk_depth_record(&d, 1u);
    hype_virtio_blk_depth_record(&d, 4u);

    CHECK_HEX("three kicks", 3u, (unsigned)d.kicks);
    CHECK_HEX("six chains", 6u, (unsigned)d.chains);
    CHECK_HEX("max depth 4", 4u, d.max_depth);
    CHECK_HEX("two kicks in bucket 0", 2u, d.hist[0]);
    CHECK_HEX("one kick in bucket 2", 1u, d.hist[2]);
    /* 6 chains / 3 kicks = 2.00 */
    CHECK_HEX("mean is 200 (2.00 chains/kick)", 200u, hype_virtio_blk_depth_mean_x100(&d));

    /* A depth of 0 is not a kick: an empty notify says nothing about how deeply
     * the guest queues, and counting it would drag the mean toward 1 and hide
     * exactly the signal this exists to find. */
    hype_virtio_blk_depth_record(&d, 0u);
    CHECK_HEX("empty notify not counted as a kick", 3u, (unsigned)d.kicks);
    CHECK_HEX("empty notify did not change the mean", 200u, hype_virtio_blk_depth_mean_x100(&d));

    /* Null-safe, like the write stats. */
    hype_virtio_blk_depth_record(0, 1u);
    hype_virtio_blk_depth_reset(0);
    CHECK_HEX("mean of NULL is 0", 0u, hype_virtio_blk_depth_mean_x100(0));
}

/* The drain must report the depth it actually saw, so the DIAG number is the
 * guest's behaviour and not an artefact of how the counter is wired. */
static void test_depth_is_recorded_by_the_drain(void) {
    tq_t q;
    hype_virtio_blk_depth_t *d = hype_virtio_blk_depth();

    hype_virtio_blk_depth_reset(d);

    /* One kick carrying two chains (the same rig as the multi-chain drain test). */
    tq_init(&q, HYPE_VIRTIO_BLK_T_FLUSH, 0);
    tq_desc(&q, 0, q.hdr, 16u, HYPE_VIRTQ_DESC_F_NEXT, 1);
    tq_desc(&q, 1, &q.status, 1u, 0, 0);
    tq_submit(&q, 0);
    tq_submit(&q, 0);
    CHECK_HEX("drain ok", 0, tq_run(&q));
    CHECK_HEX("one kick recorded", 1u, (unsigned)d->kicks);
    CHECK_HEX("depth 2 recorded", 2u, (unsigned)d->chains);
    CHECK_HEX("max depth 2", 2u, d->max_depth);

    /* A re-notify with nothing new must not be counted. */
    CHECK_HEX("re-drain ok", 0, tq_run(&q));
    CHECK_HEX("empty kick not recorded", 1u, (unsigned)d->kicks);

    hype_virtio_blk_depth_reset(d);
}


/*
 * #265: the advertised queue size bounds how many requests can be in flight at
 * once, because every virtio-blk request costs a MINIMUM of three descriptors
 * (header, one data segment, status). At the old value of 8 that ceiling was 2,
 * which is exactly the "max depth 2" the queue-depth DIAG measured -- a number
 * that looked like guest behaviour and was actually this constant. Asserted here
 * so the relationship is recorded rather than rediscovered.
 */
static void test_queue_size_bounds_requests_in_flight(void) {
    unsigned max_in_flight = HYPE_VIRTIO_BLK_QUEUE_SIZE_MAX / 3u;

    /* Enough depth for a guest to pipeline, rather than submit-and-wait. A
     * regression back to single digits would silently reimpose the ceiling. */
    if (max_in_flight < 16u) {
        printf("FAIL: queue size %u allows only %u requests in flight -- too shallow "
               "for a guest to keep the write path busy (#265)\n",
               HYPE_VIRTIO_BLK_QUEUE_SIZE_MAX, max_in_flight);
        failures++;
    }
    /* A driver may still negotiate DOWN; the device only caps. */
    {
        hype_virtio_blk_t dev;
        hype_virtio_blk_reset(&dev, 128);
        common_write(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2u, 8u);
        CHECK_HEX("a driver may negotiate a smaller queue", 8u,
                  common_read(&dev, HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE, 2u));
    }
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
    test_chain_single_segment_write_still_works();
    test_chain_multi_segment_write();
    test_chain_multi_segment_read();
    test_chain_flush_has_no_data_descriptor();
    test_chain_cycle_is_rejected();
    test_chain_malformed_shapes_change_nothing();
    test_chain_bad_requests_complete_with_ioerr();
    test_chain_via_real_gpa_map_translates_and_succeeds();
    test_chain_data_segment_outside_mapped_region_is_rejected();
    test_get_id_returns_the_serial_nul_padded();
    test_get_id_is_stable_across_calls();
    test_get_id_default_serial_is_a_valid_20_byte_field();
    test_serial_survives_a_driver_reset();
    test_set_serial_truncates_and_keeps_the_default_for_no_name();
    test_get_id_short_buffer_is_not_overrun();
    test_get_id_with_no_data_descriptor_is_rejected_not_ignored();
    test_chain_unsupported_type_is_completed();
    test_chain_multiple_pending_chains_all_drain();
    test_chain_zero_queue_size_rejected();
    test_chain_reject_log_is_rate_limited();
    test_queue_size_bounds_requests_in_flight();
    test_depth_buckets();
    test_depth_record_accumulates();
    test_depth_is_recorded_by_the_drain();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
