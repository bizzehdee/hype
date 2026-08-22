/*
 * #416: drive hype's real NTFS journal primitives (core/ntfs_journal.c) against a real
 * mkntfs-created volume, judged by ntfsinfo/ntfsfix -- the tools/464 pattern applied to NTFS.
 *
 * Usage: journal_repro <image> <op>
 *   mount        -- mount and print the dirty flag (0/1) or MOUNT-FAILED
 *   set-dirty    -- hype_ntfs_txn_open() only: sets the dirty bit and exits (leaves it set, to
 *                   demonstrate hype_ntfs_mount's own #337 refusal on the now-dirty volume)
 *   bracket      -- hype_ntfs_txn_open() + a pause + hype_ntfs_txn_close(), one mounted session
 *   restamp      -- read + hype_ntfs_record_write() record 3 back unchanged (fixup roundtrip)
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../core/ntfs.h"
#include "../../core/ntfs_journal.h"

static int g_fd;

static int img_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if (pread(g_fd, dst, (size_t)count * 512u, (off_t)(lba * 512u)) != (ssize_t)count * 512) {
        return -1;
    }
    return 0;
}

static int img_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    if (pwrite(g_fd, src, (size_t)count * 512u, (off_t)(lba * 512u)) != (ssize_t)count * 512) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    hype_ntfs_t fs;
    hype_ntfs_txn_t txn;
    uint8_t rec[HYPE_NTFS_MAX_RECORD];

    if (argc != 3) {
        fprintf(stderr, "usage: %s <image> <mount|set-dirty|bracket|restamp>\n", argv[0]);
        return 2;
    }

    g_fd = open(argv[1], O_RDWR);
    if (g_fd < 0) {
        perror("open");
        return 2;
    }

    if (hype_ntfs_mount(img_read, 0, &fs) != 0) {
        printf("MOUNT-FAILED\n");
        return 1;
    }

    if (strcmp(argv[2], "mount") == 0) {
        int dirty = hype_ntfs_volume_dirty_get(&fs);
        printf("dirty=%d\n", dirty);
        return dirty < 0 ? 1 : 0;
    }

    if (strcmp(argv[2], "set-dirty") == 0) {
        hype_ntfs_txn_init(&txn, 0);
        if (hype_ntfs_txn_open(&fs, img_write, &txn, 0) != 0) {
            printf("TXN-OPEN-FAILED\n");
            return 1;
        }
        printf("OK\n");
        return 0;
    }

    if (strcmp(argv[2], "bracket") == 0) {
        /* Open and close within ONE mounted session -- the realistic shape (hype
         * mounts once per boot and keeps the in-memory hype_ntfs_t for the whole
         * writable session). A separate process per op would have to re-mount
         * mid-session, which hype_ntfs_mount correctly refuses once the volume
         * is dirty -- that refusal is #337's own gate working as designed, not
         * a bug in the txn bracket, so this tool does not attempt that shape. */
        hype_ntfs_txn_init(&txn, 0);
        if (hype_ntfs_txn_open(&fs, img_write, &txn, 0) != 0) {
            printf("TXN-OPEN-FAILED\n");
            return 1;
        }
        printf("DIRTY-SET\n");
        fflush(stdout);
        sleep(2); /* window for the driving script to inspect the on-disk state */
        if (hype_ntfs_txn_close(&fs, img_write, &txn, 0) != 0) {
            printf("TXN-CLOSE-FAILED\n");
            return 1;
        }
        printf("OK\n");
        return 0;
    }

    if (strcmp(argv[2], "restamp") == 0) {
        if (hype_ntfs_record_read(&fs, 3, rec) != 0) {
            printf("RECORD-READ-FAILED\n");
            return 1;
        }
        if (hype_ntfs_record_write(&fs, img_write, 3, rec, 42u) != 0) {
            printf("RECORD-WRITE-FAILED\n");
            return 1;
        }
        printf("OK\n");
        return 0;
    }

    fprintf(stderr, "unknown op '%s'\n", argv[2]);
    return 2;
}
