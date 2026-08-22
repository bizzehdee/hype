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

#define EX_FAT_LBA 24u
#define EX_FAT_LEN 4u
#define EX_HEAP_LBA 32u
#define EX_CLUSTERS 360u

/*
 * #366: sized from HYPE_FILE_MAX_EXTENTS, not a fixed 400.
 *
 * The over-fragmented fixtures below must build a chain of MORE non-adjacent clusters than the cap
 * allows, so both the FAT region and the cluster heap have to scale with the cap. When the cap was
 * raised from 64 to 256 these fixtures still built a 66-cluster chain, which is now perfectly
 * mappable -- so the "too fragmented" tests passed vacuously by asserting that a legal file was
 * rejected. Deriving the geometry keeps them honest the next time the cap moves.
 */
#define FRAG_CLUSTERS (HYPE_FILE_MAX_EXTENTS + 2u)          /* two past the cap */
#define FRAG_FIRST_CL 10u
#define FRAG_LAST_CL  (FRAG_FIRST_CL + 2u * (FRAG_CLUSTERS - 1u))
#define VOL_SECTORS (EX_HEAP_LBA + FRAG_LAST_CL + 64u)
static uint8_t g_vol[VOL_SECTORS * HYPE_BLK_SECTOR_SIZE];

static uint64_t g_fail_lba = (uint64_t)-1; /* inject a read failure at this LBA */

/* #650: read-call accounting for the FAT-sector-cache tests below. g_fat_lba/
 * g_fat_len bound the region counted as "FAT reads" -- every fixture in this
 * file places its FAT at EX_FAT_LBA/EX_FAT_LEN, so the default matches; a
 * fixture with a different FAT region (there are none among the counting
 * tests) would update these before resolving. */
static unsigned int g_read_calls;
static uint64_t g_fat_lba = EX_FAT_LBA;
static uint32_t g_fat_len = EX_FAT_LEN;
static unsigned int g_fat_read_calls;
/* Fails exactly the Nth read of g_countdown_lba (0 == the very next one), then
 * lets every later read of it succeed -- a TRANSIENT failure, unlike the
 * permanent g_fail_lba above. */
static uint64_t g_countdown_lba = (uint64_t)-1;
static long g_fail_lba_countdown = -1;

static int vol_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if (count != 1u || lba >= VOL_SECTORS || lba == g_fail_lba) {
        return -1;
    }
    if (lba == g_countdown_lba && g_fail_lba_countdown >= 0 && g_fail_lba_countdown-- == 0) {
        return -1;
    }
    g_read_calls++;
    if (lba >= g_fat_lba && lba < g_fat_lba + g_fat_len) {
        g_fat_read_calls++;
    }
    memcpy(dst, g_vol + lba * HYPE_BLK_SECTOR_SIZE, HYPE_BLK_SECTOR_SIZE);
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
static uint8_t *fat_entry_ptr(uint32_t cl) { return g_vol + 32u * HYPE_BLK_SECTOR_SIZE + cl * 4u; }
static uint8_t *cluster_ptr(uint32_t cl) { return g_vol + (33u + (cl - 2u)) * HYPE_BLK_SECTOR_SIZE; }

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
    put32(fat_entry_ptr(6), 8u);          /* verylongname.iso: 6 -> 8, 1000B = 2 sectors */
    put32(fat_entry_ptr(8), 0x0FFFFFFFu);
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
    hype_file_map_t f;
    build_fat32();
    CHECK_HEX("resolve \\iso\\test.iso ok", 0, hype_fat32_resolve(vol_read, 0, "\\iso\\test.iso", &f));
    CHECK_HEX("size", 1000u, f.size_bytes);
    CHECK_HEX("one coalesced extent", 1u, f.count);
    /* clusters 4,5 => sectors 35,36; contiguous => start 35, 2 sectors (ceil 1000/512). */
    CHECK_HEX("extent start lba", 35ull, f.extents[0].start_lba);
    CHECK_HEX("extent sectors (trimmed)", 2ull, f.extents[0].sector_count);
}

static void test_fat32_forward_slash_and_case(void) {
    hype_file_map_t f;
    build_fat32();
    CHECK_HEX("resolve /ISO/TeSt.IsO ok", 0, hype_fat32_resolve(vol_read, 0, "/ISO/TeSt.IsO", &f));
    CHECK_HEX("size", 1000u, f.size_bytes);
}

static void test_fat32_lfn_match(void) {
    hype_file_map_t f;
    build_fat32();
    CHECK_HEX("resolve LFN \\verylongname.iso ok", 0,
              hype_fat32_resolve(vol_read, 0, "\\verylongname.iso", &f));
    CHECK_HEX("LFN file size", 1000u, f.size_bytes);
    /* clusters 6 then 8 -> sectors 37 and 39: not adjacent, so two extents. */
    CHECK_HEX("LFN two extents", 2u, f.count);
    CHECK_HEX("LFN cluster6 -> sector 37", 37ull, f.extents[0].start_lba);
    CHECK_HEX("LFN cluster8 -> sector 39", 39ull, f.extents[1].start_lba);
}

static void test_fat32_not_found_and_dir_as_file(void) {
    hype_file_map_t f;
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
    hype_file_map_t f;
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
 * spc=1 (shift 0). The first 24 sectors are the two boot regions, so:
 * FatOffset=24, FatLength=4 (512 FAT entries), ClusterHeapOffset=32,
 * ClusterCount=360, root=cluster2 (sector 32); cluster N => sector 32+(N-2).
 * root dir: File set for contiguous "test.iso" (cluster3, 700B, NoFatChain)
 *           File set for a directory "sub" (cluster4)
 *   sub/  : File set for chained "big.bin" (cluster5->6->8, 1200B). */

/*
 * The exFAT entry-set checksum, spelled out here straight from the spec's
 * EntrySetChecksum pseudocode rather than calling the implementation, so the
 * tests are checking the code against the spec and not against itself.
 */
static uint16_t exfat_set_checksum(const uint8_t *entries, unsigned secondary) {
    unsigned n = (secondary + 1u) * 32u, i;
    uint16_t s = 0;
    for (i = 0; i < n; i++) {
        if (i == 2u || i == 3u) continue;
        s = (uint16_t)(((s & 1u) ? 0x8000u : 0u) + (uint16_t)(s >> 1) + (uint16_t)entries[i]);
    }
    return s;
}

static void exfat_name_entry(uint8_t *e, const char *name, unsigned n) {
    unsigned i;
    memset(e, 0, 32);
    e[0] = 0xC1u;
    for (i = 0; i < n && i < 15u; i++) put16(e + 2u + i * 2u, (uint16_t)name[i]);
}
/* Recomputes and stores the set checksum of a `secondary`-secondary set. */
static void exfat_fix_checksum(uint8_t *base, unsigned secondary) {
    base[1] = (uint8_t)secondary;
    put16(base + 2, exfat_set_checksum(base, secondary));
}
static void exfat_file_set(uint8_t *base, const char *name, uint8_t is_dir, uint8_t no_fat_chain,
                           uint32_t first_cl, uint64_t data_len) {
    unsigned nlen = (unsigned)strlen(name);
    uint8_t *file = base, *stream = base + 32, *nm = base + 64;
    memset(file, 0, 96);
    file[0] = 0x85u;      /* File */
    put16(file + 4, is_dir ? 0x0010u : 0x0020u); /* attributes */
    stream[0] = 0xC0u;    /* Stream Extension */
    stream[1] = (uint8_t)(0x01u | (no_fat_chain ? 0x02u : 0x00u)); /* AllocPossible + NoFatChain */
    stream[3] = (uint8_t)nlen; /* NameLength */
    put32(stream + 0x14, first_cl);
    put64(stream + 0x18, data_len);
    exfat_name_entry(nm, name, nlen);
    exfat_fix_checksum(base, 2u); /* stream + 1 name entry */
}
static uint8_t *exfat_cluster(uint32_t cl) {
    return g_vol + (EX_HEAP_LBA + (cl - 2u)) * HYPE_BLK_SECTOR_SIZE;
}
static uint8_t *exfat_fat_entry(uint32_t cl) {
    return g_vol + EX_FAT_LBA * HYPE_BLK_SECTOR_SIZE + cl * 4u;
}
/* As exfat_cluster but for a volume with more than one sector per cluster. */
static uint8_t *exfat_cluster_spc(uint32_t cl, uint32_t spc) {
    return g_vol + (EX_HEAP_LBA + (cl - 2u) * spc) * HYPE_BLK_SECTOR_SIZE;
}

/* Fills in the boot-sector fields every exFAT volume must have. */
static void exfat_boot(uint8_t *b, uint32_t heap_lba, uint32_t clusters, uint8_t spc_shift) {
    unsigned i;
    b[3] = 'E'; b[4] = 'X'; b[5] = 'F'; b[6] = 'A'; b[7] = 'T';
    for (i = 8u; i < 11u; i++) b[i] = ' ';       /* "EXFAT   " is eight bytes */
    put64(b + 0x48, VOL_SECTORS);                /* VolumeLength */
    put32(b + 0x50, EX_FAT_LBA);                 /* FatOffset */
    put32(b + 0x54, EX_FAT_LEN);                 /* FatLength */
    put32(b + 0x58, heap_lba);                   /* ClusterHeapOffset */
    put32(b + 0x5C, clusters);                   /* ClusterCount */
    put32(b + 0x60, 2);                          /* FirstClusterOfRootDirectory */
    b[0x6C] = 9;                                 /* BytesPerSectorShift = 512 */
    b[0x6D] = spc_shift;                         /* SectorsPerClusterShift */
    b[0x6E] = 1;                                 /* NumberOfFats */
}

static void build_exfat(void) {
    memset(g_vol, 0, sizeof(g_vol));
    exfat_boot(g_vol, EX_HEAP_LBA, EX_CLUSTERS, 0u);

    /* root dir (cluster 2, sector 32). */
    exfat_file_set(exfat_cluster(2) + 0,   "test.iso", 0, 1, 3u, 700u); /* contiguous @cl3 */
    exfat_file_set(exfat_cluster(2) + 96,  "sub",      1, 1, 4u, 0u);   /* subdir @cl4 */
    exfat_file_set(exfat_cluster(2) + 192, "empty",    0, 1, 3u, 0u);   /* zero-length */
    /* #366: this slot must stay OCCUPIED. It used to hold the over-fragmented file, which now has
     * its own volume (build_exfat_frag). A 0x00 entry type is END OF DIRECTORY in exFAT, so
     * leaving the slot zeroed truncated the walk here and hid "cdir" at offset 384 -- which is
     * what build_exfat_contig_dir descends into. */
    exfat_file_set(exfat_cluster(2) + 288, "filler",   0, 1, 3u, 512u);

    /* sub dir (cluster 4, sector 34): chained big.bin @cl5->6->8 (5,6 adjacent so
     * they coalesce; 8 is non-contiguous so it opens a second extent). 1200B => 3 sectors. */
    exfat_file_set(exfat_cluster(4) + 0, "big.bin", 0, 0, 5u, 1200u);
    put32(exfat_fat_entry(5), 6u);
    put32(exfat_fat_entry(6), 8u);
    put32(exfat_fat_entry(8), 0xFFFFFFFFu);
}

static void test_exfat_contiguous(void) {
    hype_file_map_t f;
    build_exfat();
    CHECK_HEX("exfat resolve \\test.iso ok", 0, hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    CHECK_HEX("exfat size", 700u, f.size_bytes);
    CHECK_HEX("exfat one extent", 1u, f.count);
    CHECK_HEX("exfat cluster3 -> sector 33", 33ull, f.extents[0].start_lba);
    CHECK_HEX("exfat 2 sectors", 2ull, f.extents[0].sector_count);
}

static void test_exfat_bad(void) {
    hype_file_map_t f;
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
    hype_file_map_t f;
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
    hype_file_map_t f;
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
    hype_file_map_t f;
    build_exfat();
    CHECK_HEX("exfat empty file ok", 0, hype_exfat_resolve(vol_read, 0, "\\empty", &f));
    CHECK_HEX("exfat empty size 0", 0ull, f.size_bytes);
    CHECK_HEX("exfat empty 0 extents", 0u, f.count);
    build_exfat();
    CHECK_HEX("exfat dir-as-file rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\sub", &f));
}

static void test_exfat_chained_multi_extent(void) {
    hype_file_map_t f;
    build_exfat();
    CHECK_HEX("exfat chained big.bin ok", 0, hype_exfat_resolve(vol_read, 0, "\\sub\\big.bin", &f));
    CHECK_HEX("exfat 1200B size", 1200u, f.size_bytes);
    /* clusters 5,6 coalesce (sectors 35,36), 8 separate (sector 38) => 2 extents. */
    CHECK_HEX("exfat two extents", 2u, f.count);
    CHECK_HEX("exfat ext0 start", 35ull, f.extents[0].start_lba);
    CHECK_HEX("exfat ext0 sectors (coalesced)", 2ull, f.extents[0].sector_count);
    CHECK_HEX("exfat ext1 start", 38ull, f.extents[1].start_lba);
    CHECK_HEX("exfat ext1 sectors", 1ull, f.extents[1].sector_count);
}

static void test_exfat_read_failures(void) {
    hype_file_map_t f;
    build_exfat();
    g_fail_lba = 0; /* boot sector read fails */
    CHECK_HEX("exfat boot read failure", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    build_exfat();
    g_fail_lba = EX_HEAP_LBA; /* root dir read fails */
    CHECK_HEX("exfat dir read failure", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    g_fail_lba = (uint64_t)-1;
}

static void test_fat32_bpb_guards(void) {
    hype_file_map_t f;
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
    hype_file_map_t f;
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
    hype_file_map_t f;
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
    memset(exfat_cluster(2), 0, HYPE_BLK_SECTOR_SIZE);
    memset(exfat_cluster(10), 0, HYPE_BLK_SECTOR_SIZE);
    /* ei 0..14: unused (InUse bit clear) filler entries -- skipped, not end. */
    for (i = 0; i < 15u; i++) {
        exfat_cluster(2)[i * 32u] = 0x05u;
    }
    /* far.iso set straddles the boundary: File @ei15 (cluster 2), Stream @ei16
     * and Name @ei17 (cluster 10). Built in a contiguous scratch buffer so the
     * set checksum can be taken over the bytes as a whole, then split across the
     * two clusters -- the checksum must survive the boundary. */
    {
        static uint8_t set[96];
        uint8_t *file = set, *stream = set + 32, *nm = set + 64;
        memset(set, 0, sizeof set);
        file[0] = 0x85u; put16(file + 4, 0x0020u);
        stream[0] = 0xC0u; stream[1] = 0x03u; stream[3] = 7u;
        put32(stream + 0x14, 3u); put64(stream + 0x18, 700u);
        exfat_name_entry(nm, "far.iso", 7u);
        exfat_fix_checksum(set, 2u);
        memcpy(exfat_cluster(2) + 15u * 32u, file, 32);   /* ei15 */
        memcpy(exfat_cluster(10) + 0u, stream, 32);       /* ei16 */
        memcpy(exfat_cluster(10) + 32u, nm, 32);          /* ei17 */
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
    memset(g_vol, 0, sizeof(g_vol));
    /* 2 sectors/cluster, so only (VOL_SECTORS - heap)/2 clusters fit. */
    exfat_boot(g_vol, EX_HEAP_LBA, (VOL_SECTORS - EX_HEAP_LBA) / 2u, 1u);
    /* root dir @cluster2 (sectors 32,33): chained "p.bin" @cl3->4, 1536 bytes
     * (3 sectors). cl3=>sector 34, cl4=>sector 36; 3 sectors over 2 clusters
     * leaves a 1-sector (partial) final cluster. */
    exfat_file_set(exfat_cluster_spc(2, 2u) + 0, "p.bin", 0, 0, 3u, 1536u);
    put32(exfat_fat_entry(3), 4u);
    put32(exfat_fat_entry(4), 0xFFFFFFFFu);
}

static void test_exfat_multicluster_dir(void) {
    hype_file_map_t f;
    build_exfat_bigdir();
    CHECK_HEX("exfat cross-cluster file set ok", 0,
              hype_exfat_resolve(vol_read, 0, "\\far.iso", &f));
    CHECK_HEX("far.iso size", 700u, f.size_bytes);
    /* mid-set read failure: cluster 10's sector fails while reading the set. */
    build_exfat_bigdir();
    g_fail_lba = EX_HEAP_LBA + 8u;
    CHECK_HEX("exfat mid-set read failure => not found", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\far.iso", &f));
    g_fail_lba = (uint64_t)-1;
}

/*
 * Failing the FAT sector mid-chain makes the chain look like it ends early. That
 * must be an error, not a short extent list reported as success: a caller handed
 * fewer extents than the file's size needs would read whatever happens to follow
 * the data it did get.
 */
static void test_fat_chain_read_failures(void) {
    hype_file_map_t f;
    build_fat32();
    g_fail_lba = 32; /* the FAT sector */
    CHECK_HEX("fat32 chain read-fail rejected", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\frag.bin", &f));
    /* exFAT: same, on its FAT while following big.bin's chain. */
    build_exfat();
    g_fail_lba = EX_FAT_LBA;
    CHECK_HEX("exfat chain read-fail rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\sub\\big.bin", &f));
    g_fail_lba = (uint64_t)-1;
}

/*
 * Dedicated FAT32 volume whose FAT is long enough to hold the whole FRAG_CLUSTERS chain, one
 * sector per cluster. #366: the FAT length and the data start are DERIVED, because the chain
 * length now follows HYPE_FILE_MAX_EXTENTS -- a fixed 4-sector FAT held only clusters 0..511 and
 * silently truncated the chain once the cap passed 250.
 */
#define FRAG_FAT_SECTORS ((FRAG_LAST_CL + 1u) * 4u / HYPE_BLK_SECTOR_SIZE + 1u)
#define FRAG_RESERVED 32u
#define FRAG_DATA_LBA (FRAG_RESERVED + FRAG_FAT_SECTORS)

static void build_fat32_frag(void) {
    uint8_t *bpb = g_vol;
    uint32_t c;
    memset(g_vol, 0, sizeof(g_vol));
    put16(bpb + 0x0B, 512); bpb[0x0D] = 1; put16(bpb + 0x0E, (uint16_t)FRAG_RESERVED); bpb[0x10] = 1;
    put16(bpb + 0x16, 0); put32(bpb + 0x24, FRAG_FAT_SECTORS); put32(bpb + 0x2C, 2);
    put32(fat_entry_ptr(0), 0x0FFFFFF8u); put32(fat_entry_ptr(1), 0x0FFFFFFFu);
    put32(fat_entry_ptr(2), 0x0FFFFFFFu); /* root EOC */
    put_short_entry(g_vol + FRAG_DATA_LBA * HYPE_BLK_SECTOR_SIZE, "TOOFRAG BIN", 0x20u,
                    FRAG_FIRST_CL, FRAG_CLUSTERS * 512u);
    for (c = FRAG_FIRST_CL; c < FRAG_LAST_CL; c += 2u) {
        put32(fat_entry_ptr(c), c + 2u); /* non-adjacent throughout: every cluster opens an extent */
    }
    put32(fat_entry_ptr(FRAG_LAST_CL), 0x0FFFFFFFu);
}

/*
 * #366: a dedicated over-fragmented exFAT volume, mirroring build_fat32_frag.
 *
 * The chain has to be longer than HYPE_FILE_MAX_EXTENTS, and at 256 that no longer fits in the
 * shared build_exfat() volume's 4-sector FAT. Widening THAT volume moved cluster/sector numbers
 * a dozen unrelated assertions depend on, so the fragmented case gets its own geometry instead --
 * the same separation build_fat32_frag already uses, and for the same reason.
 */
#define EXFRAG_FAT_LBA EX_FAT_LBA                     /* 24 -- the conventional offset */
#define EXFRAG_FAT_LEN (EX_HEAP_LBA - EXFRAG_FAT_LBA)  /* 8 sectors = 2048 entries */

static uint8_t *exfrag_fat_entry(uint32_t cl) {
    return g_vol + EXFRAG_FAT_LBA * HYPE_BLK_SECTOR_SIZE + cl * 4u;
}

static void build_exfat_frag(void) {
    uint32_t c;
    memset(g_vol, 0, sizeof(g_vol));
    exfat_boot(g_vol, EX_HEAP_LBA, FRAG_LAST_CL + 8u, 0u);
    put32(g_vol + 0x50, EXFRAG_FAT_LBA); /* FatOffset  */
    put32(g_vol + 0x54, EXFRAG_FAT_LEN); /* FatLength  */

    exfat_file_set(exfat_cluster(2) + 0, "toofrag", 0, 0, FRAG_FIRST_CL, FRAG_CLUSTERS * 512u);
    for (c = FRAG_FIRST_CL; c < FRAG_LAST_CL; c += 2u) {
        put32(exfrag_fat_entry(c), c + 2u); /* non-adjacent throughout */
    }
    put32(exfrag_fat_entry(FRAG_LAST_CL), 0xFFFFFFFFu);
}

static void test_over_fragmented(void) {
    hype_file_map_t f;
    build_fat32_frag();
    CHECK_HEX("fat32 past the extent cap rejected", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\toofrag.bin", &f));
    build_exfat_frag();
    CHECK_HEX("exfat past the extent cap rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\toofrag", &f));
}

/*
 * #366: "too fragmented to map" must be distinguishable from every other resolve failure.
 *
 * They all returned -1, so the caller could not tell it from "no such file" or "not a FAT32
 * volume" -- and boot/main.c's diagnostic for the fragmentation case sat behind a branch that
 * only runs when resolve SUCCEEDS, making it unreachable for the one failure it described. The
 * operator saw nothing at all.
 */
static void test_too_fragmented_is_distinguishable_from_other_failures(void) {
    hype_file_map_t f;

    build_fat32_frag();
    CHECK_HEX("fat32 over-fragmented still returns -1", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\toofrag.bin", &f));
    CHECK_HEX("and says WHY", 1u, (unsigned)f.too_fragmented);

    /* The distinction that matters: a different failure must NOT claim fragmentation, or the
     * operator is told to defragment a stick that simply does not have the file on it. */
    CHECK_HEX("a missing file still returns -1", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\nosuch.bin", &f));
    CHECK_HEX("and does not claim fragmentation", 0u, (unsigned)f.too_fragmented);

    build_exfat_frag();
    CHECK_HEX("exfat over-fragmented still returns -1", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\toofrag", &f));
    CHECK_HEX("exfat says WHY too", 1u, (unsigned)f.too_fragmented);
}

/* The flag must not survive into a later resolve: the loader reuses one struct across media. */
static void test_too_fragmented_is_cleared_by_a_later_success(void) {
    hype_file_map_t f;
    build_fat32_frag();
    (void)hype_fat32_resolve(vol_read, 0, "\\toofrag.bin", &f);
    CHECK_HEX("flag set by the failure", 1u, (unsigned)f.too_fragmented);
    build_fat32();
    CHECK_HEX("a later resolve succeeds", 0,
              hype_fat32_resolve(vol_read, 0, "\\iso\\test.iso", &f));
    CHECK_HEX("and the stale reason is gone", 0u, (unsigned)f.too_fragmented);
}

/*
 * #366: the reason must survive the NEXT resolver in the chain.
 *
 * boot/main.c tries FAT32, then exFAT, then ext against the same struct. Every resolver clears
 * too_fragmented at entry, so a FAT32 "too fragmented" verdict was wiped by the exFAT attempt that
 * followed it -- and the diagnostic, by then reading a cleared flag, printed nothing. That is the
 * silence #366 was filed about, and it SURVIVED the change meant to end it: a deliberately
 * fragmented 134-extent volume still produced no message. This pins the mechanism at the level the
 * loader actually uses it.
 */
static void test_a_later_resolver_erases_the_fragmentation_reason(void) {
    hype_file_map_t f;

    build_fat32_frag();
    CHECK_HEX("fat32 reports fragmentation", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_resolve(vol_read, 0, "\\toofrag.bin", &f));
    CHECK_HEX("...and sets the flag", 1u, (unsigned)f.too_fragmented);

    /* The next resolver in boot/main.c's chain, against the same struct and the same volume. It
     * cannot read this volume at all, so it fails for an unrelated reason -- and clears the flag. */
    CHECK_HEX("exfat cannot read a FAT32 volume", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\toofrag.bin", &f));
    CHECK_HEX("and the reason is GONE -- callers must not read it after the chain", 0u,
              (unsigned)f.too_fragmented);
}

static void test_exfat_more_guards(void) {
    hype_file_map_t f;
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
    hype_file_map_t f;
    build_exfat_spc2();
    CHECK_HEX("exfat spc=2 chained ok", 0, hype_exfat_resolve(vol_read, 0, "\\p.bin", &f));
    CHECK_HEX("exfat spc=2 size", 1536u, f.size_bytes);
    /* cl3(sec18,2) + cl4(sec20, partial 1) coalesce => one extent [18,3]. */
    CHECK_HEX("exfat spc=2 one extent", 1u, f.count);
    CHECK_HEX("exfat spc=2 start 34", 34ull, f.extents[0].start_lba);
    CHECK_HEX("exfat spc=2 3 sectors", 3ull, f.extents[0].sector_count);
}

static void test_exfat_scan_past_chain_end(void) {
    hype_file_map_t f;
    build_exfat_bigdir(); /* cluster 2 -> 10 -> EOC, no 0x00 terminator */
    CHECK_HEX("exfat missing name runs off chain end", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\zzzzzzz", &f));
}

/* Patches a 32-bit Stream Extension field of an already-built set and restores
 * the set checksum, so only the field under test is out of spec. */
static void exfat_patch_stream32(uint8_t *base, unsigned off, uint32_t v) {
    put32(base + 32u + off, v);
    exfat_fix_checksum(base, base[1]);
}

/*
 * A subdirectory whose own allocation is NoFatChain: its clusters are
 * consecutive and the FAT holds NOTHING for them. Walking the FAT there follows
 * zeroes straight off the end of the directory, so the flag has to be honoured
 * when addressing a directory's second and later clusters -- the single-cluster
 * directories in the other fixtures never reach that code.
 */
static void build_exfat_contig_dir(void) {
    unsigned i;
    build_exfat();
    /* "cdir": contiguous 2-cluster directory at clusters 20,21 (sectors 50,51). */
    exfat_file_set(exfat_cluster(2) + 384, "cdir", 1, 1, 20u, 1024u);
    memset(exfat_cluster(20), 0, HYPE_BLK_SECTOR_SIZE);
    memset(exfat_cluster(21), 0, HYPE_BLK_SECTOR_SIZE);
    /* Deliberately leave FAT[20] and FAT[21] at zero, exactly as a real
     * NoFatChain allocation does. */
    for (i = 0; i < 16u; i++) {
        exfat_cluster(20)[i * 32u] = 0x05u; /* not-in-use filler: keep scanning */
    }
    /* deep.bin lives at entry index 16 -- the first entry of the SECOND cluster. */
    exfat_file_set(exfat_cluster(21) + 0, "deep.bin", 0, 1, 30u, 700u);
}

static void test_exfat_contiguous_directory(void) {
    hype_file_map_t f;
    build_exfat_contig_dir();
    CHECK_HEX("exfat NoFatChain dir descent ok", 0,
              hype_exfat_resolve(vol_read, 0, "\\cdir\\deep.bin", &f));
    CHECK_HEX("deep.bin size", 700u, f.size_bytes);
    CHECK_HEX("deep.bin one extent", 1u, f.count);
    CHECK_HEX("deep.bin cluster30 -> sector 60", 60ull, f.extents[0].start_lba);
    /* Trailing separator still means "last component". */
    build_exfat_contig_dir();
    CHECK_HEX("exfat trailing separator names a dir, rejected as a file",
              (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\cdir\\", &f));
}

/* The entry-set checksum must actually be enforced: a set whose bytes changed
 * without its checksum being updated is corrupt and must not be acted on. */
static void test_exfat_set_checksum_enforced(void) {
    hype_file_map_t f;
    build_exfat();
    CHECK_HEX("baseline resolves", 0, hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    /* Flip a byte of the Stream entry, leaving the stored checksum stale. */
    build_exfat();
    exfat_cluster(2)[32u + 0x14u] ^= 0x04u; /* first cluster, checksum NOT refreshed */
    CHECK_HEX("stale set checksum rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    /* Flip a byte of a File Name entry the same way. */
    build_exfat();
    exfat_cluster(2)[64u + 4u] ^= 0x20u;
    CHECK_HEX("stale name checksum rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    /* A set with SecondaryCount 0 has no Stream entry at all. */
    build_exfat();
    exfat_fix_checksum(exfat_cluster(2), 0u);
    CHECK_HEX("SecondaryCount 0 rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    /* A deleted set (InUse bit clear) is not a name. */
    build_exfat();
    exfat_cluster(2)[0] = 0x05u;
    CHECK_HEX("deleted set skipped", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
}

/* NameLength has to agree with the File Name entries actually present, or the
 * comparison would run past the characters the set really carries. */
static void test_exfat_name_length_guard(void) {
    hype_file_map_t f;
    build_exfat();
    exfat_cluster(2)[32u + 3u] = 30u; /* claims 30 chars; one name entry holds 15 */
    exfat_fix_checksum(exfat_cluster(2), 2u);
    CHECK_HEX("over-long NameLength rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    build_exfat();
    exfat_cluster(2)[32u + 3u] = 0u; /* a zero-length name is not a name */
    exfat_fix_checksum(exfat_cluster(2), 2u);
    CHECK_HEX("zero NameLength rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
}

/* An allocation named by a directory entry must lie inside the cluster heap. */
static void test_exfat_allocation_range(void) {
    hype_file_map_t f;
    build_exfat();
    exfat_patch_stream32(exfat_cluster(2), 0x14u, EX_CLUSTERS + 2u); /* one past the last */
    CHECK_HEX("first cluster past the heap rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    build_exfat();
    exfat_patch_stream32(exfat_cluster(2), 0x14u, 1u); /* clusters start at 2 */
    CHECK_HEX("first cluster below 2 rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    /* Contiguous run that starts inside the heap but ends past its last cluster. */
    build_exfat();
    exfat_patch_stream32(exfat_cluster(2), 0x14u, EX_CLUSTERS + 1u); /* last cluster, needs 2 */
    CHECK_HEX("contiguous run past the heap rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    /* A non-zero DataLength with no allocation at all. */
    build_exfat();
    exfat_patch_stream32(exfat_cluster(2), 0x14u, 0u);
    CHECK_HEX("zero first cluster with data rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
}

/* A chain that ends before DataLength says it should is an error, not a short
 * extent list handed back as success. */
static void test_exfat_short_chain(void) {
    hype_file_map_t f;
    build_exfat();
    put32(exfat_fat_entry(5), 0xFFFFFFFFu); /* big.bin (1200B, 3 sectors) now 1 cluster */
    CHECK_HEX("chain shorter than DataLength rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\sub\\big.bin", &f));
}

/* With two FATs, VolumeFlags bit 0 selects the live copy. Reading the stale one
 * follows chains that no longer exist, so the bit has to be honoured. */
static void test_exfat_active_fat(void) {
    hype_file_map_t f;
    unsigned s;
    build_exfat();
    g_vol[0x6E] = 2; /* NumberOfFats */
    /* Move every FAT sector to the second copy and blank the first. */
    for (s = 0; s < EX_FAT_LEN; s++) {
        memcpy(g_vol + (EX_FAT_LBA + EX_FAT_LEN + s) * HYPE_BLK_SECTOR_SIZE,
               g_vol + (EX_FAT_LBA + s) * HYPE_BLK_SECTOR_SIZE, HYPE_BLK_SECTOR_SIZE);
        memset(g_vol + (EX_FAT_LBA + s) * HYPE_BLK_SECTOR_SIZE, 0, HYPE_BLK_SECTOR_SIZE);
    }
    put16(g_vol + 0x6A, 0x0001u); /* ActiveFat = 1 */
    CHECK_HEX("chain followed through the ACTIVE (second) FAT", 0,
              hype_exfat_resolve(vol_read, 0, "\\sub\\big.bin", &f));
    CHECK_HEX("active-FAT big.bin size", 1200u, f.size_bytes);
    put16(g_vol + 0x6A, 0x0000u); /* now point at the blanked first FAT */
    CHECK_HEX("stale FAT gives no chain", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\sub\\big.bin", &f));
    /* NumberOfFats must be 1 or 2. */
    build_exfat();
    g_vol[0x6E] = 3;
    CHECK_HEX("NumberOfFats 3 rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    build_exfat();
    g_vol[0x6E] = 0;
    CHECK_HEX("NumberOfFats 0 rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
}

/* Boot-sector geometry that would make a structure overlap another, or run off
 * the volume, must be refused before any of it is believed. */
static void test_exfat_geometry_guards(void) {
    hype_file_map_t f;
    struct { const char *desc; unsigned off; int width; uint32_t val; } cases[] = {
        {"FatOffset inside the boot regions", 0x50, 32, 8u},
        {"FatLength 0", 0x54, 32, 0u},
        {"heap overlapping the FAT", 0x58, 32, EX_FAT_LBA + EX_FAT_LEN - 1u},
        {"ClusterCount 0", 0x5C, 32, 0u},
        {"ClusterCount beyond the FAT's reach", 0x5C, 32, EX_FAT_LEN * 128u},
        {"heap running past the volume", 0x5C, 32, VOL_SECTORS},
    };
    unsigned i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        build_exfat();
        put32(g_vol + cases[i].off, cases[i].val);
        CHECK_HEX(cases[i].desc, (unsigned long long)(-1),
                  (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    }
    /* The signature is eight bytes: the three trailing spaces count too. */
    for (i = 8u; i <= 10u; i++) {
        build_exfat();
        g_vol[i] = 'X';
        CHECK_HEX("exfat signature padding mismatch", (unsigned long long)(-1),
                  (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    }
    /* A root cluster outside the heap. */
    build_exfat();
    put32(g_vol + 0x60, EX_CLUSTERS + 2u);
    CHECK_HEX("root cluster past the heap rejected", (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\test.iso", &f));
    /* A path component longer than the resolver's buffer. */
    {
        char longpath[200];
        memset(longpath, 'a', sizeof longpath);
        longpath[0] = '\\';
        longpath[sizeof longpath - 1] = '\0';
        build_exfat();
        CHECK_HEX("over-long path component rejected", (unsigned long long)(-1),
                  (unsigned long long)hype_exfat_resolve(vol_read, 0, longpath, &f));
    }
}

/*
 * #650: a FAT-chained file long enough that an uncached walk would issue one FAT
 * read per cluster. Clusters 3..302 (300 of them) chain in order, so their FAT
 * entries (byte cl*4, EX_FAT_LEN==4 sectors == 512 entries) span exactly the
 * first 3 FAT sectors -- matching the ticket's "FAT entries live in 3 sectors".
 */
#define LONGCHAIN_FIRST_CL 3u
#define LONGCHAIN_CLUSTERS 300u
#define LONGCHAIN_LAST_CL (LONGCHAIN_FIRST_CL + LONGCHAIN_CLUSTERS - 1u)

static void build_exfat_longchain(void) {
    uint32_t c;
    memset(g_vol, 0, sizeof(g_vol));
    exfat_boot(g_vol, EX_HEAP_LBA, EX_CLUSTERS, 0u);
    exfat_file_set(exfat_cluster(2) + 0, "longchain", 0, 0, LONGCHAIN_FIRST_CL,
                   (uint64_t)LONGCHAIN_CLUSTERS * HYPE_BLK_SECTOR_SIZE);
    for (c = LONGCHAIN_FIRST_CL; c < LONGCHAIN_LAST_CL; c++) {
        put32(exfat_fat_entry(c), c + 1u);
    }
    put32(exfat_fat_entry(LONGCHAIN_LAST_CL), 0xFFFFFFFFu);
}

/*
 * #650: the FAT-sector-read count for resolving a 300-cluster chain must be
 * bounded by the number of DISTINCT FAT sectors the chain touches (3, here),
 * not by the cluster count -- the whole point of exfat_next_cached(). The
 * uncached implementation this replaces would have issued one FAT read per
 * cluster (300), so any bound comfortably under that proves the cache is
 * doing its job; single digits is the number the ticket asks for.
 */
static void test_exfat_fat_chain_cache_bounds_reads(void) {
    hype_file_map_t f;

    build_exfat_longchain();
    g_fat_lba = EX_FAT_LBA;
    g_fat_len = EX_FAT_LEN;
    g_fat_read_calls = 0u;
    g_read_calls = 0u;
    CHECK_HEX("longchain resolves", 0, hype_exfat_resolve(vol_read, 0, "\\longchain", &f));
    CHECK_HEX("longchain size", (unsigned long long)LONGCHAIN_CLUSTERS * HYPE_BLK_SECTOR_SIZE,
              f.size_bytes);
    /* The chain is physically contiguous (cluster N+1 always follows N on
     * disk), so the extent builder coalesces it into exactly one extent --
     * this is the correctness check the cached and uncached walks must agree
     * on ("byte-identical to the uncached result"), computed here from the
     * fixture's own geometry rather than re-run through a second code path. */
    CHECK_HEX("longchain coalesces to one extent", 1u, f.count);
    CHECK_HEX("longchain extent start", EX_HEAP_LBA + (LONGCHAIN_FIRST_CL - 2u),
              f.extents[0].start_lba);
    CHECK_HEX("longchain extent sectors", LONGCHAIN_CLUSTERS, f.extents[0].sector_count);
    CHECK_HEX("FAT reads bounded by distinct sectors touched, not cluster count", 1u,
              (unsigned)(g_fat_read_calls <= 9u));
}

/*
 * #650: a transient FAT-sector read failure partway through the walk must fail
 * the whole resolve outright (never a short/garbled extent map reported as
 * success), AND must not leave anything behind that corrupts a later, healthy
 * resolve of the same chain -- the cache is a stack-local struct created fresh
 * by every call, so there is nothing TO leave behind, but this is the
 * black-box proof of that from the exported API.
 */
static void test_exfat_fat_chain_cache_failure_not_stale(void) {
    hype_file_map_t f;

    build_exfat_longchain();
    g_countdown_lba = EX_FAT_LBA + 2u; /* the third FAT sector: reached partway through the walk */
    g_fail_lba_countdown = 0;          /* fail the very first read of it */
    CHECK_HEX("a transient FAT-sector failure fails the whole resolve",
              (unsigned long long)(-1),
              (unsigned long long)hype_exfat_resolve(vol_read, 0, "\\longchain", &f));
    g_countdown_lba = (uint64_t)-1;
    g_fail_lba_countdown = -1;

    /* Same fixture, same chain, no injected failure this time: must succeed
     * with correct data. */
    CHECK_HEX("a later healthy resolve of the same chain is unaffected", 0,
              hype_exfat_resolve(vol_read, 0, "\\longchain", &f));
    CHECK_HEX("recovered size correct",
              (unsigned long long)LONGCHAIN_CLUSTERS * HYPE_BLK_SECTOR_SIZE, f.size_bytes);
    CHECK_HEX("recovered extent count correct", 1u, f.count);
}

int main(void) {
    test_exfat_fat_chain_cache_bounds_reads();
    test_exfat_fat_chain_cache_failure_not_stale();
    test_exfat_contiguous_directory();
    test_exfat_set_checksum_enforced();
    test_exfat_name_length_guard();
    test_exfat_allocation_range();
    test_exfat_short_chain();
    test_exfat_active_fat();
    test_exfat_geometry_guards();
    test_fat32_nested_file();
    test_fat32_bpb_guards();
    test_exfat_multicluster_dir();
    test_fat_chain_read_failures();
    test_over_fragmented();
    test_too_fragmented_is_distinguishable_from_other_failures();
    test_too_fragmented_is_cleared_by_a_later_success();
    test_a_later_resolver_erases_the_fragmentation_reason();
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
