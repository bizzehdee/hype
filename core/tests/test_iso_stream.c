#include <stdio.h>
#include "../iso_stream.h"

static int failures = 0;
static unsigned g_reads; /* count disk reads, to check the bounce loop */

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* Deterministic byte at absolute disk offset D -- a mixing hash so an off-by-one
 * or wrong-sector bug shows up (unlike a low-byte pattern that repeats every 256). */
static uint8_t pat(uint64_t d) {
    return (uint8_t)((d * 1103515245ull + 12345ull) >> 16);
}

#define PART_START 4096ull
#define ISO_SIZE 200000ull

/* Synthetic infinite disk: sector `lba` holds pat(lba*512 + i). */
static int fake_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    (void)ctx;
    g_reads++;
    for (i = 0; i < count * 512u; i++) {
        d[i] = pat(lba * 512ull + i);
    }
    return 0;
}

/* The correct streamed byte for logical ISO offset O = disk byte at
 * PART_START*512 + O. */
static uint8_t expect_byte(uint64_t off) {
    return pat(PART_START * 512ull + off);
}

static void check_range(const char *desc, hype_iso_stream_t *s, uint64_t off, uint32_t len) {
    static uint8_t buf[262144];
    uint32_t i;
    int mism = -1;
    CHECK_HEX(desc, 0, hype_iso_stream_read(s, off, buf, len));
    for (i = 0; i < len; i++) {
        if (buf[i] != expect_byte(off + i)) {
            mism = (int)i;
            break;
        }
    }
    CHECK_HEX(desc, -1, mism); /* -1 == no mismatch */
}

int main(void) {
    hype_iso_stream_t s;
    s.read = fake_read;
    s.ctx = 0;
    s.part_start_lba = PART_START;
    s.iso_size = ISO_SIZE;

    check_range("aligned 1 sector", &s, 0, 512);
    check_range("aligned multi-sector", &s, 1024, 2048);
    check_range("misaligned within a sector", &s, 100, 50);
    check_range("misaligned spanning a sector boundary", &s, 500, 1000);
    check_range("2048-byte CD sector at a high offset", &s, 4096 * 2048ull % ISO_SIZE, 2048);

    /* Large read must loop the 64 KiB bounce and still assemble correctly. */
    g_reads = 0;
    check_range("100000-byte read crosses the bounce buffer", &s, 0, 100000);
    CHECK_HEX("large read took multiple disk fills", 1, (g_reads > 1) ? 1 : 0);

    /* Bounds. */
    {
        static uint8_t buf[32];
        CHECK_HEX("off past end rejected", -1,
                  hype_iso_stream_read(&s, ISO_SIZE + 1u, buf, 1));
        CHECK_HEX("off+len past end rejected", -1,
                  hype_iso_stream_read(&s, ISO_SIZE - 10u, buf, 20));
        CHECK_HEX("exact-end read ok", 0,
                  hype_iso_stream_read(&s, ISO_SIZE - 16u, buf, 16));
    }

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
