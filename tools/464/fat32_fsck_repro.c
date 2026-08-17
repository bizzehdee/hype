/*
 * #464: drive hype's FAT32 writer against a REAL mkfs.vfat volume, fail the durability barrier
 * the way the operator's stick does, and leave the image for fsck.vfat to judge.
 *
 * The unit tests assert the invariant against a synthetic in-RAM volume. This harness closes the
 * gap that mattered on hardware: the volume is produced by mkfs.vfat and inspected by fsck.vfat,
 * so "consistent" means what the operator's own machine means by it, not what hype believes.
 *
 * Barrier policy is chosen by argv[2]:
 *   ok     -- every SYNCHRONIZE CACHE succeeds (control: the volume must be clean)
 *   dead   -- the barrier fails from the second call onward and never recovers, which is what a
 *             device rejecting SYNCHRONIZE CACHE(10) looked like before #516
 *
 * Build and run through tools/464/run-464.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/fat_write_fs.h"

#define SECSZ 512u

static FILE *g_img;
static long g_sync_calls;
static int g_sync_dead;  /* selected policy: fail from the second barrier of the growth onward */
static int g_sync_armed; /* the policy applies only once the file is seeded */

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

/*
 * Reads the state fsck will judge, straight off the image: what the entry claims, how far its
 * chain actually reaches, how many clusters the FAT marks in use, and what FSInfo believes.
 */
static void report_state(hype_fat32_fs_t *fs, const hype_fat32_wfile_t *f) {
    uint8_t sec[SECSZ];
    uint32_t first, size, cl, used = 0, walked = 0, i;
    uint32_t fat_lba = fs->reserved;
    uint32_t free_count;

    if (img_read(0, f->dirent_lba, 1u, sec) != 0) return;
    {
        const uint8_t *e = sec + f->dirent_off;
        size = (uint32_t)e[28] | ((uint32_t)e[29] << 8) | ((uint32_t)e[30] << 16) |
               ((uint32_t)e[31] << 24);
        first = ((uint32_t)e[26] | ((uint32_t)e[27] << 8)) |
                (((uint32_t)e[20] | ((uint32_t)e[21] << 8)) << 16);
    }

    cl = first;
    while (cl >= 2u && cl < 0x0FFFFFF8u && walked < 400000u) {
        uint8_t fsec[SECSZ];
        uint32_t off = cl / 128u, idx = cl % 128u;
        walked++;
        if (img_read(0, fat_lba + off, 1u, fsec) != 0) break;
        cl = ((uint32_t)fsec[idx * 4] | ((uint32_t)fsec[idx * 4 + 1] << 8) |
              ((uint32_t)fsec[idx * 4 + 2] << 16) | ((uint32_t)fsec[idx * 4 + 3] << 24)) & 0x0FFFFFFFu;
    }
    for (i = 0; i < fs->fat_size; i++) {
        uint8_t fsec[SECSZ];
        uint32_t k;
        if (img_read(0, fat_lba + i, 1u, fsec) != 0) break;
        for (k = 0; k < 128u; k++) {
            uint32_t v = ((uint32_t)fsec[k * 4] | ((uint32_t)fsec[k * 4 + 1] << 8) |
                          ((uint32_t)fsec[k * 4 + 2] << 16) | ((uint32_t)fsec[k * 4 + 3] << 24)) &
                         0x0FFFFFFFu;
            if (v != 0u) used++;
        }
    }
    if (img_read(0, 1u, 1u, sec) == 0) {
        free_count = (uint32_t)sec[0x1E8] | ((uint32_t)sec[0x1E9] << 8) |
                     ((uint32_t)sec[0x1EA] << 16) | ((uint32_t)sec[0x1EB] << 24);
    } else {
        free_count = 0xFFFFFFFFu;
    }
    printf("  entry: size=%u first=%u chain=%u clusters (%u bytes) | FAT used=%u | FSInfo free=%u\n",
           size, first, walked, walked * fs->spc * SECSZ, used, free_count);
    printf("  invariant entry<=chain: %s\n",
           (size <= walked * fs->spc * SECSZ) ? "HOLDS" : "VIOLATED");
}

int main(int argc, char **argv) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    static uint8_t data[512u * 1024u];
    unsigned int i;
    int grew;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <image> <ok|dead>\n", argv[0]);
        return 2;
    }
    g_sync_dead = (strcmp(argv[2], "dead") == 0);

    g_img = fopen(argv[1], "r+b");
    if (g_img == 0) {
        perror("open image");
        return 2;
    }
    for (i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7u + 3u);

    if (hype_fat32_fs_mount(img_read, img_write, 0, &fs) != 0) {
        fprintf(stderr, "mount failed\n");
        return 2;
    }
    hype_fat32_fs_set_sync(&fs, img_sync);

    /*
     * Seed the file while barriers still work, then grow it. Under the "dead" policy the growth's
     * first barrier succeeds -- publishing the larger size -- and every barrier after it fails,
     * which is the window that produced "fat_bmap_cluster: request beyond EOF" on the stick.
     */
    if (hype_fat32_create(&fs, "GROW.BIN", &f) != 0) {
        fprintf(stderr, "create failed\n");
        return 2;
    }
    if (hype_fat32_append(&f, data, 4096u) != 0) {
        fprintf(stderr, "seed append failed\n");
        return 2;
    }
    g_sync_calls = 0;
    g_sync_armed = 1;
    grew = hype_fat32_write_at(&f, 0, data, sizeof data);
    printf("growth rc=%d, barriers=%ld, rollback_failures=%llu\n", grew, g_sync_calls,
           (unsigned long long)hype_fat_write_rollback_failures());
    report_state(&fs, &f);
    fflush(g_img);
    fclose(g_img);
    return 0;
}
