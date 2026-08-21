/*
 * #597: host smoke-test of hype_fat32_selftest_run() -- the SAME code hype runs on the stick,
 * driven against a real mkfs.vfat image via fs_ops on this workstation. Proves the battery loop,
 * the fs_ops mount, and the self-check glue before a stick is ever cut. run-selftest-host.sh
 * then judges the image with fsck.vfat.
 *
 * usage: selftest_host <image>
 */
#include <stdio.h>
#include <string.h>

#include "../../core/blk_io.h"
#include "../../core/fat32_selftest.h"
#include "../../core/fs_ops.h"

#define SECSZ 512u
static FILE *g_img;

static void logcb(void *ctx, const hype_fat32_selftest_event_t *ev) {
    (void)ctx;
    printf("  [%u] %s seed=0x%x len=%u mode=%s first_cluster=%u -> %s\n", ev->idx, ev->path,
           ev->seed, ev->len, hype_fat32_selftest_mode_name(ev->mode), ev->first_cluster,
           ev->refused ? "REFUSED" : (ev->selfcheck_ok ? "OK" : "SELFCHECK-FAIL"));
}

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
    fflush(g_img);
    return 0;
}

int main(int argc, char **argv) {
    hype_fs_t fs;
    hype_rtc_time_t now;
    hype_fat32_selftest_result_t res;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <image>\n", argv[0]);
        return 2;
    }
    g_img = fopen(argv[1], "r+b");
    if (!g_img) { perror("open image"); return 2; }

    if (hype_fs_mount_auto(&fs, img_read, img_write, 0) != 0) {
        fprintf(stderr, "fs_ops mount_auto failed (not a recognised FAT32?)\n");
        return 2;
    }
    hype_fs_set_barrier(&fs, img_sync);

    memset(&now, 0, sizeof now);
    now.year = 2026; now.month = 8; now.day = 21; now.hour = 12;

    rc = hype_fat32_selftest_run(&fs, &now, &res, logcb, 0);
    printf("selftest: written=%u refused=%u selfcheck_fail=%u%s%s\n", res.files_written,
           res.files_refused, res.selfcheck_fail, res.first_fail[0] ? " first=" : "", res.first_fail);
    fflush(g_img);
    fclose(g_img);
    return (rc == 0 && res.files_refused == 0) ? 0 : 1;
}
