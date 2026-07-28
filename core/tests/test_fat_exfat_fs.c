#include <stdio.h>
#include <string.h>
#include "../fat_exfat_fs.h"
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

/*
 * ---- Synthetic exFAT volume in RAM ----
 *
 * Shaped like a real one: 24 reserved boot-region sectors (main at 0, backup at
 * 12), then one FAT, then the cluster heap. Cluster 2 holds the allocation
 * bitmap, cluster 3 the up-case table, cluster 4 the root directory -- the same
 * ordering mkfs.exfat produces. 512-byte clusters (spc == 1) so cluster and
 * sector boundaries coincide and are easy to reason about in assertions.
 */
#define SECSZ 512u
/* Sized for the larger fixture below (build_vol_big), whose bitmap spans more
 * than one sector -- the only way to reach the wrap-around and partial-last-
 * sector legs of the allocator from a unit test. */
#define VOL_SECTORS 5200u
#define FAT_LBA 24u
#define FAT_LEN 4u /* 512 FAT entries */
#define HEAP_LBA 32u
#define CLUSTERS 500u
#define BITMAP_CL 2u
#define UPCASE_CL 3u
#define ROOT_CL 4u
#define BACKUP_BOOT_LBA 12u

/* The larger fixture: 5000 clusters need two bitmap sectors (625 bytes), so the
 * bitmap occupies clusters 2 and 3, the up-case table cluster 4 and the root
 * cluster 5. 5002 FAT entries need ceil(5002/128) == 40 FAT sectors. */
#define BIG_FAT_LEN 40u
#define BIG_HEAP_LBA (FAT_LBA + BIG_FAT_LEN) /* 64 */
#define BIG_CLUSTERS 5000u
#define BIG_UPCASE_CL 4u
#define BIG_ROOT_CL 5u

static uint8_t g_vol[VOL_SECTORS * SECSZ];

/* The layout the helpers below address, set by whichever builder ran last. */
static uint32_t g_fat_len = FAT_LEN;
static uint32_t g_heap = HEAP_LBA;
static uint32_t g_clusters = CLUSTERS;
static uint32_t g_root = ROOT_CL;
static uint32_t g_bitmap_cl = BITMAP_CL;
static uint32_t g_upcase_cl = UPCASE_CL;
static uint32_t g_vol_len = VOL_SECTORS;
static uint64_t g_fail_read_lba = (uint64_t)-1;
static uint64_t g_fail_write_lba = (uint64_t)-1;
static long g_read_countdown = -1;
static long g_write_countdown = -1;

static int vol_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if (lba + count > VOL_SECTORS) return -1;
    if (lba == g_fail_read_lba) return -1;
    if (g_read_countdown >= 0 && g_read_countdown-- == 0) return -1;
    memcpy(dst, g_vol + lba * SECSZ, (size_t)count * SECSZ);
    return 0;
}
static int vol_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    if (lba + count > VOL_SECTORS) return -1;
    if (lba == g_fail_write_lba) return -1;
    if (g_write_countdown >= 0 && g_write_countdown-- == 0) return -1;
    memcpy(g_vol + lba * SECSZ, src, (size_t)count * SECSZ);
    return 0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

static uint64_t clba(uint32_t cl) { return g_heap + (cl - 2u); } /* spc == 1 */
static uint8_t *cluster(uint32_t cl) { return g_vol + clba(cl) * SECSZ; }
static uint8_t *fat_ent(uint32_t cl) { return g_vol + FAT_LBA * SECSZ + cl * 4u; }
static uint32_t fat_get(uint32_t cl) { return get32(fat_ent(cl)); }
/* The bitmap is contiguous, so bit N lives at a plain byte offset from its start. */
static uint8_t *bitmap_byte(uint32_t cl) {
    uint32_t b = cl - 2u;
    return g_vol + clba(g_bitmap_cl) * SECSZ + b / 8u;
}
static int bit_used(uint32_t cl) { return (*bitmap_byte(cl) & (1u << ((cl - 2u) % 8u))) ? 1 : 0; }
static void bit_mark(uint32_t cl, int used) {
    uint8_t mask = (uint8_t)(1u << ((cl - 2u) % 8u));
    if (used) {
        *bitmap_byte(cl) |= mask;
    } else {
        *bitmap_byte(cl) &= (uint8_t)~mask;
    }
}
static unsigned used_count(void) {
    unsigned n = 0, i;
    for (i = 0; i < g_clusters; i++) {
        if (bit_used(2u + i)) n++;
    }
    return n;
}

/* Spec pseudocode, written out here so the tests judge the implementation
 * against the specification and not against itself. */
static uint16_t ref_set_checksum(const uint8_t *bytes, unsigned n) {
    uint16_t s = 0;
    unsigned i;
    for (i = 0; i < n; i++) {
        if (i == 2u || i == 3u) continue;
        s = (uint16_t)(((s & 1u) ? 0x8000u : 0u) + (uint16_t)(s >> 1) + (uint16_t)bytes[i]);
    }
    return s;
}
static uint16_t ref_name_hash_ascii(const char *name) {
    uint16_t h = 0;
    unsigned i;
    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        uint16_t u = (uint16_t)((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
        h = (uint16_t)(((h & 1u) ? 0x8000u : 0u) + (uint16_t)(h >> 1) + (uint16_t)(u & 0xFFu));
        h = (uint16_t)(((h & 1u) ? 0x8000u : 0u) + (uint16_t)(h >> 1) + (uint16_t)(u >> 8));
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

/* The reference-shaped compressed up-case table (see test_fat_exfat.c). */
static unsigned build_upcase_table(uint8_t *out) {
    unsigned n = 0, c;
#define W(v) do { put16(out + n, (uint16_t)(v)); n += 2u; } while (0)
    W(0xFFFF); W(0x61);
    for (c = 0x61u; c <= 0x7Au; c++) W(c - 0x20u);
    W(0xFFFF); W(0x65);
    for (c = 0xE0u; c <= 0xFEu; c++) W((c == 0xF7u) ? c : (c - 0x20u));
    W(0x0178);
    W(0xFFFF);
#undef W
    return n;
}

static unsigned g_upcase_len;

static void boot_sector(uint8_t *b, uint8_t spc_shift, uint8_t nfats) {
    unsigned i;
    memset(b, 0, SECSZ);
    b[3] = 'E'; b[4] = 'X'; b[5] = 'F'; b[6] = 'A'; b[7] = 'T';
    for (i = 8u; i < 11u; i++) b[i] = ' ';
    put64(b + 0x48, g_vol_len);
    put32(b + 0x50, FAT_LBA);
    put32(b + 0x54, g_fat_len);
    put32(b + 0x58, g_heap);
    put32(b + 0x5C, g_clusters);
    put32(b + 0x60, g_root);
    b[0x6C] = 9;
    b[0x6D] = spc_shift;
    b[0x6E] = nfats;
    put16(b + 0x1FE, 0xAA55u);
}

/*
 * Lays out a volume against the current g_* layout: the FAT, an allocation
 * bitmap spanning as many clusters as it needs, the up-case table, and a root
 * directory carrying the volume label plus the bitmap and up-case entries.
 */
static void build_layout(void) {
    uint8_t table[512];
    uint64_t bitmap_bytes = (g_clusters + 7u) / 8u;
    uint32_t bitmap_clusters = (uint32_t)((bitmap_bytes + SECSZ - 1u) / SECSZ);
    uint32_t c;

    memset(g_vol, 0, sizeof g_vol);
    g_upcase_len = build_upcase_table(table);

    boot_sector(g_vol, 0u, 1u);
    boot_sector(g_vol + BACKUP_BOOT_LBA * SECSZ, 0u, 1u);

    put32(fat_ent(0), 0xFFFFFFF8u);
    put32(fat_ent(1), 0xFFFFFFFFu);
    /* The bitmap's clusters are consecutive and FAT-chained (it has no
     * NoFatChain flag of its own). */
    for (c = 0; c < bitmap_clusters; c++) {
        put32(fat_ent(g_bitmap_cl + c),
              (c + 1u < bitmap_clusters) ? (g_bitmap_cl + c + 1u) : 0xFFFFFFFFu);
    }
    put32(fat_ent(g_upcase_cl), 0xFFFFFFFFu);
    put32(fat_ent(g_root), 0xFFFFFFFFu);

    for (c = 0; c < bitmap_clusters; c++) bit_mark(g_bitmap_cl + c, 1);
    bit_mark(g_upcase_cl, 1);
    bit_mark(g_root, 1);

    memcpy(cluster(g_upcase_cl), table, g_upcase_len);
    {
        uint8_t *root = cluster(g_root);
        uint8_t *label = root;
        uint8_t *bmp = root + 32;
        uint8_t *upc = root + 64;
        label[0] = HYPE_EXFAT_ENT_LABEL;
        label[1] = 4u;
        put16(label + 2, 'T'); put16(label + 4, 'E'); put16(label + 6, 'S'); put16(label + 8, 'T');
        bmp[0] = HYPE_EXFAT_ENT_BITMAP;
        put32(bmp + 20, g_bitmap_cl);
        put64(bmp + 24, bitmap_bytes);
        upc[0] = HYPE_EXFAT_ENT_UPCASE;
        put32(upc + 4, ref_table_checksum(table, g_upcase_len));
        put32(upc + 20, g_upcase_cl);
        put64(upc + 24, g_upcase_len);
    }
}

static void build_vol(void) {
    g_fat_len = FAT_LEN;
    g_heap = HEAP_LBA;
    g_clusters = CLUSTERS;
    g_root = ROOT_CL;
    g_bitmap_cl = BITMAP_CL;
    g_upcase_cl = UPCASE_CL;
    g_vol_len = VOL_SECTORS;
    build_layout();
}

/* 5000 clusters: the allocation bitmap spans two sectors. */
static void build_vol_big(void) {
    g_fat_len = BIG_FAT_LEN;
    g_heap = BIG_HEAP_LBA;
    g_clusters = BIG_CLUSTERS;
    g_root = BIG_ROOT_CL;
    g_bitmap_cl = BITMAP_CL;
    g_upcase_cl = BIG_UPCASE_CL;
    g_vol_len = BIG_HEAP_LBA + BIG_CLUSTERS;
    build_layout();
}

/* Reads back the on-disk entry set at `ei` of the root directory and checks its
 * stored checksum, name and stream fields. */
static void verify_set(const char *what, uint32_t ei, const char *name, uint32_t first_cl,
                       uint64_t size, int contiguous) {
    uint8_t *root = cluster(ROOT_CL);
    uint8_t *file = root + ei * 32u;
    uint8_t *stream = file + 32u;
    unsigned nlen = (unsigned)strlen(name);
    unsigned entries = 2u + (nlen + 14u) / 15u;
    unsigned i;
    char desc[160];

    snprintf(desc, sizeof desc, "%s: File entry type", what);
    CHECK_HEX(desc, HYPE_EXFAT_ENT_FILE, file[0]);
    snprintf(desc, sizeof desc, "%s: SecondaryCount", what);
    CHECK_HEX(desc, entries - 1u, file[1]);
    snprintf(desc, sizeof desc, "%s: set checksum", what);
    CHECK_HEX(desc, ref_set_checksum(file, entries * 32u), get16(file + 2));
    snprintf(desc, sizeof desc, "%s: Stream entry type", what);
    CHECK_HEX(desc, HYPE_EXFAT_ENT_STREAM, stream[0]);
    snprintf(desc, sizeof desc, "%s: NoFatChain", what);
    CHECK_HEX(desc, contiguous ? 0x03u : 0x01u, stream[1]);
    snprintf(desc, sizeof desc, "%s: NameLength", what);
    CHECK_HEX(desc, nlen, stream[3]);
    snprintf(desc, sizeof desc, "%s: NameHash", what);
    CHECK_HEX(desc, ref_name_hash_ascii(name), get16(stream + 4));
    snprintf(desc, sizeof desc, "%s: ValidDataLength", what);
    CHECK_HEX(desc, size, get64(stream + 8));
    snprintf(desc, sizeof desc, "%s: FirstCluster", what);
    CHECK_HEX(desc, first_cl, get32(stream + 20));
    snprintf(desc, sizeof desc, "%s: DataLength", what);
    CHECK_HEX(desc, size, get64(stream + 24));
    for (i = 0; i < nlen; i++) {
        uint8_t *ne = file + (2u + i / 15u) * 32u;
        if (ne[0] != HYPE_EXFAT_ENT_NAME || get16(ne + 2u + (i % 15u) * 2u) != (uint16_t)name[i]) {
            snprintf(desc, sizeof desc, "%s: name char %u", what, i);
            CHECK(desc, 0);
            break;
        }
    }
}

/* Gathers a file's data by following the on-disk FAT chain, test-side. */
static unsigned gather(uint32_t first, int contiguous, uint8_t *buf, unsigned max) {
    uint32_t cl = first;
    unsigned n = 0, guard = 0;
    while (cl >= 2u && cl < 0xFFFFFFF8u && n < max && guard++ < 600u) {
        unsigned k;
        for (k = 0; k < SECSZ && n < max; k++) buf[n++] = cluster(cl)[k];
        cl = contiguous ? (cl + 1u) : fat_get(cl);
    }
    return n;
}

static uint8_t pat(unsigned i) { return (uint8_t)(i * 31u + 7u); }

static hype_exfat_fs_t g_fs;

/* ---- mount ---- */

static void test_mount(void) {
    build_vol();
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("spc", 1u, g_fs.spc);
    CHECK_HEX("fat lba", FAT_LBA, g_fs.fat_lba);
    CHECK_HEX("fat length", FAT_LEN, g_fs.fat_length);
    CHECK_HEX("heap lba", HEAP_LBA, g_fs.heap_lba);
    CHECK_HEX("cluster count", CLUSTERS, g_fs.cluster_count);
    CHECK_HEX("root cluster", ROOT_CL, g_fs.root_cluster);
    CHECK_HEX("volume length", VOL_SECTORS, g_fs.volume_length);
    CHECK_HEX("bitmap lba", clba(BITMAP_CL), g_fs.bitmap_lba);
    CHECK_HEX("bitmap bytes", (CLUSTERS + 7u) / 8u, g_fs.bitmap_bytes);
    CHECK_HEX("upcase cluster", UPCASE_CL, g_fs.upcase_cluster);
    CHECK_HEX("upcase bytes", g_upcase_len, g_fs.upcase_bytes);
    CHECK_HEX("used clusters counted from the bitmap", 3u, g_fs.used_clusters);
    CHECK_HEX("volume starts clean", 0u, g_fs.dirty);
    /* The up-case table really was decoded, not just checksummed. */
    CHECK_HEX("upcase 'a'", 'A', hype_exfat_upcase(&g_fs.upcase, 'a'));
    CHECK_HEX("upcase 0xE0", 0xC0u, hype_exfat_upcase(&g_fs.upcase, 0xE0u));
    CHECK_HEX("upcase 0xFF", 0x0178u, hype_exfat_upcase(&g_fs.upcase, 0xFFu));

    /* A read-only mount (no write callback) is allowed. */
    build_vol();
    CHECK_HEX("read-only mount ok", 0, hype_exfat_fs_mount(vol_read, 0, 0, &g_fs));
}

static void test_mount_rejections(void) {
    struct { const char *desc; unsigned off; int width; uint32_t val; } cases[] = {
        {"broken signature", 3, 8, 'N'},
        {"signature padding", 9, 8, 'X'},
        {"4K sectors", 0x6C, 8, 12u},
        {"implausible cluster shift", 0x6D, 8, 20u},
        {"NumberOfFats 0", 0x6E, 8, 0u},
        {"NumberOfFats 3", 0x6E, 8, 3u},
        {"FatLength 0", 0x54, 32, 0u},
        {"FatOffset inside the boot regions", 0x50, 32, 8u},
        {"heap overlapping the FAT", 0x58, 32, FAT_LBA + FAT_LEN - 1u},
        {"ClusterCount 0", 0x5C, 32, 0u},
        {"ClusterCount beyond the FAT's reach", 0x5C, 32, FAT_LEN * 128u},
        {"heap running past the volume", 0x5C, 32, VOL_SECTORS},
        {"root cluster below 2", 0x60, 32, 1u},
        {"root cluster past the heap", 0x60, 32, CLUSTERS + 2u},
    };
    unsigned i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        build_vol();
        if (cases[i].width == 8) {
            g_vol[cases[i].off] = (uint8_t)cases[i].val;
        } else {
            put32(g_vol + cases[i].off, cases[i].val);
        }
        CHECK_HEX(cases[i].desc, -1, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    }
    /* Boot sector unreadable. */
    build_vol();
    g_fail_read_lba = 0;
    CHECK_HEX("boot read failure", -1, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    g_fail_read_lba = (uint64_t)-1;
}

static void test_mount_critical_structures(void) {
    uint8_t *root;

    /* No Allocation Bitmap entry: nothing could ever be allocated. */
    build_vol();
    cluster(ROOT_CL)[32] = 0x01u; /* clear the InUse bit on the bitmap entry */
    CHECK_HEX("missing bitmap rejected", -1, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));

    /* No Up-case Table entry: names could not be compared the way other drivers do. */
    build_vol();
    cluster(ROOT_CL)[64] = 0x02u;
    CHECK_HEX("missing up-case rejected", -1, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));

    /* An end-of-directory marker before them has the same effect. */
    build_vol();
    cluster(ROOT_CL)[32] = 0x00u;
    CHECK_HEX("bitmap after end-of-directory rejected", -1,
              hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));

    /* A bitmap too small to describe every cluster. */
    build_vol();
    put64(cluster(ROOT_CL) + 32 + 24, ((CLUSTERS + 7u) / 8u) - 1u);
    CHECK_HEX("undersized bitmap rejected", -1, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));

    /* A bitmap whose chain is not physically contiguous. */
    build_vol();
    put64(cluster(ROOT_CL) + 32 + 24, 700u); /* two clusters' worth */
    put32(fat_ent(BITMAP_CL), 9u);           /* ...but 2 -> 9, not 2 -> 3 */
    put32(fat_ent(9u), 0xFFFFFFFFu);
    CHECK_HEX("fragmented bitmap rejected", -1, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));

    /* A bitmap whose chain ends before its DataLength. */
    build_vol();
    put64(cluster(ROOT_CL) + 32 + 24, 700u);
    CHECK_HEX("bitmap chain too short rejected", -1,
              hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));

    /* Up-case table whose stored TableChecksum does not match its bytes: the one
     * check that proves hype would fold names the same way every other exFAT
     * implementation does. */
    build_vol();
    put32(cluster(ROOT_CL) + 64 + 4, 0xDEADBEEFu);
    CHECK_HEX("up-case checksum mismatch rejected", -1,
              hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));

    /* Up-case table lengths that cannot be a whole number of 16-bit entries, or
     * are absurd. */
    build_vol();
    put64(cluster(ROOT_CL) + 64 + 24, g_upcase_len - 1u);
    CHECK_HEX("odd up-case length rejected", -1, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    build_vol();
    put64(cluster(ROOT_CL) + 64 + 24, 0u);
    CHECK_HEX("zero up-case length rejected", -1,
              hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    build_vol();
    put64(cluster(ROOT_CL) + 64 + 24, 0x20002u);
    CHECK_HEX("oversized up-case length rejected", -1,
              hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));

    /* A malformed (over-long identity run) up-case table. */
    build_vol();
    root = cluster(UPCASE_CL);
    put16(root + 0, 0xFFFF); put16(root + 2, 0xFFFF);
    put16(root + 4, 0xFFFF); put16(root + 6, 0xFFFF);
    put32(cluster(ROOT_CL) + 64 + 4, ref_table_checksum(root, g_upcase_len));
    CHECK_HEX("malformed up-case table rejected", -1,
              hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));

    /* With two FATs, the bitmap whose BitmapFlags do not name the active FAT is
     * the wrong one and must be ignored -- here it is the only one, so mounting
     * has to fail rather than use it. */
    build_vol();
    g_vol[0x6E] = 2;
    put16(g_vol + 0x6A, 0x0001u); /* ActiveFat = 1 */
    CHECK_HEX("bitmap for the inactive FAT ignored", -1,
              hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
}

static void test_mount_two_fats(void) {
    unsigned s;
    build_vol();
    g_vol[0x6E] = 2;
    put16(g_vol + 0x6A, 0x0001u);          /* ActiveFat = 1 */
    cluster(ROOT_CL)[32 + 1] = 0x01u;      /* BitmapFlags: this bitmap is FAT 1's */
    put32(g_vol + 0x58, FAT_LBA + 2u * FAT_LEN); /* heap must clear both FATs */
    /* Two FATs push the heap out by FAT_LEN sectors, so rebuild everything that
     * depends on the heap position: move the FAT copy and re-place the clusters. */
    for (s = 0; s < FAT_LEN; s++) {
        memcpy(g_vol + (FAT_LBA + FAT_LEN + s) * SECSZ, g_vol + (FAT_LBA + s) * SECSZ, SECSZ);
    }
    CHECK_HEX("two-FAT mount picks the active FAT", 0,
              hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("active FAT is the second copy", FAT_LBA + FAT_LEN, g_fs.fat_lba);
}

/* ---- create + append ---- */

static void test_create_append(void) {
    hype_exfat_wfile_t f;
    static uint8_t data[2000];
    static uint8_t back[4096];
    unsigned i, got;

    build_vol();
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("create ok", 0, hype_exfat_create(&g_fs, "hype.log", &f));
    /* No data cluster until there is data to put in it. */
    CHECK_HEX("no allocation yet", 0u, f.first_cluster);
    CHECK_HEX("size 0", 0ull, f.size);
    CHECK_HEX("set placed after the three root entries", 3u, f.set_index);
    CHECK_HEX("secondary count for an 8-char name", 2u, f.secondary);
    verify_set("fresh file", 3u, "hype.log", 0u, 0u, 0);
    CHECK_HEX("VolumeDirty set on the medium", HYPE_EXFAT_VOLUME_DIRTY,
              get16(g_vol + 0x6A) & HYPE_EXFAT_VOLUME_DIRTY);
    CHECK_HEX("VolumeDirty mirrored to the backup boot sector", HYPE_EXFAT_VOLUME_DIRTY,
              get16(g_vol + BACKUP_BOOT_LBA * SECSZ + 0x6A) & HYPE_EXFAT_VOLUME_DIRTY);

    for (i = 0; i < sizeof data; i++) data[i] = pat(i);
    CHECK_HEX("append 100", 0, hype_exfat_append(&f, data, 100u));
    CHECK_HEX("size 100", 100ull, f.size);
    CHECK_HEX("first cluster allocated", 5u, f.first_cluster);
    CHECK_HEX("cluster 5 marked used", 1, bit_used(5u));
    CHECK_HEX("FAT[5] end-of-chain", 0xFFFFFFFFu, fat_get(5u));
    verify_set("after 100 bytes", 3u, "hype.log", 5u, 100u, 0);

    /* Fill the cluster exactly, then one byte more: the chain must grow. */
    CHECK_HEX("append to exactly one cluster", 0, hype_exfat_append(&f, data + 100u, 412u));
    CHECK_HEX("size 512", 512ull, f.size);
    CHECK_HEX("still one cluster", 0xFFFFFFFFu, fat_get(5u));
    CHECK_HEX("append one byte past the boundary", 0, hype_exfat_append(&f, data + 512u, 1u));
    CHECK_HEX("size 513", 513ull, f.size);
    CHECK_HEX("FAT[5] -> 6", 6u, fat_get(5u));
    CHECK_HEX("FAT[6] end-of-chain", 0xFFFFFFFFu, fat_get(6u));
    CHECK_HEX("cluster 6 marked used", 1, bit_used(6u));

    /* A multi-cluster append in one call. */
    CHECK_HEX("append 1487 more", 0, hype_exfat_append(&f, data + 513u, 1487u));
    CHECK_HEX("size 2000", 2000ull, f.size);
    verify_set("after 2000 bytes", 3u, "hype.log", 5u, 2000u, 0);
    CHECK_HEX("four clusters used for 2000 bytes", 0xFFFFFFFFu, fat_get(8u));
    got = gather(f.first_cluster, 0, back, sizeof back);
    CHECK("gathered at least 2000 bytes", got >= 2000u);
    for (i = 0; i < 2000u; i++) {
        if (back[i] != pat(i)) {
            CHECK_HEX("content byte", pat(i), back[i]);
            break;
        }
    }
    CHECK_HEX("used clusters tracked", used_count(), g_fs.used_clusters);

    /* Flush: VolumeDirty cleared, PercentInUse refreshed, in both boot sectors. */
    CHECK_HEX("sync ok", 0, hype_exfat_fs_sync(&g_fs));
    CHECK_HEX("VolumeDirty cleared", 0u, get16(g_vol + 0x6A) & HYPE_EXFAT_VOLUME_DIRTY);
    CHECK_HEX("backup VolumeDirty cleared", 0u,
              get16(g_vol + BACKUP_BOOT_LBA * SECSZ + 0x6A) & HYPE_EXFAT_VOLUME_DIRTY);
    CHECK_HEX("PercentInUse", (g_fs.used_clusters * 100u) / CLUSTERS, g_vol[0x70]);
    CHECK_HEX("backup PercentInUse", (g_fs.used_clusters * 100u) / CLUSTERS,
              g_vol[BACKUP_BOOT_LBA * SECSZ + 0x70]);
    /* The boot signature and every checksum-covered byte are untouched. */
    CHECK_HEX("boot signature intact", 0xAA55u, get16(g_vol + 0x1FE));
    CHECK_HEX("FatOffset intact", FAT_LBA, get32(g_vol + 0x50));
}

static void test_truncate(void) {
    hype_exfat_wfile_t f;
    static uint8_t data[1500];
    unsigned before;

    build_vol();
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("create ok", 0, hype_exfat_create(&g_fs, "hype.log", &f));
    CHECK_HEX("append 1500", 0, hype_exfat_append(&f, data, sizeof data));
    before = used_count();
    CHECK("three data clusters taken", before == 6u);

    /* Re-creating the same name frees the old chain and reuses the same slot. */
    CHECK_HEX("recreate ok", 0, hype_exfat_create(&g_fs, "hype.log", &f));
    CHECK_HEX("size reset", 0ull, f.size);
    CHECK_HEX("allocation released", 0u, f.first_cluster);
    CHECK_HEX("same dirent slot reused", 3u, f.set_index);
    CHECK_HEX("data clusters returned to the bitmap", 3u, used_count());
    CHECK_HEX("FAT entries cleared", 0u, fat_get(5u));
    verify_set("after truncate", 3u, "hype.log", 0u, 0u, 0);
    /* And it can be written again. */
    CHECK_HEX("append after truncate", 0, hype_exfat_append(&f, "again", 5u));
    CHECK_HEX("size 5", 5ull, f.size);
    CHECK("content rewritten", memcmp(cluster(f.first_cluster), "again", 5) == 0);
}

static void test_long_names(void) {
    hype_exfat_wfile_t f;
    char name[HYPE_EXFAT_MAX_NAME + 2];
    unsigned i;

    build_vol();
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    /* Exactly 15 characters: one File Name entry, no spill. */
    CHECK_HEX("15-char name", 0, hype_exfat_create(&g_fs, "abcdefghijklmno", &f));
    CHECK_HEX("one name entry", 2u, f.secondary);
    verify_set("15-char", f.set_index, "abcdefghijklmno", 0u, 0u, 0);
    /* 16 characters spill into a second name entry. */
    CHECK_HEX("16-char name", 0, hype_exfat_create(&g_fs, "abcdefghijklmnop", &f));
    CHECK_HEX("two name entries", 3u, f.secondary);
    verify_set("16-char", f.set_index, "abcdefghijklmnop", 0u, 0u, 0);
    /* The exFAT maximum: 255 characters, 17 name entries. */
    for (i = 0; i < HYPE_EXFAT_MAX_NAME; i++) name[i] = (char)('a' + (i % 26u));
    name[HYPE_EXFAT_MAX_NAME] = '\0';
    CHECK_HEX("255-char name", 0, hype_exfat_create(&g_fs, name, &f));
    CHECK_HEX("18 secondary entries", 18u, f.secondary);
    verify_set("255-char", f.set_index, name, 0u, 0u, 0);
    CHECK_HEX("append to the long-named file", 0, hype_exfat_append(&f, "x", 1u));
    verify_set("255-char after append", f.set_index, name, f.first_cluster, 1u, 0);
    /* One character over the maximum. */
    name[HYPE_EXFAT_MAX_NAME] = 'z';
    name[HYPE_EXFAT_MAX_NAME + 1u] = '\0';
    CHECK_HEX("256-char name rejected", -1, hype_exfat_create(&g_fs, name, &f));
}

static void test_name_rejections(void) {
    hype_exfat_wfile_t f;
    static const char *bad[] = {"", "a/b", "a\\b", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b",
                                "a|b"};
    unsigned i;
    build_vol();
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK_HEX("invalid name rejected", -1, hype_exfat_create(&g_fs, bad[i], &f));
    }
    /*
     * A character outside the cached part of the up-case table: hype cannot know
     * how the volume folds it, so it cannot compute a NameHash other exFAT
     * implementations would agree with, and must refuse rather than write one
     * that only it believes.
     */
    {
        char high[4];
        high[0] = 'a';
        high[1] = (char)0xC3; /* >= 0x80 once widened, outside the fixture's table */
        high[2] = '\0';
        build_vol();
        /* Shorten the table so 0xC3 is past its end and therefore not exact. */
        put64(cluster(ROOT_CL) + 64 + 24, 8u);
        put32(cluster(ROOT_CL) + 64 + 4, ref_table_checksum(cluster(UPCASE_CL), 8u));
        CHECK_HEX("mount with a short table", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
        CHECK_HEX("uncovered character refused", -1, hype_exfat_create(&g_fs, high, &f));
        CHECK_HEX("plain ASCII still refused past the table", -1,
                  hype_exfat_create(&g_fs, "zzz", &f));
    }
}

/* ---- lookup ---- */

/* Places a pre-existing file (and a subdirectory holding another) in the volume
 * by hand, so lookup is tested against entries this code did not write. */
static void place_set(uint8_t *at, const char *name, uint16_t attr, uint32_t first_cl,
                      uint64_t len, int contiguous) {
    unsigned nlen = (unsigned)strlen(name);
    unsigned name_entries = (nlen + 14u) / 15u;
    unsigned entries = 2u + name_entries;
    unsigned k, i;
    uint16_t chars[HYPE_EXFAT_MAX_NAME];

    memset(at, 0, entries * 32u);
    for (i = 0; i < nlen; i++) chars[i] = (uint16_t)name[i];
    at[0] = HYPE_EXFAT_ENT_FILE;
    at[1] = (uint8_t)(entries - 1u);
    put16(at + 4, attr);
    put32(at + 8, HYPE_EXFAT_TIMESTAMP_EPOCH);
    at[32] = HYPE_EXFAT_ENT_STREAM;
    at[33] = (uint8_t)(0x01u | (contiguous ? 0x02u : 0x00u));
    at[35] = (uint8_t)nlen;
    put16(at + 32 + 4, ref_name_hash_ascii(name));
    put64(at + 32 + 8, len);
    put32(at + 32 + 20, first_cl);
    put64(at + 32 + 24, len);
    for (k = 0; k < name_entries; k++) {
        uint8_t *ne = at + (2u + k) * 32u;
        unsigned off = k * 15u;
        unsigned count = nlen - off;
        if (count > 15u) count = 15u;
        ne[0] = HYPE_EXFAT_ENT_NAME;
        for (i = 0; i < count; i++) put16(ne + 2u + i * 2u, chars[off + i]);
    }
    put16(at + 2, ref_set_checksum(at, entries * 32u));
}

static void build_vol_with_files(void) {
    unsigned i;
    build_vol();
    /* image.img: 3 contiguous clusters (10,11,12), 1400 bytes. */
    place_set(cluster(ROOT_CL) + 96, "image.img", HYPE_EXFAT_ATTR_ARCHIVE, 10u, 1400u, 1);
    /* subdir: one cluster (20), FAT-chained, holding deep.bin at clusters 30->32. */
    place_set(cluster(ROOT_CL) + 192, "subdir", HYPE_EXFAT_ATTR_DIRECTORY, 20u, 512u, 0);
    place_set(cluster(20u) + 0, "deep.bin", HYPE_EXFAT_ATTR_ARCHIVE, 30u, 700u, 0);
    put32(fat_ent(20u), 0xFFFFFFFFu);
    put32(fat_ent(30u), 32u);
    put32(fat_ent(32u), 0xFFFFFFFFu);
    for (i = 0; i < 3u; i++) bit_mark(10u + i, 1);
    bit_mark(20u, 1);
    bit_mark(30u, 1);
    bit_mark(32u, 1);
    /* cdir: a NoFatChain (contiguous) 2-cluster directory at 40,41 whose second
     * cluster holds a file, so descending into it addresses a directory cluster
     * the FAT says nothing about. A deleted entry sits in front of that file. */
    place_set(cluster(ROOT_CL) + 288, "cdir", HYPE_EXFAT_ATTR_DIRECTORY, 40u, 1024u, 1);
    bit_mark(40u, 1);
    bit_mark(41u, 1);
    for (i = 0; i < 16u; i++) cluster(40u)[i * 32u] = 0x05u; /* not-in-use filler */
    place_set(cluster(41u) + 0, "gone.bin", HYPE_EXFAT_ATTR_ARCHIVE, 50u, 512u, 1);
    cluster(41u)[0] &= (uint8_t)~(uint8_t)HYPE_EXFAT_ENT_INUSE; /* deleted set */
    place_set(cluster(41u) + 96, "inner.bin", HYPE_EXFAT_ATTR_ARCHIVE, 51u, 600u, 0);
    put32(fat_ent(51u), 52u);
    put32(fat_ent(52u), 0xFFFFFFFFu);
    bit_mark(51u, 1);
    bit_mark(52u, 1);
    /* Fill the pre-allocated image with a known pattern. */
    for (i = 0; i < 1400u; i++) cluster(10u + i / SECSZ)[i % SECSZ] = pat(i);
}

static void test_lookup(void) {
    hype_exfat_wfile_t f;
    build_vol_with_files();
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));

    CHECK_HEX("lookup \\image.img", 0, hype_exfat_lookup(&g_fs, "\\image.img", 0, &f));
    CHECK_HEX("size", 1400ull, f.size);
    CHECK_HEX("first cluster", 10u, f.first_cluster);
    CHECK_HEX("contiguous", 1u, f.contiguous);
    CHECK_HEX("not a directory", 0u, f.is_dir);
    /* Case folding goes through the up-case table. */
    CHECK_HEX("lookup is case-insensitive", 0, hype_exfat_lookup(&g_fs, "\\IMAGE.IMG", 0, &f));
    CHECK_HEX("forward slashes accepted", 0, hype_exfat_lookup(&g_fs, "/Image.Img", 0, &f));
    /* Subdirectory descent. */
    CHECK_HEX("lookup \\subdir\\deep.bin", 0, hype_exfat_lookup(&g_fs, "\\subdir\\deep.bin", 0, &f));
    CHECK_HEX("deep.bin size", 700ull, f.size);
    CHECK_HEX("deep.bin first cluster", 30u, f.first_cluster);
    CHECK_HEX("directory looked up as a directory", 0,
              hype_exfat_lookup(&g_fs, "\\subdir", 1, &f));
    CHECK_HEX("subdir is a directory", 1u, f.is_dir);
    /* A trailing separator still names the same component. */
    CHECK_HEX("trailing separator on a directory", 0, hype_exfat_lookup(&g_fs, "\\subdir\\", 1, &f));
    /*
     * Descend into a NoFatChain directory. Its clusters are consecutive and the
     * FAT holds nothing for them, so the flag has to be honoured to reach the
     * second cluster at all -- and the deleted entry set sitting in front of the
     * target must be stepped over.
     */
    CHECK_HEX("lookup inside a contiguous directory", 0,
              hype_exfat_lookup(&g_fs, "\\cdir\\inner.bin", 0, &f));
    CHECK_HEX("inner.bin size", 600ull, f.size);
    CHECK_HEX("inner.bin first cluster", 51u, f.first_cluster);
    CHECK_HEX("its directory is contiguous", 1u, f.dir_contiguous);
    CHECK_HEX("a deleted set is not a name", -1, hype_exfat_lookup(&g_fs, "\\cdir\\gone.bin", 0, &f));
    /* Wrong kind, missing, malformed. */
    CHECK_HEX("directory as file rejected", -1, hype_exfat_lookup(&g_fs, "\\subdir", 0, &f));
    CHECK_HEX("file as directory rejected", -1, hype_exfat_lookup(&g_fs, "\\image.img", 1, &f));
    CHECK_HEX("missing name", -1, hype_exfat_lookup(&g_fs, "\\nope.bin", 0, &f));
    CHECK_HEX("empty path", -1, hype_exfat_lookup(&g_fs, "\\", 0, &f));
    CHECK_HEX("non-directory mid-path", -1,
              hype_exfat_lookup(&g_fs, "\\image.img\\inner", 0, &f));
    CHECK_HEX("missing directory mid-path", -1,
              hype_exfat_lookup(&g_fs, "\\nodir\\deep.bin", 0, &f));
    /* A corrupt entry set must not hide the entries after it. */
    build_vol_with_files();
    cluster(ROOT_CL)[96 + 2] ^= 0xFFu; /* wreck image.img's stored checksum */
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("corrupt set is not resolvable", -1, hype_exfat_lookup(&g_fs, "\\image.img", 0, &f));
    CHECK_HEX("entries after a corrupt set still found", 0,
              hype_exfat_lookup(&g_fs, "\\subdir\\deep.bin", 0, &f));
    /* An allocation pointing outside the heap is refused, not followed. */
    build_vol_with_files();
    put32(cluster(ROOT_CL) + 96 + 32 + 20, CLUSTERS + 5u);
    put16(cluster(ROOT_CL) + 96 + 2,
          ref_set_checksum(cluster(ROOT_CL) + 96, 3u * 32u));
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("out-of-heap allocation refused", -1,
              hype_exfat_lookup(&g_fs, "\\image.img", 0, &f));
    /* A path component longer than exFAT allows. */
    {
        char longname[HYPE_EXFAT_MAX_NAME + 8];
        unsigned i;
        longname[0] = '\\';
        for (i = 1; i < sizeof longname - 1u; i++) longname[i] = 'a';
        longname[sizeof longname - 1u] = '\0';
        build_vol_with_files();
        CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
        CHECK_HEX("over-long component rejected", -1, hype_exfat_lookup(&g_fs, longname, 0, &f));
    }
}

/* ---- in-place writes: the pre-allocated-backing-file case ---- */

static void test_write_at(void) {
    hype_exfat_wfile_t f;
    static uint8_t buf[1400];
    static uint8_t back[1400];
    unsigned i;

    build_vol_with_files();
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("lookup ok", 0, hype_exfat_lookup(&g_fs, "\\image.img", 0, &f));

    /* Read the pre-existing pattern back out. */
    CHECK_HEX("read_at whole file", 0, hype_exfat_read_at(&f, 0u, back, 1400u));
    for (i = 0; i < 1400u; i++) {
        if (back[i] != pat(i)) { CHECK_HEX("pre-existing byte", pat(i), back[i]); break; }
    }

    /* Unaligned overwrite spanning three sectors and two clusters. */
    for (i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(0xA5u ^ (i * 13u));
    CHECK_HEX("write_at unaligned span", 0, hype_exfat_write_at(&f, 300u, buf, 900u));
    CHECK_HEX("read back the span", 0, hype_exfat_read_at(&f, 300u, back, 900u));
    CHECK("in-place span round-trips", memcmp(back, buf, 900u) == 0);
    /* Bytes either side are untouched. */
    CHECK_HEX("byte before the span", pat(299u), cluster(10u)[299]);
    CHECK_HEX("byte after the span", pat(1200u), cluster(10u + 1200u / SECSZ)[1200u % SECSZ]);

    /* A whole-sector aligned write takes the no-read-modify-write path. */
    CHECK_HEX("write_at aligned sector", 0, hype_exfat_write_at(&f, 512u, buf, 512u));
    CHECK("aligned sector landed", memcmp(cluster(11u), buf, 512u) == 0);

    /* Single byte at the very last offset. */
    CHECK_HEX("write_at last byte", 0, hype_exfat_write_at(&f, 1399u, buf, 1u));
    CHECK_HEX("last byte landed", buf[0], cluster(10u + 1399u / SECSZ)[1399u % SECSZ]);

    /* Zero length is a no-op, not an error. */
    CHECK_HEX("zero-length write", 0, hype_exfat_write_at(&f, 0u, buf, 0u));
    CHECK_HEX("zero-length read", 0, hype_exfat_read_at(&f, 0u, back, 0u));
    CHECK_HEX("zero-length append", 0, hype_exfat_append(&f, buf, 0u));
    CHECK_HEX("zero-length append does not resize", 1400ull, f.size);

    /* Out of range in every direction: refused, never clamped. This is the
     * offset+length validation AGENTS.md requires on anything a guest can steer. */
    CHECK_HEX("write past the end", -1, hype_exfat_write_at(&f, 1400u, buf, 1u));
    CHECK_HEX("write straddling the end", -1, hype_exfat_write_at(&f, 1399u, buf, 2u));
    CHECK_HEX("read past the end", -1, hype_exfat_read_at(&f, 1400u, back, 1u));
    CHECK_HEX("read straddling the end", -1, hype_exfat_read_at(&f, 1396u, back, 8u));
    CHECK_HEX("absurd offset", -1, hype_exfat_write_at(&f, 0xFFFFFFFFFFFFFFFFull, buf, 1u));
    CHECK_HEX("null buffer refused", -1, hype_exfat_write_at(&f, 0u, 0, 1u));
    CHECK_HEX("null read buffer refused", -1, hype_exfat_read_at(&f, 0u, 0, 1u));
    /* Writing does not resize or re-point the file. */
    verify_set("after in-place writes", 3u, "image.img", 10u, 1400u, 1);

    /* A chained file's offsets follow the FAT, and the seek cache must not
     * mis-map a backwards seek. */
    CHECK_HEX("lookup chained file", 0, hype_exfat_lookup(&g_fs, "\\subdir\\deep.bin", 0, &f));
    CHECK_HEX("write cluster 0 of the chain", 0, hype_exfat_write_at(&f, 0u, "AAAA", 4u));
    CHECK_HEX("write cluster 1 of the chain", 0, hype_exfat_write_at(&f, 512u, "BBBB", 4u));
    CHECK_HEX("write cluster 0 again (backwards seek)", 0,
              hype_exfat_write_at(&f, 4u, "CCCC", 4u));
    CHECK("chain cluster 0 holds AAAACCCC", memcmp(cluster(30u), "AAAACCCC", 8) == 0);
    CHECK("chain cluster 1 holds BBBB", memcmp(cluster(32u), "BBBB", 4) == 0);
    CHECK_HEX("read across the chain boundary", 0, hype_exfat_read_at(&f, 508u, back, 8u));
    CHECK("bytes straddling two clusters", memcmp(back + 4, "BBBB", 4) == 0);
}

/* Appending to a NoFatChain file has to turn its allocation into a real FAT
 * chain first, or the next cluster would have to be the next physical one. */
static void test_contiguous_grow(void) {
    hype_exfat_wfile_t f;
    static uint8_t buf[600];
    build_vol_with_files();
    /* Make cluster 13 (the one right after image.img) unavailable, so a naive
     * "just take the next cluster" grow would corrupt something. */
    bit_mark(13u, 1);
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("lookup ok", 0, hype_exfat_lookup(&g_fs, "\\image.img", 0, &f));
    CHECK_HEX("starts contiguous", 1u, f.contiguous);
    /* 1400 bytes over 512-byte clusters leaves 88 bytes of slack in cluster 12;
     * appending 600 crosses into a fourth cluster. */
    CHECK_HEX("append across the end", 0, hype_exfat_append(&f, buf, sizeof buf));
    CHECK_HEX("size 2000", 2000ull, f.size);
    CHECK_HEX("no longer contiguous", 0u, f.contiguous);
    CHECK_HEX("FAT chain materialised 10 -> 11", 11u, fat_get(10u));
    CHECK_HEX("FAT chain materialised 11 -> 12", 12u, fat_get(11u));
    CHECK("cluster 12 now links onward", fat_get(12u) >= 2u && fat_get(12u) < 0xFFFFFFF8u);
    CHECK("the new cluster is not the blocked one", fat_get(12u) != 13u);
    verify_set("grown contiguous file", 3u, "image.img", 10u, 2000u, 0);
    /* The original bytes survived the conversion. */
    {
        static uint8_t back[1400];
        unsigned i;
        CHECK_HEX("read back the original span", 0, hype_exfat_read_at(&f, 0u, back, 1400u));
        for (i = 0; i < 1400u; i++) {
            if (back[i] != pat(i)) { CHECK_HEX("preserved byte", pat(i), back[i]); break; }
        }
    }
}

/* ---- directory growth, full volume, read-only ---- */

static void test_root_growth(void) {
    hype_exfat_wfile_t f;
    char name[16];
    unsigned i;
    build_vol();
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    /* 16 entries per 512-byte cluster; three are taken, and every file needs
     * three, so the fifth file cannot fit in the root's first cluster. */
    for (i = 0; i < 12u; i++) {
        snprintf(name, sizeof name, "f%u.dat", i);
        CHECK_HEX("create in a growing root", 0, hype_exfat_create(&g_fs, name, &f));
        CHECK_HEX("append", 0, hype_exfat_append(&f, name, (unsigned)strlen(name)));
    }
    CHECK("root directory chain was extended", fat_get(ROOT_CL) >= 2u &&
                                                  fat_get(ROOT_CL) < 0xFFFFFFF8u);
    /* Every one of them is findable again, including those in the new cluster. */
    for (i = 0; i < 12u; i++) {
        snprintf(name, sizeof name, "\\f%u.dat", i);
        CHECK_HEX("all files resolvable after growth", 0,
                  hype_exfat_lookup(&g_fs, name, 0, &f));
        CHECK_HEX("size matches", strlen(name) - 1u, f.size);
    }
    CHECK_HEX("sync ok", 0, hype_exfat_fs_sync(&g_fs));
}

/* An entry set that straddles two clusters of the root directory must still read
 * back correctly -- the set checksum covers bytes on both sides of the seam. */
static void test_set_across_cluster_boundary(void) {
    hype_exfat_wfile_t f;
    char name[HYPE_EXFAT_MAX_NAME];
    unsigned i;
    build_vol();
    /* Extend the root to two clusters and leave entries 3..14 occupied so a
     * 5-entry set has to start at entry 15 and spill into the second cluster. */
    put32(fat_ent(ROOT_CL), 40u);
    put32(fat_ent(40u), 0xFFFFFFFFu);
    bit_mark(40u, 1);
    memset(cluster(40u), 0, SECSZ);
    for (i = 3u; i < 15u; i++) {
        cluster(ROOT_CL)[i * 32u] = HYPE_EXFAT_ENT_LABEL; /* in-use, not a File set */
    }
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    for (i = 0; i < 40u; i++) name[i] = (char)('a' + (i % 26u));
    name[40] = '\0';
    CHECK_HEX("create a straddling set", 0, hype_exfat_create(&g_fs, name, &f));
    CHECK_HEX("set starts at entry 15", 15u, f.set_index);
    CHECK_HEX("append", 0, hype_exfat_append(&f, "spanning", 8u));
    /* Reading it back through the full validating path proves the checksum
     * survived the seam. */
    {
        char path[HYPE_EXFAT_MAX_NAME + 2];
        path[0] = '\\';
        memcpy(path + 1, name, 41);
        CHECK_HEX("straddling set resolves", 0, hype_exfat_lookup(&g_fs, path, 0, &f));
        CHECK_HEX("straddling set size", 8ull, f.size);
    }
}

static void test_volume_full(void) {
    hype_exfat_wfile_t f;
    static uint8_t buf[SECSZ];
    unsigned i;
    build_vol();
    /* Mark every cluster but 5 and 6 as taken. */
    for (i = 0; i < CLUSTERS; i++) {
        bit_mark(2u + i, 1);
    }
    bit_mark(5u, 0);
    bit_mark(CLUSTERS + 1u, 0); /* the heap's very last cluster */
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("used clusters counted", CLUSTERS - 2u, g_fs.used_clusters);
    CHECK_HEX("create ok", 0, hype_exfat_create(&g_fs, "big.bin", &f));
    CHECK_HEX("first 512 bytes fit", 0, hype_exfat_append(&f, buf, SECSZ));
    CHECK_HEX("second cluster fits", 0, hype_exfat_append(&f, buf, SECSZ));
    CHECK_HEX("third cluster does not", -1, hype_exfat_append(&f, buf, 1u));
    /* Now nothing is free at all. */
    CHECK_HEX("create with no clusters left still succeeds (no data yet)", 0,
              hype_exfat_create(&g_fs, "empty.bin", &f));
    CHECK_HEX("but appending fails", -1, hype_exfat_append(&f, buf, 1u));
}

static void test_read_only_mount(void) {
    hype_exfat_wfile_t f;
    static uint8_t back[16];
    build_vol_with_files();
    CHECK_HEX("read-only mount", 0, hype_exfat_fs_mount(vol_read, 0, 0, &g_fs));
    CHECK_HEX("lookup still works", 0, hype_exfat_lookup(&g_fs, "\\image.img", 0, &f));
    CHECK_HEX("read_at still works", 0, hype_exfat_read_at(&f, 0u, back, sizeof back));
    CHECK_HEX("create refused", -1, hype_exfat_create(&g_fs, "no.txt", &f));
    CHECK_HEX("lookup again", 0, hype_exfat_lookup(&g_fs, "\\image.img", 0, &f));
    CHECK_HEX("write_at refused", -1, hype_exfat_write_at(&f, 0u, back, sizeof back));
    CHECK_HEX("append refused", -1, hype_exfat_append(&f, back, sizeof back));
    CHECK_HEX("sync refused", -1, hype_exfat_fs_sync(&g_fs));
    /* Appending to a directory is refused whatever the mount mode. */
    CHECK_HEX("read-write mount", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("lookup the directory", 0, hype_exfat_lookup(&g_fs, "\\subdir", 1, &f));
    CHECK_HEX("append to a directory refused", -1, hype_exfat_append(&f, back, 1u));
    /* Creating over an existing directory name is refused. */
    CHECK_HEX("create over a directory refused", -1, hype_exfat_create(&g_fs, "subdir", &f));
}

/*
 * A set carrying more secondary entries than hype would generate for the same
 * name cannot be reused in place: rewriting only the entries hype knows about
 * would leave a stale one inside a set whose checksum no longer covers it. It
 * must be retired and a fresh set placed instead.
 */
static void test_recreate_odd_shaped_set(void) {
    hype_exfat_wfile_t f;
    uint8_t *at = cluster(ROOT_CL) + 96;
    build_vol();
    place_set(at, "odd.bin", HYPE_EXFAT_ATTR_ARCHIVE, 10u, 512u, 1);
    at[1] = 3u;                       /* claim one extra secondary entry... */
    at[3u * 32u] = 0xE0u;             /* ...an unknown secondary type */
    put16(at + 2, ref_set_checksum(at, 4u * 32u));
    bit_mark(10u, 1);
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("odd-shaped set resolves first", 0, hype_exfat_lookup(&g_fs, "\\odd.bin", 0, &f));
    CHECK_HEX("recreate ok", 0, hype_exfat_create(&g_fs, "odd.bin", &f));
    /* The replacement set has the shape hype generates -- two secondary entries,
     * not the three the retired one claimed -- and the extra entry the old set
     * carried is no longer in use, so it is not inside any set's checksum. */
    CHECK_HEX("replacement set has hype's own shape", 2u, f.secondary);
    CHECK_HEX("the old extra secondary entry is retired", 0u,
              cluster(ROOT_CL)[(f.set_index + 3u) * 32u] & HYPE_EXFAT_ENT_INUSE);
    CHECK_HEX("old data cluster freed", 0, bit_used(10u));
    CHECK_HEX("append to the recreated file", 0, hype_exfat_append(&f, "new", 3u));
    verify_set("recreated", f.set_index, "odd.bin", f.first_cluster, 3u, 0);
    /* Exactly one set now answers to the name, and it is the new one. */
    CHECK_HEX("lookup finds the new set", 0, hype_exfat_lookup(&g_fs, "\\odd.bin", 0, &f));
    CHECK_HEX("new set size", 3ull, f.size);
}

/*
 * Sweep an injected I/O failure across every operation of a full lifecycle. The
 * results are intentionally ignored: the point is that no ordering of failures
 * crashes, loops, or leaves the code reading outside the volume, and that each
 * defensive error leg is taken at least once.
 */
static void run_cycle(void) {
    hype_exfat_wfile_t f;
    static uint8_t buf[1600];
    if (hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs) != 0) return;
    if (hype_exfat_create(&g_fs, "sweep.log", &f) != 0) return;
    if (hype_exfat_append(&f, buf, sizeof buf) != 0) return;
    if (hype_exfat_write_at(&f, 100u, buf, 900u) != 0) return;
    (void)hype_exfat_read_at(&f, 100u, buf, 900u);
    (void)hype_exfat_lookup(&g_fs, "\\sweep.log", 0, &f);
    (void)hype_exfat_create(&g_fs, "sweep.log", &f); /* truncate path */
    (void)hype_exfat_fs_sync(&g_fs);
}

static void test_fault_sweep(void) {
    long k;
    for (k = 0; k < 220; k++) {
        build_vol();
        g_read_countdown = k;
        g_write_countdown = -1;
        run_cycle();
        build_vol();
        g_read_countdown = -1;
        g_write_countdown = k;
        run_cycle();
    }
    /* Same sweep over a volume that already holds files, so the lookup,
     * contiguous-grow and free-chain legs are exercised under failure too. */
    for (k = 0; k < 200; k++) {
        hype_exfat_wfile_t f;
        static uint8_t buf[900];
        int pass;
        for (pass = 0; pass < 2; pass++) {
            build_vol_with_files();
            g_read_countdown = (pass == 0) ? k : -1;
            g_write_countdown = (pass == 0) ? -1 : k;
            if (hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs) != 0) continue;
            if (hype_exfat_lookup(&g_fs, "\\cdir\\inner.bin", 0, &f) == 0) {
                (void)hype_exfat_read_at(&f, 500u, buf, 40u);
            }
            if (hype_exfat_lookup(&g_fs, "\\image.img", 0, &f) != 0) continue;
            (void)hype_exfat_write_at(&f, 10u, buf, sizeof buf);
            (void)hype_exfat_write_at(&f, 512u, buf, 512u); /* whole-sector path */
            (void)hype_exfat_append(&f, buf, sizeof buf);
            /* Truncating image.img exercises freeing a CONTIGUOUS allocation. */
            (void)hype_exfat_create(&g_fs, "image.img", &f);
        }
    }
    g_read_countdown = -1;
    g_write_countdown = -1;
    CHECK("fault sweep completed without crashing", 1);
}

/* Specific I/O failure points, asserted rather than merely survived. */
static void test_io_failures(void) {
    hype_exfat_wfile_t f;
    build_vol();
    g_fail_read_lba = clba(UPCASE_CL);
    CHECK_HEX("up-case read failure fails the mount", -1,
              hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    g_fail_read_lba = (uint64_t)-1;

    build_vol();
    g_fail_read_lba = clba(ROOT_CL);
    CHECK_HEX("root read failure fails the mount", -1,
              hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    g_fail_read_lba = (uint64_t)-1;

    build_vol();
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    g_fail_write_lba = 0; /* the VolumeDirty write */
    CHECK_HEX("cannot mark the volume dirty", -1, hype_exfat_create(&g_fs, "x.log", &f));
    g_fail_write_lba = (uint64_t)-1;

    build_vol();
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("create ok", 0, hype_exfat_create(&g_fs, "x.log", &f));
    g_fail_write_lba = clba(BITMAP_CL); /* the allocation write */
    CHECK_HEX("bitmap write failure surfaces", -1, hype_exfat_append(&f, "data", 4u));
    g_fail_write_lba = (uint64_t)-1;

    build_vol();
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("create ok", 0, hype_exfat_create(&g_fs, "x.log", &f));
    CHECK_HEX("append ok", 0, hype_exfat_append(&f, "data", 4u));
    g_fail_write_lba = clba(f.first_cluster);
    CHECK_HEX("data write failure surfaces", -1, hype_exfat_append(&f, "more", 4u));
    CHECK_HEX("in-place data write failure surfaces", -1,
              hype_exfat_write_at(&f, 0u, "zzzz", 4u));
    g_fail_write_lba = (uint64_t)-1;

    build_vol();
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("create ok", 0, hype_exfat_create(&g_fs, "x.log", &f));
    CHECK_HEX("append ok", 0, hype_exfat_append(&f, "data", 4u));
    g_fail_write_lba = 0;
    CHECK_HEX("sync write failure surfaces", -1, hype_exfat_fs_sync(&g_fs));
    g_fail_write_lba = (uint64_t)-1;

    /* A missing backup boot region is tolerated: the main one still flushes. */
    build_vol();
    memset(g_vol + BACKUP_BOOT_LBA * SECSZ, 0, SECSZ);
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("create ok", 0, hype_exfat_create(&g_fs, "x.log", &f));
    CHECK_HEX("sync tolerates a missing backup region", 0, hype_exfat_fs_sync(&g_fs));
    CHECK_HEX("main VolumeDirty cleared", 0u, get16(g_vol + 0x6A) & HYPE_EXFAT_VOLUME_DIRTY);
}

/*
 * Directory entries whose allocation fields are individually out of spec. Each
 * has a valid set checksum, so the only thing that can reject them is the
 * allocation range-checking -- which is what stops a corrupt or hostile
 * directory from steering reads and writes outside the cluster heap.
 */
static void test_bad_allocations(void) {
    hype_exfat_wfile_t f;
    uint8_t *at = cluster(ROOT_CL) + 96;

    /* FirstCluster 0 with a non-zero DataLength: there is nothing to read. */
    build_vol();
    place_set(at, "ghost.bin", HYPE_EXFAT_ATTR_ARCHIVE, 0u, 512u, 0);
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("data without an allocation refused", -1,
              hype_exfat_lookup(&g_fs, "\\ghost.bin", 0, &f));

    /* A DataLength needing more clusters than the volume has. */
    build_vol();
    place_set(at, "huge.bin", HYPE_EXFAT_ATTR_ARCHIVE, 10u, (uint64_t)CLUSTERS * SECSZ + SECSZ, 0);
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("DataLength larger than the volume refused", -1,
              hype_exfat_lookup(&g_fs, "\\huge.bin", 0, &f));

    /* A contiguous run that starts inside the heap but ends past its last cluster. */
    build_vol();
    place_set(at, "over.bin", HYPE_EXFAT_ATTR_ARCHIVE, CLUSTERS + 1u, 3u * SECSZ, 1);
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("contiguous run past the heap refused", -1,
              hype_exfat_lookup(&g_fs, "\\over.bin", 0, &f));

    /* An empty file (FirstCluster 0, DataLength 0) is legal, and re-creating it
     * must cope with there being no chain to free. */
    build_vol();
    place_set(at, "empty.bin", HYPE_EXFAT_ATTR_ARCHIVE, 0u, 0u, 0);
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("empty file resolves", 0, hype_exfat_lookup(&g_fs, "\\empty.bin", 0, &f));
    CHECK_HEX("empty file size", 0ull, f.size);
    CHECK_HEX("reading nothing from an empty file", 0, hype_exfat_read_at(&f, 0u, at, 0u));
    CHECK_HEX("reading a byte from an empty file refused", -1,
              hype_exfat_read_at(&f, 0u, at, 1u));
    CHECK_HEX("recreate over an unallocated file", 0, hype_exfat_create(&g_fs, "empty.bin", &f));
    CHECK_HEX("still empty", 0ull, f.size);
    CHECK_HEX("used clusters unchanged", 3u, used_count());
}

/*
 * Corrupt FAT chains. A chain that leaves the cluster heap, or loops back on
 * itself, must be refused -- never followed into whatever the resulting LBA
 * happens to land on, and never allowed to spin forever.
 */
static void test_corrupt_chains(void) {
    hype_exfat_wfile_t f;
    static uint8_t back[16];
    unsigned i;

    /* A file whose chain points outside the heap partway along. */
    build_vol_with_files();
    put32(fat_ent(30u), CLUSTERS + 9u); /* deep.bin: 30 -> out of range */
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("lookup ok", 0, hype_exfat_lookup(&g_fs, "\\subdir\\deep.bin", 0, &f));
    CHECK_HEX("reading into the broken link refused", -1,
              hype_exfat_read_at(&f, 512u, back, 4u));
    CHECK_HEX("appending onto a broken chain refused", -1, hype_exfat_append(&f, back, 4u));

    /* A chain that ends before the recorded size: the cluster after the first is
     * simply not there. */
    build_vol_with_files();
    put32(fat_ent(30u), 0xFFFFFFFFu);
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("lookup ok", 0, hype_exfat_lookup(&g_fs, "\\subdir\\deep.bin", 0, &f));
    CHECK_HEX("reading past the real end of the chain refused", -1,
              hype_exfat_read_at(&f, 512u, back, 4u));

    /* Truncating a file whose chain loops must terminate and report failure
     * rather than freeing clusters round and round. */
    build_vol();
    place_set(cluster(ROOT_CL) + 96, "loopy.bin", HYPE_EXFAT_ATTR_ARCHIVE, 10u, 3u * SECSZ, 0);
    put32(fat_ent(10u), 11u);
    put32(fat_ent(11u), 10u); /* 10 -> 11 -> 10 -> ... */
    bit_mark(10u, 1);
    bit_mark(11u, 1);
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("truncating a looping chain terminates", -1,
              hype_exfat_create(&g_fs, "loopy.bin", &f));

    /* A subdirectory whose chain ends before the scan does: entries past the end
     * of its real allocation must read as "not there", not as whatever follows. */
    build_vol_with_files();
    for (i = 0; i < 16u; i++) {
        cluster(20u)[i * 32u] = HYPE_EXFAT_ENT_LABEL; /* in-use, never terminating */
    }
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("scan stops at the end of the directory's allocation", -1,
              hype_exfat_lookup(&g_fs, "\\subdir\\missing.bin", 0, &f));

    /* The root directory's own chain pointing out of range. Mounting still works
     * -- the critical entries live in the first cluster -- but anything that has
     * to walk the chain must refuse rather than follow the broken link. */
    build_vol();
    put32(fat_ent(ROOT_CL), CLUSTERS + 9u);
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("create over a broken root chain refused", -1,
              hype_exfat_create(&g_fs, "x.bin", &f));

    /* A root chain that loops: mount reads the bitmap and up-case entries out of
     * the first cluster fine, but creating a file has to walk the chain. */
    build_vol();
    put32(fat_ent(ROOT_CL), ROOT_CL);
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("create over a looping root chain terminates", -1,
              hype_exfat_create(&g_fs, "loop.bin", &f));
}

/* A contiguous directory whose run reaches the last cluster of the heap: the
 * entry index just past it addresses a cluster that does not exist. */
static void test_contiguous_dir_end_of_heap(void) {
    hype_exfat_wfile_t f;
    unsigned i;
    uint32_t first = CLUSTERS - 1u; /* first + 3 clusters == the heap's end exactly */
    build_vol();
    place_set(cluster(ROOT_CL) + 96, "edge", HYPE_EXFAT_ATTR_DIRECTORY, first, 3u * SECSZ, 1);
    for (i = 0; i < 3u * 16u; i++) {
        cluster(first + i / 16u)[(i % 16u) * 32u] = HYPE_EXFAT_ENT_LABEL; /* never terminates */
    }
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("scan stops at the heap's last cluster", -1,
              hype_exfat_lookup(&g_fs, "\\edge\\nothing", 0, &f));
}

static void test_volume_length_guard(void) {
    build_vol();
    put64(g_vol + 0x48, 100u); /* the heap would run far past the volume's end */
    CHECK_HEX("heap past the volume length rejected", -1,
              hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    build_vol();
    put32(g_vol + 0x5C, 0xFFFFFFF5u); /* ClusterCount above the addressable maximum */
    CHECK_HEX("absurd ClusterCount rejected", -1,
              hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
}

/* A volume whose VolumeDirty flag is already set was left unflushed by whoever
 * wrote it last; mounting must notice rather than assume it is clean. */
static void test_mount_already_dirty(void) {
    hype_exfat_wfile_t f;
    build_vol();
    put16(g_vol + 0x6A, HYPE_EXFAT_VOLUME_DIRTY);
    put16(g_vol + BACKUP_BOOT_LBA * SECSZ + 0x6A, HYPE_EXFAT_VOLUME_DIRTY);
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("dirty flag carried over from the medium", 1u, g_fs.dirty);
    /* Already dirty, so the first mutation does not rewrite the boot sector. */
    g_fail_write_lba = 0;
    CHECK_HEX("create needs no boot-sector write", 0, hype_exfat_create(&g_fs, "d.log", &f));
    g_fail_write_lba = (uint64_t)-1;
    CHECK_HEX("sync clears it", 0, hype_exfat_fs_sync(&g_fs));
    CHECK_HEX("clean again", 0u, get16(g_vol + 0x6A) & HYPE_EXFAT_VOLUME_DIRTY);
    CHECK_HEX("fs no longer dirty", 0u, g_fs.dirty);
}

/* A directory whose chain leaves the heap partway along: entries in the missing
 * cluster must read as absent, not as whatever LBA the arithmetic produces. */
static void test_directory_chain_leaves_heap(void) {
    hype_exfat_wfile_t f;
    unsigned i;
    build_vol_with_files();
    /* Fill subdir's only cluster with in-use non-File entries so the scan runs
     * past it, and point its chain at a cluster outside the heap. */
    for (i = 0; i < 16u; i++) cluster(20u)[i * 32u] = HYPE_EXFAT_ENT_LABEL;
    put32(fat_ent(20u), CLUSTERS + 4u);
    CHECK_HEX("mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("scan refuses to follow the chain out of the heap", -1,
              hype_exfat_lookup(&g_fs, "\\subdir\\anything", 0, &f));
}

/* Fault sweep over the root-directory growth path, which the main sweep's
 * volume never reaches. */
static void test_growth_fault_sweep(void) {
    long k;
    for (k = 0; k < 90; k++) {
        hype_exfat_wfile_t f;
        char name[16];
        unsigned i;
        int pass;
        for (pass = 0; pass < 2; pass++) {
            build_vol();
            g_read_countdown = (pass == 0) ? k : -1;
            g_write_countdown = (pass == 0) ? -1 : k;
            if (hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs) != 0) continue;
            for (i = 0; i < 8u; i++) {
                snprintf(name, sizeof name, "g%u.dat", i);
                if (hype_exfat_create(&g_fs, name, &f) != 0) break;
                if (hype_exfat_append(&f, name, (unsigned)strlen(name)) != 0) break;
            }
        }
    }
    g_read_countdown = -1;
    g_write_countdown = -1;
    CHECK("growth fault sweep completed without crashing", 1);
}

/*
 * A volume whose allocation bitmap spans more than one sector. Every real medium
 * hype writes to is like this (a 512 MiB stick with 512-byte clusters has 254
 * bitmap sectors), and it is the only way to exercise the allocator's
 * sector-to-sector advance, its wrap-around, and the partial final sector.
 */
static void test_multi_sector_bitmap(void) {
    hype_exfat_wfile_t f;
    static uint8_t buf[SECSZ];
    uint32_t i;

    build_vol_big();
    CHECK_HEX("big mount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("big cluster count", BIG_CLUSTERS, g_fs.cluster_count);
    CHECK_HEX("big bitmap spans two sectors", 625u, (unsigned)g_fs.bitmap_bytes);
    CHECK_HEX("big used clusters", 4u, g_fs.used_clusters); /* 2 bitmap + upcase + root */

    /* Every cluster described by the FIRST bitmap sector is taken, so the
     * allocator has to step into the second one. */
    for (i = 0; i < 4096u; i++) bit_mark(2u + i, 1);
    CHECK_HEX("remount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("create ok", 0, hype_exfat_create(&g_fs, "second.dat", &f));
    CHECK_HEX("append ok", 0, hype_exfat_append(&f, buf, 4u));
    CHECK("allocated out of the second bitmap sector", f.first_cluster >= 2u + 4096u);
    CHECK_HEX("its bit is set", 1, bit_used(f.first_cluster));

    /* Now fill the second sector too, leaving only a cluster the search has to
     * wrap around to find. The hint sits past the end of the last (partial)
     * bitmap sector's valid bits, so the search must skip that sector and wrap. */
    build_vol_big();
    for (i = 0; i < BIG_CLUSTERS; i++) bit_mark(2u + i, 1);
    bit_mark(2u + 10u, 0); /* one free cluster, early in the FIRST sector */
    CHECK_HEX("remount ok", 0, hype_exfat_fs_mount(vol_read, vol_write, 0, &g_fs));
    CHECK_HEX("used clusters", BIG_CLUSTERS - 1u, g_fs.used_clusters);
    /* Drive the hint into the last bitmap sector by allocating and freeing there. */
    CHECK_HEX("create ok", 0, hype_exfat_create(&g_fs, "wrap.dat", &f));
    CHECK_HEX("append finds the wrapped-to cluster", 0, hype_exfat_append(&f, buf, 4u));
    CHECK_HEX("allocated the only free cluster", 12u, f.first_cluster);
    CHECK_HEX("volume now full", -1, hype_exfat_append(&f, buf, SECSZ));
    CHECK_HEX("sync ok", 0, hype_exfat_fs_sync(&g_fs));
    CHECK_HEX("PercentInUse is 100", 100u, g_vol[0x70]);
    build_vol(); /* leave the small layout in place for the tests that follow */
}


/* exFAT's fallback must be the 1980 epoch, never zero: month/day are 1-based
 * there, so an all-zero timestamp is out of spec and trips fsck. */
static void test_exfat_set_time(void) {
    hype_exfat_fs_t fs;
    hype_rtc_time_t now;
    uint8_t ent[32];
    uint32_t ts;

    now.year = 2026; now.month = 7; now.day = 28;
    now.hour = 14; now.minute = 35; now.second = 6;

    fs.now.year = 0;
    hype_exfat_fs_set_time(&fs, &now);
    CHECK_HEX("exfat set_time stored", 2026, fs.now.year);
    hype_exfat_fs_set_time(&fs, 0);
    CHECK_HEX("exfat set_time(0) invalidates", 0, fs.now.year);
    hype_exfat_fs_set_time(0, &now); /* must not crash */

    hype_exfat_file_entry(ent, HYPE_EXFAT_ATTR_ARCHIVE, 2u, &now);
    ts = (uint32_t)ent[8] | ((uint32_t)ent[9] << 8) | ((uint32_t)ent[10] << 16) |
         ((uint32_t)ent[11] << 24);
    CHECK_HEX("exfat entry stamp", hype_exfat_encode_timestamp(&now), ts);

    hype_exfat_file_entry(ent, HYPE_EXFAT_ATTR_ARCHIVE, 2u, 0);
    ts = (uint32_t)ent[8] | ((uint32_t)ent[9] << 8) | ((uint32_t)ent[10] << 16) |
         ((uint32_t)ent[11] << 24);
    CHECK_HEX("exfat entry no clock -> epoch", 0x00210000u, ts);
}

int main(void) {
    test_exfat_set_time();
    test_multi_sector_bitmap();
    test_bad_allocations();
    test_corrupt_chains();
    test_contiguous_dir_end_of_heap();
    test_volume_length_guard();
    test_mount_already_dirty();
    test_directory_chain_leaves_heap();
    test_growth_fault_sweep();
    test_mount();
    test_mount_rejections();
    test_mount_critical_structures();
    test_mount_two_fats();
    test_create_append();
    test_truncate();
    test_long_names();
    test_name_rejections();
    test_lookup();
    test_write_at();
    test_contiguous_grow();
    test_root_growth();
    test_set_across_cluster_boundary();
    test_volume_full();
    test_read_only_mount();
    test_recreate_odd_shaped_set();
    test_io_failures();
    test_fault_sweep();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
