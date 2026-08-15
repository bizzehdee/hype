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

#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)

/*
 * #463: a minimal DEFLATE reader for BTYPE=01 (fixed Huffman) -- the only block type
 * hype_png_write() emits. Its whole job is to prove the bitstream hype produces is one a real
 * decoder accepts: structural checks on chunk headers cannot tell a valid Huffman stream from a
 * plausible-looking broken one, and that is exactly the mistake that shipped a screenshot
 * feature nobody had opened in a viewer.
 */
typedef struct { const uint8_t *d; unsigned n, pos, bit; } br_t;

static int br_bit(br_t *b) {
    int v;
    if (b->pos >= b->n) return -1;
    v = (b->d[b->pos] >> b->bit) & 1;
    if (++b->bit == 8u) { b->bit = 0; b->pos++; }
    return v;
}

static long br_bits(br_t *b, unsigned count) { /* LSB-first: headers and extra bits */
    unsigned long v = 0; unsigned i;
    for (i = 0; i < count; i++) { int x = br_bit(b); if (x < 0) return -1; v |= (unsigned long)x << i; }
    return (long)v;
}

/* Fixed literal/length decode: read bits MSB-first and match the RFC 1951 3.2.6 ranges. */
static long br_fixed_symbol(br_t *b) {
    unsigned long code = 0; unsigned len;
    for (len = 1; len <= 9u; len++) {
        int x = br_bit(b);
        if (x < 0) return -1;
        code = (code << 1) | (unsigned long)x;
        if (len == 7u && code <= 0x17u) return (long)(256u + code);
        if (len == 8u && code >= 0x30u && code <= 0xBFu) return (long)(code - 0x30u);
        if (len == 8u && code >= 0xC0u && code <= 0xC7u) return (long)(280u + (code - 0xC0u));
        if (len == 9u && code >= 0x190u) return (long)(144u + (code - 0x190u));
    }
    return -1;
}

/* Inflates one fixed-Huffman stream into `out`; returns bytes produced, or -1. */
static long inflate_fixed(const uint8_t *src, unsigned srclen, uint8_t *out, unsigned outcap) {
    static const unsigned short lbase[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
                                           67,83,99,115,131,163,195,227,258};
    static const unsigned char lextra[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const unsigned short dbase[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,
                                           513,769,1025,1537,2049,3073,4097,6145};
    static const unsigned char dextra[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11};
    br_t b; unsigned n = 0; long final, type;

    b.d = src; b.n = srclen; b.pos = 0; b.bit = 0;
    final = br_bits(&b, 1u);
    type = br_bits(&b, 2u);
    if (final != 1 || type != 1) return -1; /* one final fixed-Huffman block is all we emit */
    for (;;) {
        long sym = br_fixed_symbol(&b);
        if (sym < 0) return -1;
        if (sym == 256) return (long)n;
        if (sym < 256) {
            if (n >= outcap) return -1;
            out[n++] = (uint8_t)sym;
            continue;
        }
        {
            unsigned li = (unsigned)sym - 257u, i;
            long e, dsym, de, len, dist;
            if (li >= sizeof(lbase)/sizeof(lbase[0])) return -1;
            e = lextra[li] ? br_bits(&b, lextra[li]) : 0;
            if (e < 0) return -1;
            len = lbase[li] + e;
            dsym = 0;
            for (i = 0; i < 5u; i++) { int x = br_bit(&b); if (x < 0) return -1; dsym = (dsym << 1) | x; }
            if ((unsigned)dsym >= sizeof(dbase)/sizeof(dbase[0])) return -1;
            de = dextra[dsym] ? br_bits(&b, dextra[dsym]) : 0;
            if (de < 0) return -1;
            dist = dbase[dsym] + de;
            if (dist > (long)n) return -1;
            for (i = 0; i < (unsigned)len; i++) {
                if (n >= outcap) return -1;
                out[n] = out[n - (unsigned)dist];  /* overlapping copy is legal and is the RLE */
                n++;
            }
        }
    }
}

/* Locates the IDAT payload in a PNG and inflates it, then checks it against the scanlines the
 * caller says it should be (filter byte 0 + RGB per row). */
static void check_roundtrip(const char *what, const uint8_t *png, unsigned png_len,
                            const uint8_t *rgb, unsigned width, unsigned height,
                            unsigned stride) {
    unsigned p = 8u; /* past the signature */
    unsigned raw_len = height * (1u + width * 3u);
    uint8_t *got = malloc(raw_len ? raw_len : 1u);
    long n = -1;

    while (p + 12u <= png_len) {
        unsigned clen = ((unsigned)png[p] << 24) | ((unsigned)png[p+1] << 16) |
                        ((unsigned)png[p+2] << 8) | png[p+3];
        if (png[p+4] == 'I' && png[p+5] == 'D' && png[p+6] == 'A' && png[p+7] == 'T') {
            /* skip the 2-byte zlib header, stop before the 4-byte adler32 */
            n = inflate_fixed(png + p + 8u + 2u, clen - 6u, got, raw_len);
            break;
        }
        p += 12u + clen;
    }
    if (n != (long)raw_len) {
        printf("FAIL: %s: inflate produced %ld bytes, expected %u\n", what, n, raw_len);
        failures++; free(got); return;
    }
    {
        unsigned r, c, k = 0, bad = 0;
        for (r = 0; r < height; r++) {
            if (got[k++] != 0u) bad++;                       /* filter type None */
            for (c = 0; c < width * 3u; c++) {
                if (got[k++] != rgb[r * stride + c]) bad++;
            }
        }
        if (bad != 0) { printf("FAIL: %s: %u byte(s) differ after round-trip\n", what, bad); failures++; }
    }
    free(got);
}

static void test_roundtrip_solid_and_mixed(void) {
    /* A solid image is all RLE; a gradient is all literals; a striped one alternates. All three
     * must decode back byte-for-byte. */
    static uint8_t solid[16 * 8 * 3];
    static uint8_t grad[16 * 8 * 3];
    static uint8_t stripe[16 * 8 * 3];
    static uint8_t out[262144];
    unsigned i;
    uint32_t n;

    for (i = 0; i < sizeof(solid); i++) solid[i] = 0x20u;
    for (i = 0; i < sizeof(grad); i++) grad[i] = (uint8_t)(i * 7u);
    for (i = 0; i < sizeof(stripe); i++) stripe[i] = (uint8_t)((i / 24u) % 2u ? 0xFFu : 0x00u);

    n = hype_png_write(solid, 16u, 8u, 48u, out, sizeof(out));
    CHECK("solid image encodes", n > 0);
    check_roundtrip("solid", out, n, solid, 16u, 8u, 48u);
    /* The whole point of #463. Kept loose at this size because ~57 bytes of fixed PNG chunk
     * overhead dominate a 384-byte image; the long-run test below is where the ratio is
     * actually asserted, and a real 1920x1080 all-black capture comes to 39 KB against 6.2 MB
     * raw. */
    CHECK("solid image actually compresses", n < sizeof(solid) / 2u);

    n = hype_png_write(grad, 16u, 8u, 48u, out, sizeof(out));
    CHECK("gradient image encodes", n > 0);
    check_roundtrip("gradient", out, n, grad, 16u, 8u, 48u);

    n = hype_png_write(stripe, 16u, 8u, 48u, out, sizeof(out));
    CHECK("striped image encodes", n > 0);
    check_roundtrip("striped", out, n, stripe, 16u, 8u, 48u);
}

static void test_roundtrip_long_runs_cross_match_limit(void) {
    /* Runs longer than DEFLATE's 258-byte maximum match must be split correctly, including the
     * awkward remainders of 1 and 2 that cannot be a match at all. */
    static uint8_t img[1024 * 3];
    static uint8_t out[65536];
    unsigned len;
    for (len = 0; len < sizeof(img); len++) img[len] = 0x7Fu;
    /* one differing byte near the end, so the final run has an odd tail */
    img[sizeof(img) - 2u] = 0x01u;
    {
        uint32_t n = hype_png_write(img, 1024u, 1u, 3072u, out, sizeof(out));
        CHECK("long-run image encodes", n > 0);
        check_roundtrip("long runs", out, n, img, 1024u, 1u, 3072u);
        CHECK("long runs compress hard", n < sizeof(img) / 8u);
    }
}

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
    /* #463: the encoder compresses now, so the size function is an upper bound, not an exact
     * count. Both directions matter: it must never under-predict (the buffer would be too
     * small), and a real image must actually fit. */
    CHECK("hype_png_encoded_size bounds the real output length", actual > 0 && actual <= predicted);
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
    CHECK("large image fits inside its predicted bound", actual > 0 && actual <= predicted);
    CHECK_HEX("multi-block image encodes successfully", 1, actual > 0u);
    free(out);
}

int main(void) {
    test_signature_and_chunk_types();
    test_roundtrip_solid_and_mixed();
    test_roundtrip_long_runs_cross_match_limit();
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
