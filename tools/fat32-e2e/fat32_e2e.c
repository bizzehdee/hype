/*
 * #597: end-to-end exercise of hype's FAT32 writer (core/fat_write_fs.c + core/fat_write.c)
 * against a REAL mkfs.vfat volume, driving every write path across many small, medium and large
 * files, and leaving the volume for an INDEPENDENT reader (fsck.vfat + mtools) to judge.
 *
 * The unit tests round-trip through hype's OWN reader; #596 was a chain that hype could round-trip
 * but Linux rejected (EIO at a cluster boundary), so a hype-reads-hype test cannot see it. This
 * harness therefore does two independent things:
 *   1. hype-side self-check: every file is read back with hype_fat32_read_at and compared
 *      byte-exact to what was written (a fail here is a writer/reader disagreement).
 *   2. external judgement: the bytes written are also dumped to <expectdir>/<flat> and the paths
 *      recorded in <manifest>, so run-fat32-e2e.sh can mcopy each file out and cmp it, and run
 *      fsck.vfat over the whole volume. That is the check that catches #596.
 *
 * argv: <image> <ok|dead> <expectdir> <manifest>
 *   ok   -- every durability barrier succeeds (the volume must be clean).
 *   dead -- the barrier fails from the second call onward and never recovers (#516: a device
 *           rejecting SYNCHRONIZE CACHE(10)); the volume must STILL be structurally clean.
 *
 * Build and run through tools/fat32-e2e/run-fat32-e2e.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/fat_write_fs.h"

#define SECSZ 512u

static FILE *g_img;
static long g_sync_calls;
static int g_sync_dead;
static int g_sync_armed; /* the dead policy applies only once the battery has started */
static int g_failures;

static int img_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if (fseek(g_img, (long)(lba * SECSZ), SEEK_SET) != 0) return -1;
    return fread(dst, SECSZ, count, g_img) == count ? 0 : -1;
}
static int img_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    if (fseek(g_img, (long)(lba * SECSZ), SEEK_SET) != 0) return -1;
    return fwrite(src, SECSZ, count, g_img) == count ? 0 : -1;
}
static int img_sync(void *ctx) {
    (void)ctx;
    g_sync_calls++;
    if (g_sync_armed && g_sync_dead && g_sync_calls >= 2) return -1;
    fflush(g_img);
    return 0;
}

/* Deterministic content: distinct per (seed,offset) and non-zero-dominated, so a wrong cluster
 * reads back as a detectable mismatch rather than a plausible run of zeros. */
static void fill(unsigned char *buf, unsigned int seed, unsigned int len) {
    unsigned int i;
    for (i = 0; i < len; i++) {
        unsigned int v = seed * 2654435761u + i * 40503u + (i >> 8) * 97u + (i >> 16) * 131u;
        buf[i] = (unsigned char)((v >> 13) ^ v ^ (seed + i));
        if (buf[i] == 0) buf[i] = (unsigned char)(0xA5u ^ (i & 0xFFu) ^ (seed & 0xFFu));
    }
}

static char *g_expectdir;
static FILE *g_manifest;

/* Flatten a path into an expectdir filename. */
static void flatten(const char *path, char *out) {
    unsigned int i;
    for (i = 0; path[i]; i++) out[i] = (path[i] == '/') ? '_' : path[i];
    out[i] = '\0';
}

/* Dump the expected bytes and record the surviving path+size for external validation. */
static void record(const char *path, const unsigned char *buf, unsigned int len) {
    char flat[512];
    char fn[1024];
    FILE *e;
    flatten(path, flat);
    snprintf(fn, sizeof fn, "%s/%s", g_expectdir, flat);
    e = fopen(fn, "wb");
    if (!e) { printf("FAIL: cannot open expect file %s\n", fn); g_failures++; return; }
    if (len && fwrite(buf, 1u, len, e) != len) { printf("FAIL: short write to %s\n", fn); g_failures++; }
    fclose(e);
    fprintf(g_manifest, "%s %u\n", path, len);
}

/* hype-side self-check: re-open by path and compare every byte to `buf`, reading in awkward
 * spans so cluster-boundary crossings are exercised. */
static void verify_hype(hype_fat32_fs_t *fs, const char *path, const unsigned char *buf,
                        unsigned int len) {
    hype_fat32_wfile_t f;
    unsigned char got[8192];
    uint64_t off = 0;
    if (hype_fat32_open(fs, path, &f) != 0) {
        printf("FAIL: hype cannot re-open %s\n", path);
        g_failures++;
        return;
    }
    if (f.size != len) {
        printf("FAIL: %s hype size=%llu expected=%u\n", path, (unsigned long long)f.size, len);
        g_failures++;
    }
    while (off < len) {
        unsigned int rem = (unsigned int)(len - off);
        unsigned int want = (rem < 1000u) ? rem : (700u + (off & 0x1FFu));
        if (want > rem) want = rem; /* never read past EOF -- read_at refuses, not clamps */
        if (want > sizeof got) want = sizeof got;
        if (hype_fat32_read_at(&f, off, got, want) != 0) {
            printf("FAIL: %s hype read_at(%llu,%u) failed\n", path, (unsigned long long)off, want);
            g_failures++;
            return;
        }
        if (memcmp(got, buf + off, want) != 0) {
            printf("FAIL: %s hype content mismatch at offset %llu\n", path, (unsigned long long)off);
            g_failures++;
            return;
        }
        off += want;
    }
}

static unsigned char g_buf[8u * 1024u * 1024u];

/* Write a file by one of three paths, self-check it, and record it for external validation.
 * mode: 0 = single append; 1 = many small appends (the log pattern behind #596);
 *       2 = write_at growing from empty. Returns 0 on success, -1 if the writer refused
 *       (e.g. the volume is full) -- a refusal is not recorded and not a failure by itself. */
static int make_file_r(hype_fat32_fs_t *fs, const char *path, unsigned int seed, unsigned int len,
                       int mode, int do_record) {
    hype_fat32_wfile_t f;
    if (len > sizeof g_buf) len = sizeof g_buf;
    fill(g_buf, seed, len);
    if (hype_fat32_create(fs, path, &f) != 0) return -1;
    if (mode == 0) {
        if (len && hype_fat32_append(&f, g_buf, len) != 0) return -1;
    } else if (mode == 1) {
        unsigned int done = 0;
        while (done < len) {
            unsigned int chunk = 137u + (done & 0x1FFu); /* deliberately un-aligned to clusters */
            if (chunk > len - done) chunk = len - done;
            if (hype_fat32_append(&f, g_buf + done, chunk) != 0) return -1;
            done += chunk;
        }
    } else {
        if (len && hype_fat32_write_at(&f, 0, g_buf, len) < 0) return -1;
    }
    verify_hype(fs, path, g_buf, len);
    if (do_record) record(path, g_buf, len);
    return 0;
}

static int make_file(hype_fat32_fs_t *fs, const char *path, unsigned int seed, unsigned int len,
                     int mode) {
    return make_file_r(fs, path, seed, len, mode, 1);
}

int main(int argc, char **argv) {
    hype_fat32_fs_t fs;
    hype_rtc_time_t now;
    char path[256];
    unsigned int i;

    if (argc < 5) {
        fprintf(stderr, "usage: %s <image> <ok|dead> <expectdir> <manifest>\n", argv[0]);
        return 2;
    }
    g_sync_dead = (strcmp(argv[2], "dead") == 0);
    g_expectdir = argv[3];
    g_img = fopen(argv[1], "r+b");
    if (!g_img) { perror("open image"); return 2; }
    g_manifest = fopen(argv[4], "wb");
    if (!g_manifest) { perror("open manifest"); return 2; }

    if (hype_fat32_fs_mount(img_read, img_write, 0, &fs) != 0) {
        fprintf(stderr, "mount failed\n");
        return 2;
    }
    memset(&now, 0, sizeof now);
    now.year = 2026; now.month = 8; now.day = 21; now.hour = 12; now.minute = 0; now.second = 0;
    hype_fat32_fs_set_time(&fs, &now);
    hype_fat32_fs_set_sync(&fs, img_sync);
    g_sync_armed = 1; /* the dead policy is in force for the whole battery */

    /* --- Small files: every mode, boundary sizes, plus a crowd (fills the root dir, LFN + 8.3). */
    {
        static const unsigned int small[] = {0u, 1u, 63u, 127u, 511u, 512u, 513u, 1000u};
        for (i = 0; i < sizeof small / sizeof small[0]; i++) {
            int m;
            for (m = 0; m < 3; m++) {
                snprintf(path, sizeof path, "SMALL_%u_M%d.BIN", small[i], m);
                make_file(&fs, path, 0x1000u + i * 3u + (unsigned)m, small[i], m);
            }
        }
        for (i = 0; i < 64u; i++) {
            /* long, mixed-case names -> LFN entries; a crowd of them stresses directory growth. */
            snprintf(path, sizeof path, "many-small-file-number-%02u.log", i);
            make_file(&fs, path, 0x2000u + i, 100u + i * 7u, 1);
        }
    }

    /* --- Medium files: a few clusters each, both the log-append pattern and write_at. */
    {
        static const unsigned int med[] = {4096u, 12288u, 40000u, 65536u};
        for (i = 0; i < sizeof med / sizeof med[0]; i++) {
            snprintf(path, sizeof path, "MED_%u_APPEND.BIN", med[i]);
            make_file(&fs, path, 0x3000u + i, med[i], 1);
            snprintf(path, sizeof path, "MED_%u_WRITEAT.BIN", med[i]);
            make_file(&fs, path, 0x3800u + i, med[i], 2);
        }
    }

    /* --- Large files: many clusters -> FAT-sector spanning, FSInfo free_count/next_free churn. */
    {
        static const unsigned int large[] = {262144u, 1048576u, 4194304u};
        for (i = 0; i < sizeof large / sizeof large[0]; i++) {
            snprintf(path, sizeof path, "LARGE_%u_APPEND.BIN", large[i]);
            make_file(&fs, path, 0x4000u + i, large[i], 1);
            snprintf(path, sizeof path, "LARGE_%u_WRITEAT.BIN", large[i]);
            make_file(&fs, path, 0x4800u + i, large[i], 2);
        }
    }

    /* --- Subdirectories, files inside, and renames. */
    if (hype_fat32_mkdir(&fs, "SUBDIR") == 0) {
        make_file(&fs, "SUBDIR/INNER1.BIN", 0x5000u, 30000u, 1);
        /* written but not recorded -- it is about to be renamed away. */
        make_file_r(&fs, "SUBDIR/INNER2.BIN", 0x5001u, 200u, 0, 0);
        fill(g_buf, 0x5001u, 200u); /* the bytes travel with whichever name survives */
        if (hype_fat32_rename(&fs, "SUBDIR/INNER2.BIN", "SUBDIR/RENAMED.BIN") == 0) {
            record("SUBDIR/RENAMED.BIN", g_buf, 200u);
        } else {
            record("SUBDIR/INNER2.BIN", g_buf, 200u);
        }
    }

    /* --- Fragmentation: fill with medium files, punch holes, then allocate large across them.
     * Even-indexed files are unlinked, so they are written WITHOUT recording; odd ones survive and
     * are recorded at write time. */
    {
        char frag[128];
        for (i = 0; i < 40u; i++) {
            snprintf(frag, sizeof frag, "FRAG_%03u.BIN", i);
            make_file_r(&fs, frag, 0x6000u + i, 20000u + i * 111u, 1, (int)(i & 1u));
        }
        for (i = 0; i < 40u; i += 2u) { /* free every other one -> a checkerboard of holes */
            snprintf(frag, sizeof frag, "FRAG_%03u.BIN", i);
            hype_fat32_unlink(&fs, frag);
        }
        for (i = 0; i < 6u; i++) { /* large allocations that must thread the holes */
            snprintf(frag, sizeof frag, "FRAGFILL_%u.BIN", i);
            make_file(&fs, frag, 0x7000u + i, 96000u, 1);
        }
    }

    /* --- write_at in place (overwrite, no growth) on a dedicated file. */
    {
        hype_fat32_wfile_t f;
        make_file_r(&fs, "INPLACE.BIN", 0xABCDu, 40000u, 1, 0); /* base content, not yet recorded */
        if (hype_fat32_open(&fs, "INPLACE.BIN", &f) == 0) {
            fill(g_buf, 0xABCDu, 40000u);          /* reconstruct the base ... */
            fill(g_buf + 4096u, 0x9999u, 8192u);   /* ... then overwrite a middle span in place */
            if (hype_fat32_write_at(&f, 4096u, g_buf + 4096u, 8192u) >= 0) {
                verify_hype(&fs, "INPLACE.BIN", g_buf, 40000u);
                record("INPLACE.BIN", g_buf, 40000u);
            }
        }
    }

    fflush(g_img);
    fclose(g_manifest);
    if (g_failures) {
        printf("hype-side self-check: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("hype-side self-check: all files read back byte-exact\n");
    return 0;
}
