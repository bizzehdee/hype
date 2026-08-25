#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
/*
 * #508 real-image harness: drive a SPARSE qcow2 -- hype_blk_image_sparse_t as the underlying
 * file, hype_qcow2_t layered on top, exactly how boot/main.c's
 * fw_1_disk_use_sparse_image_file_unlocked() wires the two together -- against a real
 * mke2fs-made ext4 image holding a real mkdisk-created sparse qcow2. Mirrors tools/506 and
 * tools/507's harnesses.
 *
 *   qcow2-sparse-harness <image> <path-in-volume> mount-check
 *   qcow2-sparse-harness <image> <path-in-volume> write <guest-lba> <byte-hex-pattern>
 *   qcow2-sparse-harness <image> <path-in-volume> read-verify <guest-lba> <byte-hex-pattern>
 *
 * `guest-lba` addresses the qcow2's VIRTUAL disk (what a guest would see), not the underlying
 * file -- this is exactly the layering #508 adds no new code for: hype_qcow2_t's own cluster
 * allocator issues the write against the underlying hype_blk_image_sparse_t backend, which
 * decides DATA-fast-path vs grow-through-a-hole on its own.
 *
 * Exit 0 on success.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../core/blk_image_sparse.h"
#include "../../core/blk_qcow2.h"

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
    static hype_file_rmap_t rmap;
    static hype_blk_image_sparse_t simg;
    static hype_blk_backend_t raw_be;
    static hype_qcow2_t qcow2;
    static hype_blk_backend_t be;
    const char *image_path, *vol_path, *mode;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <image> <path-in-volume> <mount-check|write|read-verify> "
                        "[guest-lba] [pattern-byte-hex]\n",
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
    if (hype_blk_image_sparse_init(&simg, &raw_be, &rmap, 0u, img_read, img_write, 0, &fs, &f,
                                   vol_path) != 0) {
        fprintf(stderr, "hype_blk_image_sparse_init failed\n");
        return 1;
    }
    if (hype_qcow2_init(&qcow2, &be, &raw_be, 0) != 0) {
        fprintf(stderr, "hype_qcow2_init failed -- not a valid qcow2 image\n");
        return 1;
    }

    if (strcmp(mode, "mount-check") == 0) {
        printf("qcow2 v%u cluster_size=%llu virtual_sectors=%llu (raw file backend: %llu "
               "sectors underlying)\n",
               qcow2.version, (unsigned long long)qcow2.cluster_size,
               (unsigned long long)be.total_sectors, (unsigned long long)raw_be.total_sectors);
        printf("mount-check OK\n");
        return 0;
    }

    if (strcmp(mode, "write") == 0 || strcmp(mode, "read-verify") == 0) {
        uint64_t lba;
        unsigned pat;
        uint8_t buf[512];

        if (argc < 6) { fprintf(stderr, "write/read-verify need <guest-lba> <pattern-byte-hex>\n"); return 2; }
        lba = strtoull(argv[4], 0, 10);
        pat = (unsigned)strtoul(argv[5], 0, 16);
        memset(buf, (int)pat, sizeof buf);

        if (strcmp(mode, "write") == 0) {
            if (be.write == 0) { fprintf(stderr, "backend is read-only\n"); return 1; }
            if (be.write(be.ctx, lba, 1, buf) != 0) {
                fprintf(stderr, "write at guest lba %llu failed\n", (unsigned long long)lba);
                return 1;
            }
            printf("write OK: guest_lba=%llu pattern=0x%02x\n", (unsigned long long)lba, pat);
            return 0;
        } else {
            uint8_t got[512];
            if (be.read(be.ctx, lba, 1, got) != 0) {
                fprintf(stderr, "read at guest lba %llu failed\n", (unsigned long long)lba);
                return 1;
            }
            if (memcmp(got, buf, sizeof buf) != 0) {
                fprintf(stderr, "readback mismatch at guest lba %llu\n", (unsigned long long)lba);
                return 1;
            }
            printf("read-verify OK: guest_lba=%llu matches pattern=0x%02x after a fresh mount\n",
                   (unsigned long long)lba, pat);
            return 0;
        }
    }

    fprintf(stderr, "unknown mode '%s'\n", mode);
    return 2;
}
