#include <stdio.h>
#include "../../devices/bochs_vbe.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void write_reg(hype_bochs_vbe_t *dev, uint32_t index, uint16_t value) {
    int rc = hype_bochs_vbe_mmio_write(dev, index * 2u, value);
    if (rc != 0) {
        printf("FAIL: write_reg(%u) unexpectedly rejected\n", index);
        failures++;
    }
}

static uint16_t read_reg(hype_bochs_vbe_t *dev, uint32_t index) {
    uint16_t value = 0xFFFFu;
    int rc = hype_bochs_vbe_mmio_read(dev, index * 2u, &value);
    if (rc != 0) {
        printf("FAIL: read_reg(%u) unexpectedly rejected\n", index);
        failures++;
    }
    return value;
}

static void test_reset_clears_all_registers(void) {
    hype_bochs_vbe_t dev;
    uint32_t i;

    hype_bochs_vbe_reset(&dev);
    for (i = 0; i < HYPE_BOCHS_VBE_NUM_REGS; i++) {
        CHECK_HEX("register starts at 0 after reset", 0, dev.regs[i]);
    }
}

static void test_id_register_always_reads_id5(void) {
    hype_bochs_vbe_t dev;
    uint16_t value;

    hype_bochs_vbe_reset(&dev);
    value = read_reg(&dev, HYPE_BOCHS_VBE_INDEX_ID);
    CHECK_HEX("ID reads ID5 before any write", HYPE_BOCHS_VBE_ID5, value);

    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_ID, (uint16_t)HYPE_BOCHS_VBE_ID0);
    value = read_reg(&dev, HYPE_BOCHS_VBE_INDEX_ID);
    CHECK_HEX("ID write is ignored -- still reads ID5", HYPE_BOCHS_VBE_ID5, value);
}

static void test_ordinary_registers_roundtrip(void) {
    hype_bochs_vbe_t dev;

    hype_bochs_vbe_reset(&dev);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_XRES, 1024u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_YRES, 768u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_BPP, 32u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_BANK, 7u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_X_OFFSET, 3u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_Y_OFFSET, 5u);

    CHECK_HEX("XRES roundtrips", 1024u, read_reg(&dev, HYPE_BOCHS_VBE_INDEX_XRES));
    CHECK_HEX("YRES roundtrips", 768u, read_reg(&dev, HYPE_BOCHS_VBE_INDEX_YRES));
    CHECK_HEX("BPP roundtrips", 32u, read_reg(&dev, HYPE_BOCHS_VBE_INDEX_BPP));
    CHECK_HEX("BANK roundtrips", 7u, read_reg(&dev, HYPE_BOCHS_VBE_INDEX_BANK));
    CHECK_HEX("X_OFFSET roundtrips", 3u, read_reg(&dev, HYPE_BOCHS_VBE_INDEX_X_OFFSET));
    CHECK_HEX("Y_OFFSET roundtrips", 5u, read_reg(&dev, HYPE_BOCHS_VBE_INDEX_Y_OFFSET));
}

static void test_video_memory_64k_is_read_only_and_computed(void) {
    hype_bochs_vbe_t dev;
    uint16_t value;

    hype_bochs_vbe_reset(&dev);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_XRES, 640u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_YRES, 480u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_BPP, 32u);
    /* 640*480*4 = 1,228,800 bytes = 18.75 64K units -> rounds up to 19 */
    value = read_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIDEO_MEMORY_64K);
    CHECK_HEX("VIDEO_MEMORY_64K computed from XRES*YRES*bpp, rounded up", 19u, value);

    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIDEO_MEMORY_64K, 0xFFFFu);
    value = read_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIDEO_MEMORY_64K);
    CHECK_HEX("VIDEO_MEMORY_64K write is ignored -- still computed", 19u, value);
}

static void test_mmio_read_write_reject_misaligned_or_out_of_range(void) {
    hype_bochs_vbe_t dev;
    uint16_t value = 0;
    int rc;

    hype_bochs_vbe_reset(&dev);
    rc = hype_bochs_vbe_mmio_read(&dev, 1u, &value); /* odd offset */
    CHECK_HEX("misaligned read is rejected", -1, rc);
    rc = hype_bochs_vbe_mmio_write(&dev, 1u, 0u);
    CHECK_HEX("misaligned write is rejected", -1, rc);

    rc = hype_bochs_vbe_mmio_read(&dev, HYPE_BOCHS_VBE_DISPI_SIZE, &value); /* one past the end */
    CHECK_HEX("out-of-range read is rejected", -1, rc);
    rc = hype_bochs_vbe_mmio_write(&dev, HYPE_BOCHS_VBE_DISPI_SIZE, 0u);
    CHECK_HEX("out-of-range write is rejected", -1, rc);
}

static void test_mode_invalid_when_disabled(void) {
    hype_bochs_vbe_t dev;
    hype_bochs_vbe_mode_t mode;

    hype_bochs_vbe_reset(&dev);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_XRES, 640u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_YRES, 480u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_BPP, 32u);
    /* ENABLE left at 0 (disabled) */
    hype_bochs_vbe_get_mode(&dev, &mode);
    CHECK_HEX("mode is invalid while ENABLE is 0", 0, mode.valid);
}

static void test_mode_invalid_without_lfb_enabled(void) {
    hype_bochs_vbe_t dev;
    hype_bochs_vbe_mode_t mode;

    hype_bochs_vbe_reset(&dev);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_XRES, 640u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_YRES, 480u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_BPP, 32u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_ENABLE, (uint16_t)HYPE_BOCHS_VBE_ENABLE_ENABLED);
    hype_bochs_vbe_get_mode(&dev, &mode);
    CHECK_HEX("mode is invalid without LFB_ENABLED", 0, mode.valid);
}

static void test_mode_invalid_for_unsupported_bpp(void) {
    hype_bochs_vbe_t dev;
    hype_bochs_vbe_mode_t mode;

    hype_bochs_vbe_reset(&dev);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_XRES, 640u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_YRES, 480u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_BPP, 24u); /* legacy-VBE-only bpp, not modeled */
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_ENABLE,
              (uint16_t)(HYPE_BOCHS_VBE_ENABLE_ENABLED | HYPE_BOCHS_VBE_ENABLE_LFB_ENABLED));
    hype_bochs_vbe_get_mode(&dev, &mode);
    CHECK_HEX("mode is invalid for an unsupported bpp", 0, mode.valid);
}

static void test_mode_invalid_for_zero_resolution(void) {
    hype_bochs_vbe_t dev;
    hype_bochs_vbe_mode_t mode;

    hype_bochs_vbe_reset(&dev);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_BPP, 32u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_ENABLE,
              (uint16_t)(HYPE_BOCHS_VBE_ENABLE_ENABLED | HYPE_BOCHS_VBE_ENABLE_LFB_ENABLED));
    hype_bochs_vbe_get_mode(&dev, &mode);
    CHECK_HEX("mode is invalid when XRES/YRES were never set", 0, mode.valid);
}

static void test_mode_valid_16bpp_simple(void) {
    hype_bochs_vbe_t dev;
    hype_bochs_vbe_mode_t mode;

    hype_bochs_vbe_reset(&dev);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_XRES, 320u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_YRES, 200u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_BPP, 16u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_ENABLE,
              (uint16_t)(HYPE_BOCHS_VBE_ENABLE_ENABLED | HYPE_BOCHS_VBE_ENABLE_LFB_ENABLED));
    hype_bochs_vbe_get_mode(&dev, &mode);

    CHECK_HEX("16bpp mode is valid", 1, mode.valid);
    CHECK_HEX("width", 320u, mode.width);
    CHECK_HEX("height", 200u, mode.height);
    CHECK_HEX("bytes_per_pixel for 16bpp", 2u, mode.bytes_per_pixel);
    /* VIRT_WIDTH never set (0) -- auto-raised to XRES */
    CHECK_HEX("stride defaults to width*bpp when VIRT_WIDTH unset", 320u * 2u, mode.stride_bytes);
    CHECK_HEX("fb_offset is 0 with no panning offsets set", 0u, mode.fb_offset_bytes);
}

static void test_mode_valid_32bpp_with_virtual_width_and_panning(void) {
    hype_bochs_vbe_t dev;
    hype_bochs_vbe_mode_t mode;

    hype_bochs_vbe_reset(&dev);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_XRES, 640u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_YRES, 480u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_BPP, 32u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIRT_WIDTH, 800u); /* wider than XRES -- panning */
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_X_OFFSET, 10u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_Y_OFFSET, 2u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_ENABLE,
              (uint16_t)(HYPE_BOCHS_VBE_ENABLE_ENABLED | HYPE_BOCHS_VBE_ENABLE_LFB_ENABLED));
    hype_bochs_vbe_get_mode(&dev, &mode);

    CHECK_HEX("32bpp panned mode is valid", 1, mode.valid);
    CHECK_HEX("bytes_per_pixel for 32bpp", 4u, mode.bytes_per_pixel);
    CHECK_HEX("stride uses the guest's own (wider) VIRT_WIDTH", 800u * 4u, mode.stride_bytes);
    /* fb_offset = x_offset*bpp + y_offset*stride = 10*4 + 2*3200 = 40 + 6400 */
    CHECK_HEX("fb_offset reflects both panning offsets", 40u + 6400u, mode.fb_offset_bytes);
}

static void test_virt_width_smaller_than_xres_is_clamped_up(void) {
    hype_bochs_vbe_t dev;
    hype_bochs_vbe_mode_t mode;

    hype_bochs_vbe_reset(&dev);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_XRES, 640u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_YRES, 480u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_BPP, 32u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIRT_WIDTH, 100u); /* smaller than XRES */
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_ENABLE,
              (uint16_t)(HYPE_BOCHS_VBE_ENABLE_ENABLED | HYPE_BOCHS_VBE_ENABLE_LFB_ENABLED));
    hype_bochs_vbe_get_mode(&dev, &mode);

    CHECK_HEX("a too-small VIRT_WIDTH is clamped up to XRES", 640u * 4u, mode.stride_bytes);
}

int main(void) {
    test_reset_clears_all_registers();
    test_id_register_always_reads_id5();
    test_ordinary_registers_roundtrip();
    test_video_memory_64k_is_read_only_and_computed();
    test_mmio_read_write_reject_misaligned_or_out_of_range();
    test_mode_invalid_when_disabled();
    test_mode_invalid_without_lfb_enabled();
    test_mode_invalid_for_unsupported_bpp();
    test_mode_invalid_for_zero_resolution();
    test_mode_valid_16bpp_simple();
    test_mode_valid_32bpp_with_virtual_width_and_panning();
    test_virt_width_smaller_than_xres_is_clamped_up();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
