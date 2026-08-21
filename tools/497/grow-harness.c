#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
/*
 * #497 real-image harness: drive hype's OWN ext writers against images made by real mke2fs,
 * appending the ticket's 64 MiB, so e2fsck and a byte-exact host readback judge the result.
 *
 *   grow-harness <image> <path-in-volume> <mib-to-append> <seed>
 *
 * Exit 0 = append + in-hype readback OK. The BYTES are deterministic from <seed>, so run-497.sh
 * regenerates them independently for its own byte-exact comparison after e2fsck.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../core/fs_ops.h"

static FILE *g_img;

static int img_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if (fseeko(g_img, (off_t)lba * 512, SEEK_SET) != 0) return -1;
    return fread(dst, 512, count, g_img) == (size_t)count ? 0 : -1;
}
static int img_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    if (fseeko(g_img, (off_t)lba * 512, SEEK_SET) != 0) return -1;
    return fwrite(src, 512, count, g_img) == (size_t)count ? 0 : -1;
}

int main(int argc, char **argv) {
    static hype_fs_t fs;
    static hype_fs_file_t f;
    static unsigned char chunk[1024 * 1024];
    unsigned long long mib, i, sz0;
    unsigned seed, j;

    if (argc != 5) { fprintf(stderr, "usage: %s <img> <path> <mib> <seed>\n", argv[0]); return 2; }
    g_img = fopen(argv[1], "r+b");
    if (!g_img) { perror("image"); return 2; }
    mib = strtoull(argv[3], 0, 10);
    seed = (unsigned)strtoul(argv[4], 0, 10);

    if (hype_fs_mount_auto(&fs, img_read, img_write, 0) != 0) {
        fprintf(stderr, "mount failed\n"); return 1;
    }
    if (!(hype_fs_caps(&fs) & HYPE_FS_CAP_APPEND)) {
        fprintf(stderr, "no append cap\n"); return 1;
    }
    if (hype_fs_lookup(&fs, argv[2], &f) != 0) { fprintf(stderr, "lookup failed\n"); return 1; }
    sz0 = f.size;
    fprintf(stderr, "file=%s size=%llu -> appending %llu MiB\n", argv[2], sz0, mib);

    for (i = 0; i < mib; i++) {
        for (j = 0; j < sizeof(chunk); j++) {
            chunk[j] = (unsigned char)((seed * 2654435761u + (unsigned)(i * sizeof(chunk) + j)) >> 3);
        }
        if (hype_fs_append(&f, chunk, sizeof(chunk)) != 0) {
            fprintf(stderr, "append failed at MiB %llu\n", i); return 1;
        }
    }
    if (f.size != sz0 + mib * sizeof(chunk)) { fprintf(stderr, "size wrong\n"); return 1; }

    /* in-hype byte-exact readback of the whole appended range */
    for (i = 0; i < mib; i++) {
        static unsigned char back[1024 * 1024];
        if (hype_fs_read_at(&f, sz0 + i * sizeof(back), back, sizeof(back)) != 0) {
            fprintf(stderr, "readback failed at MiB %llu\n", i); return 1;
        }
        for (j = 0; j < sizeof(back); j++) {
            unsigned char want = (unsigned char)((seed * 2654435761u + (unsigned)(i * sizeof(back) + j)) >> 3);
            if (back[j] != want) { fprintf(stderr, "byte mismatch MiB %llu +%u\n", i, j); return 1; }
        }
    }
    fclose(g_img);
    fprintf(stderr, "append + readback OK\n");
    return 0;
}
