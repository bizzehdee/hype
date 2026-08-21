/*
 * #597: judge a pulled FAT32 self-test stick from the HOST side.
 *
 * hype's on-medium battery (core/fat32_selftest.c) left \F32TEST\*.BIN on the boot volume. After
 * the stick is pulled and mounted, this reads every battery file back through the operating
 * system's own vfat driver and checks it byte-exact against the SAME deterministic content hype
 * wrote (the schedule and content come from core/fat32_selftest.h, shared with hype). A byte
 * mismatch, a short file, or a file the OS cannot read is the #596 class of defect.
 *
 * This is the DATA half. Run fsck.vfat -n on the raw device for the STRUCTURAL half
 * (run-fat32-e2e.sh's cousin, validate-stick.sh, does both).
 *
 * usage: validate_stick <mount-point>   e.g. validate_stick /run/media/you/HYPEHW
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "../../core/fat32_selftest.h"

/* Read one battery file back through the OS and check it byte-exact. Returns 1 if OK, 0 if bad;
 * sets *was_missing when the file could not be opened at all. */
static int check_item(const char *mnt, const hype_fat32_selftest_item_t *it, int *was_missing) {
    char full[512];
    FILE *fp;
    unsigned int off = 0;
    int ok = 1;
    long sz;

    *was_missing = 0;
    snprintf(full, sizeof full, "%s/%s", mnt, it->path);
    fp = fopen(full, "rb");
    if (!fp) {
        printf("MISSING: %s [seed=0x%x len=%u mode=%s] at %s (%s)\n", it->path, it->seed, it->len,
               hype_fat32_selftest_mode_name(it->mode), full, strerror(errno));
        *was_missing = 1;
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz != (long)it->len) {
        printf("SIZE: %s [seed=0x%x mode=%s] got %ld expected %u\n", it->path, it->seed,
               hype_fat32_selftest_mode_name(it->mode), sz, it->len);
        ok = 0;
    }
    while (off < it->len && ok) {
        unsigned char buf[4096];
        unsigned char exp[4096];
        unsigned int n = it->len - off;
        unsigned int i;
        size_t got;
        if (n > sizeof buf) n = sizeof buf;
        got = fread(buf, 1u, n, fp);
        if (got != n) {
            printf("READ SHORT/EIO: %s at offset %u (wanted %u, got %zu)\n", it->path, off, n, got);
            ok = 0;
            break;
        }
        for (i = 0; i < n; i++) exp[i] = hype_fat32_selftest_byte(it->seed, off + i);
        if (memcmp(buf, exp, n) != 0) {
            printf("CONTENT: %s [seed=0x%x len=%u mode=%s] mismatch at offset %u\n", it->path,
                   it->seed, it->len, hype_fat32_selftest_mode_name(it->mode), off);
            ok = 0;
            break;
        }
        off += n;
    }
    fclose(fp);
    return ok;
}

int main(int argc, char **argv) {
    hype_fat32_selftest_item_t it;
    unsigned int idx;
    unsigned int files = 0, bad = 0, missing = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <mount-point>\n", argv[0]);
        return 2;
    }

    for (idx = 0; hype_fat32_selftest_item(idx, &it); idx++) {
        int miss;
        files++;
        if (!check_item(argv[1], &it, &miss)) { bad++; missing += miss; }
    }
    for (idx = 0; hype_fat32_selftest_interleaved_item(idx, &it); idx++) {
        int miss;
        files++;
        if (!check_item(argv[1], &it, &miss)) { bad++; missing += miss; }
    }

    printf("---\n%u file(s) checked, %u OK, %u bad (%u missing)\n", files, files - bad, bad, missing);
    if (bad) {
        printf("FAIL: the FAT32 self-test volume is not byte-exact as the OS reads it\n");
        return 1;
    }
    printf("PASS: every battery file read back byte-exact\n");
    return 0;
}
