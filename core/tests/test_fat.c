#include <stdio.h>
#include <string.h>
#include "../fat.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define VOL_SECTORS 400u
static uint8_t g_vol[VOL_SECTORS * HYPE_FAT_SECTOR_SIZE];

static uint64_t g_fail_lba = (uint64_t)-1; /* inject a read failure at this LBA */

static int vol_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if (count != 1u || lba >= VOL_SECTORS || lba == g_fail_lba) {
        return -1;
    }
    memcpy(dst, g_vol + lba * HYPE_FAT_SECTOR_SIZE, HYPE_FAT_SECTOR_SIZE);
    return 0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* ---- FAT32 synthetic volume ----
 * reserved=32, numFATs=1, FATSz=1 -> FAT at sector 32, data_start=33, spc=1.
 * cluster N -> sector 33 + (N-2). Root=cluster 2 (sector 33).
 *  root: dir "ISO" (cluster 3), file "verylongname.iso" (LFN, cluster 6, 1000B)
 *  ISO/: file "TEST.ISO" (cluster 4->5 chained, 1000 bytes => 2 sectors) */
static uint8_t *fat_entry_ptr(uint32_t cl) { return g_vol + 32u * HYPE_FAT_SECTOR_SIZE + cl * 4u; }
static uint8_t *cluster_ptr(uint32_t cl) { return g_vol + (33u + (cl - 2u)) * HYPE_FAT_SECTOR_SIZE; }

static void put_short_entry(uint8_t *e, const char *n83, uint8_t attr, uint32_t first_cl,
                            uint32_t size) {
    unsigned i;
    for (i = 0; i < 11u; i++) e[i] = (uint8_t)n83[i];
    e[0x0B] = attr;
    put16(e + 0x14, (uint16_t)(first_cl >> 16));
    put16(e + 0x1A, (uint16_t)(first_cl & 0xFFFFu));
    put32(e + 0x1C, size);
}

static void build_fat32(void) {
    uint8_t *bpb = g_vol;
    uint8_t *root, *isodir;
    memset(g_vol, 0, sizeof(g_vol));

    put16(bpb + 0x0B, 512);   /* bytes/sector */
    bpb[0x0D] = 1;            /* spc */
    put16(bpb + 0x0E, 32);    /* reserved */
    bpb[0x10] = 1;            /* numFATs */
    put16(bpb + 0x16, 0);     /* FATSz16 = 0 => FAT32 */
    put32(bpb + 0x24, 1);     /* FATSz32 = 1 sector */
    put32(bpb + 0x2C, 2);     /* root cluster */

    /* FAT: [0],[1] reserved; 2=EOC(root); 3=EOC(ISO dir); 4->5->EOC (TEST.ISO);
     * 6=EOC (verylongname.iso). */
    put32(fat_entry_ptr(0), 0x0FFFFFF8u);
    put32(fat_entry_ptr(1), 0x0FFFFFFFu);
    put32(fat_entry_ptr(2), 0x0FFFFFFFu);
    put32(fat_entry_ptr(3), 0x0FFFFFFFu);
    put32(fat_entry_ptr(4), 5u);
    put32(fat_entry_ptr(5), 0x0FFFFFFFu);
    put32(fat_entry_ptr(6), 0x0FFFFFFFu);
    put32(fat_entry_ptr(7), 9u);          /* frag.bin: 7 -> 9 (non-contiguous) */
    put32(fat_entry_ptr(9), 0x0FFFFFFFu);

    /* Root dir (cluster 2): "ISO" dir @cluster3; then an LFN set for
     * "verylongname.iso" (13-char fits? no -> 16 chars => 2 LFN entries) @cluster6. */
    root = cluster_ptr(2);
    put_short_entry(root, "ISO        ", 0x10u /* dir */, 3u, 0u);

    /* LFN set for "verylongname.iso" (16 chars => entries seq 2 then seq 1|0x40). */
    {
        static const char *ln = "verylongname.iso"; /* 16 chars */
        unsigned len = 16u;
        uint8_t *l2 = root + 32;      /* physical first = highest seq (0x42) */
        uint8_t *l1 = root + 64;      /* seq 1 (0x01) */
        uint8_t *sh = root + 96;      /* 8.3 entry */
        static const unsigned off[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
        unsigned i;
        /* seq2 holds chars 13..25 (only 13,14,15 present then terminate) */
        l2[0] = 0x42u; l2[0x0B] = 0x0Fu;
        for (i = 0; i < 13u; i++) {
            unsigned ci = 13u + i;
            uint16_t u = (ci < len) ? (uint16_t)ln[ci] : (ci == len ? 0u : 0xFFFFu);
            put16(l2 + off[i], u);
        }
        /* seq1 holds chars 0..12 */
        l1[0] = 0x01u; l1[0x0B] = 0x0Fu;
        for (i = 0; i < 13u; i++) put16(l1 + off[i], (uint16_t)ln[i]);
        put_short_entry(sh, "VERYLO~1ISO", 0x20u, 6u, 1000u);
    }

    /* root: empty file (first cluster 0, size 0) and a fragmented file. */
    put_short_entry(root + 128, "EMPTY   DAT", 0x20u, 0u, 0u);
    put_short_entry(root + 160, "FRAG    BIN", 0x20u, 7u, 700u);


    /* ISO dir (cluster 3): a stray LFN entry with an out-of-range sequence
     * number (lfn_piece must clamp its writes), then a deleted entry + a
     * volume-label entry, then the real "TEST.ISO" file. */
    isodir = cluster_ptr(3);
    {
        static const unsigned off[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
        unsigned i;
        isodir[0] = 0x14u;      /* LFN sequence 20 -> base index 247, writes clamp at 255 */
        isodir[0x0B] = 0x0Fu;
        for (i = 0; i < 13u; i++) {
            put16(isodir + off[i], (uint16_t)('a' + (int)i));
        }
    }
    isodir[32] = 0xE5u;                                          /* deleted entry */
    put_short_entry(isodir + 64, "VOLLABEL   ", 0x08u, 0u, 0u);  /* volume label */
    put_short_entry(isodir + 96, "TEST    ISO", 0x20u, 4u, 1000u);
}

static void test_fat32_nested_file(void) {
    hype_fat_file_t f;
    build_fat32();
    CHECK_HEX("resolve \\iso\\test.iso ok", 0, hype_fat32_resolve(vol_read, 0, "\\iso\\test.iso", &f));
    CHECK_HEX("size", 1000u, f.size_bytes);
    CHECK_HEX("one coalesced extent", 1u, f.count);
    /* clusters 4,5 => sectors 35,36; contiguous => start 35, 2 sectors (ceil 1000/512). */
    CHECK_HEX("extent start lba", 35ull, f.extents[0].start_lba);
    CHECK_HEX("extent sectors (trimmed)", 2ull, f.extents[0].sector_count);
}

static void test_fat32_forward_slash_and_case(void) {
    hype_fat_file_t f;
    build_fat32();
    CHECK_HEX("resolve /ISO/TeSt.IsO ok", 0, hype_fat32_resolve(vol_read, 0, "/ISO/TeSt.IsO", &f));
    CHECK_HEX("size", 1000u, f.size_bytes);
}

static void test_fat32_lfn_match(void) {
    hype_fat_file_t f;
    build_fat32();
    CHECK_HEX("resolve LFN \\verylongname.iso ok", 0,
              hype_fat32_resolve(vol_read, 0, "\\verylongname.iso", &f));
    CHECK_HEX("LFN file size", 1000u, f.size_bytes);
    CHECK_HEX("LFN cluster6 -> sector 37", 37ull, f.extents[0].start_lba);
}

static void test_fat32_not_found_and_dir_as_file(void) {
    hype_fat_file_t f;
    build_fat32();
    CHECK_HEX("missing file", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\iso\\nope.iso", &f));
    CHECK_HEX("dir resolved as file rejected", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\iso", &f));
    CHECK_HEX("empty path rejected", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\", &f));
    CHECK_HEX("non-dir mid-path rejected", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\iso\\test.iso\\x", &f));
}

static void test_fat32_bad_bpb(void) {
    hype_fat_file_t f;
    build_fat32();
    put16(g_vol + 0x0B, 4096); /* non-512 sector */
    CHECK_HEX("non-512 sector rejected", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\iso\\test.iso", &f));
    build_fat32();
    put16(g_vol + 0x16, 8);    /* FATSz16 nonzero => not FAT32 */
    CHECK_HEX("FAT16-shaped bpb rejected", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\iso\\test.iso", &f));
}

/* ---- exFAT synthetic volume ----
 * spc=1 (shift 0). fat_offset=8, cluster_heap=16, root=cluster2 (sector16).
 * root dir: File set for contiguous "test.iso" (cluster3, 700B, NoFatChain)
 *           File set for a directory "sub" (cluster4)
 *   sub/  : File set for chained "big.bin" (cluster5->6, 700B). */
static void exfat_name_entry(uint8_t *e, const char *name, unsigned n) {
    unsigned i;
    memset(e, 0, 32);
    e[0] = 0xC1u;
    for (i = 0; i < n && i < 15u; i++) put16(e + 2u + i * 2u, (uint16_t)name[i]);
}
static void exfat_file_set(uint8_t *base, const char *name, uint8_t is_dir, uint8_t no_fat_chain,
                           uint32_t first_cl, uint64_t data_len) {
    unsigned nlen = (unsigned)strlen(name);
    uint8_t *file = base, *stream = base + 32, *nm = base + 64;
    memset(file, 0, 96);
    file[0] = 0x85u;      /* File */
    file[1] = 2u;         /* secondary count: stream + 1 name entry */
    put16(file + 4, is_dir ? 0x0010u : 0x0020u); /* attributes */
    stream[0] = 0xC0u;    /* Stream Extension */
    stream[1] = (uint8_t)(0x01u | (no_fat_chain ? 0x02u : 0x00u)); /* AllocPossible + NoFatChain */
    put32(stream + 0x14, first_cl);
    put64(stream + 0x18, data_len);
    exfat_name_entry(nm, name, nlen);
}
static uint8_t *exfat_cluster(uint32_t cl) { return g_vol + (16u + (cl - 2u)) * HYPE_FAT_SECTOR_SIZE; }
static uint8_t *exfat_fat_entry(uint32_t cl) { return g_vol + 8u * HYPE_FAT_SECTOR_SIZE + cl * 4u; }

static void build_exfat(void) {
    uint8_t *b = g_vol;
    memset(g_vol, 0, sizeof(g_vol));
    b[3] = 'E'; b[4] = 'X'; b[5] = 'F'; b[6] = 'A'; b[7] = 'T';
    put32(b + 0x50, 8);   /* FatOffset */
    put32(b + 0x58, 16);  /* ClusterHeapOffset */
    put32(b + 0x60, 2);   /* root cluster */
    b[0x6C] = 9;          /* BytesPerSectorShift = 512 */
    b[0x6D] = 0;          /* SectorsPerClusterShift = 1 */

    /* root dir (cluster 2, sector 16). */
    exfat_file_set(exfat_cluster(2) + 0,   "test.iso", 0, 1, 3u, 700u); /* contiguous @cl3 */
    exfat_file_set(exfat_cluster(2) + 96,  "sub",      1, 1, 4u, 0u);   /* subdir @cl4 */
    exfat_file_set(exfat_cluster(2) + 192, "empty",    0, 1, 3u, 0u);   /* zero-length */
    /* over-fragmented chained file: 66 non-adjacent clusters => > MAX_EXTENTS. */
    exfat_file_set(exfat_cluster(2) + 288, "toofrag", 0, 0, 100u, 66u * 512u);
    {
        uint32_t c;
        for (c = 100u; c < 230u; c += 2u) {
            put32(exfat_fat_entry(c), c + 2u);
        }
        put32(exfat_fat_entry(230u), 0xFFFFFFFFu);
    }

    /* sub dir (cluster 4, sector 18): chained big.bin @cl5->6->8 (5,6 adjacent so
     * they coalesce; 8 is non-contiguous so it opens a second extent). 1200B => 3 sectors. */
    exfat_file_set(exfat_cluster(4) + 0, "big.bin", 0, 0, 5u, 1200u);
    put32(exfat_fat_entry(5), 6u);
    put32(exfat_fat_entry(6), 8u);
    put32(exfat_fat_entry(8), 0xFFFFFFFFu);
}

static void test_exfat_contiguous(void) {
    hype_fat_file_t f;
    build_exfat();
    CHECK_HEX("exfat resolve \\test.iso ok", 0, hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    CHECK_HEX("exfat size", 700u, f.size_bytes);
    CHECK_HEX("exfat one extent", 1u, f.count);
    CHECK_HEX("exfat cluster3 -> sector 17", 17ull, f.extents[0].start_lba);
    CHECK_HEX("exfat 2 sectors", 2ull, f.extents[0].sector_count);
}

static void test_exfat_bad(void) {
    hype_fat_file_t f;
    build_exfat();
    CHECK_HEX("exfat missing file", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\nope", &f));
    build_exfat();
    g_vol[3] = 'N'; /* break EXFAT signature */
    CHECK_HEX("bad exfat sig rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    build_exfat();
    g_vol[0x6C] = 12; /* 4K sectors unsupported */
    CHECK_HEX("exfat non-512 rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
}

static void test_fat32_skip_and_edge_entries(void) {
    hype_fat_file_t f;
    build_fat32();
    /* TEST.ISO is preceded by a deleted + a volume-label entry in the ISO dir. */
    CHECK_HEX("resolve past deleted/volume entries", 0,
              hype_fat32_resolve(vol_read, 0, "\\iso\\test.iso", &f));
    /* empty file: zero extents, zero size. */
    build_fat32();
    CHECK_HEX("empty file ok", 0, hype_fat32_resolve(vol_read, 0, "\\empty.dat", &f));
    CHECK_HEX("empty file size 0", 0ull, f.size_bytes);
    CHECK_HEX("empty file 0 extents", 0u, f.count);
    /* fragmented file: clusters 7,9 => sectors 40,42 => two extents. */
    build_fat32();
    CHECK_HEX("fragmented file ok", 0, hype_fat32_resolve(vol_read, 0, "\\frag.bin", &f));
    CHECK_HEX("fragmented: two extents", 2u, f.count);
    CHECK_HEX("frag extent0 lba", 38ull, f.extents[0].start_lba); /* cluster 7 => 33+(7-2) */
    CHECK_HEX("frag extent0 sectors", 1ull, f.extents[0].sector_count);
    CHECK_HEX("frag extent1 lba", 40ull, f.extents[1].start_lba); /* cluster 9 => 33+(9-2) */
    CHECK_HEX("frag extent1 sectors", 1ull, f.extents[1].sector_count);
}

static void test_fat32_read_failures(void) {
    hype_fat_file_t f;
    build_fat32();
    g_fail_lba = 0; /* BPB read fails */
    CHECK_HEX("bpb read failure", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\iso\\test.iso", &f));
    build_fat32();
    g_fail_lba = 33; /* root-dir cluster read fails */
    CHECK_HEX("dir read failure", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\iso\\test.iso", &f));
    g_fail_lba = (uint64_t)-1;
}

static void test_exfat_empty_and_dir(void) {
    hype_fat_file_t f;
    build_exfat();
    CHECK_HEX("exfat empty file ok", 0, hype_exfat_resolve(vol_read, 0, "\\empty", &f));
    CHECK_HEX("exfat empty size 0", 0ull, f.size_bytes);
    CHECK_HEX("exfat empty 0 extents", 0u, f.count);
    build_exfat();
    CHECK_HEX("exfat dir-as-file rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\sub", &f));
}

static void test_exfat_chained_multi_extent(void) {
    hype_fat_file_t f;
    build_exfat();
    CHECK_HEX("exfat chained big.bin ok", 0, hype_exfat_resolve(vol_read, 0, "\\sub\\big.bin", &f));
    CHECK_HEX("exfat 1200B size", 1200u, f.size_bytes);
    /* clusters 5,6 coalesce (sectors 19,20), 8 separate (sector 22) => 2 extents. */
    CHECK_HEX("exfat two extents", 2u, f.count);
    CHECK_HEX("exfat ext0 start", 19ull, f.extents[0].start_lba);
    CHECK_HEX("exfat ext0 sectors (coalesced)", 2ull, f.extents[0].sector_count);
    CHECK_HEX("exfat ext1 start", 22ull, f.extents[1].start_lba);
    CHECK_HEX("exfat ext1 sectors", 1ull, f.extents[1].sector_count);
}

static void test_exfat_read_failures(void) {
    hype_fat_file_t f;
    build_exfat();
    g_fail_lba = 0; /* boot sector read fails */
    CHECK_HEX("exfat boot read failure", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    build_exfat();
    g_fail_lba = 16; /* root dir read fails */
    CHECK_HEX("exfat dir read failure", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    g_fail_lba = (uint64_t)-1;
}

static void test_fat32_bpb_guards(void) {
    hype_fat_file_t f;
    struct { unsigned off; int is16; uint32_t val; const char *d; } cases[] = {
        {0x24, 0, 0u, "FATSz32=0"},   /* rd32 0x24 == 0 */
        {0x0D, 1, 0u, "spc=0"},        /* bpb[0x0D] via 16-bit low byte 0 */
        {0x0E, 1, 0u, "reserved=0"},
        {0x2C, 0, 1u, "root_cluster<2"},
    };
    unsigned i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        build_fat32();
        if (cases[i].is16) {
            put16(g_vol + cases[i].off, (uint16_t)cases[i].val);
        } else {
            put32(g_vol + cases[i].off, cases[i].val);
        }
        CHECK_HEX(cases[i].d, (unsigned long long)(-1),
                  (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\iso\\test.iso", &f));
    }
}

static void test_exfat_signature_bytes(void) {
    hype_fat_file_t f;
    unsigned k;
    /* Break each of the E,X,F,A,T signature bytes in turn (offsets 3..7). */
    for (k = 3u; k <= 7u; k++) {
        build_exfat();
        g_vol[k] = '?';
        CHECK_HEX("exfat sig byte mismatch", (unsigned long long)(-1),
                  (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    }
    /* root_cluster < 2 guard. */
    build_exfat();
    put32(g_vol + 0x60, 1);
    CHECK_HEX("exfat root_cluster<2", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
}

static void test_exfat_corrupt_entry_sets(void) {
    hype_fat_file_t f;
    /* test.iso's stream slot corrupted (not a 0xC0 Stream) => set skipped. */
    build_exfat();
    exfat_cluster(2)[32] = 0xC1u; /* name where a stream should be */
    CHECK_HEX("exfat missing stream => not found", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    /* name slot corrupted (not a 0xC1 Name) => assembled name empty => no match. */
    build_exfat();
    exfat_cluster(2)[64] = 0xC0u; /* stream where a name should be */
    CHECK_HEX("exfat missing name => not found", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
}

/* exFAT volume whose root directory spans two clusters (2 -> 10), with a file
 * set that straddles the cluster boundary -- exercises exfat_read_entry's
 * cross-cluster traversal and a mid-set read failure. */
static void build_exfat_bigdir(void) {
    unsigned i;
    build_exfat();
    /* Chain root dir cluster 2 -> 10. */
    put32(exfat_fat_entry(2), 10u);
    put32(exfat_fat_entry(10), 0xFFFFFFFFu);
    memset(exfat_cluster(2), 0, HYPE_FAT_SECTOR_SIZE);
    memset(exfat_cluster(10), 0, HYPE_FAT_SECTOR_SIZE);
    /* ei 0..14: unused (InUse bit clear) filler entries -- skipped, not end. */
    for (i = 0; i < 15u; i++) {
        exfat_cluster(2)[i * 32u] = 0x05u;
    }
    /* far.iso set straddles the boundary: File @ei15 (cluster 2), Stream @ei16
     * and Name @ei17 (cluster 10). */
    {
        uint8_t *file = exfat_cluster(2) + 15u * 32u;   /* ei15 */
        uint8_t *stream = exfat_cluster(10) + 0u;       /* ei16 */
        uint8_t *nm = exfat_cluster(10) + 32u;          /* ei17 */
        file[0] = 0x85u; file[1] = 2u; put16(file + 4, 0x0020u);
        stream[0] = 0xC0u; stream[1] = 0x03u; put32(stream + 0x14, 3u); put64(stream + 0x18, 700u);
        exfat_name_entry(nm, "far.iso", 7u);
    }
    /* Fill the rest of cluster 10 (ei18..31) with unused fillers and NO 0x00
     * terminator, so scanning for a missing name runs off the end of the chain
     * (cluster 2 -> 10 -> EOC) and exercises exfat_read_entry's chain-end guard. */
    {
        unsigned e;
        for (e = 2u; e < 16u; e++) {
            exfat_cluster(10)[e * 32u] = 0x05u;
        }
    }
}

/* exFAT volume with 2-sector clusters (SectorsPerClusterShift=1), used to hit
 * the partial-last-cluster path in the chained extent builder. */
static void build_exfat_spc2(void) {
    uint8_t *b = g_vol;
    memset(g_vol, 0, sizeof(g_vol));
    b[3] = 'E'; b[4] = 'X'; b[5] = 'F'; b[6] = 'A'; b[7] = 'T';
    put32(b + 0x50, 8);   /* FatOffset */
    put32(b + 0x58, 16);  /* ClusterHeapOffset */
    put32(b + 0x60, 2);   /* root cluster */
    b[0x6C] = 9;          /* 512-byte sectors */
    b[0x6D] = 1;          /* SectorsPerClusterShift = 1 => 2 sectors/cluster */
    /* root dir @cluster2 (sectors 16,17): chained "p.bin" @cl3->4, 1536 bytes
     * (3 sectors). cl3=>sector 18, cl4=>sector 20; 3 sectors over 2 clusters
     * leaves a 1-sector (partial) final cluster. */
    exfat_file_set(exfat_cluster(2) + 0, "p.bin", 0, 0, 3u, 1536u);
    put32(exfat_fat_entry(3), 4u);
    put32(exfat_fat_entry(4), 0xFFFFFFFFu);
}

static void test_exfat_multicluster_dir(void) {
    hype_fat_file_t f;
    build_exfat_bigdir();
    CHECK_HEX("exfat cross-cluster file set ok", 0,
              hype_exfat_resolve(vol_read, 0, "\\far.iso", &f));
    CHECK_HEX("far.iso size", 700u, f.size_bytes);
    /* mid-set read failure: cluster-10 sector (24) fails while reading the set. */
    build_exfat_bigdir();
    g_fail_lba = 24;
    CHECK_HEX("exfat mid-set read failure => not found", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\far.iso", &f));
    g_fail_lba = (uint64_t)-1;
}

static void test_fat_chain_read_failures(void) {
    hype_fat_file_t f;
    /* FAT32: failing the FAT sector while following a chain truncates the chain
     * (fat32_next returns EOC) rather than erroring -- still returns a file. */
    build_fat32();
    g_fail_lba = 32; /* the FAT sector */
    CHECK_HEX("fat32 chain read-fail truncates (still ok)", 0,
              hype_fat32_resolve(vol_read, 0, "\\frag.bin", &f));
    CHECK_HEX("fat32 truncated to first extent", 1u, f.count);
    /* exFAT: same, on its FAT (sector 8) while following big.bin's chain. */
    build_exfat();
    g_fail_lba = 8;
    CHECK_HEX("exfat chain read-fail truncates (still ok)", 0,
              hype_exfat_resolve(vol_read, 0, "\\sub\\big.bin", &f));
    g_fail_lba = (uint64_t)-1;
}

/* Dedicated FAT32 volume with a 4-sector FAT (holds clusters 0..511) so a
 * 66-cluster non-adjacent chain fits in the FAT region without colliding with
 * the data area. data_start = 32 + 1*4 = 36 (cluster 2 => sector 36). */
static void build_fat32_frag(void) {
    uint8_t *bpb = g_vol;
    uint32_t c;
    memset(g_vol, 0, sizeof(g_vol));
    put16(bpb + 0x0B, 512); bpb[0x0D] = 1; put16(bpb + 0x0E, 32); bpb[0x10] = 1;
    put16(bpb + 0x16, 0); put32(bpb + 0x24, 4); put32(bpb + 0x2C, 2);
    put32(fat_entry_ptr(0), 0x0FFFFFF8u); put32(fat_entry_ptr(1), 0x0FFFFFFFu);
    put32(fat_entry_ptr(2), 0x0FFFFFFFu); /* root EOC */
    put_short_entry(g_vol + 36u * HYPE_FAT_SECTOR_SIZE, "TOOFRAG BIN", 0x20u, 10u, 66u * 512u);
    for (c = 10u; c < 140u; c += 2u) {
        put32(fat_entry_ptr(c), c + 2u); /* 10,12,...,140: 66 non-adjacent clusters */
    }
    put32(fat_entry_ptr(140u), 0x0FFFFFFFu);
}

static void test_over_fragmented(void) {
    hype_fat_file_t f;
    build_fat32_frag();
    CHECK_HEX("fat32 >64 extents rejected", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\toofrag.bin", &f));
    build_exfat();
    CHECK_HEX("exfat >64 extents rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\toofrag", &f));
}

static void test_exfat_more_guards(void) {
    hype_fat_file_t f;
    /* empty component (path is only separators). */
    build_exfat();
    CHECK_HEX("exfat empty component rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\", &f));
    /* non-final component that is a file, not a directory. */
    build_exfat();
    CHECK_HEX("exfat non-dir mid-path rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso\\x", &f));
    /* implausible SectorsPerClusterShift. */
    build_exfat();
    g_vol[0x6D] = 30;
    CHECK_HEX("exfat huge cluster-shift rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
}

static void test_exfat_spc2_partial_cluster(void) {
    hype_fat_file_t f;
    build_exfat_spc2();
    CHECK_HEX("exfat spc=2 chained ok", 0, hype_exfat_resolve(vol_read, 0, "\\p.bin", &f));
    CHECK_HEX("exfat spc=2 size", 1536u, f.size_bytes);
    /* cl3(sec18,2) + cl4(sec20, partial 1) coalesce => one extent [18,3]. */
    CHECK_HEX("exfat spc=2 one extent", 1u, f.count);
    CHECK_HEX("exfat spc=2 start 18", 18ull, f.extents[0].start_lba);
    CHECK_HEX("exfat spc=2 3 sectors", 3ull, f.extents[0].sector_count);
}

static void test_exfat_scan_past_chain_end(void) {
    hype_fat_file_t f;
    build_exfat_bigdir(); /* cluster 2 -> 10 -> EOC, no 0x00 terminator */
    CHECK_HEX("exfat missing name runs off chain end", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\zzzzzzz", &f));
}

int main(void) {
    test_fat32_nested_file();
    test_fat32_bpb_guards();
    test_exfat_multicluster_dir();
    test_fat_chain_read_failures();
    test_over_fragmented();
    test_exfat_more_guards();
    test_exfat_spc2_partial_cluster();
    test_exfat_scan_past_chain_end();
    test_exfat_signature_bytes();
    test_exfat_corrupt_entry_sets();
    test_fat32_skip_and_edge_entries();
    test_fat32_read_failures();
    test_exfat_empty_and_dir();
    test_exfat_chained_multi_extent();
    test_exfat_read_failures();
    test_fat32_forward_slash_and_case();
    test_fat32_lfn_match();
    test_fat32_not_found_and_dir_as_file();
    test_fat32_bad_bpb();
    test_exfat_contiguous();
    test_exfat_bad();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
