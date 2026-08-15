#include <stdio.h>
#include <stdlib.h>
#include "../png_write.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_signature_and_chunk_types(void) {
    uint8_t rgb[2 * 2 * 3] = {
        255, 0, 0,  0, 255, 0,
        0, 0, 255,  255, 255, 255,
    };
    uint8_t out[256];
    uint32_t n = hype_png_write(rgb, 2u, 2u, 6u, out, sizeof(out));

    static const uint8_t expected_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    unsigned i;

    CHECK_HEX("encodes something", 1, n > 0u);
    for (i = 0; i < 8u; i++) {
        CHECK_HEX("PNG signature byte", expected_sig[i], out[i]);
    }
    /* IHDR immediately follows the signature: length=13, then "IHDR". */
    CHECK_HEX("IHDR length high byte", 0, out[8]);
    CHECK_HEX("IHDR length low byte (13)", 13, out[11]);
    CHECK_HEX("IHDR type 'I'", 'I', out[12]);
    CHECK_HEX("IHDR type 'H'", 'H', out[13]);
    CHECK_HEX("IHDR type 'D'", 'D', out[14]);
    CHECK_HEX("IHDR type 'R'", 'R', out[15]);
    /* width (4 bytes BE) = 2 */
    CHECK_HEX("IHDR width", 2, out[19]);
    /* height (4 bytes BE) = 2 */
    CHECK_HEX("IHDR height", 2, out[23]);
    CHECK_HEX("bit depth 8", 8, out[24]);
    CHECK_HEX("color type 2 (truecolor)", 2, out[25]);

    /* The file must end with IEND (length 0, type "IEND", crc). */
    CHECK_HEX("ends with IEND type 'I'", 'I', out[n - 8u]);
    CHECK_HEX("ends with IEND type 'E'", 'E', out[n - 7u]);
    CHECK_HEX("ends with IEND type 'N'", 'N', out[n - 6u]);
    CHECK_HEX("ends with IEND type 'D'", 'D', out[n - 5u]);
    CHECK_HEX("IEND length is 0 (byte before type)", 0, out[n - 12u]);
}

static void test_encoded_size_matches_actual_output(void) {
    uint8_t rgb[4 * 3 * 3];
    uint8_t out[512];
    uint32_t predicted, actual;
    unsigned i;
    for (i = 0; i < sizeof(rgb); i++) {
        rgb[i] = (uint8_t)i;
    }
    predicted = hype_png_encoded_size(3u, 4u);
    actual = hype_png_write(rgb, 3u, 4u, 9u, out, sizeof(out));
    CHECK_HEX("hype_png_encoded_size predicts the real output length", predicted, actual);
}

static void test_refuses_when_buffer_too_small(void) {
    uint8_t rgb[3] = {1, 2, 3};
    uint8_t out[4]; /* far too small for even a 1x1 PNG */
    uint32_t n = hype_png_write(rgb, 1u, 1u, 3u, out, sizeof(out));
    CHECK_HEX("returns 0 rather than a truncated file", 0, n);
}

static void test_rejects_invalid_arguments(void) {
    uint8_t rgb[3] = {1, 2, 3};
    uint8_t out[256];
    CHECK_HEX("zero width rejected", 0, hype_png_write(rgb, 0u, 1u, 3u, out, sizeof(out)));
    CHECK_HEX("zero height rejected", 0, hype_png_write(rgb, 1u, 0u, 3u, out, sizeof(out)));
    CHECK_HEX("null rgb rejected", 0, hype_png_write(0, 1u, 1u, 3u, out, sizeof(out)));
    CHECK_HEX("null out rejected", 0, hype_png_write(rgb, 1u, 1u, 3u, 0, 256u));
    CHECK_HEX("stride shorter than a row rejected", 0, hype_png_write(rgb, 2u, 1u, 3u, out, sizeof(out)));
    CHECK_HEX("zero width/height size query is 0", 0, hype_png_encoded_size(0u, 5u));
}

/* A row wider than 65535/3 pixels forces the stored-DEFLATE encoder across more than one
 * block boundary -- exercises the multi-block path, not just the common single-block case. */
static void test_large_image_spans_multiple_deflate_blocks(void) {
    static uint8_t rgb[40000u * 3u]; /* one row: > 65535 raw bytes once the filter byte is added */
    uint8_t *out;
    uint32_t predicted, actual;
    unsigned i;
    for (i = 0; i < sizeof(rgb); i++) {
        rgb[i] = (uint8_t)(i & 0xFFu);
    }
    predicted = hype_png_encoded_size(40000u, 1u);
    out = (uint8_t *)malloc(predicted);
    if (out == 0) {
        printf("FAIL: test setup could not allocate %u bytes\n", predicted);
        failures++;
        return;
    }
    actual = hype_png_write(rgb, 40000u, 1u, 40000u * 3u, out, predicted);
    CHECK_HEX("multi-block image still matches its predicted size", predicted, actual);
    CHECK_HEX("multi-block image encodes successfully", 1, actual > 0u);
    free(out);
}

int main(void) {
    test_signature_and_chunk_types();
    test_encoded_size_matches_actual_output();
    test_refuses_when_buffer_too_small();
    test_rejects_invalid_arguments();
    test_large_image_spans_multiple_deflate_blocks();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
