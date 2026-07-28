#include <stdio.h>
#include <string.h>
#include "../fat_exfat.h"

static int failures = 0;
#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)
#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

/*
 * Reference implementations, transcribed from the exFAT specification's own
 * pseudocode (EntrySetChecksum / NameHash / TableChecksum) rather than from
 * core/fat_exfat.c, so these tests compare the code against the spec instead of
 * against itself. The hard-coded vectors below were additionally confirmed
 * end-to-end: images written through this code are accepted by exfatprogs'
 * fsck.exfat, which recomputes both the set checksum and the name hash.
 */
static uint16_t ref_set_checksum(const uint8_t *bytes, unsigned n) {
    uint16_t s = 0;
    unsigned i;
    for (i = 0; i < n; i++) {
        if (i == 2u || i == 3u) continue;
        s = (uint16_t)(((s & 1u) ? 0x8000u : 0u) + (uint16_t)(s >> 1) + (uint16_t)bytes[i]);
    }
    return s;
}
static uint16_t ref_name_hash(const uint16_t *chars, unsigned n) {
    uint16_t h = 0;
    unsigned i;
    for (i = 0; i < n * 2u; i++) {
        uint8_t b = (i & 1u) ? (uint8_t)(chars[i / 2u] >> 8) : (uint8_t)(chars[i / 2u] & 0xFFu);
        h = (uint16_t)(((h & 1u) ? 0x8000u : 0u) + (uint16_t)(h >> 1) + (uint16_t)b);
    }
    return h;
}
static uint32_t ref_table_checksum(const uint8_t *bytes, unsigned n) {
    uint32_t c = 0;
    unsigned i;
    for (i = 0; i < n; i++) {
        c = ((c & 1u) ? 0x80000000u : 0u) + (c >> 1) + (uint32_t)bytes[i];
    }
    return c;
}

static void test_set_checksum(void) {
    uint8_t buf[96];
    unsigned i;
    for (i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)i;

    /* 32 bytes 0x00..0x1F: a fixed vector, cross-checked against the spec's
     * pseudocode by hand and against fsck.exfat on a real volume. */
    CHECK_HEX("set checksum of 0..31", 0x0009u,
              hype_exfat_set_checksum_update(0u, 0u, buf, 32u));
    /* Bytes 2 and 3 are the checksum field itself and must not contribute. */
    {
        uint8_t alt[32];
        memcpy(alt, buf, 32);
        alt[2] = 0xAA;
        alt[3] = 0x55;
        CHECK_HEX("checksum ignores its own field", 0x0009u,
                  hype_exfat_set_checksum_update(0u, 0u, alt, 32u));
    }
    /* Feeding in pieces must equal feeding in one go -- the streaming form is
     * what lets a set spanning two clusters be checksummed without buffering. */
    for (i = 1u; i <= 96u; i++) {
        uint16_t whole = hype_exfat_set_checksum_update(0u, 0u, buf, 96u);
        uint16_t split = hype_exfat_set_checksum_update(0u, 0u, buf, i);
        split = hype_exfat_set_checksum_update(split, i, buf + i, 96u - i);
        if (whole != split) {
            CHECK_HEX("split checksum equals whole", whole, split);
            break;
        }
    }
    CHECK_HEX("checksum matches the spec reference", ref_set_checksum(buf, 96u),
              hype_exfat_set_checksum_update(0u, 0u, buf, 96u));
    /* A one-bit change anywhere outside bytes 2..3 must change the checksum. */
    {
        unsigned changed = 0;
        for (i = 4u; i < 96u; i++) {
            uint8_t alt[96];
            memcpy(alt, buf, 96);
            alt[i] ^= 0x01u;
            if (hype_exfat_set_checksum_update(0u, 0u, alt, 96u) !=
                hype_exfat_set_checksum_update(0u, 0u, buf, 96u)) {
                changed++;
            }
        }
        CHECK_HEX("every byte outside the field affects the checksum", 92u, changed);
    }
}

static void test_name_hash(void) {
    static const char *names[] = {"HELLO.TXT", "A", "DISK IMAGE 01.IMG", "TEST.ISO"};
    static const uint16_t expect[] = {0x3046u, 0x8020u, 0x9B14u, 0xBB64u};
    unsigned k;
    for (k = 0; k < sizeof expect / sizeof expect[0]; k++) {
        uint16_t chars[64];
        unsigned n = (unsigned)strlen(names[k]);
        unsigned i;
        for (i = 0; i < n; i++) chars[i] = (uint16_t)names[k][i];
        CHECK_HEX(names[k], expect[k], hype_exfat_name_hash_update(0u, chars, n));
        CHECK_HEX("name hash matches the spec reference", ref_name_hash(chars, n),
                  hype_exfat_name_hash_update(0u, chars, n));
        /* Incremental feeding must match a single call. */
        if (n > 1u) {
            uint16_t part = hype_exfat_name_hash_update(0u, chars, 1u);
            part = hype_exfat_name_hash_update(part, chars + 1u, n - 1u);
            CHECK_HEX("incremental name hash", expect[k], part);
        }
    }
    /* The hash is over UTF-16 code units, so the high byte matters. */
    {
        uint16_t a[2], b[2];
        a[0] = 0x0041u; a[1] = 0x0042u;
        b[0] = 0x4100u; b[1] = 0x4200u;
        CHECK("byte order is significant",
              hype_exfat_name_hash_update(0u, a, 2u) != hype_exfat_name_hash_update(0u, b, 2u));
    }
    CHECK_HEX("empty name hashes to 0", 0u, hype_exfat_name_hash_update(0u, 0, 0u));
}

static void test_table_checksum(void) {
    uint8_t buf[4] = {1, 2, 3, 4};
    unsigned i;
    CHECK_HEX("table checksum of 01 02 03 04", 0x20000006u,
              hype_exfat_upcase_checksum_update(0u, buf, 4u));
    CHECK_HEX("table checksum matches the spec reference", ref_table_checksum(buf, 4u),
              hype_exfat_upcase_checksum_update(0u, buf, 4u));
    /* Streaming in one-byte pieces must match. */
    {
        uint32_t c = 0;
        for (i = 0; i < 4u; i++) c = hype_exfat_upcase_checksum_update(c, buf + i, 1u);
        CHECK_HEX("streamed table checksum", 0x20000006u, c);
    }
}

/* ---- up-case table ---- */

/*
 * Builds a compressed up-case table shaped like the reference one every
 * formatter writes: an identity run for 0x00..0x60, explicit mappings for
 * 'a'..'z', an identity run over the rest of ASCII, explicit Latin-1 mappings,
 * and a trailing 0xFFFF marker with no count word behind it (which is how the
 * real table ends -- it must terminate the table, not be treated as corrupt).
 */
static unsigned build_upcase(uint8_t *out, unsigned cap) {
    unsigned n = 0;
    unsigned c;
#define W(v) do { if (n + 2u <= cap) { put16(out + n, (uint16_t)(v)); } n += 2u; } while (0)
    W(0xFFFF); W(0x61);          /* chars 0x00..0x60 map to themselves */
    for (c = 0x61u; c <= 0x7Au; c++) {
        W(c - 0x20u);            /* 'a'..'z' -> 'A'..'Z' */
    }
    W(0xFFFF); W(0x65);          /* 0x7B..0xDF identity (0x65 == 101 chars) */
    for (c = 0xE0u; c <= 0xFEu; c++) {
        W((c == 0xF7u) ? c : (c - 0x20u)); /* Latin-1 lowercase block, minus 0xF7 */
    }
    W(0x0178);                   /* 0xFF -> U+0178, as in the reference table */
    W(0xFFFF);                   /* trailing marker, no count word: end of table */
#undef W
    return n;
}

static void test_upcase_decompress(void) {
    uint8_t table[512];
    unsigned len = build_upcase(table, sizeof table);
    hype_exfat_upcase_t u;
    unsigned chunk;

    CHECK("upcase fixture fits", len <= sizeof table);

    /* Whole table in one feed. */
    hype_exfat_upcase_reset(&u);
    hype_exfat_upcase_feed(&u, table, len);
    CHECK_HEX("no malformed flag", 0u, u.malformed);
    CHECK_HEX("checksum matches the reference", ref_table_checksum(table, len), u.checksum);
    CHECK_HEX("'a' -> 'A'", 'A', hype_exfat_upcase(&u, 'a'));
    CHECK_HEX("'z' -> 'Z'", 'Z', hype_exfat_upcase(&u, 'z'));
    CHECK_HEX("'A' unchanged", 'A', hype_exfat_upcase(&u, 'A'));
    CHECK_HEX("'0' unchanged", '0', hype_exfat_upcase(&u, '0'));
    CHECK_HEX("'{' (just past 'z') unchanged", '{', hype_exfat_upcase(&u, '{'));
    CHECK_HEX("0x60 (just before 'a') unchanged", 0x60u, hype_exfat_upcase(&u, 0x60u));
    CHECK_HEX("0xE0 -> 0xC0", 0xC0u, hype_exfat_upcase(&u, 0xE0u));
    CHECK_HEX("0xF7 (division sign) unchanged", 0xF7u, hype_exfat_upcase(&u, 0xF7u));
    CHECK_HEX("0xFF -> 0x0178", 0x0178u, hype_exfat_upcase(&u, 0xFFu));
    CHECK_HEX("characters described", 0x100u, u.chars);
    CHECK("every cached character is exact", hype_exfat_upcase_exact(&u, 'a') &&
                                                 hype_exfat_upcase_exact(&u, 0xFFu));
    CHECK("a character past the cache is not exact",
          !hype_exfat_upcase_exact(&u, HYPE_EXFAT_UPCASE_CACHE));
    CHECK_HEX("uncached character maps to itself", 0x0250u, hype_exfat_upcase(&u, 0x0250u));

    /*
     * Feeding the same table in every chunk size must give the same result. Odd
     * chunk sizes split 16-bit entries, and a chunk boundary can also land
     * between a 0xFFFF marker and its count word -- both need carried state.
     */
    for (chunk = 1u; chunk <= len; chunk++) {
        hype_exfat_upcase_t v;
        unsigned off;
        hype_exfat_upcase_reset(&v);
        for (off = 0; off < len; off += chunk) {
            unsigned n = (len - off < chunk) ? (len - off) : chunk;
            hype_exfat_upcase_feed(&v, table + off, n);
        }
        if (v.malformed || v.chars != u.chars || v.checksum != u.checksum ||
            hype_exfat_upcase(&v, 'a') != 'A' || hype_exfat_upcase(&v, 0xFFu) != 0x0178u) {
            printf("FAIL: upcase feed chunk size %u disagrees\n", chunk);
            failures++;
            break;
        }
    }

    /* An uncompressed table (no 0xFFFF markers at all) must work too -- that is
     * what mkfs.exfat's own table looks like for its leading identity region. */
    {
        uint8_t plain[64];
        unsigned i;
        for (i = 0; i < 32u; i++) {
            put16(plain + i * 2u, (uint16_t)((i >= 'a' && i <= 'z') ? i - 0x20u : i));
        }
        hype_exfat_upcase_reset(&u);
        hype_exfat_upcase_feed(&u, plain, 64u);
        CHECK_HEX("uncompressed table: 32 chars", 32u, u.chars);
        CHECK_HEX("uncompressed table maps identity", 5u, hype_exfat_upcase(&u, 5u));
        CHECK("char past the table's end is not exact", !hype_exfat_upcase_exact(&u, 40u));
    }

    /* A run that claims more characters than the 0x10000 code-unit space holds is
     * corrupt and must be reported, not silently clamped. */
    {
        uint8_t bad[8];
        put16(bad + 0, 0xFFFF);
        put16(bad + 2, 0xFFFF); /* 65535 identity chars */
        put16(bad + 4, 0xFFFF);
        put16(bad + 6, 0xFFFF); /* another 65535: over-runs the space */
        hype_exfat_upcase_reset(&u);
        hype_exfat_upcase_feed(&u, bad, 8u);
        CHECK_HEX("over-long identity run flagged", 1u, u.malformed);
    }
    /* A literal mapping past the last code unit is equally corrupt. */
    {
        uint8_t bad[6];
        unsigned i;
        put16(bad + 0, 0xFFFF);
        put16(bad + 2, 0xFFFF); /* 65535 identity chars: cursor now at 0xFFFF */
        put16(bad + 4, 0x0041); /* char 0xFFFF -> 'A': legal, fills the space */
        hype_exfat_upcase_reset(&u);
        hype_exfat_upcase_feed(&u, bad, 6u);
        CHECK_HEX("filling the space exactly is fine", 0u, u.malformed);
        CHECK_HEX("cursor at the end of the space", 0x10000u, u.chars);
        for (i = 0; i < 2u; i++) {
            put16(bad + 4, 0x0041);
            hype_exfat_upcase_feed(&u, bad + 4, 2u); /* one literal too many */
        }
        CHECK_HEX("literal past the code-unit space flagged", 1u, u.malformed);
    }
    /* A reset table is the identity mapping and reports nothing as exact. */
    hype_exfat_upcase_reset(&u);
    CHECK_HEX("reset table is identity", 'a', hype_exfat_upcase(&u, 'a'));
    CHECK("reset table knows nothing exactly", !hype_exfat_upcase_exact(&u, 'a'));
}

/* ---- allocation bitmap ---- */

static void test_bitmap(void) {
    uint8_t sec[HYPE_EXFAT_SECTOR_SIZE];
    uint64_t lba;
    unsigned int bit, i;

    memset(sec, 0, sizeof sec);
    /* Cluster 2 is bit 0, LSB first within each byte. */
    hype_exfat_bitmap_location(2u, 100u, &lba, &bit);
    CHECK_HEX("cluster 2 sector", 100ull, lba);
    CHECK_HEX("cluster 2 bit", 0u, bit);
    hype_exfat_bitmap_location(2u + 4095u, 100u, &lba, &bit);
    CHECK_HEX("last bit of sector 0", 100ull, lba);
    CHECK_HEX("last bit index", 4095u, bit);
    hype_exfat_bitmap_location(2u + 4096u, 100u, &lba, &bit);
    CHECK_HEX("first bit of sector 1", 101ull, lba);
    CHECK_HEX("wraps to bit 0", 0u, bit);

    hype_exfat_bitmap_set(sec, 0u, 1);
    CHECK_HEX("bit 0 lands in byte 0 bit 0", 0x01u, sec[0]);
    hype_exfat_bitmap_set(sec, 7u, 1);
    CHECK_HEX("bit 7 lands in byte 0 bit 7", 0x81u, sec[0]);
    hype_exfat_bitmap_set(sec, 8u, 1);
    CHECK_HEX("bit 8 lands in byte 1", 0x01u, sec[1]);
    CHECK_HEX("get sees bit 7", 1, hype_exfat_bitmap_get(sec, 7u));
    CHECK_HEX("get sees a clear bit", 0, hype_exfat_bitmap_get(sec, 6u));
    hype_exfat_bitmap_set(sec, 7u, 0);
    CHECK_HEX("clearing leaves neighbours alone", 0x01u, sec[0]);

    /* find_free honours the starting bit and the limit. */
    memset(sec, 0, sizeof sec);
    sec[0] = 0xFFu; /* bits 0..7 taken */
    CHECK_HEX("find_free skips a full byte", 0, hype_exfat_bitmap_find_free(sec, 0u, 4096u, &bit));
    CHECK_HEX("first free bit is 8", 8u, bit);
    CHECK_HEX("find_free respects from_bit", 0,
              hype_exfat_bitmap_find_free(sec, 100u, 4096u, &bit));
    CHECK_HEX("found bit 100", 100u, bit);
    CHECK_HEX("find_free respects the limit", -1,
              hype_exfat_bitmap_find_free(sec, 0u, 8u, &bit));
    memset(sec, 0xFFu, sizeof sec);
    CHECK_HEX("a full sector has nothing free", -1,
              hype_exfat_bitmap_find_free(sec, 0u, 4096u, &bit));
    CHECK_HEX("a full sector counts 4096", 4096u, hype_exfat_bitmap_count(sec, 4096u));
    CHECK_HEX("count honours the limit", 10u, hype_exfat_bitmap_count(sec, 10u));
    /* An over-large limit is clamped to the sector rather than read past it. */
    CHECK_HEX("count clamps to the sector", 4096u, hype_exfat_bitmap_count(sec, 99999u));
    CHECK_HEX("find_free clamps to the sector", -1,
              hype_exfat_bitmap_find_free(sec, 0u, 99999u, &bit));
    /* One free bit in the middle of an otherwise full sector. */
    memset(sec, 0xFFu, sizeof sec);
    hype_exfat_bitmap_set(sec, 2000u, 0);
    CHECK_HEX("finds the lone free bit", 0, hype_exfat_bitmap_find_free(sec, 0u, 4096u, &bit));
    CHECK_HEX("lone free bit index", 2000u, bit);
    CHECK_HEX("count with one hole", 4095u, hype_exfat_bitmap_count(sec, 4096u));
    memset(sec, 0, sizeof sec);
    for (i = 0; i < 4096u; i++) {
        if ((i % 3u) == 0u) hype_exfat_bitmap_set(sec, i, 1);
    }
    CHECK_HEX("count of every third bit", 1366u, hype_exfat_bitmap_count(sec, 4096u));
}

/* ---- addressing ---- */

static void test_addressing(void) {
    uint32_t ci, sic;
    unsigned int off;

    CHECK_HEX("cluster 2 is the heap's first sector", 100ull,
              hype_exfat_cluster_lba(100u, 8u, 2u));
    CHECK_HEX("cluster 3 with spc 8", 108ull, hype_exfat_cluster_lba(100u, 8u, 3u));
    CHECK_HEX("large cluster number does not overflow 32 bits", 100ull + 8ull * 1000000ull,
              hype_exfat_cluster_lba(100u, 8u, 1000002u));

    /* 16 entries per sector; with spc 1 that is 16 entries per cluster. */
    hype_exfat_entry_pos(0u, 1u, &ci, &sic, &off);
    CHECK_HEX("entry 0 cluster index", 0u, ci);
    CHECK_HEX("entry 0 sector", 0u, sic);
    CHECK_HEX("entry 0 offset", 0u, off);
    hype_exfat_entry_pos(15u, 1u, &ci, &sic, &off);
    CHECK_HEX("entry 15 stays in cluster 0", 0u, ci);
    CHECK_HEX("entry 15 offset", 480u, off);
    hype_exfat_entry_pos(16u, 1u, &ci, &sic, &off);
    CHECK_HEX("entry 16 crosses to cluster 1", 1u, ci);
    CHECK_HEX("entry 16 offset", 0u, off);
    /* spc 8: 128 entries per cluster, 16 per sector. */
    hype_exfat_entry_pos(17u, 8u, &ci, &sic, &off);
    CHECK_HEX("spc8 entry 17 cluster", 0u, ci);
    CHECK_HEX("spc8 entry 17 sector", 1u, sic);
    CHECK_HEX("spc8 entry 17 offset", 32u, off);
    hype_exfat_entry_pos(128u, 8u, &ci, &sic, &off);
    CHECK_HEX("spc8 entry 128 cluster", 1u, ci);
    CHECK_HEX("spc8 entry 128 sector", 0u, sic);
    CHECK_HEX("spc8 entry 128 offset", 0u, off);
}

/* ---- directory-entry encoders ---- */

static void test_encoders(void) {
    uint8_t ent[32];
    uint16_t chars[20];
    unsigned i;

    hype_exfat_file_entry(ent, HYPE_EXFAT_ATTR_ARCHIVE, 3u);
    CHECK_HEX("File entry type", 0x85u, ent[0]);
    CHECK_HEX("SecondaryCount", 3u, ent[1]);
    CHECK_HEX("checksum field starts zeroed", 0u, get16(ent + 2));
    CHECK_HEX("attributes", 0x20u, get16(ent + 4));
    CHECK_HEX("create timestamp is a legal date", HYPE_EXFAT_TIMESTAMP_EPOCH, get32(ent + 8));
    CHECK_HEX("modify timestamp", HYPE_EXFAT_TIMESTAMP_EPOCH, get32(ent + 12));
    CHECK_HEX("access timestamp", HYPE_EXFAT_TIMESTAMP_EPOCH, get32(ent + 16));
    /* The month and day fields are 1-based, so a zero timestamp is out of spec. */
    CHECK_HEX("timestamp month is 1", 1u, (HYPE_EXFAT_TIMESTAMP_EPOCH >> 21) & 0x0Fu);
    CHECK_HEX("timestamp day is 1", 1u, (HYPE_EXFAT_TIMESTAMP_EPOCH >> 16) & 0x1Fu);
    hype_exfat_file_entry_set_checksum(ent, 0xBEEFu);
    CHECK_HEX("checksum written little-endian", 0xBEEFu, get16(ent + 2));

    hype_exfat_stream_entry(ent, 9u, 0x3046u, 1234u, 77u, 5678u, 0);
    CHECK_HEX("Stream entry type", 0xC0u, ent[0]);
    CHECK_HEX("AllocationPossible set, NoFatChain clear", 0x01u, ent[1]);
    CHECK_HEX("NameLength", 9u, ent[3]);
    CHECK_HEX("NameHash", 0x3046u, get16(ent + 4));
    CHECK_HEX("ValidDataLength", 1234ull, get64(ent + 8));
    CHECK_HEX("FirstCluster", 77u, get32(ent + 20));
    CHECK_HEX("DataLength", 5678ull, get64(ent + 24));
    hype_exfat_stream_entry(ent, 1u, 0u, 0u, 2u, 0u, 1);
    CHECK_HEX("NoFatChain set", 0x03u, ent[1]);

    for (i = 0; i < 20u; i++) chars[i] = (uint16_t)('a' + i);
    hype_exfat_name_entry(ent, chars, 20u);
    CHECK_HEX("Name entry type", 0xC1u, ent[0]);
    CHECK_HEX("first char", 'a', get16(ent + 2));
    /* Only 15 code units fit; the rest belong to the next entry. */
    CHECK_HEX("15th char", (uint16_t)('a' + 14), get16(ent + 2 + 14 * 2));
    hype_exfat_name_entry(ent, chars, 3u);
    CHECK_HEX("short name pads with zeroes", 0u, get16(ent + 2 + 3 * 2));
}

static void test_name_to_utf16(void) {
    uint16_t out[HYPE_EXFAT_MAX_NAME];
    char big[HYPE_EXFAT_MAX_NAME + 8];
    unsigned i;
    static const char *bad[] = {"", "a\"b", "a*b", "a/b", "a:b", "a<b", "a>b", "a?b",
                                "a\\b", "a|b", "a\tb"};

    CHECK_HEX("simple name length", 9, hype_exfat_name_to_utf16("hello.txt", out, sizeof out / 2));
    CHECK_HEX("first code unit", 'h', out[0]);
    CHECK_HEX("last code unit", 't', out[8]);
    CHECK_HEX("spaces are allowed", 5, hype_exfat_name_to_utf16("a b c", out, sizeof out / 2));
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK_HEX("invalid name rejected", -1,
                  hype_exfat_name_to_utf16(bad[i], out, sizeof out / 2));
    }
    /* Exactly at the limit is fine; one over is not. */
    for (i = 0; i < HYPE_EXFAT_MAX_NAME; i++) big[i] = 'x';
    big[HYPE_EXFAT_MAX_NAME] = '\0';
    CHECK_HEX("255-character name accepted", (int)HYPE_EXFAT_MAX_NAME,
              hype_exfat_name_to_utf16(big, out, sizeof out / 2));
    big[HYPE_EXFAT_MAX_NAME] = 'x';
    big[HYPE_EXFAT_MAX_NAME + 1u] = '\0';
    CHECK_HEX("256-character name rejected", -1,
              hype_exfat_name_to_utf16(big, out, sizeof out / 2));
    /* The caller's own capacity is honoured even when it is below the exFAT max. */
    CHECK_HEX("name longer than the buffer rejected", -1,
              hype_exfat_name_to_utf16("abcdef", out, 3u));
    CHECK_HEX("name exactly filling the buffer accepted", 3,
              hype_exfat_name_to_utf16("abc", out, 3u));
}

/* ---- entry-set reading ---- */

/* A directory laid out in memory, fed to hype_exfat_set_read entry by entry. */
#define DIR_ENTRIES 32u
static uint8_t g_dir[DIR_ENTRIES * 32u];
static uint32_t g_dir_limit = DIR_ENTRIES;
static uint32_t g_fail_entry = 0xFFFFFFFFu;

static int dir_read(void *ctx, uint32_t ei, uint8_t ent[32]) {
    (void)ctx;
    if (ei >= g_dir_limit || ei == g_fail_entry) return -1;
    memcpy(ent, g_dir + ei * 32u, 32u);
    return 0;
}

/* Builds a File + Stream + name-entries set at entry index 0. */
static void build_set(const char *name, unsigned name_entries, uint16_t attr, uint32_t first_cl,
                      uint64_t len) {
    unsigned nlen = (unsigned)strlen(name);
    unsigned k;
    uint16_t chars[HYPE_EXFAT_MAX_NAME];
    unsigned i;
    memset(g_dir, 0, sizeof g_dir);
    for (i = 0; i < nlen; i++) chars[i] = (uint16_t)name[i];
    hype_exfat_file_entry(g_dir, attr, (uint8_t)(1u + name_entries));
    hype_exfat_stream_entry(g_dir + 32u, nlen, 0u, len, first_cl, len, 0);
    for (k = 0; k < name_entries; k++) {
        unsigned off = k * 15u;
        unsigned count = (nlen > off) ? (nlen - off) : 0u;
        if (count > 15u) count = 15u;
        hype_exfat_name_entry(g_dir + (2u + k) * 32u, chars + off, count);
    }
    hype_exfat_file_entry_set_checksum(
        g_dir, hype_exfat_set_checksum_update(0u, 0u, g_dir, (2u + name_entries) * 32u));
}

static void test_set_read(void) {
    hype_exfat_set_t set;
    g_dir_limit = DIR_ENTRIES;
    g_fail_entry = 0xFFFFFFFFu;

    build_set("report.txt", 1u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 900u);
    CHECK_HEX("well-formed set accepted", 0, hype_exfat_set_read(dir_read, 0, 0u, &set));
    CHECK_HEX("secondary count", 2u, set.secondary);
    CHECK_HEX("attributes", 0x20u, set.attributes);
    CHECK_HEX("first cluster", 5u, set.first_cluster);
    CHECK_HEX("data length", 900ull, set.data_length);
    CHECK_HEX("valid length", 900ull, set.valid_length);
    CHECK_HEX("name length", 10u, set.name_length);
    CHECK_HEX("name first char", 'r', set.name[0]);
    CHECK_HEX("name last char", 't', set.name[9]);
    CHECK_HEX("not contiguous", 0u, set.contiguous);

    /* A name needing three File Name entries (31 characters). */
    build_set("abcdefghijklmnopqrstuvwxyz01234", 3u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 1u);
    CHECK_HEX("three-name-entry set accepted", 0, hype_exfat_set_read(dir_read, 0, 0u, &set));
    CHECK_HEX("31-char name length", 31u, set.name_length);
    CHECK_HEX("char 15 (second entry)", 'p', set.name[15]);
    CHECK_HEX("char 30 (third entry)", '4', set.name[30]);

    /* Corrupted checksum. */
    build_set("report.txt", 1u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 900u);
    g_dir[2] ^= 0x01u;
    CHECK_HEX("bad checksum rejected", -1, hype_exfat_set_read(dir_read, 0, 0u, &set));

    /* Not a File entry / not in use. */
    build_set("report.txt", 1u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 900u);
    g_dir[0] = 0x05u;
    CHECK_HEX("not-in-use File entry rejected", -1, hype_exfat_set_read(dir_read, 0, 0u, &set));
    build_set("report.txt", 1u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 900u);
    g_dir[0] = HYPE_EXFAT_ENT_BITMAP;
    CHECK_HEX("bitmap entry is not a File set", -1, hype_exfat_set_read(dir_read, 0, 0u, &set));

    /* SecondaryCount out of range in both directions. */
    build_set("report.txt", 1u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 900u);
    g_dir[1] = 0u;
    hype_exfat_file_entry_set_checksum(g_dir,
                                       hype_exfat_set_checksum_update(0u, 0u, g_dir, 32u));
    CHECK_HEX("SecondaryCount 0 rejected", -1, hype_exfat_set_read(dir_read, 0, 0u, &set));
    build_set("report.txt", 1u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 900u);
    g_dir[1] = (uint8_t)(HYPE_EXFAT_MAX_SECONDARY + 1u);
    CHECK_HEX("SecondaryCount over the max rejected", -1,
              hype_exfat_set_read(dir_read, 0, 0u, &set));

    /* Missing Stream Extension entry. */
    build_set("report.txt", 1u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 900u);
    g_dir[32] = HYPE_EXFAT_ENT_NAME;
    hype_exfat_file_entry_set_checksum(g_dir,
                                       hype_exfat_set_checksum_update(0u, 0u, g_dir, 96u));
    CHECK_HEX("missing Stream entry rejected", -1, hype_exfat_set_read(dir_read, 0, 0u, &set));

    /* NameLength longer than the name entries can hold. */
    build_set("report.txt", 1u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 900u);
    g_dir[32 + 3] = 40u;
    hype_exfat_file_entry_set_checksum(g_dir,
                                       hype_exfat_set_checksum_update(0u, 0u, g_dir, 96u));
    CHECK_HEX("over-long NameLength rejected", -1, hype_exfat_set_read(dir_read, 0, 0u, &set));

    /* ValidDataLength beyond DataLength. */
    build_set("report.txt", 1u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 900u);
    g_dir[32 + 8] = 0xFFu;
    hype_exfat_file_entry_set_checksum(g_dir,
                                       hype_exfat_set_checksum_update(0u, 0u, g_dir, 96u));
    CHECK_HEX("ValidDataLength > DataLength rejected", -1,
              hype_exfat_set_read(dir_read, 0, 0u, &set));

    /* A set cut short by the end of the directory. */
    build_set("report.txt", 1u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 900u);
    g_dir_limit = 2u; /* the name entry at index 2 is unreachable */
    CHECK_HEX("truncated set rejected", -1, hype_exfat_set_read(dir_read, 0, 0u, &set));
    g_dir_limit = DIR_ENTRIES;

    /* An I/O failure on any entry of the set. */
    build_set("report.txt", 1u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 900u);
    g_fail_entry = 0u;
    CHECK_HEX("File entry read failure", -1, hype_exfat_set_read(dir_read, 0, 0u, &set));
    g_fail_entry = 1u;
    CHECK_HEX("Stream entry read failure", -1, hype_exfat_set_read(dir_read, 0, 0u, &set));
    g_fail_entry = 2u;
    CHECK_HEX("Name entry read failure", -1, hype_exfat_set_read(dir_read, 0, 0u, &set));
    g_fail_entry = 0xFFFFFFFFu;

    /* An unrecognised secondary entry type still counts towards the checksum but
     * contributes no name characters. */
    build_set("report.txt", 2u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 900u);
    g_dir[3u * 32u] = 0xE0u; /* a secondary type hype does not know */
    hype_exfat_file_entry_set_checksum(g_dir,
                                       hype_exfat_set_checksum_update(0u, 0u, g_dir, 128u));
    CHECK_HEX("unknown secondary entry tolerated", 0, hype_exfat_set_read(dir_read, 0, 0u, &set));
    CHECK_HEX("name still 10 chars", 10u, set.name_length);

    /* A directory entry set works from a non-zero index too. */
    build_set("report.txt", 1u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 900u);
    memmove(g_dir + 5u * 32u, g_dir, 96u);
    memset(g_dir, 0, 96u);
    CHECK_HEX("set at index 5 accepted", 0, hype_exfat_set_read(dir_read, 0, 5u, &set));
    CHECK_HEX("index-5 name length", 10u, set.name_length);

    /* Contiguous (NoFatChain) streams are reported as such. */
    build_set("report.txt", 1u, HYPE_EXFAT_ATTR_ARCHIVE, 5u, 900u);
    g_dir[32 + 1] |= HYPE_EXFAT_FLAG_NO_FAT_CHAIN;
    hype_exfat_file_entry_set_checksum(g_dir,
                                       hype_exfat_set_checksum_update(0u, 0u, g_dir, 96u));
    CHECK_HEX("NoFatChain surfaced", 0, hype_exfat_set_read(dir_read, 0, 0u, &set));
    CHECK_HEX("contiguous flag", 1u, set.contiguous);

    /* Directory attribute surfaced. */
    build_set("adir", 1u, HYPE_EXFAT_ATTR_DIRECTORY, 5u, 4096u);
    CHECK_HEX("directory set accepted", 0, hype_exfat_set_read(dir_read, 0, 0u, &set));
    CHECK_HEX("directory attribute", HYPE_EXFAT_ATTR_DIRECTORY,
              set.attributes & HYPE_EXFAT_ATTR_DIRECTORY);
}

int main(void) {
    test_set_checksum();
    test_name_hash();
    test_table_checksum();
    test_upcase_decompress();
    test_bitmap();
    test_addressing();
    test_encoders();
    test_name_to_utf16();
    test_set_read();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
