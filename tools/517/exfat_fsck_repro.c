/*
 * #517: drive hype's exFAT writer against a REAL mkfs.exfat volume and leave the image for
 * fsck.exfat to judge -- the exFAT counterpart of tools/464/run-464.sh.
 *
 * The window this reproduces: set_flush() publishes the Stream Extension entry (which carries
 * DataLength) BEFORE the File entry, so a write failure between the two leaves the larger size on
 * the medium and sends the writer into its rollback. The rollback must not then free the clusters
 * that size depends on.
 *
 * Failure policy is chosen by argv[2]:
 *   ok    -- every write succeeds (control: the volume must be clean)
 *   dead  -- directory writes fail from the second one of the growth onward, and never recover
 *
 * Build and run through tools/517/run-517.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/fat_exfat_fs.h"

#define SECSZ 512u

static FILE *g_img;
static int g_fail_dir_writes;   /* selected policy */
static int g_armed;             /* the policy applies only once the file is seeded */
static long g_dir_writes_seen;
static uint64_t g_dir_lba;      /* the sector the file's entry set lives in */

static int img_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if (fseek(g_img, (long)(lba * SECSZ), SEEK_SET) != 0) return -1;
    return fread(dst, SECSZ, count, g_img) == count ? 0 : -1;
}

static int img_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    if (g_armed && g_fail_dir_writes && lba == g_dir_lba) {
        if (g_dir_writes_seen >= 1) return -1; /* one entry write lands, the rest do not */
        g_dir_writes_seen++;
    }
    if (fseek(g_img, (long)(lba * SECSZ), SEEK_SET) != 0) return -1;
    return fwrite(src, SECSZ, count, g_img) == count ? 0 : -1;
}

int main(int argc, char **argv) {
    hype_exfat_fs_t fs;
    hype_exfat_wfile_t f;
    static uint8_t data[512u * 1024u];
    unsigned int i;
    int grew;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <image> <ok|dead>\n", argv[0]);
        return 2;
    }
    g_fail_dir_writes = (strcmp(argv[2], "dead") == 0);

    g_img = fopen(argv[1], "r+b");
    if (g_img == 0) {
        perror("open image");
        return 2;
    }
    for (i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 5u + 1u);

    if (hype_exfat_fs_mount(img_read, img_write, 0, &fs) != 0) {
        fprintf(stderr, "mount failed\n");
        return 2;
    }
    if (hype_exfat_create(&fs, "GROW517.BIN", &f) != 0) {
        fprintf(stderr, "create failed\n");
        return 2;
    }
    if (hype_exfat_write_at(&f, 0, data, 4096u) != 0) {
        fprintf(stderr, "seed failed\n");
        return 2;
    }

    /* The entry set's own sector, so the policy hits the publish rather than file data. */
    g_dir_lba = (uint64_t)fs.heap_lba + (uint64_t)(f.dir_cluster - 2u) * fs.spc;
    g_dir_writes_seen = 0;
    g_armed = 1;
    grew = hype_exfat_write_at(&f, 0, data, sizeof data);
    printf("growth rc=%d, rollback_failures=%llu\n", grew,
           (unsigned long long)hype_exfat_write_rollback_failures());
    fflush(g_img);
    fclose(g_img);
    return 0;
}
