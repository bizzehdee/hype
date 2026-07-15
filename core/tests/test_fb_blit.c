#include <stdio.h>
#include "../../devices/fb_blit.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_bytes_per_pixel(void) {
    CHECK_HEX("XRGB8888 is 4 bytes", 4u, hype_fb_bytes_per_pixel(HYPE_FB_PIXEL_FORMAT_XRGB8888));
    CHECK_HEX("XBGR8888 is 4 bytes", 4u, hype_fb_bytes_per_pixel(HYPE_FB_PIXEL_FORMAT_XBGR8888));
    CHECK_HEX("RGB565 is 2 bytes", 2u, hype_fb_bytes_per_pixel(HYPE_FB_PIXEL_FORMAT_RGB565));
}

static void test_copy_same_format_is_a_straight_copy(void) {
    uint8_t src[2 * 2 * 4] = {
        0x11, 0x22, 0x33, 0x00, /* pixel (0,0): B=11 G=22 R=33 */
        0x44, 0x55, 0x66, 0x00, /* pixel (1,0) */
        0x77, 0x88, 0x99, 0x00, /* pixel (0,1) */
        0xAA, 0xBB, 0xCC, 0x00, /* pixel (1,1) */
    };
    uint8_t dst[2 * 2 * 4];
    unsigned int i;

    for (i = 0; i < sizeof(dst); i++) {
        dst[i] = 0xFFu;
    }

    hype_fb_blit_copy(src, 2u, 2u, 8u, HYPE_FB_PIXEL_FORMAT_XRGB8888, dst, 2u, 2u, 8u,
                       HYPE_FB_PIXEL_FORMAT_XRGB8888);

    for (i = 0; i < sizeof(src); i++) {
        CHECK_HEX("same-format copy is byte-identical", src[i], dst[i]);
    }
}

static void test_xrgb_to_xbgr_swaps_red_and_blue(void) {
    uint8_t src[4] = {0x11, 0x22, 0x33, 0x00}; /* XRGB8888: B=0x11 G=0x22 R=0x33 */
    uint8_t dst[4] = {0, 0, 0, 0};

    hype_fb_blit_copy(src, 1u, 1u, 4u, HYPE_FB_PIXEL_FORMAT_XRGB8888, dst, 1u, 1u, 4u,
                       HYPE_FB_PIXEL_FORMAT_XBGR8888);

    /* XBGR8888: byte0=R byte1=G byte2=B */
    CHECK_HEX("R channel lands at byte0", 0x33u, dst[0]);
    CHECK_HEX("G channel unchanged at byte1", 0x22u, dst[1]);
    CHECK_HEX("B channel lands at byte2", 0x11u, dst[2]);
}

static void test_rgb565_round_trip_preserves_top_bits(void) {
    uint8_t src[4] = {0xFF, 0xFF, 0xFF, 0x00}; /* XRGB8888: pure white */
    uint8_t mid[2] = {0, 0};
    uint8_t dst[4] = {0, 0, 0, 0};

    hype_fb_blit_copy(src, 1u, 1u, 4u, HYPE_FB_PIXEL_FORMAT_XRGB8888, mid, 1u, 1u, 2u,
                       HYPE_FB_PIXEL_FORMAT_RGB565);
    CHECK_HEX("white packs to 0xFFFF in RGB565", 0xFFFFu, (unsigned)(mid[0] | (mid[1] << 8)));

    hype_fb_blit_copy(mid, 1u, 1u, 2u, HYPE_FB_PIXEL_FORMAT_RGB565, dst, 1u, 1u, 4u,
                       HYPE_FB_PIXEL_FORMAT_XRGB8888);
    CHECK_HEX("white survives an RGB565 round trip (B)", 0xFFu, dst[0]);
    CHECK_HEX("white survives an RGB565 round trip (G)", 0xFFu, dst[1]);
    CHECK_HEX("white survives an RGB565 round trip (R)", 0xFFu, dst[2]);
}

static void test_rgb565_black_stays_black(void) {
    uint8_t src[2] = {0x00, 0x00};
    uint8_t dst[4] = {0xFF, 0xFF, 0xFF, 0xFF};

    hype_fb_blit_copy(src, 1u, 1u, 2u, HYPE_FB_PIXEL_FORMAT_RGB565, dst, 1u, 1u, 4u,
                       HYPE_FB_PIXEL_FORMAT_XRGB8888);
    CHECK_HEX("black B channel", 0x00u, dst[0]);
    CHECK_HEX("black G channel", 0x00u, dst[1]);
    CHECK_HEX("black R channel", 0x00u, dst[2]);
}

static void test_xbgr_source_converts_to_xrgb(void) {
    uint8_t src[4] = {0x33, 0x22, 0x11, 0x00}; /* XBGR8888: R=0x33 G=0x22 B=0x11 */
    uint8_t dst[4] = {0, 0, 0, 0};

    hype_fb_blit_copy(src, 1u, 1u, 4u, HYPE_FB_PIXEL_FORMAT_XBGR8888, dst, 1u, 1u, 4u,
                       HYPE_FB_PIXEL_FORMAT_XRGB8888);

    /* XRGB8888: byte0=B byte1=G byte2=R */
    CHECK_HEX("B channel lands at byte0", 0x11u, dst[0]);
    CHECK_HEX("G channel unchanged at byte1", 0x22u, dst[1]);
    CHECK_HEX("R channel lands at byte2", 0x33u, dst[2]);
}

static void test_unrecognized_format_is_handled_safely(void) {
    uint8_t src[4] = {0x11, 0x22, 0x33, 0x00};
    uint8_t dst[4] = {0xAB, 0xCD, 0xEF, 0x12};
    hype_fb_pixel_format_t bogus = (hype_fb_pixel_format_t)99;

    CHECK_HEX("bytes-per-pixel of an unrecognized format is 0", 0u, hype_fb_bytes_per_pixel(bogus));

    /* An unrecognized src/dst format makes hype_fb_bytes_per_pixel()
     * return 0, which hype_fb_blit_copy() treats as "nothing to
     * copy" -- this is the only validation an out-of-range format
     * gets; extract_rgb()/pack_rgb() are only ever reached once this
     * check has already passed, so they can assume (and their switch
     * statements enumerate, with no default) exactly the 3 real
     * format values. */
    hype_fb_blit_copy(src, 1u, 1u, 4u, bogus, dst, 1u, 1u, 4u, HYPE_FB_PIXEL_FORMAT_XRGB8888);
    CHECK_HEX("unrecognized src format is a safe no-op (zero bpp)", 0xABu, dst[0]);

    hype_fb_blit_copy(src, 1u, 1u, 4u, HYPE_FB_PIXEL_FORMAT_XRGB8888, dst, 1u, 1u, 4u, bogus);
    CHECK_HEX("unrecognized dst format is a safe no-op (zero bpp)", 0xABu, dst[0]);
}

static void test_clips_to_the_smaller_of_src_and_dst(void) {
    /* src is 3x1, dst is 1x1 -- only the first pixel should be copied,
     * and only into the single dst pixel. */
    uint8_t src[3 * 4] = {
        0x01, 0x02, 0x03, 0x00,
        0xAA, 0xBB, 0xCC, 0x00,
        0xDD, 0xEE, 0xFF, 0x00,
    };
    uint8_t dst[4] = {0x99, 0x99, 0x99, 0x99};

    hype_fb_blit_copy(src, 3u, 1u, 12u, HYPE_FB_PIXEL_FORMAT_XRGB8888, dst, 1u, 1u, 4u,
                       HYPE_FB_PIXEL_FORMAT_XRGB8888);

    CHECK_HEX("only the first (clipped) pixel is copied", 0x01u, dst[0]);
    CHECK_HEX("first pixel G", 0x02u, dst[1]);
    CHECK_HEX("first pixel R", 0x03u, dst[2]);
}

static void test_clips_by_height_leaves_extra_dst_rows_untouched(void) {
    uint8_t src[1 * 4] = {0x10, 0x20, 0x30, 0x00}; /* 1x1 source */
    uint8_t dst[2 * 4] = {
        0x55, 0x55, 0x55, 0x55,
        0x66, 0x66, 0x66, 0x66,
    }; /* 1x2 destination */

    hype_fb_blit_copy(src, 1u, 1u, 4u, HYPE_FB_PIXEL_FORMAT_XRGB8888, dst, 1u, 2u, 4u,
                       HYPE_FB_PIXEL_FORMAT_XRGB8888);

    CHECK_HEX("row 0 was overwritten by the copy", 0x10u, dst[0]);
    CHECK_HEX("row 1 is untouched -- src only had one row", 0x66u, dst[4]);
}

static void test_null_pointers_are_a_safe_no_op(void) {
    uint8_t dst[4] = {0xAB, 0xCD, 0xEF, 0x12};
    uint8_t src[4] = {0, 0, 0, 0};

    hype_fb_blit_copy(0, 1u, 1u, 4u, HYPE_FB_PIXEL_FORMAT_XRGB8888, dst, 1u, 1u, 4u,
                       HYPE_FB_PIXEL_FORMAT_XRGB8888);
    CHECK_HEX("NULL src leaves dst untouched", 0xABu, dst[0]);

    hype_fb_blit_copy(src, 1u, 1u, 4u, HYPE_FB_PIXEL_FORMAT_XRGB8888, 0, 1u, 1u, 4u,
                       HYPE_FB_PIXEL_FORMAT_XRGB8888);
    /* Nothing to assert beyond "did not crash" -- a NULL dst has
     * nowhere to write, so this call is purely a no-op. */
}

static void test_zero_stride_is_a_safe_no_op(void) {
    uint8_t src[4] = {0x11, 0x22, 0x33, 0x00};
    uint8_t dst[4] = {0xAB, 0xCD, 0xEF, 0x12};

    hype_fb_blit_copy(src, 1u, 1u, 0u, HYPE_FB_PIXEL_FORMAT_XRGB8888, dst, 1u, 1u, 4u,
                       HYPE_FB_PIXEL_FORMAT_XRGB8888);
    CHECK_HEX("zero src stride leaves dst untouched", 0xABu, dst[0]);

    hype_fb_blit_copy(src, 1u, 1u, 4u, HYPE_FB_PIXEL_FORMAT_XRGB8888, dst, 1u, 1u, 0u,
                       HYPE_FB_PIXEL_FORMAT_XRGB8888);
    CHECK_HEX("zero dst stride leaves dst untouched", 0xABu, dst[0]);
}

int main(void) {
    test_bytes_per_pixel();
    test_copy_same_format_is_a_straight_copy();
    test_xrgb_to_xbgr_swaps_red_and_blue();
    test_xbgr_source_converts_to_xrgb();
    test_unrecognized_format_is_handled_safely();
    test_rgb565_round_trip_preserves_top_bits();
    test_rgb565_black_stays_black();
    test_clips_to_the_smaller_of_src_and_dst();
    test_clips_by_height_leaves_extra_dst_rows_untouched();
    test_null_pointers_are_a_safe_no_op();
    test_zero_stride_is_a_safe_no_op();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
