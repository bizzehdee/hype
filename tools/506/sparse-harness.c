#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
/*
 * #506 real-image harness: drive hype's OWN sparse guest-disk backend
 * (hype_blk_image_sparse_t) against a REAL sparse file on a REAL mke2fs-made ext4 image, so
 * run-506.sh can run e2fsck -fn and a host-side debugfs byte check exactly as the ticket's bar
 * requires. Mirrors tools/498/ns-harness.c's shape (fopen/fseeko/fread/fwrite standing in for
 * the disk-absolute host read/write pair hype's own EFI build gets from AHCI/NVMe).
 *
 *   sparse-harness <image> <path-in-volume> mount-check
 *   sparse-harness <image> <path-in-volume> write <lba> <byte-hex-pattern>
 *   sparse-harness <image> <path-in-volume> read-verify <lba> <byte-hex-pattern>
 *
 * mount-check: mounts, looks up the file, maps its ranges, and prints the range count/kinds --
 *   confirms the seed file really is sparse (a HOLE range exists) before the write leg runs.
 * write: mounts fresh (as a VM boot would), inits the sparse backend, writes one sector of
 *   `pattern` repeated at `lba`. Exercises the growth path if `lba` falls in a HOLE.
 * read-verify: mounts AGAIN fresh (simulating a VM restart: nothing carried over but the bytes
 *   on the medium), inits the sparse backend from a freshly re-resolved map, reads `lba` back,
 *   and confirms it matches `pattern` -- proving persistence survives a real remount, not just
 *   an in-memory cache.
 *
 * Exit 0 on success.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../core/blk_image_sparse.h"

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

static const char *kind_name(unsigned k) {
    switch (k) {
    case HYPE_RANGE_DATA: return "DATA";
    case HYPE_RANGE_HOLE: return "HOLE";
    case HYPE_RANGE_UNWRITTEN: return "UNWRITTEN";
    default: return "?";
    }
}

int main(int argc, char **argv) {
    static hype_fs_t fs;
    static hype_fs_file_t f;
    static hype_file_rmap_t rmap;
    static hype_blk_image_sparse_t img;
    static hype_blk_backend_t be;
    const char *image_path;
    const char *vol_path;
    const char *mode;
    unsigned i;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <image> <path-in-volume> <mount-check|write|read-verify> "
                        "[lba] [pattern-byte-hex]\n",
                argv[0]);
        return 2;
    }
    image_path = argv[1];
    vol_path = argv[2];
    mode = argv[3];

    g_img = fopen(image_path, "r+b");
    if (!g_img) { perror("image"); return 2; }

    if (hype_fs_mount_auto(&fs, img_read, img_write, 0) != 0) {
        fprintf(stderr, "mount failed\n");
        return 1;
    }
    if (hype_fs_lookup(&fs, vol_path, &f) != 0) {
        fprintf(stderr, "lookup %s failed\n", vol_path);
        return 1;
    }
    if (hype_fs_map_ranges(&fs, vol_path, &rmap) != 0) {
        fprintf(stderr, "map_ranges %s failed\n", vol_path);
        return 1;
    }

    if (strcmp(mode, "mount-check") == 0) {
        int has_hole = 0;
        printf("size_bytes=%llu ranges=%u\n", (unsigned long long)rmap.size_bytes, rmap.count);
        for (i = 0; i < rmap.count; i++) {
            printf("  range %u: %s sectors=%llu start_lba=%llu\n", i,
                   kind_name(rmap.ranges[i].kind), (unsigned long long)rmap.ranges[i].sector_count,
                   (unsigned long long)rmap.ranges[i].start_lba);
            if (rmap.ranges[i].kind == HYPE_RANGE_HOLE) has_hole = 1;
        }
        if (!has_hole) {
            fprintf(stderr, "seed file has no HOLE range -- not actually sparse\n");
            return 1;
        }
        printf("mount-check OK: genuinely sparse\n");
        return 0;
    }

    if (hype_blk_image_sparse_init(&img, &be, &rmap, 0u, img_read, img_write, 0, &fs, &f,
                                   vol_path) != 0) {
        fprintf(stderr, "hype_blk_image_sparse_init failed\n");
        return 1;
    }

    if (strcmp(mode, "write") == 0 || strcmp(mode, "read-verify") == 0) {
        uint64_t lba;
        unsigned pat;
        uint8_t buf[512];

        if (argc < 6) { fprintf(stderr, "write/read-verify need <lba> <pattern-byte-hex>\n"); return 2; }
        lba = strtoull(argv[4], 0, 10);
        pat = (unsigned)strtoul(argv[5], 0, 16);
        memset(buf, (int)pat, sizeof buf);

        if (strcmp(mode, "write") == 0) {
            if (be.write == 0) { fprintf(stderr, "backend is read-only\n"); return 1; }
            if (be.write(be.ctx, lba, 1, buf) != 0) {
                fprintf(stderr, "write at lba %llu failed\n", (unsigned long long)lba);
                return 1;
            }
            printf("write OK: lba=%llu pattern=0x%02x\n", (unsigned long long)lba, pat);
            return 0;
        } else {
            uint8_t got[512];
            if (be.read(be.ctx, lba, 1, got) != 0) {
                fprintf(stderr, "read at lba %llu failed\n", (unsigned long long)lba);
                return 1;
            }
            if (memcmp(got, buf, sizeof buf) != 0) {
                fprintf(stderr, "readback mismatch at lba %llu: expected 0x%02x throughout, got "
                                "0x%02x at byte 0\n",
                        (unsigned long long)lba, pat, got[0]);
                return 1;
            }
            printf("read-verify OK: lba=%llu matches pattern=0x%02x after a fresh mount\n",
                   (unsigned long long)lba, pat);
            return 0;
        }
    }

    fprintf(stderr, "unknown mode '%s'\n", mode);
    return 2;
}
