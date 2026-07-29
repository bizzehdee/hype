#include <stdio.h>
#include <string.h>
#include "../fat_write.h"

static int failures = 0;
#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)
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
    hype_fat_dirent_build(ent, name, HYPE_FAT_ATTR_ARCHIVE, 0x00031234, 4096, 0);
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


/* ---- #247 LFN primitives ---- */

static void test_lfn_checksum(void) {
    /* Reference value computed with the FAT spec's ChkSum() pseudocode. */
    static const uint8_t n11[11] = {'A','L','O','N','G','F','~','1','T','X','T'};
    uint8_t sum = 0;
    unsigned int i;
    for (i = 0; i < 11u; i++) {
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + n11[i]);
    }
    CHECK_HEX("checksum matches the spec pseudocode", sum, hype_fat_shortname_checksum(n11));
}

static void test_name_valid(void) {
    static const char *bad[] = {"", ".", "..", "...", "a/b", "a\\b", "a:b", "a*b",
                                "a?b", "a\"b", "a<b", "a>b", "a|b", "a\tb"};
    unsigned int i;
    char big[300];
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK("invalid long name rejected", hype_fat_name_valid(bad[i]) == 0);
    }
    CHECK("plain name valid", hype_fat_name_valid("hello world.txt") == 1);
    CHECK("dotted name valid", hype_fat_name_valid(".hidden") == 1);
    for (i = 0; i < sizeof big - 1u; i++) big[i] = 'a';
    big[sizeof big - 1u] = '\0';
    CHECK("over-long name rejected", hype_fat_name_valid(big) == 0);
    big[255] = '\0';
    CHECK("255 chars accepted", hype_fat_name_valid(big) == 1);
}

static void test_name_is_83(void) {
    CHECK("plain 8.3", hype_fat_name_is_83("HYPELOG.TXT") == 1);
    CHECK("no extension", hype_fat_name_is_83("README") == 1);
    CHECK("punctuation ok", hype_fat_name_is_83("A~1$%(){}") == 0); /* 9-char base */
    CHECK("8-char base + punct", hype_fat_name_is_83("A~1$%(){") == 1);
    CHECK("lowercase needs LFN", hype_fat_name_is_83("hypelog.txt") == 0);
    CHECK("9-char base too long", hype_fat_name_is_83("ABCDEFGHI.TXT") == 0);
    CHECK("4-char ext too long", hype_fat_name_is_83("A.TEXT") == 0);
    CHECK("two dots", hype_fat_name_is_83("A.B.C") == 0);
    CHECK("trailing dot (empty ext)", hype_fat_name_is_83("ABC.") == 0);
    CHECK("leading dot (empty base)", hype_fat_name_is_83(".TXT") == 0);
    CHECK("space needs LFN", hype_fat_name_is_83("A B.TXT") == 0);
    CHECK("plus needs LFN", hype_fat_name_is_83("A+B.TXT") == 0);
}

static void test_shortname_tail(void) {
    uint8_t n11[11];
    CHECK_HEX("classic ~1", 0, hype_fat_shortname_tail("A Long FileName.txt", 1u, n11));
    CHECK("classic ~1 bytes", memcmp(n11, "ALONGF~1TXT", 11) == 0);
    CHECK_HEX("~23", 0, hype_fat_shortname_tail("A Long FileName.txt", 23u, n11));
    CHECK("~23 bytes (base shrinks for the digits)", memcmp(n11, "ALONG~23TXT", 11) == 0);
    CHECK("~23 ext intact", memcmp(n11 + 8, "TXT", 3) == 0);
    /* Larger n shrinks the base, not the digits. */
    CHECK_HEX("~123456", 0, hype_fat_shortname_tail("A Long FileName.txt", 123456u, n11));
    CHECK("6-digit tail leaves 1 base char", n11[0] == 'A' && n11[1] == '~');
    CHECK("n too large", hype_fat_shortname_tail("x.txt", 1234567u, n11) != 0);
    CHECK("n zero", hype_fat_shortname_tail("x.txt", 0u, n11) != 0);
    /* Invalid characters map to '_'; dots and spaces are dropped. */
    CHECK_HEX("mapped base", 0, hype_fat_shortname_tail("a+b c.d.txt", 1u, n11));
    CHECK("mapped bytes", memcmp(n11, "A_BCD~1 TXT", 11) == 0);
    /* A base with nothing usable still gets a stem. */
    CHECK_HEX("dot-leading name", 0, hype_fat_shortname_tail(".config", 2u, n11));
    CHECK("stem for empty base", n11[0] == '_' && n11[1] == '~' && n11[2] == '2');
}

static void test_lfn_entries(void) {
    uint8_t ent[32];
    char back[256];
    const char *name = "A Long FileName.txt"; /* 19 chars: 2 pieces */
    unsigned int i;

    /* Piece 1 (chars 0..12). */
    hype_fat_lfn_entry_build(ent, name, 19u, 1u, 0, 0x42u);
    CHECK_HEX("piece 1 sequence", 0x01u, ent[0]);
    CHECK_HEX("piece attr", 0x0Fu, ent[11]);
    CHECK_HEX("piece type", 0x00u, ent[12]);
    CHECK_HEX("piece checksum", 0x42u, ent[13]);
    CHECK_HEX("piece cluster field zero", 0u, (unsigned)(ent[26] | (ent[27] << 8)));
    for (i = 0; i < sizeof back; i++) back[i] = 0x55;
    CHECK_HEX("chars extract seq 1", 1u, hype_fat_lfn_entry_chars(ent, back));
    CHECK("piece 1 chars", memcmp(back, "A Long FileNa", 13) == 0);

    /* Piece 2 (chars 13..18 + terminator + fill), LAST-flagged. */
    hype_fat_lfn_entry_build(ent, name, 19u, 2u, 1, 0x42u);
    CHECK_HEX("piece 2 sequence | LAST", 0x42u, ent[0]);
    CHECK_HEX("chars extract seq 2", 2u, hype_fat_lfn_entry_chars(ent, back));
    CHECK("piece 2 chars", memcmp(back + 13, "me.txt", 6) == 0);
    CHECK_HEX("terminator", 0, back[19]);

    /* A sequence number of zero, or one past the name maximum, is invalid. */
    ent[0] = 0x00u;
    CHECK_HEX("sequence 0 rejected", 0u, hype_fat_lfn_entry_chars(ent, back));
    ent[0] = 0x3Fu; /* (63-1)*13 >= 255 */
    CHECK_HEX("out-of-range sequence rejected", 0u, hype_fat_lfn_entry_chars(ent, back));

    /* A real UCS-2 character above 0xFF cannot match any ASCII query. */
    hype_fat_lfn_entry_build(ent, "abc", 3u, 1u, 1, 0x11u);
    ent[1] = 0x01u; ent[2] = 0x30u; /* U+3001 */
    CHECK_HEX("extract with wide char", 1u, hype_fat_lfn_entry_chars(ent, back));
    CHECK_HEX("wide char becomes 0x7F", 0x7Fu, (unsigned char)back[0]);

    /* dirent_set_cluster touches only the cluster words. */
    {
        uint8_t d[32];
        for (i = 0; i < 32u; i++) d[i] = 0xAA;
        hype_fat_dirent_set_cluster(d, 0x00031234u);
        CHECK_HEX("set_cluster value", 0x00031234u, hype_fat_dirent_cluster(d));
        CHECK_HEX("size bytes untouched", 0xAAu, d[28]);
        CHECK_HEX("attr byte untouched", 0xAAu, d[11]);
    }
}

int main(void) {
    test_fat_entry();
    test_fat_location();
    test_find_free();
    test_shortname();
    test_dirent();
    test_fsinfo();
    test_lfn_checksum();
    test_name_valid();
    test_name_is_83();
    test_shortname_tail();
    test_lfn_entries();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
