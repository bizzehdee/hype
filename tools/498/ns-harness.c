#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
/*
 * #498 real-image harness: drive hype's OWN namespace writer (create/write/
 * mkdir/rename/unlink/rmdir) against an image made by real mkfs, one step
 * per invocation, so run-498.sh can run e2fsck -fn and debugfs read-only
 * checks BETWEEN every step, exactly as the ticket's bar requires.
 *
 *   ns-harness <image> <step 1..6>
 *
 * Steps (fixed names/content, so the shell script can assert on them):
 *   1: create /hype_test.txt (empty)
 *   2: append a deterministic 4096-byte payload to /hype_test.txt
 *   3: mkdir /hype_dir
 *   4: rename /hype_test.txt -> /hype_dir/hype_test.txt
 *   5: unlink /hype_dir/hype_test.txt
 *   6: rmdir /hype_dir
 *
 * Exit 0 on success.
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

#define PAYLOAD_LEN 4096u

int main(int argc, char **argv) {
    static hype_fs_t fs;
    static hype_fs_file_t f;
    static unsigned char payload[PAYLOAD_LEN];
    int step;
    unsigned j;

    if (argc != 3) { fprintf(stderr, "usage: %s <img> <step 1..6>\n", argv[0]); return 2; }
    step = atoi(argv[2]);
    g_img = fopen(argv[1], "r+b");
    if (!g_img) { perror("image"); return 2; }

    if (hype_fs_mount_auto(&fs, img_read, img_write, 0) != 0) {
        fprintf(stderr, "mount failed\n"); return 1;
    }
    if (!(hype_fs_caps(&fs) & HYPE_FS_CAP_NAMESPACE)) {
        fprintf(stderr, "no namespace cap\n"); return 1;
    }
    {
        /* A real caller always has a wall-clock source (core/rtc.c) before
         * touching the namespace; stamp a plausible date so this harness
         * exercises that path too, not just the "never called set_time"
         * 0-mtime convention. */
        hype_rtc_time_t now;
        now.year = 2026; now.month = 8; now.day = 22;
        now.hour = 12; now.minute = 0; now.second = 0;
        hype_fs_set_time(&fs, &now);
    }

    for (j = 0; j < PAYLOAD_LEN; j++) {
        payload[j] = (unsigned char)((498u * 2654435761u + j) >> 3);
    }

    switch (step) {
    case 1:
        if (hype_fs_create(&fs, "/hype_test.txt", &f) != 0) {
            fprintf(stderr, "create failed\n"); return 1;
        }
        break;
    case 2:
        if (hype_fs_lookup(&fs, "/hype_test.txt", &f) != 0) {
            fprintf(stderr, "lookup(hype_test.txt) failed\n"); return 1;
        }
        if (hype_fs_append(&f, payload, PAYLOAD_LEN) != 0) {
            fprintf(stderr, "append failed\n"); return 1;
        }
        if (f.size != PAYLOAD_LEN) { fprintf(stderr, "size wrong after append\n"); return 1; }
        break;
    case 3:
        if (hype_fs_mkdir(&fs, "/hype_dir") != 0) { fprintf(stderr, "mkdir failed\n"); return 1; }
        break;
    case 4:
        if (hype_fs_rename(&fs, "/hype_test.txt", "/hype_dir/hype_test.txt") != 0) {
            fprintf(stderr, "rename failed\n"); return 1;
        }
        /* byte-exact readback through hype's own reader, post-rename */
        if (hype_fs_lookup(&fs, "/hype_dir/hype_test.txt", &f) != 0) {
            fprintf(stderr, "lookup after rename failed\n"); return 1;
        }
        if (f.size != PAYLOAD_LEN) { fprintf(stderr, "size wrong after rename\n"); return 1; }
        {
            static unsigned char back[PAYLOAD_LEN];
            if (hype_fs_read_at(&f, 0, back, PAYLOAD_LEN) != 0) {
                fprintf(stderr, "readback failed\n"); return 1;
            }
            if (memcmp(back, payload, PAYLOAD_LEN) != 0) {
                fprintf(stderr, "byte mismatch after rename\n"); return 1;
            }
        }
        break;
    case 5:
        if (hype_fs_unlink(&fs, "/hype_dir/hype_test.txt") != 0) {
            fprintf(stderr, "unlink failed\n"); return 1;
        }
        break;
    case 6:
        if (hype_fs_rmdir(&fs, "/hype_dir") != 0) { fprintf(stderr, "rmdir failed\n"); return 1; }
        break;
    default:
        fprintf(stderr, "bad step %d\n", step); return 2;
    }

    fclose(g_img);
    fprintf(stderr, "step %d OK\n", step);
    return 0;
}
