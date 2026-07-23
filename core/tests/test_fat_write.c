#include <stdio.h>
#include <string.h>
#include "../fat_write.h"

static int failures = 0;
#define CHECK_HEX(desc, expected, actual) \
    do { if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
        printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
               (unsigned long long)(expected), (unsigned long long)(actual)); failures++; } } while (0)

static void test_fat_entry(void) {
    uint8_t sec[512] = {0};
    /* set the top reserved nibble on entry 3 to prove it's preserved */
    sec[3 * 4 + 3] = 0xF0;
    hype_fat32_entry_set(sec, 3, HYPE_FAT32_EOC);
    CHECK_HEX("entry get masks to 28-bit", 0x0FFFFFFFu, hype_fat32_entry_get(sec, 3));
    /* reserved top nibble preserved in the raw dword */
    CHECK_HEX("reserved nibble preserved", 0xFFu, sec[3 * 4 + 3]);

    hype_fat32_entry_set(sec, 10, 0x123456);
    CHECK_HEX("entry 10 value", 0x123456u, hype_fat32_entry_get(sec, 10));
}

static void test_fat_location(void) {
    uint64_t s; unsigned int idx;
    hype_fat32_fat_location(0, 32, &s, &idx);
    CHECK_HEX("cluster0 sector", 32u, s);
    CHECK_HEX("cluster0 idx", 0u, idx);
    hype_fat32_fat_location(128, 32, &s, &idx);
    CHECK_HEX("cluster128 sector", 33u, s);
    CHECK_HEX("cluster128 idx", 0u, idx);
    hype_fat32_fat_location(200, 32, &s, &idx);
    CHECK_HEX("cluster200 sector", 33u, s);
    CHECK_HEX("cluster200 idx", 72u, idx);
    hype_fat32_fat_location(5, 32, (void *)0, (void *)0); /* NULL outs: no crash */
}

static void test_find_free(void) {
    uint8_t sec[512] = {0};
    unsigned int idx = 999;
    hype_fat32_entry_set(sec, 0, 0x0FFFFFF8); /* media */
    hype_fat32_entry_set(sec, 1, HYPE_FAT32_EOC);
    hype_fat32_entry_set(sec, 2, HYPE_FAT32_EOC);
    CHECK_HEX("first free is 3", 0, hype_fat32_find_free_in_sector(sec, 128, &idx));
    CHECK_HEX("free idx 3", 3u, idx);
    /* fully allocated region -> none */
    {
        uint8_t full[512];
        unsigned int i;
        for (i = 0; i < 512u; i++) full[i] = 0xFF;
        CHECK_HEX("no free", -1, hype_fat32_find_free_in_sector(full, 128, &idx));
    }
    /* entries clamped to 128 */
    CHECK_HEX("clamped free scan", 0, hype_fat32_find_free_in_sector(sec, 200, &idx));
    CHECK_HEX("clamped idx still 3", 3u, idx);
}

static void test_shortname(void) {
    uint8_t n[11];
    hype_fat_shortname_83("HYPELOG.TXT", n);
    CHECK_HEX("83 base ok", 0, memcmp(n, "HYPELOG TXT", 11));
    hype_fat_shortname_83("hype.cfg", n);
    CHECK_HEX("83 lowercase upcased", 0, memcmp(n, "HYPE    CFG", 11));
    hype_fat_shortname_83("README", n); /* no extension */
    CHECK_HEX("83 no ext", 0, memcmp(n, "README     ", 11));
    hype_fat_shortname_83("VERYLONGNAME.DATA", n); /* truncate base+ext */
    CHECK_HEX("83 truncated", 0, memcmp(n, "VERYLONGDAT", 11));
    hype_fat_shortname_83("A.B.TXT", n); /* dots inside base region are skipped */
    CHECK_HEX("83 multi-dot base skips dots", 0, memcmp(n, "AB      TXT", 11));
}

static void test_dirent(void) {
    uint8_t name[11], ent[32];
    hype_fat_shortname_83("HYPELOG.TXT", name);
    hype_fat_dirent_build(ent, name, HYPE_FAT_ATTR_ARCHIVE, 0x00031234, 4096);
    CHECK_HEX("dirent name", 0, memcmp(ent, "HYPELOG TXT", 11));
    CHECK_HEX("dirent attr", HYPE_FAT_ATTR_ARCHIVE, ent[11]);
    CHECK_HEX("dirent cluster roundtrip", 0x00031234u, hype_fat_dirent_cluster(ent));
    CHECK_HEX("dirent size roundtrip", 4096u, hype_fat_dirent_size(ent));
    CHECK_HEX("dirent cluster hi", 0x0003u, (unsigned)(ent[20] | (ent[21] << 8)));
    CHECK_HEX("dirent cluster lo", 0x1234u, (unsigned)(ent[26] | (ent[27] << 8)));
}

static void test_fsinfo(void) {
    uint8_t s[512] = {0};
    /* not an FSInfo -> rejected */
    CHECK_HEX("bad fsinfo rejected", -1, hype_fat32_fsinfo_set(s, 100, 5));
    /* lead signature 0x41615252 */
    s[0] = 0x52; s[1] = 0x52; s[2] = 0x61; s[3] = 0x41;
    CHECK_HEX("fsinfo ok", 0, hype_fat32_fsinfo_set(s, 1000, 42));
    CHECK_HEX("free count", 1000u,
              (unsigned)(s[0x1E8] | (s[0x1E9] << 8) | (s[0x1EA] << 16) | (s[0x1EB] << 24)));
    CHECK_HEX("next free", 42u,
              (unsigned)(s[0x1EC] | (s[0x1ED] << 8) | (s[0x1EE] << 16) | (s[0x1EF] << 24)));
}

int main(void) {
    test_fat_entry();
    test_fat_location();
    test_find_free();
    test_shortname();
    test_dirent();
    test_fsinfo();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
