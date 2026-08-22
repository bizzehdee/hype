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

/*
 * #565: enabling the device must LATCH the effective virtual dimensions into the registers a GUEST
 * reads, not only into hype's own computed mode.
 *
 * The two views used to disagree, with only the guest's being wrong: hype rendered correctly from
 * effective_virtual_dimension() while a driver reading VIRT_WIDTH got the 0 it had never set, and
 * therefore a stride of zero. The in-binary VIDEO-3 test could not see this -- it wrote every
 * register from the host and then read hype's computed mode, never the register.
 */
static void test_enable_latches_virtual_dimensions(void) {
    hype_bochs_vbe_t dev;
    hype_bochs_vbe_reset(&dev);
    /* A driver that programs a mode and never touches VIRT_WIDTH -- the ordinary case. */
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_XRES, 640u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_YRES, 480u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_BPP, 32u);

    /* Before enabling, the register still reads what the guest wrote: nothing. */
        CHECK_HEX("virt_width is 0 before enable", 0u, read_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIRT_WIDTH));

    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_ENABLE, HYPE_BOCHS_VBE_ENABLE_ENABLED);

        CHECK_HEX("enable latches virt_width to xres", 640u, read_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIRT_WIDTH));
        CHECK_HEX("enable latches virt_height to yres", 480u, read_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIRT_HEIGHT));
}

/* A guest that DID set a larger virtual width keeps it -- latching must not overwrite an
 * explicit choice, which is what panning depends on. */
static void test_enable_keeps_a_larger_virtual_width(void) {
    hype_bochs_vbe_t dev;
    hype_bochs_vbe_reset(&dev);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_XRES, 640u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_YRES, 480u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_BPP, 32u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIRT_WIDTH, 1024u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_ENABLE, HYPE_BOCHS_VBE_ENABLE_ENABLED);

        CHECK_HEX("a larger virt_width survives enable", 1024u, read_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIRT_WIDTH));
}

/* Disabling must NOT latch: a driver reprogramming a mode writes ENABLE=0 first, and latching
 * there would fix the old mode's dimensions into the new one. */
static void test_disable_does_not_latch(void) {
    hype_bochs_vbe_t dev;
    hype_bochs_vbe_reset(&dev);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_XRES, 800u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_ENABLE, 0u);
        CHECK_HEX("disable leaves virt_width alone", 0u, read_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIRT_WIDTH));
}

/*
 * #655: stride_bytes/fb_offset_bytes and the VIDEO_MEMORY_64K report are all products of
 * guest-controlled 16-bit register values. At large enough values (near the 0xFFFF a guest can
 * legitimately write to any DISPI register) the products exceed UINT32_MAX; a plain uint32_t
 * multiply wraps to a small, wrong value instead of the true (too-large-to-represent) one.
 */
static void test_large_registers_do_not_overflow_stride_or_offset(void) {
    hype_bochs_vbe_t dev;
    hype_bochs_vbe_mode_t mode;

    hype_bochs_vbe_reset(&dev);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_XRES, 0xFFFFu);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_YRES, 0xFFFFu);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_BPP, 32u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIRT_WIDTH, 0xFFFFu);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIRT_HEIGHT, 0xFFFFu);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_Y_OFFSET, 0xFFFFu);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_ENABLE,
             HYPE_BOCHS_VBE_ENABLE_ENABLED | HYPE_BOCHS_VBE_ENABLE_LFB_ENABLED);

    hype_bochs_vbe_get_mode(&dev, &mode);
    /* stride = 0xFFFF * 4 = 0x3FFFC, well within 32 bits -- not the overflowing factor here. */
    CHECK_HEX("stride computed correctly", 0xFFFFu * 4u, mode.stride_bytes);
    /* fb_offset = y_offset(0xFFFF) * stride(0x3FFFC) ~= 17.18e9, which does NOT fit in 32 bits --
     * the mode must be reported invalid rather than publish a wrapped small offset. */
    CHECK_HEX("mode reported invalid rather than a wrapped fb_offset", 0u, mode.valid);
}

static void test_video_memory_64k_saturates_rather_than_wraps(void) {
    hype_bochs_vbe_t dev;
    uint16_t units;

    hype_bochs_vbe_reset(&dev);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_XRES, 0xFFFFu);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_YRES, 0xFFFFu);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_BPP, 32u);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIRT_WIDTH, 0xFFFFu);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIRT_HEIGHT, 0xFFFFu);
    write_reg(&dev, HYPE_BOCHS_VBE_INDEX_ENABLE,
             HYPE_BOCHS_VBE_ENABLE_ENABLED | HYPE_BOCHS_VBE_ENABLE_LFB_ENABLED);

    /* stride(0x3FFFC) * virt_height(0xFFFF) ~= 17.18e9 -- a 32-bit product wraps to a SMALL
     * value (about 262145 mod 2^32, itself still > 0xFFFF, but the point generalizes: any
     * wrapped product could land anywhere, including implausibly small). The fixed 64-bit
     * computation must saturate at the register's own 16-bit ceiling instead. */
    units = read_reg(&dev, HYPE_BOCHS_VBE_INDEX_VIDEO_MEMORY_64K);
    CHECK_HEX("VIDEO_MEMORY_64K saturates at 0xFFFF rather than wrapping", 0xFFFFu, units);
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
    test_enable_latches_virtual_dimensions();
    test_enable_keeps_a_larger_virtual_width();
    test_disable_does_not_latch();
    test_large_registers_do_not_overflow_stride_or_offset();
    test_video_memory_64k_saturates_rather_than_wraps();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
