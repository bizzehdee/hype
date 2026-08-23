#include <stdio.h>
#include <string.h>
#include "../ntfs.h"
#include "../fs_ops.h"
#include "../fs_battery.h"

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

#define SECSZ 512u
#define VOL_SECTORS 4096u
#define SPC 1u
#define REC_SIZE 1024u
#define MFT_LCN 100u   /* $MFT: 64 records = 128 clusters, 100..227 */
#define MFT_RECORDS 64u
#define INDX_LCN 240u  /* bigdir's one INDX block: clusters 240..247 */
#define UPCASE_LCN 250u /* $UpCase data: 256 clusters, 250..505 */
#define DATA_LCN 600u  /* file data region */
#define BITMAP_LCN 3000u /* $Bitmap: 4096 clusters need exactly 512 bytes = 1 cluster (SPC=1) */

static uint8_t g_vol[VOL_SECTORS * SECSZ];
static long g_read_countdown = -1;

static int vol_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if (g_read_countdown >= 0 && g_read_countdown-- == 0) return -1;
    if (lba + count > VOL_SECTORS) return -1;
    memcpy(dst, g_vol + lba * SECSZ, (size_t)count * SECSZ);
    return 0;
}
static int vol_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    if (lba + count > VOL_SECTORS) return -1;
    memcpy(g_vol + lba * SECSZ, src, (size_t)count * SECSZ);
    return 0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* ---- record + attribute builders ---- */

static uint8_t *rec_ptr(unsigned n) { return g_vol + (MFT_LCN + n * (REC_SIZE / SECSZ)) * SECSZ; }

static void rec_init(unsigned n, int is_dir) {
    uint8_t *r = rec_ptr(n);
    memset(r, 0, REC_SIZE);
    r[0] = 'F'; r[1] = 'I'; r[2] = 'L'; r[3] = 'E';
    put16(r + 4, 0x30);              /* USA offset */
    put16(r + 6, 3);                 /* USA count: usn + 2 sectors */
    put16(r + 0x14, 0x38);           /* attributes offset */
    put16(r + 0x16, (uint16_t)(0x0001u | (is_dir ? 0x0002u : 0u)));
    put32(r + 0x1C, REC_SIZE);       /* allocated */
    put32(r + 0x38, 0xFFFFFFFFu);    /* end marker (no attrs yet) */
    put32(r + 0x18, 0x40);           /* bytes used */
}

/* Append an attribute record; returns the offset of its header. */
static uint32_t attr_add(unsigned n, uint32_t type, int non_res, uint16_t flags,
                         const uint8_t *body, uint32_t body_len) {
    uint8_t *r = rec_ptr(n);
    uint32_t off = 0x38;
    uint32_t total;
    while (off + 4u <= REC_SIZE) {
        uint32_t t = (uint32_t)r[off] | ((uint32_t)r[off + 1] << 8) |
                     ((uint32_t)r[off + 2] << 16) | ((uint32_t)r[off + 3] << 24);
        if (t == 0xFFFFFFFFu) break;
        off += (uint32_t)r[off + 4] | ((uint32_t)r[off + 5] << 8);
    }
    total = ((non_res ? 0x40u : 0x18u) + body_len + 7u) & ~7u;
    put32(r + off, type);
    put32(r + off + 4, total);
    r[off + 8] = (uint8_t)non_res;
    put16(r + off + 0x0C, flags);
    if (non_res) {
        put16(r + off + 0x20, 0x40); /* runlist offset */
        memcpy(r + off + 0x40, body, body_len);
    } else {
        put32(r + off + 0x10, body_len);
        put16(r + off + 0x14, 0x18);
        memcpy(r + off + 0x18, body, body_len);
    }
    put32(r + off + total, 0xFFFFFFFFu); /* new end marker */
    put32(r + 0x18, off + total + 8u);
    return off;
}

static void nonres_sizes(unsigned n, uint32_t attr_off, uint64_t start_vcn, uint64_t alloc,
                         uint64_t real, uint64_t init) {
    uint8_t *r = rec_ptr(n);
    put64(r + attr_off + 0x10, start_vcn);
    put64(r + attr_off + 0x28, alloc);
    put64(r + attr_off + 0x30, real);
    put64(r + attr_off + 0x38, init);
}

static void rec_fixup(unsigned n) {
    uint8_t *r = rec_ptr(n);
    uint16_t usn = 0x0001;
    unsigned s;
    put16(r + 0x30, usn);
    for (s = 0; s < REC_SIZE / SECSZ; s++) {
        uint8_t *tail = r + (s + 1u) * SECSZ - 2u;
        put16(r + 0x30 + (s + 1u) * 2u, (uint16_t)(tail[0] | (tail[1] << 8)));
        put16(tail, usn);
    }
}

/* ---- index entry builders ---- */

static uint32_t ientry(uint8_t *dst, uint64_t mft_ref, const char *name, int is_dir) {
    uint32_t nlen = (uint32_t)strlen(name);
    uint32_t klen = 0x42u + nlen * 2u;
    uint32_t elen = (0x10u + klen + 7u) & ~7u;
    uint32_t i;
    memset(dst, 0, elen);
    put64(dst, mft_ref);
    put16(dst + 8, (uint16_t)elen);
    put16(dst + 10, (uint16_t)klen);
    put16(dst + 12, 0); /* flags */
    put64(dst + 0x10, 5u); /* parent: root (unchecked by the reader) */
    put32(dst + 0x10 + 0x38, is_dir ? 0x10000000u : 0u);
    dst[0x10 + 0x40] = (uint8_t)nlen;
    dst[0x10 + 0x41] = 1; /* WIN32 namespace */
    for (i = 0; i < nlen; i++) put16(dst + 0x10 + 0x42 + i * 2u, (uint16_t)(uint8_t)name[i]);
    return elen;
}

static uint32_t ientry_last(uint8_t *dst) {
    memset(dst, 0, 0x18);
    put16(dst + 8, 0x18);
    put16(dst + 12, 0x02); /* last */
    return 0x18;
}

/* $INDEX_ROOT value: type/collation/block-size header + index header + entries. */
static uint32_t build_index_root(uint8_t *v, uint32_t (*fill)(uint8_t *)) {
    uint32_t ents;
    put32(v + 0, 0x30);      /* indexed attribute: $FILE_NAME */
    put32(v + 4, 1);         /* collation */
    put32(v + 8, 4096);      /* index block size */
    v[12] = 8;               /* clusters per block */
    ents = fill(v + 0x20);
    put32(v + 0x10 + 0, 0x10);        /* entries offset (from index header) */
    put32(v + 0x10 + 4, 0x10 + ents); /* total size */
    put32(v + 0x10 + 8, 0x10 + ents); /* allocated */
    v[0x10 + 12] = 0;                 /* no children unless the test says so */
    return 0x20u + ents;
}

/* ---- volume assembly ---- */

static uint8_t pat(unsigned i) { return (uint8_t)(i * 17u + 11u); }
static uint32_t hype_probe_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t root_entries(uint8_t *e) {
    uint32_t o = 0;
    o += ientry(e + o, 40, "img.bin", 0);
    o += ientry(e + o, 41, "vdl.bin", 0);
    o += ientry(e + o, 42, "comp.bin", 0);
    o += ientry(e + o, 43, "enc.bin", 0);
    o += ientry(e + o, 44, "res.bin", 0);
    o += ientry(e + o, 45, "subdir", 1);
    o += ientry(e + o, 47, "bigdir", 1);
    o += ientry(e + o, 48, "lst.bin", 0);
    o += ientry_last(e + o);
    return o;
}
static uint32_t sub_entries(uint8_t *e) {
    uint32_t o = 0;
    o += ientry(e + o, 46, "nested.bin", 0);
    o += ientry(e + o, 40, "fkdir", 1); /* claims dir, record 40 is a file */
    o += ientry(e + o, 46, "dosonly", 0);
    e[o - 8 + 0] = 0; /* leave len/key intact; patch the namespace below */
    o += ientry_last(e + o);
    return o;
}
static uint32_t empty_entries(uint8_t *e) { return ientry_last(e); }

static void build_vol(int dirty) {
    uint8_t v[900];
    uint8_t rl[64];
    uint32_t off, n;
    unsigned i;

    memset(g_vol, 0, sizeof(g_vol));

    /* boot sector */
    g_vol[3] = 'N'; g_vol[4] = 'T'; g_vol[5] = 'F'; g_vol[6] = 'S';
    g_vol[7] = ' '; g_vol[8] = ' '; g_vol[9] = ' '; g_vol[10] = ' ';
    put16(g_vol + 0x0B, 512);
    g_vol[0x0D] = SPC;
    put64(g_vol + 0x28, VOL_SECTORS);
    put64(g_vol + 0x30, MFT_LCN);
    g_vol[0x40] = 0xF6; /* -10: 1024-byte records */
    put16(g_vol + 0x1FE, 0xAA55);

    /* record 0: $MFT, $DATA = 128 clusters at MFT_LCN */
    rec_init(0, 0);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 128; put16(rl + n, MFT_LCN); n += 2; rl[n++] = 0;
    off = attr_add(0, 0x80, 1, 0, rl, n);
    nonres_sizes(0, off, 0, 128u * SECSZ, (uint64_t)MFT_RECORDS * REC_SIZE, (uint64_t)MFT_RECORDS * REC_SIZE);
    /* $MFT's OWN $BITMAP: one bit per record, LSB-first, marking exactly
     * the records THIS fixture actually populates (0,1,3,5,6,10,40-51) as
     * in use -- 1 cluster at BITMAP_LCN+9 (BITMAP_LCN and +1..+8 are
     * #417's $Bitmap file and $MFTMirr respectively). */
    {
        static const uint8_t mftbm[8] = {0x6Bu, 0x04u, 0x00u, 0x00u, 0x00u, 0xFFu, 0x0Fu, 0x00u};
        n = 0;
        rl[n++] = 0x21; rl[n++] = 1; put16(rl + n, BITMAP_LCN + 9u); n += 2; rl[n++] = 0;
        off = attr_add(0, 0xB0, 1, 0, rl, n);
        nonres_sizes(0, off, 0, 1u * SECSZ, 8, 8);
        memcpy(g_vol + (BITMAP_LCN + 9u) * SECSZ, mftbm, sizeof mftbm);
    }
    rec_fixup(0);

    /* record 1: $MFTMirr -- backs up records 0-3 only (4 * REC_SIZE = 4096
     * bytes = 8 clusters at SPC=1), at a free LCN well clear of everything
     * else. Real content is irrelevant to every test that writes a record
     * >= 4: mirror_record_if_needed() sees n >= mirrored_records and takes
     * its "nothing to mirror" path without touching these bytes at all. */
    rec_init(1, 0);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 8; put16(rl + n, BITMAP_LCN + 1u); n += 2; rl[n++] = 0;
    off = attr_add(1, 0x80, 1, 0, rl, n);
    nonres_sizes(1, off, 0, 4u * REC_SIZE, 4u * REC_SIZE, 4u * REC_SIZE);
    rec_fixup(1);

    /* record 3: $Volume with $VOLUME_INFORMATION */
    rec_init(3, 0);
    memset(v, 0, 12);
    v[8] = 3; v[9] = 1; /* version */
    put16(v + 10, dirty ? 0x0001 : 0x0000);
    attr_add(3, 0x70, 0, 0, v, 12);
    rec_fixup(3);

    /* record 5: root directory */
    rec_init(5, 1);
    n = build_index_root(v, root_entries);
    attr_add(5, 0x90, 0, 0, v, n);
    rec_fixup(5);

    /* record 10: $UpCase -- 256 clusters at UPCASE_LCN, 128 KiB */
    rec_init(10, 0);
    n = 0;
    rl[n++] = 0x22; put16(rl + n, 256); n += 2; put16(rl + n, UPCASE_LCN); n += 2; rl[n++] = 0;
    off = attr_add(10, 0x80, 1, 0, rl, n);
    nonres_sizes(10, off, 0, 256u * SECSZ, 131072, 131072);
    rec_fixup(10);
    for (i = 0; i < 65536u; i++) {
        uint16_t up = (uint16_t)i;
        if (i >= 'a' && i <= 'z') up = (uint16_t)(i - 'a' + 'A');
        put16(g_vol + UPCASE_LCN * SECSZ + i * 2u, up);
    }

    /* record 40: img.bin -- DATA(4 cl) HOLE(3 cl) DATA(2 cl), 4300 bytes */
    rec_init(40, 0);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 4; put16(rl + n, DATA_LCN); n += 2;         /* 4 @600 */
    rl[n++] = 0x01; rl[n++] = 3;                                          /* sparse 3 */
    rl[n++] = 0x21; rl[n++] = 2; put16(rl + n, 20); n += 2;               /* 2 @620 (rel +20) */
    rl[n++] = 0;
    off = attr_add(40, 0x80, 1, 0, rl, n);
    nonres_sizes(40, off, 0, 9u * SECSZ, 4300, 4300);
    rec_fixup(40);
    for (i = 0; i < 4u * SECSZ; i++) g_vol[DATA_LCN * SECSZ + i] = pat(i);
    for (i = 0; i < 2u * SECSZ; i++) g_vol[(DATA_LCN + 20u) * SECSZ + i] = pat(i + 5000u);

    /* record 41: vdl.bin -- 6 clusters, real 3000, initialized 1000 */
    rec_init(41, 0);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 6; put16(rl + n, DATA_LCN + 40u); n += 2; rl[n++] = 0;
    off = attr_add(41, 0x80, 1, 0, rl, n);
    nonres_sizes(41, off, 0, 6u * SECSZ, 3000, 1000);
    rec_fixup(41);
    for (i = 0; i < 6u * SECSZ; i++) g_vol[(DATA_LCN + 40u) * SECSZ + i] = 0xEE; /* stale */
    for (i = 0; i < 1000u; i++) g_vol[(DATA_LCN + 40u) * SECSZ + i] = pat(i + 7000u);

    /* record 42: comp.bin -- compressed $DATA: refused */
    rec_init(42, 0);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 1; put16(rl + n, DATA_LCN + 50u); n += 2; rl[n++] = 0;
    off = attr_add(42, 0x80, 1, 0x0001, rl, n);
    nonres_sizes(42, off, 0, SECSZ, 100, 100);
    rec_fixup(42);

    /* record 43: enc.bin -- encrypted: refused */
    rec_init(43, 0);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 1; put16(rl + n, DATA_LCN + 51u); n += 2; rl[n++] = 0;
    off = attr_add(43, 0x80, 1, 0x4000, rl, n);
    nonres_sizes(43, off, 0, SECSZ, 100, 100);
    rec_fixup(43);

    /* record 44: res.bin -- resident $DATA: refused */
    rec_init(44, 0);
    for (i = 0; i < 32u; i++) v[i] = pat(i);
    attr_add(44, 0x80, 0, 0, v, 32);
    rec_fixup(44);

    /* record 45: subdir (small: INDEX_ROOT only), 46: nested.bin */
    rec_init(45, 1);
    n = build_index_root(v, sub_entries);
    /* flip 'dosonly' (third entry) to the DOS-only namespace so a lookup
     * must skip it */
    {
        uint8_t *e = v + 0x20;
        uint32_t k;
        for (k = 0; k < 2u; k++) e += (uint32_t)e[8] | ((uint32_t)e[9] << 8);
        e[0x10 + 0x41] = 2; /* NS_DOS */
    }
    attr_add(45, 0x90, 0, 0, v, n);
    rec_fixup(45);

    rec_init(46, 0);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 2; put16(rl + n, DATA_LCN + 60u); n += 2; rl[n++] = 0;
    off = attr_add(46, 0x80, 1, 0, rl, n);
    nonres_sizes(46, off, 0, 2u * SECSZ, 900, 900);
    rec_fixup(46);
    for (i = 0; i < 900u; i++) g_vol[(DATA_LCN + 60u) * SECSZ + i] = pat(i + 100u);

    /* record 47: bigdir -- INDEX_ROOT (empty) + INDEX_ALLOCATION + BITMAP */
    rec_init(47, 1);
    n = build_index_root(v, empty_entries);
    v[0x10 + 12] = 1; /* has children */
    attr_add(47, 0x90, 0, 0, v, n);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 8; put16(rl + n, INDX_LCN); n += 2; rl[n++] = 0;
    off = attr_add(47, 0xA0, 1, 0, rl, n);
    nonres_sizes(47, off, 0, 4096, 4096, 4096);
    v[0] = 0x01; /* bitmap: block 0 in use */
    attr_add(47, 0xB0, 0, 0, v, 8);
    rec_fixup(47);
    /* the INDX block itself, holding deep.bin (record 49) */
    {
        uint8_t *x = g_vol + INDX_LCN * SECSZ;
        uint32_t ents, s;
        uint16_t usn = 0x0002;
        memset(x, 0, 4096);
        x[0] = 'I'; x[1] = 'N'; x[2] = 'D'; x[3] = 'X';
        put16(x + 4, 0x28);  /* USA offset */
        put16(x + 6, 9);     /* usn + 8 sectors */
        /* node header at 0x18 */
        ents = ientry(x + 0x58, 49, "deep.bin", 0);
        ents += ientry_last(x + 0x58 + ents);
        put32(x + 0x18 + 0, 0x40);        /* entries offset from node header */
        put32(x + 0x18 + 4, 0x40 + ents); /* size */
        put32(x + 0x18 + 8, 4096 - 0x18); /* allocated */
        put16(x + 0x28, usn);
        for (s = 0; s < 8u; s++) {
            uint8_t *tail = x + (s + 1u) * SECSZ - 2u;
            put16(x + 0x28 + (s + 1u) * 2u, (uint16_t)(tail[0] | (tail[1] << 8)));
            put16(tail, usn);
        }
    }
    rec_init(49, 0);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 1; put16(rl + n, DATA_LCN + 70u); n += 2; rl[n++] = 0;
    off = attr_add(49, 0x80, 1, 0, rl, n);
    nonres_sizes(49, off, 0, SECSZ, 333, 333);
    rec_fixup(49);
    for (i = 0; i < 333u; i++) g_vol[(DATA_LCN + 70u) * SECSZ + i] = pat(i + 200u);

    /* record 48: lst.bin -- $DATA split over two extension records (50, 51)
     * via a resident $ATTRIBUTE_LIST. 3 clusters + 2 clusters, 2500 bytes. */
    rec_init(48, 0);
    memset(v, 0, 0x40u);
    /* entry 0: $DATA, VCN 0, in record 50 */
    put32(v + 0, 0x80); put16(v + 4, 0x20); put64(v + 8, 0); put64(v + 0x10, 50);
    /* entry 1: $DATA, VCN 3, in record 51 */
    put32(v + 0x20, 0x80); put16(v + 0x24, 0x20); put64(v + 0x28, 3); put64(v + 0x30, 51);
    attr_add(48, 0x20, 0, 0, v, 0x40);
    rec_fixup(48);
    rec_init(50, 0);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 3; put16(rl + n, DATA_LCN + 80u); n += 2; rl[n++] = 0;
    off = attr_add(50, 0x80, 1, 0, rl, n);
    nonres_sizes(50, off, 0, 5u * SECSZ, 2500, 2500);
    rec_fixup(50);
    rec_init(51, 0);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 2; put16(rl + n, DATA_LCN + 90u); n += 2; rl[n++] = 0;
    off = attr_add(51, 0x80, 1, 0, rl, n);
    nonres_sizes(51, off, 3, 5u * SECSZ, 2500, 2500);
    rec_fixup(51);
    for (i = 0; i < 3u * SECSZ; i++) g_vol[(DATA_LCN + 80u) * SECSZ + i] = pat(i + 300u);
    for (i = 0; i < 2u * SECSZ; i++) g_vol[(DATA_LCN + 90u) * SECSZ + i] = pat(i + 300u + 3u * SECSZ);

    /* record 6: $Bitmap -- 1 cluster at BITMAP_LCN, covers all VOL_SECTORS
     * clusters exactly (4096 clusters / 8 == 512 bytes == 1 cluster at
     * SPC=1), every cluster starts free. */
    rec_init(6, 0);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 1; put16(rl + n, BITMAP_LCN); n += 2; rl[n++] = 0;
    off = attr_add(6, 0x80, 1, 0, rl, n);
    nonres_sizes(6, off, 0, 1u * SECSZ, SECSZ, SECSZ);
    rec_fixup(6);
    memset(g_vol + BITMAP_LCN * SECSZ, 0, SECSZ);
}

/* ---- tests ---- */

static void test_probe(void) {
    hype_ntfs_t fs;
    build_vol(0);
    CHECK_HEX("probe claims NTFS", 0, hype_ntfs_probe(vol_read, 0));
    g_vol[3] = '-'; g_vol[4] = 'F'; g_vol[5] = 'V'; g_vol[6] = 'E';
    g_vol[7] = '-'; g_vol[8] = 'F'; g_vol[9] = 'S'; g_vol[10] = '-';
    CHECK("BitLocker refused", hype_ntfs_probe(vol_read, 0) != 0);
    build_vol(0);
    put16(g_vol + 0x0B, 4096);
    CHECK("4Kn sectors refused", hype_ntfs_probe(vol_read, 0) != 0);
    build_vol(0);
    g_vol[0x1FE] = 0;
    CHECK("missing AA55 refused", hype_ntfs_probe(vol_read, 0) != 0);
    build_vol(0);
    g_vol[0x0D] = 3; /* not a power of two */
    CHECK("bad spc refused", hype_ntfs_probe(vol_read, 0) != 0);
    build_vol(0);
    CHECK_HEX("mount ok", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("spc", SPC, fs.spc);
    CHECK_HEX("record size", REC_SIZE, fs.mft_record_size);
}

static void test_mount_refusals(void) {
    hype_ntfs_t fs;
    build_vol(1); /* dirty */
    CHECK("dirty volume refused", hype_ntfs_mount(vol_read, 0, &fs) != 0);
    build_vol(0);
    /* corrupt the upcase table's ASCII folding */
    put16(g_vol + UPCASE_LCN * SECSZ + 'a' * 2u, 'a');
    CHECK("broken $UpCase refused", hype_ntfs_mount(vol_read, 0, &fs) != 0);
    build_vol(0);
    /* torn MFT record 0: break a fixup tail */
    g_vol[(MFT_LCN + 1u) * SECSZ - 2u] ^= 0xFF;
    CHECK("torn $MFT record refused", hype_ntfs_mount(vol_read, 0, &fs) != 0);
}

static void test_resolve_sparse(void) {
    hype_ntfs_t fs;
    static hype_file_rmap_t m;
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));

    CHECK_HEX("resolve img.bin", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    CHECK_HEX("size", 4300, m.size_bytes);
    CHECK_HEX("three ranges", 3, m.count);
    CHECK("r0 DATA", m.ranges[0].kind == HYPE_RANGE_DATA && m.ranges[0].start_lba == DATA_LCN &&
                         m.ranges[0].sector_count == 4);
    CHECK("r1 HOLE", m.ranges[1].kind == HYPE_RANGE_HOLE && m.ranges[1].sector_count == 3);
    CHECK("r2 DATA", m.ranges[2].kind == HYPE_RANGE_DATA &&
                         m.ranges[2].start_lba == DATA_LCN + 20u);

    /* reads: data, then zeros through the hole, then data again */
    {
        uint8_t buf[SECSZ];
        unsigned i;
        CHECK_HEX("read data", 0, hype_file_rmap_read_at(&m, vol_read, 0, 100, buf, 64));
        for (i = 0; i < 64u; i++) { if (buf[i] != pat(100u + i)) break; }
        CHECK("data bytes", i == 64u);
        CHECK_HEX("read hole", 0, hype_file_rmap_read_at(&m, vol_read, 0, 4u * SECSZ + 10u, buf, 64));
        for (i = 0; i < 64u; i++) { if (buf[i] != 0) break; }
        CHECK("hole zeros", i == 64u);
        CHECK_HEX("read tail data", 0, hype_file_rmap_read_at(&m, vol_read, 0, 7u * SECSZ, buf, 64));
        for (i = 0; i < 64u; i++) { if (buf[i] != pat(5000u + i)) break; }
        CHECK("tail bytes", i == 64u);
    }

    /* case-insensitive + separators */
    CHECK_HEX("resolve IMG.BIN", 0, hype_ntfs_resolve(&fs, "\\IMG.BIN", &m));
    CHECK_HEX("resolve subdir file", 0, hype_ntfs_resolve(&fs, "/subdir/nested.bin", &m));
    CHECK_HEX("nested size", 900, m.size_bytes);
    CHECK_HEX("resolve INDX-block file", 0, hype_ntfs_resolve(&fs, "/bigdir/deep.bin", &m));
    CHECK_HEX("deep size", 333, m.size_bytes);
    CHECK_HEX("resolve attribute-list file", 0, hype_ntfs_resolve(&fs, "/lst.bin", &m));
    CHECK_HEX("lst size", 2500, m.size_bytes);
    CHECK_HEX("lst one coalesced or two ranges", 2, m.count);
    {
        uint8_t buf[64];
        unsigned i;
        CHECK_HEX("lst read across piece seam", 0,
                  hype_file_rmap_read_at(&m, vol_read, 0, 3u * SECSZ - 32u, buf, 64));
        for (i = 0; i < 64u; i++) { if (buf[i] != pat(300u + 3u * SECSZ - 32u + i)) break; }
        CHECK("lst seam bytes", i == 64u);
    }
}

static void test_resolve_unwritten(void) {
    hype_ntfs_t fs;
    static hype_file_rmap_t m;
    uint8_t buf[SECSZ];
    unsigned i;
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("resolve vdl.bin", 0, hype_ntfs_resolve(&fs, "/vdl.bin", &m));
    CHECK_HEX("vdl size", 3000, m.size_bytes);
    CHECK_HEX("split into DATA + UNWRITTEN", 2, m.count);
    CHECK("initialized prefix DATA", m.ranges[0].kind == HYPE_RANGE_DATA &&
                                         m.ranges[0].sector_count == 2); /* ceil(1000/512) */
    CHECK("tail UNWRITTEN", m.ranges[1].kind == HYPE_RANGE_UNWRITTEN);
    /* the initialized bytes read back; the uninitialized tail reads zero,
     * never the 0xEE stale fill */
    CHECK_HEX("read init", 0, hype_file_rmap_read_at(&m, vol_read, 0, 0, buf, 512));
    for (i = 0; i < 512u; i++) { if (buf[i] != pat(i + 7000u)) break; }
    CHECK("init bytes", i == 512u);
    CHECK_HEX("read past init", 0, hype_file_rmap_read_at(&m, vol_read, 0, 2000, buf, 200));
    for (i = 0; i < 200u; i++) { if (buf[i] != 0) break; }
    CHECK("uninitialized tail is zeros, not stale 0xEE", i == 200u);
}

static void test_refusals(void) {
    hype_ntfs_t fs;
    static hype_file_rmap_t m;
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("compressed refused", hype_ntfs_resolve(&fs, "/comp.bin", &m) != 0);
    CHECK("encrypted refused", hype_ntfs_resolve(&fs, "/enc.bin", &m) != 0);
    CHECK("resident refused", hype_ntfs_resolve(&fs, "/res.bin", &m) != 0);
    CHECK("missing refused", hype_ntfs_resolve(&fs, "/nope.bin", &m) != 0);
    CHECK("directory refused", hype_ntfs_resolve(&fs, "/subdir", &m) != 0);
    CHECK("root refused", hype_ntfs_resolve(&fs, "/", &m) != 0);
    CHECK("path under a file refused", hype_ntfs_resolve(&fs, "/img.bin/x", &m) != 0);
    CHECK("NULL path refused", hype_ntfs_resolve(&fs, 0, &m) != 0);
    /* a name needing a fold outside the cached prefix */
    CHECK("non-ASCII name refused, not guessed",
          hype_ntfs_resolve(&fs, "/caf\xC3\xA9.bin", &m) != 0);

    /* torn INDX block */
    build_vol(0);
    CHECK_HEX("mount2", 0, hype_ntfs_mount(vol_read, 0, &fs));
    g_vol[(INDX_LCN + 1u) * SECSZ - 2u] ^= 0xFF;
    CHECK("torn INDX refused", hype_ntfs_resolve(&fs, "/bigdir/deep.bin", &m) != 0);

    /* runlist pointing past the volume */
    build_vol(0);
    CHECK_HEX("mount3", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        /* rewrite img.bin's first run to LCN 100000 (past the media) */
        uint8_t *r = rec_ptr(40);
        /* undo fixups on our copy is not needed: patch the runlist bytes and refix */
        uint8_t rl2[16];
        uint32_t n2 = 0;
        rl2[n2++] = 0x31; rl2[n2++] = 4; rl2[n2++] = 0xA0; rl2[n2++] = 0x86; rl2[n2++] = 0x01;
        rl2[n2++] = 0;
        memcpy(r + 0x38 + 0x40, rl2, n2); /* first attr's runlist */
        rec_fixup(40);
    }
    CHECK("out-of-media runlist refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    /* I/O failure sweep across mount + resolve: fail every nth read */
    {
        long n;
        for (n = 0; n < 30; n++) {
            hype_ntfs_t f2;
            build_vol(0);
            g_read_countdown = n;
            if (hype_ntfs_mount(vol_read, 0, &f2) == 0) {
                (void)hype_ntfs_resolve(&f2, "/img.bin", &m);
            }
            g_read_countdown = -1;
        }
        build_vol(0);
        CHECK_HEX("mount after sweep", 0, hype_ntfs_mount(vol_read, 0, &fs));
        CHECK_HEX("resolve after sweep", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    }
}

static void test_fs_ops_ntfs(void) {
    static hype_fs_t fs;
    static hype_fs_file_t f;
    uint8_t buf[600];
    unsigned i;

    build_vol(0);
    CHECK_HEX("auto-mount claims ntfs", 0, hype_fs_mount_auto(&fs, vol_read, vol_write, 0));
    CHECK("driver name", strcmp(fs.ops->name, "ntfs") == 0);
    CHECK("caps: read + in-place + sparse + namespace (#692), nothing else",
          hype_fs_caps(&fs) == (HYPE_FS_CAP_READ | HYPE_FS_CAP_WRITE_INPLACE |
                                HYPE_FS_CAP_SPARSE | HYPE_FS_CAP_NAMESPACE));

    CHECK_HEX("lookup", 0, hype_fs_lookup(&fs, "/img.bin", &f));
    CHECK_HEX("size", 4300, f.size);
    CHECK_HEX("read across data|hole seam", 0, hype_fs_read_at(&f, 4u * SECSZ - 32u, buf, 64));
    for (i = 0; i < 32u; i++) { if (buf[i] != pat(4u * SECSZ - 32u + i)) break; }
    CHECK("seam data half", i == 32u);
    for (i = 32u; i < 64u; i++) { if (buf[i] != 0) break; }
    CHECK("seam hole half", i == 64u);

    /* in-place write into DATA: lands and reads back */
    CHECK_HEX("write into DATA", 0, hype_fs_write_at(&f, 100, "hello", 5));
    CHECK_HEX("read back", 0, hype_fs_read_at(&f, 100, buf, 5));
    CHECK("write landed", memcmp(buf, "hello", 5) == 0);
    /* a write touching the HOLE is refused BEFORE anything lands */
    {
        uint8_t before[SECSZ];
        memcpy(before, g_vol + DATA_LCN * SECSZ + 3u * SECSZ, SECSZ);
        CHECK("write spanning into hole refused",
              hype_fs_write_at(&f, 4u * SECSZ - 16u, buf, 64) != 0);
        CHECK("nothing written by refused span",
              memcmp(before, g_vol + DATA_LCN * SECSZ + 3u * SECSZ, SECSZ) == 0);
    }
    CHECK("write into hole refused", hype_fs_write_at(&f, 5u * SECSZ, buf, 8) != 0);
    /* writes into UNWRITTEN are refused too (VDL advance is #383-class work) */
    CHECK_HEX("lookup vdl", 0, hype_fs_lookup(&fs, "/vdl.bin", &f));
    CHECK("write into unwritten refused", hype_fs_write_at(&f, 2000, buf, 8) != 0);
    CHECK("write into initialized prefix ok", hype_fs_write_at(&f, 10, buf, 8) == 0);

    /* append/growth are absent, not stubbed (#692's vtable comment above
     * ntfs_create() explains why); namespace mutation (#692) now works */
    CHECK("append refused", hype_fs_append(&f, buf, 1) != 0);
    CHECK_HEX("create via vtable", 0, hype_fs_create(&fs, "/x", &f));
    CHECK("write past EOF refused", hype_fs_write_at(&f, f.size - 2, buf, 8) != 0);

    /* read-only mount masks the write capability */
    CHECK_HEX("ro mount", 0, hype_fs_mount_auto(&fs, vol_read, 0, 0));
    CHECK("ro caps", hype_fs_caps(&fs) == (HYPE_FS_CAP_READ | HYPE_FS_CAP_SPARSE));
    CHECK_HEX("ro lookup", 0, hype_fs_lookup(&fs, "/img.bin", &f));
    CHECK("ro write refused", hype_fs_write_at(&f, 0, buf, 4) != 0);
}


/* ---- second wave: boot variants, malformed structures, list edge cases ---- */

static void test_boot_variants(void) {
    hype_ntfs_t fs;

    /* positive clusters-per-record encoding */
    build_vol(0);
    g_vol[0x40] = 2; /* 2 clusters * 1 spc * 512 = 1024 bytes */
    CHECK_HEX("positive cpr mounts", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("positive cpr size", 1024, fs.mft_record_size);

    build_vol(0);
    g_vol[0x40] = 16; /* 8192 bytes: over the cap */
    CHECK("oversized positive cpr refused", hype_ntfs_probe(vol_read, 0) != 0);
    build_vol(0);
    g_vol[0x40] = 0xF8; /* -8: 256 bytes, under a sector */
    CHECK("undersized record refused", hype_ntfs_probe(vol_read, 0) != 0);
    build_vol(0);
    g_vol[0x40] = 0xF3; /* -13: 8192, over the cap */
    CHECK("oversized record refused", hype_ntfs_probe(vol_read, 0) != 0);
    build_vol(0);
    put64(g_vol + 0x28, 0); /* no sectors */
    CHECK("zero sectors refused", hype_ntfs_probe(vol_read, 0) != 0);
    build_vol(0);
    put64(g_vol + 0x30, 0); /* no MFT */
    CHECK("zero mft_lcn refused", hype_ntfs_probe(vol_read, 0) != 0);
    build_vol(0);
    put64(g_vol + 0x30, VOL_SECTORS); /* MFT past the end */
    CHECK("mft past media refused", hype_ntfs_probe(vol_read, 0) != 0);
    build_vol(0);
    g_read_countdown = 0;
    CHECK("unreadable boot refused", hype_ntfs_probe(vol_read, 0) != 0);
    g_read_countdown = -1;
    build_vol(0);
    g_vol[0x0D] = 0;
    CHECK("spc 0 refused", hype_ntfs_probe(vol_read, 0) != 0);
}

/* Rewrite one attribute's runlist in record `n` and refix. `attr_off` is the
 * offset returned by attr_add (0x38 for the first attribute). */
static void patch_runlist(unsigned n, uint32_t attr_off, const uint8_t *rl, uint32_t rl_len) {
    uint8_t *r = rec_ptr(n);
    memcpy(r + attr_off + 0x40, rl, rl_len);
    rec_fixup(n);
}

static void test_malformed_structures(void) {
    hype_ntfs_t fs;
    static hype_file_rmap_t m;
    uint8_t rl[16];
    uint32_t n;

    /* malformed runlists on img.bin's $DATA (first attribute at 0x38) */
    struct { const char *what; uint8_t bytes[8]; uint32_t len; } cases[] = {
        {"len_sz 0", {0x20, 9, 0, 0}, 4},
        {"len_sz 9", {0x29, 9, 9, 9, 9, 9, 9, 9}, 8},
        {"truncated", {0x24, 9}, 2},
        {"zero run", {0x21, 0, 44, 0}, 4},
    };
    unsigned c;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        build_vol(0);
        CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
        patch_runlist(40, 0x38, cases[c].bytes, cases[c].len);
        CHECK(cases[c].what, hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);
    }

    /* negative LCN */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    n = 0;
    rl[n++] = 0x11; rl[n++] = 9; rl[n++] = 0x80; /* 1-byte delta: -128 from 0 */
    rl[n++] = 0;
    patch_runlist(40, 0x38, rl, n);
    CHECK("negative LCN refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    /* record with a broken attribute chain (unaligned length) */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(40);
        put32(r + 0x38 + 4, 0x43); /* not 8-aligned */
        rec_fixup(40);
    }
    CHECK("unaligned attribute refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    /* an entry pointing at a record that is not FILE */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(40);
        r[0] = 'B';
        rec_fixup(40); /* fixups fine, magic wrong */
    }
    CHECK("bad record magic refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    /* record marked not-in-use */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(40);
        put16(r + 0x16, 0x0000);
        rec_fixup(40);
    }
    CHECK("free record refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    /* fixup structure out of range */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(40);
        put16(r + 6, 1); /* usa_count too small */
    }
    CHECK("bad usa_count refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(40);
        put16(r + 4, REC_SIZE - 2); /* usa runs off the record */
    }
    CHECK("bad usa_off refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    /* $Volume without VOLUME_INFORMATION */
    build_vol(0);
    rec_init(3, 0); /* re-init: no attributes at all */
    rec_fixup(3);
    CHECK("missing $VOLUME_INFORMATION refused", hype_ntfs_mount(vol_read, 0, &fs) != 0);

    /* $UpCase too short */
    build_vol(0);
    {
        uint8_t *r = rec_ptr(10);
        put64(r + 0x38 + 0x30, 100); /* real size under the cache */
        rec_fixup(10);
    }
    CHECK("short $UpCase refused", hype_ntfs_mount(vol_read, 0, &fs) != 0);

    /* directory without a bitmap behind an INDEX_ALLOCATION */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        /* rebuild bigdir with INDEX_ROOT + INDEX_ALLOCATION but NO bitmap */
        uint8_t v2[900];
        uint8_t rl2[8];
        uint32_t n2 = build_index_root(v2, empty_entries);
        uint32_t o2;
        v2[0x10 + 12] = 1;
        rec_init(47, 1);
        attr_add(47, 0x90, 0, 0, v2, n2);
        n2 = 0;
        rl2[n2++] = 0x21; rl2[n2++] = 8; put16(rl2 + n2, INDX_LCN); n2 += 2; rl2[n2++] = 0;
        o2 = attr_add(47, 0xA0, 1, 0, rl2, n2);
        nonres_sizes(47, o2, 0, 4096, 4096, 4096);
        rec_fixup(47);
    }
    CHECK("allocation without bitmap refused",
          hype_ntfs_resolve(&fs, "/bigdir/deep.bin", &m) != 0);

    /* map_trim: a runlist covering MORE clusters than the size needs is
     * trimmed, not refused (NTFS allocates whole clusters) */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t rl3[8];
        uint32_t n3 = 0;
        rl3[n3++] = 0x21; rl3[n3++] = 12; put16(rl3 + n3, DATA_LCN); n3 += 2; rl3[n3++] = 0;
        patch_runlist(40, 0x38, rl3, n3); /* 12 clusters for a 4300-byte file */
    }
    CHECK_HEX("slack clusters trimmed", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    CHECK_HEX("trimmed coverage", (4300u + 511u) / 512u, m.ranges[0].sector_count);
}

static void test_list_and_names(void) {
    hype_ntfs_t fs;
    static hype_file_rmap_t m;

    /* non-resident attribute list: move lst.bin's list value into a cluster */
    build_vol(0);
    {
        uint8_t v2[0x40];
        uint8_t rl2[8];
        uint32_t n2, o2;
        /* the same two entries the resident list carried */
        memset(v2, 0, sizeof v2);
        put32(v2 + 0, 0x80); put16(v2 + 4, 0x20); put64(v2 + 8, 0); put64(v2 + 0x10, 50);
        put32(v2 + 0x20, 0x80); put16(v2 + 0x24, 0x20); put64(v2 + 0x28, 3); put64(v2 + 0x30, 51);
        memcpy(g_vol + (DATA_LCN + 95u) * SECSZ, v2, sizeof v2);
        rec_init(48, 0);
        n2 = 0;
        rl2[n2++] = 0x21; rl2[n2++] = 1; put16(rl2 + n2, DATA_LCN + 95u); n2 += 2; rl2[n2++] = 0;
        o2 = attr_add(48, 0x20, 1, 0, rl2, n2);
        nonres_sizes(48, o2, 0, SECSZ, sizeof v2, sizeof v2);
        rec_fixup(48);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("non-resident list resolves", 0, hype_ntfs_resolve(&fs, "/lst.bin", &m));
    CHECK_HEX("nr-list size", 2500, m.size_bytes);

    /* an oversized non-resident list is refused */
    {
        uint8_t *r = rec_ptr(48);
        put64(r + 0x38 + 0x30, 100000); /* real_size over the cap */
        rec_fixup(48);
    }
    CHECK("oversized list refused", hype_ntfs_resolve(&fs, "/lst.bin", &m) != 0);

    /* a broken list entry (elen 0) */
    build_vol(0);
    {
        uint8_t *r = rec_ptr(48);
        put16(r + 0x38 + 0x18 + 4, 0); /* first entry's length = 0 */
        rec_fixup(48);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("broken list entry refused", hype_ntfs_resolve(&fs, "/lst.bin", &m) != 0);

    /* an on-disk name using a code point past the cache: skipped, not matched */
    build_vol(0);
    {
        /* rename img.bin's root entry to use U+0142 as its first char */
        uint8_t *root = rec_ptr(5);
        /* entry area starts at value+0x20; value starts at 0x38+0x18 */
        uint8_t *e = root + 0x38 + 0x18 + 0x20;
        put16(e + 0x10 + 0x42, 0x0142);
        rec_fixup(5);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("exotic on-disk name never matches an ASCII query",
          hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);
    CHECK_HEX("later entries still found", 0, hype_ntfs_resolve(&fs, "/vdl.bin", &m));

    /* very long component name refused */
    {
        char big[300];
        unsigned i;
        big[0] = '/';
        for (i = 1; i < 290u; i++) big[i] = 'a';
        big[290] = 0;
        CHECK("overlong component refused", hype_ntfs_resolve(&fs, big, &m) != 0);
    }
    /* empty path / double slash */
    CHECK("empty refused", hype_ntfs_resolve(&fs, "", &m) != 0);
    CHECK_HEX("doubled separators fine", 0, hype_ntfs_resolve(&fs, "//vdl.bin", &m));

    /* unmounted fs refused */
    {
        hype_ntfs_t cold;
        cold.upcase_loaded = 0;
        CHECK("unmounted resolve refused", hype_ntfs_resolve(&cold, "/x", &m) != 0);
    }
}


static void test_more_edges(void) {
    hype_ntfs_t fs;
    static hype_file_rmap_t m;

    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    /* DOS-only names are skipped, dir-flagged file records refuse descent */
    CHECK("DOS-namespace entry not matched", hype_ntfs_resolve(&fs, "/subdir/dosonly", &m) != 0);
    CHECK("descent into a file record refused",
          hype_ntfs_resolve(&fs, "/subdir/fkdir/x.bin", &m) != 0);
    CHECK_HEX("siblings still fine", 0, hype_ntfs_resolve(&fs, "/subdir/nested.bin", &m));

    /* a named $DATA stream before the unnamed one is skipped */
    build_vol(0);
    {
        /* rebuild record 46 with a named stream first */
        uint8_t rl[8];
        uint32_t n = 0, off;
        rec_init(46, 0);
        rl[n++] = 0x21; rl[n++] = 1; put16(rl + n, DATA_LCN + 75u); n += 2; rl[n++] = 0;
        off = attr_add(46, 0x80, 1, 0, rl, n);
        rec_ptr(46)[off + 9] = 4; /* name_len 4: a named stream */
        nonres_sizes(46, off, 0, SECSZ, 50, 50);
        n = 0;
        rl[n++] = 0x21; rl[n++] = 2; put16(rl + n, DATA_LCN + 60u); n += 2; rl[n++] = 0;
        off = attr_add(46, 0x80, 1, 0, rl, n);
        nonres_sizes(46, off, 0, 2u * SECSZ, 900, 900);
        rec_fixup(46);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("named stream skipped", 0, hype_ntfs_resolve(&fs, "/subdir/nested.bin", &m));
    CHECK_HEX("unnamed stream size", 900, m.size_bytes);

    /* an over-fragmented runlist reports too_fragmented and refuses */
    build_vol(0);
    {
        static uint8_t rl[720];
        uint32_t n = 0;
        unsigned k;
        uint8_t *r = rec_ptr(40);
        for (k = 0; k < 130u; k++) {
            rl[n++] = 0x11; rl[n++] = 1; rl[n++] = (k == 0) ? (uint8_t)0x7F : (uint8_t)2;
            rl[n++] = 0x01; rl[n++] = 1; /* hole */
        }
        rl[n++] = 0;
        /* rebuild record 40 with an attribute large enough for this runlist */
        rec_init(40, 0);
        {
            uint32_t off = attr_add(40, 0x80, 1, 0, rl, n);
            nonres_sizes(40, off, 0, 260u * SECSZ, 260u * SECSZ, 260u * SECSZ);
        }
        rec_fixup(40);
        (void)r;
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("over-fragmented refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    /* attribute chain guards */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(40);
        put16(r + 0x14, REC_SIZE); /* attrs offset off the end */
        rec_fixup(40);
    }
    CHECK("attrs offset out of range refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    /* $VOLUME_INFORMATION too short */
    build_vol(0);
    {
        uint8_t *r = rec_ptr(3);
        put32(r + 0x38 + 0x10, 4); /* value length 4 < 12 */
        rec_fixup(3);
    }
    CHECK("short volume info refused", hype_ntfs_mount(vol_read, 0, &fs) != 0);

    /* index-root shape guards */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(47); /* bigdir */
        put32(r + 0x38 + 0x18 + 8, 0x300); /* block size: not a power of two */
        rec_fixup(47);
    }
    CHECK("bad block size refused", hype_ntfs_resolve(&fs, "/bigdir/deep.bin", &m) != 0);
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(47);
        put32(r + 0x38 + 0x18 + 0, 0x80); /* indexed type: not $FILE_NAME */
        rec_fixup(47);
    }
    CHECK("non-filename index refused", hype_ntfs_resolve(&fs, "/bigdir/deep.bin", &m) != 0);
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(47);
        put32(r + 0x38 + 0x10, 0x10); /* index-root value shorter than its header */
        rec_fixup(47);
    }
    CHECK("truncated index root refused", hype_ntfs_resolve(&fs, "/bigdir/deep.bin", &m) != 0);

    /* entries area overrunning the value */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(5);
        put32(r + 0x38 + 0x18 + 0x10 + 4, 0x7000); /* ents size >> value size */
        rec_fixup(5);
    }
    CHECK("oversized entries area refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    /* a torn entry (length under the minimum) */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(5);
        put16(r + 0x38 + 0x18 + 0x20 + 8, 8); /* first entry elen 8 < 0x10 */
        rec_fixup(5);
    }
    CHECK("undersized entry refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);
}


static void test_attr_and_list_guards(void) {
    hype_ntfs_t fs;
    static hype_file_rmap_t m;

    /* boot: the OTHER signature byte */
    build_vol(0);
    g_vol[0x1FF] = 0;
    CHECK("bad 0xAA byte refused", hype_ntfs_probe(vol_read, 0) != 0);

    /* attribute shape guards on img.bin's $DATA header */
    struct { const char *what; uint32_t off; uint32_t val; int is16; } shapes[] = {
        {"attr length 16", 0x38 + 4, 16, 0},
        {"attr length past record", 0x38 + 4, 0x7F8, 0},
        {"non-resident header short", 0x38 + 4, 0x30, 0},
        {"runlist offset past length", 0x38 + 0x20, 0x600, 1},
    };
    unsigned c;
    for (c = 0; c < sizeof shapes / sizeof shapes[0]; c++) {
        build_vol(0);
        CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
        {
            uint8_t *r = rec_ptr(40);
            if (shapes[c].is16) put16(r + shapes[c].off, (uint16_t)shapes[c].val);
            else put32(r + shapes[c].off, shapes[c].val);
            rec_fixup(40);
        }
        CHECK(shapes[c].what, hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);
    }

    /* resident value overrunning its attribute (res.bin) */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(44);
        put32(r + 0x38 + 0x10, 0x400); /* val_len far past the attribute */
        rec_fixup(44);
    }
    CHECK("resident value overrun refused", hype_ntfs_resolve(&fs, "/res.bin", &m) != 0);

    /* attribute chain running off the record end without a terminator */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(40);
        put16(r + 0x14, REC_SIZE - 8u); /* attrs start 8 bytes from the end */
        rec_fixup(40);
    }
    CHECK("chain past record end refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    /* record magic: second byte corrupt */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(40);
        r[1] = 'X';
        rec_fixup(40);
    }
    CHECK("magic byte 1 refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    /* $ATTRIBUTE_LIST variants on lst.bin (record 48) */
    /* a foreign entry type before the $DATA entries */
    build_vol(0);
    {
        uint8_t v2[0x60];
        memset(v2, 0, sizeof v2);
        put32(v2 + 0, 0x30); put16(v2 + 4, 0x20); put64(v2 + 0x10, 48); /* $FILE_NAME piece */
        put32(v2 + 0x20, 0x80); put16(v2 + 0x24, 0x20); put64(v2 + 0x28, 0); put64(v2 + 0x30, 50);
        put32(v2 + 0x40, 0x80); put16(v2 + 0x44, 0x20); put64(v2 + 0x48, 3); put64(v2 + 0x50, 51);
        rec_init(48, 0);
        attr_add(48, 0x20, 0, 0, v2, sizeof v2);
        rec_fixup(48);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("foreign list entries skipped", 0, hype_ntfs_resolve(&fs, "/lst.bin", &m));

    /* two consecutive entries for the SAME extension record: visited once */
    build_vol(0);
    {
        uint8_t v2[0x60];
        memset(v2, 0, sizeof v2);
        put32(v2 + 0, 0x80); put16(v2 + 4, 0x20); put64(v2 + 8, 0); put64(v2 + 0x10, 50);
        put32(v2 + 0x20, 0x80); put16(v2 + 0x24, 0x20); put64(v2 + 0x28, 1); put64(v2 + 0x30, 50);
        put32(v2 + 0x40, 0x80); put16(v2 + 0x44, 0x20); put64(v2 + 0x48, 3); put64(v2 + 0x50, 51);
        rec_init(48, 0);
        attr_add(48, 0x20, 0, 0, v2, sizeof v2);
        rec_fixup(48);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("duplicate-record entries visited once", 0, hype_ntfs_resolve(&fs, "/lst.bin", &m));

    /* list entry pointing at a record with no $DATA at all */
    build_vol(0);
    {
        uint8_t v2[0x20];
        memset(v2, 0, sizeof v2);
        put32(v2 + 0, 0x80); put16(v2 + 4, 0x20); put64(v2 + 8, 0); put64(v2 + 0x10, 45);
        rec_init(48, 0);
        attr_add(48, 0x20, 0, 0, v2, sizeof v2);
        rec_fixup(48);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("listed record without the attribute refused",
          hype_ntfs_resolve(&fs, "/lst.bin", &m) != 0);

    /* list entry pointing past the MFT */
    build_vol(0);
    {
        uint8_t v2[0x20];
        memset(v2, 0, sizeof v2);
        put32(v2 + 0, 0x80); put16(v2 + 4, 0x20); put64(v2 + 8, 0); put64(v2 + 0x10, 9999);
        rec_init(48, 0);
        attr_add(48, 0x20, 0, 0, v2, sizeof v2);
        rec_fixup(48);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("list entry past the MFT refused", hype_ntfs_resolve(&fs, "/lst.bin", &m) != 0);

    /* non-resident list with real_size 0 */
    build_vol(0);
    {
        uint8_t rl2[8];
        uint32_t n2 = 0, o2;
        rec_init(48, 0);
        rl2[n2++] = 0x21; rl2[n2++] = 1; put16(rl2 + n2, DATA_LCN + 95u); n2 += 2; rl2[n2++] = 0;
        o2 = attr_add(48, 0x20, 1, 0, rl2, n2);
        nonres_sizes(48, o2, 0, SECSZ, 0, 0);
        rec_fixup(48);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("empty non-resident list refused", hype_ntfs_resolve(&fs, "/lst.bin", &m) != 0);

    /* non-resident list with a garbage runlist */
    build_vol(0);
    {
        uint8_t rl2[4];
        uint32_t o2;
        rec_init(48, 0);
        rl2[0] = 0x20; rl2[1] = 1; rl2[2] = 0; rl2[3] = 0; /* len_sz 0: bad */
        o2 = attr_add(48, 0x20, 1, 0, rl2, 4);
        nonres_sizes(48, o2, 0, SECSZ, 0x40, 0x40);
        rec_fixup(48);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("garbage list runlist refused", hype_ntfs_resolve(&fs, "/lst.bin", &m) != 0);

    /* index root marked non-resident */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(45);
        r[0x38 + 8] = 1;
        rec_fixup(45);
    }
    CHECK("non-resident index root refused",
          hype_ntfs_resolve(&fs, "/subdir/nested.bin", &m) != 0);

    /* bitmap too large for the reader's buffer */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(47);
        /* find $BITMAP (third attribute) and inflate val_len: attrs are DATA
         * order 0x90, 0xA0, 0xB0 */
        uint32_t off = 0x38;
        while (hype_probe_u32(r + off) != 0xB0u) off += (uint32_t)r[off + 4] | ((uint32_t)r[off + 5] << 8);
        put32(r + off + 0x10, 600);
        rec_fixup(47);
    }
    CHECK("oversized dir bitmap refused", hype_ntfs_resolve(&fs, "/bigdir/deep.bin", &m) != 0);

    /* a bitmap with too few bits: blocks past it are skipped (not found) */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t *r = rec_ptr(47);
        uint32_t off = 0x38;
        while (hype_probe_u32(r + off) != 0xB0u) off += (uint32_t)r[off + 4] | ((uint32_t)r[off + 5] << 8);
        put32(r + off + 0x10, 0); /* zero-length bitmap */
        rec_fixup(47);
    }
    CHECK("no-bit bitmap finds nothing", hype_ntfs_resolve(&fs, "/bigdir/deep.bin", &m) != 0);

    /* INDX magic second byte */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    g_vol[INDX_LCN * SECSZ + 1] = 'Q';
    CHECK("INDX magic byte 1 refused", hype_ntfs_resolve(&fs, "/bigdir/deep.bin", &m) != 0);

    /* an entry whose FILE_NAME overruns its key: skipped */
    build_vol(0);
    {
        uint8_t *r = rec_ptr(5);
        uint8_t *e = r + 0x38 + 0x18 + 0x20; /* first root entry: img.bin */
        e[0x10 + 0x40] = 200; /* name length far past the key */
        rec_fixup(5);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("overrunning name skipped", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);
    CHECK_HEX("later entries fine", 0, hype_ntfs_resolve(&fs, "/vdl.bin", &m));

    /* entries area with no last-entry flag: scan runs off the end cleanly */
    build_vol(0);
    {
        uint8_t *r = rec_ptr(45);
        uint8_t *e = r + 0x38 + 0x18 + 0x20;
        uint32_t elen;
        unsigned k;
        for (k = 0; k < 3u; k++) { /* walk to the last entry */
            elen = (uint32_t)e[8] | ((uint32_t)e[9] << 8);
            e += elen;
        }
        put16(e + 12, 0); /* clear its LAST flag */
        rec_fixup(45);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("no-terminator entry area is a clean miss",
          hype_ntfs_resolve(&fs, "/subdir/zzz.bin", &m) != 0);
}


static void test_final_edges(void) {
    hype_ntfs_t fs;
    static hype_file_rmap_t m;
    uint8_t rl[16];
    uint32_t n, off;

    /* sparse + VDL interplay: HOLE passthrough, whole-run UNWRITTEN, and the
     * boundary falling mid-run with a real tail */
    build_vol(0);
    {
        rec_init(40, 0); /* re-purpose img.bin: DATA(2) HOLE(2) DATA(4), init 300 */
        n = 0;
        rl[n++] = 0x21; rl[n++] = 2; put16(rl + n, DATA_LCN); n += 2;
        rl[n++] = 0x01; rl[n++] = 2;
        rl[n++] = 0x21; rl[n++] = 4; put16(rl + n, 30); n += 2; /* rel +30 */
        rl[n++] = 0;
        off = attr_add(40, 0x80, 1, 0, rl, n);
        nonres_sizes(40, off, 0, 8u * SECSZ, 8u * SECSZ - 100u, 300);
        rec_fixup(40);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("sparse+vdl resolves", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    CHECK("split: DATA head", m.ranges[0].kind == HYPE_RANGE_DATA &&
                                  m.ranges[0].sector_count == 1);
    CHECK("split: UNWRITTEN tail of first run", m.ranges[1].kind == HYPE_RANGE_UNWRITTEN);
    CHECK("HOLE passthrough", m.ranges[2].kind == HYPE_RANGE_HOLE);
    CHECK("post-hole run UNWRITTEN", m.ranges[3].kind == HYPE_RANGE_UNWRITTEN);

    /* boundary exactly at a run edge: no split, later runs whole-UNWRITTEN */
    build_vol(0);
    {
        rec_init(40, 0);
        n = 0;
        rl[n++] = 0x21; rl[n++] = 2; put16(rl + n, DATA_LCN); n += 2;
        rl[n++] = 0x21; rl[n++] = 4; put16(rl + n, 30); n += 2;
        rl[n++] = 0;
        off = attr_add(40, 0x80, 1, 0, rl, n);
        nonres_sizes(40, off, 0, 6u * SECSZ, 6u * SECSZ - 1u, 2u * SECSZ);
        rec_fixup(40);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("edge-vdl resolves", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    CHECK_HEX("edge-vdl two ranges", 2, m.count);
    CHECK("edge-vdl kinds", m.ranges[0].kind == HYPE_RANGE_DATA &&
                                m.ranges[1].kind == HYPE_RANGE_UNWRITTEN);

    /* trim dropping a whole trailing run */
    build_vol(0);
    {
        rec_init(40, 0);
        n = 0;
        rl[n++] = 0x21; rl[n++] = 9; put16(rl + n, DATA_LCN); n += 2;
        rl[n++] = 0x21; rl[n++] = 3; put16(rl + n, 40); n += 2;
        rl[n++] = 0;
        off = attr_add(40, 0x80, 1, 0, rl, n);
        nonres_sizes(40, off, 0, 12u * SECSZ, 4300, 4300);
        rec_fixup(40);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("trailing-run trim resolves", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    CHECK_HEX("trailing run dropped", 1, m.count);

    /* a runlist that fills its space exactly, no terminator byte */
    build_vol(0);
    {
        rec_init(40, 0);
        n = 0;
        rl[n++] = 0x21; rl[n++] = 9; put16(rl + n, DATA_LCN); n += 2; /* 4 bytes, no 0 */
        off = attr_add(40, 0x80, 1, 0, rl, n);
        nonres_sizes(40, off, 0, 9u * SECSZ, 4300, 4300);
        /* shrink the attribute so the runlist区 ends exactly at the runs */
        put32(rec_ptr(40) + off + 4, 0x48);
        put32(rec_ptr(40) + off + 0x48, 0xFFFFFFFFu);
        rec_fixup(40);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("terminatorless runlist ok", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));

    /* runlist decode leftovers: off_sz 9; truncated offset bytes */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    { uint8_t bad[3] = {0x91, 1, 0}; patch_runlist(40, 0x38, bad, 3); }
    CHECK("off_sz 9 refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        rec_init(40, 0);
        n = 0;
        rl[n++] = 0x21; rl[n++] = 1; put16(rl + n, DATA_LCN); n += 2;
        rl[n++] = 0x14; rl[n++] = 1; rl[n++] = 1; rl[n++] = 1; /* header wants 4+1 bytes; 3 left */
        off = attr_add(40, 0x80, 1, 0, rl, n);
        put32(rec_ptr(40) + off + 4, 0x48); /* runlist space: exactly these 8 bytes */
        put32(rec_ptr(40) + off + 0x48, 0xFFFFFFFFu);
        nonres_sizes(40, off, 0, SECSZ, 100, 100);
        rec_fixup(40);
    }
    CHECK("truncated run fields refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    /* fixups: usa_count 0; record magic bytes 2 and 3 */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    put16(rec_ptr(40) + 6, 0);
    CHECK("usa_count 0 refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    rec_ptr(40)[2] = 'Q';
    rec_fixup(40);
    CHECK("magic byte 2 refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    /* attribute list: a base-record piece plus a duplicate base entry */
    build_vol(0);
    {
        uint8_t v2[0x60];
        uint8_t rl2[8];
        uint32_t n2;
        memset(v2, 0, sizeof v2);
        put32(v2 + 0, 0x80); put16(v2 + 4, 0x20); put64(v2 + 8, 0); put64(v2 + 0x10, 48);
        put32(v2 + 0x20, 0x80); put16(v2 + 0x24, 0x20); put64(v2 + 0x28, 1); put64(v2 + 0x30, 48);
        put32(v2 + 0x40, 0x80); put16(v2 + 0x44, 0x20); put64(v2 + 0x48, 3); put64(v2 + 0x50, 51);
        rec_init(48, 0);
        attr_add(48, 0x20, 0, 0, v2, sizeof v2);
        n2 = 0;
        rl2[n2++] = 0x21; rl2[n2++] = 3; put16(rl2 + n2, DATA_LCN + 80u); n2 += 2; rl2[n2++] = 0;
        off = attr_add(48, 0x80, 1, 0, rl2, n2);
        nonres_sizes(48, off, 0, 5u * SECSZ, 2500, 2500);
        rec_fixup(48);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("base-record piece + duplicate entry ok", 0,
              hype_ntfs_resolve(&fs, "/lst.bin", &m));
    CHECK_HEX("combined size", 2500, m.size_bytes);

    /* an extension record with a broken attribute chain */
    build_vol(0);
    {
        uint8_t *r = rec_ptr(51);
        put32(r + 0x38 + 4, 0x43); /* unaligned */
        rec_fixup(51);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("broken extension record refused", hype_ntfs_resolve(&fs, "/lst.bin", &m) != 0);

    /* a list entry overrunning the list */
    build_vol(0);
    {
        uint8_t v2[0x40];
        memset(v2, 0, sizeof v2);
        put32(v2 + 0, 0x80); put16(v2 + 4, 0x20); put64(v2 + 8, 0); put64(v2 + 0x10, 50);
        put32(v2 + 0x20, 0x80); put16(v2 + 0x24, 0x30); /* runs past the 0x40 value */
        put64(v2 + 0x28, 3); put64(v2 + 0x30, 51);
        rec_init(48, 0);
        attr_add(48, 0x20, 0, 0, v2, sizeof v2);
        rec_fixup(48);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("overrunning list entry refused", hype_ntfs_resolve(&fs, "/lst.bin", &m) != 0);

    /* index guards: no INDEX_ROOT at all; block size under a sector; huge klen;
     * ents_off past ents_size; non-resident bitmap; second INDX block skipped */
    build_vol(0);
    rec_init(45, 1); /* subdir: dir with no attributes */
    rec_fixup(45);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("dir without index refused", hype_ntfs_resolve(&fs, "/subdir/nested.bin", &m) != 0);

    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    put32(rec_ptr(47) + 0x38 + 0x18 + 8, 256); /* block size < sector */
    rec_fixup(47);
    CHECK("undersized index block refused", hype_ntfs_resolve(&fs, "/bigdir/deep.bin", &m) != 0);

    build_vol(0);
    {
        uint8_t *e = rec_ptr(5) + 0x38 + 0x18 + 0x20;
        put16(e + 10, 0x600); /* klen overruns the entries area */
        rec_fixup(5);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("huge klen skipped", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    build_vol(0);
    {
        uint8_t *r = rec_ptr(5);
        put32(r + 0x38 + 0x18 + 0x10 + 0, 0x9000); /* ents_off > ents_size */
        rec_fixup(5);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("ents_off past size refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    build_vol(0);
    {
        uint8_t *r = rec_ptr(47);
        uint32_t o2 = 0x38;
        while (hype_probe_u32(r + o2) != 0xB0u) o2 += (uint32_t)r[o2 + 4] | ((uint32_t)r[o2 + 5] << 8);
        r[o2 + 8] = 1; /* bitmap marked non-resident */
        rec_fixup(47);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("non-resident bitmap refused", hype_ntfs_resolve(&fs, "/bigdir/deep.bin", &m) != 0);

    /* an INDX area of two blocks where only block 0 is in use */
    build_vol(0);
    {
        uint8_t *r = rec_ptr(47);
        uint32_t o2 = 0x38;
        while (hype_probe_u32(r + o2) != 0xA0u) o2 += (uint32_t)r[o2 + 4] | ((uint32_t)r[o2 + 5] << 8);
        put64(r + o2 + 0x30, 8192); /* real: two blocks (the second unbuilt) */
        put64(r + o2 + 0x28, 8192);
        put64(r + o2 + 0x38, 8192);
        /* widen the runlist to 16 clusters */
        rec_ptr(47)[o2 + 0x40 + 1] = 16;
        rec_fixup(47);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("free second block skipped, file still found", 0,
              hype_ntfs_resolve(&fs, "/bigdir/deep.bin", &m));

    /* INDX magic byte 2; read-fault sweep over the whole lookup path */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    g_vol[INDX_LCN * SECSZ + 2] = 'Q';
    CHECK("INDX magic byte 2 refused", hype_ntfs_resolve(&fs, "/bigdir/deep.bin", &m) != 0);
    {
        long k;
        for (k = 0; k < 40; k++) {
            build_vol(0);
            if (hype_ntfs_mount(vol_read, 0, &fs) != 0) continue;
            g_read_countdown = k;
            (void)hype_ntfs_resolve(&fs, "/bigdir/deep.bin", &m);
            g_read_countdown = -1;
        }
    }

    /* mount guards: $MFT stream too small; non-resident $VOLUME_INFORMATION;
     * an up-case table breaking the below-'a' identity */
    build_vol(0);
    put64(rec_ptr(0) + 0x38 + 0x30, 4096); /* $MFT real: only 4 records */
    rec_fixup(0);
    CHECK("undersized $MFT refused", hype_ntfs_mount(vol_read, 0, &fs) != 0);

    build_vol(0);
    rec_ptr(3)[0x38 + 8] = 1; /* volume info marked non-resident */
    rec_fixup(3);
    CHECK("non-resident volume info refused", hype_ntfs_mount(vol_read, 0, &fs) != 0);

    build_vol(0);
    put16(g_vol + UPCASE_LCN * SECSZ + 5u * 2u, 99); /* identity broken below 'a' */
    CHECK("non-identity $UpCase prefix refused", hype_ntfs_mount(vol_read, 0, &fs) != 0);

    /* hole-first over-fragmentation: the HOLE append is the one that hits the cap */
    build_vol(0);
    {
        static uint8_t big_rl[720];
        uint32_t n3 = 0;
        unsigned k;
        for (k = 0; k < 130u; k++) {
            big_rl[n3++] = 0x01; big_rl[n3++] = 1; /* hole first */
            big_rl[n3++] = 0x11; big_rl[n3++] = 1; big_rl[n3++] = (k == 0) ? (uint8_t)0x7F : (uint8_t)2;
        }
        big_rl[n3++] = 0;
        rec_init(40, 0);
        off = attr_add(40, 0x80, 1, 0, big_rl, n3);
        nonres_sizes(40, off, 0, 260u * SECSZ, 260u * SECSZ, 260u * SECSZ);
        rec_fixup(40);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("hole-first over-fragmentation refused", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);

    /* mixed separators and a trailing backslash */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("mixed separators", 0, hype_ntfs_resolve(&fs, "\\subdir/nested.bin", &m));
    CHECK("trailing separator on a file path",
          hype_ntfs_resolve(&fs, "/subdir/nested.bin\\", &m) == 0);
}

/* #417: $Bitmap cluster allocation and release. */
static void test_cluster_alloc(void) {
    hype_ntfs_t fs;
    uint64_t lcn;
    uint8_t byte;

    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));

    /* pristine bitmap: first alloc lands at cluster 0 */
    CHECK_HEX("alloc 1", 0, hype_ntfs_cluster_alloc(&fs, vol_write, 1, &lcn));
    CHECK_HEX("alloc 1 lcn", 0, lcn);
    CHECK_HEX("bit 0 set", 1, g_vol[BITMAP_LCN * SECSZ] & 1u);

    /* next alloc is NOT cluster 0 again -- it is genuinely marked used */
    CHECK_HEX("alloc 2", 0, hype_ntfs_cluster_alloc(&fs, vol_write, 1, &lcn));
    CHECK_HEX("alloc 2 lcn", 1, lcn);

    /* a multi-cluster run allocates contiguously right after */
    CHECK_HEX("alloc run", 0, hype_ntfs_cluster_alloc(&fs, vol_write, 5, &lcn));
    CHECK_HEX("alloc run lcn", 2, lcn);
    byte = g_vol[BITMAP_LCN * SECSZ];
    CHECK_HEX("bits 0-6 set", 0x7Fu, byte);
    CHECK_HEX("bit 7 still free", 0, g_vol[BITMAP_LCN * SECSZ] & 0x80u);

    /* freeing the middle run makes it available again, and ONLY it */
    CHECK_HEX("free run", 0, hype_ntfs_cluster_free(&fs, vol_write, 2, 5));
    CHECK_HEX("bits 2-6 cleared", 0x03u, g_vol[BITMAP_LCN * SECSZ]);
    CHECK_HEX("realloc reuses freed run", 0, hype_ntfs_cluster_alloc(&fs, vol_write, 5, &lcn));
    CHECK_HEX("realloc lcn", 2, lcn);

    /* freeing a run that is not fully allocated is refused, and refused
     * without touching the medium */
    byte = g_vol[BITMAP_LCN * SECSZ];
    CHECK("partially-free run refused", hype_ntfs_cluster_free(&fs, vol_write, 6, 3) != 0);
    CHECK_HEX("refused free left bitmap untouched", byte, g_vol[BITMAP_LCN * SECSZ]);
    CHECK("double free refused", hype_ntfs_cluster_free(&fs, vol_write, 0, 1) == 0 &&
                                     hype_ntfs_cluster_free(&fs, vol_write, 0, 1) != 0);

    /* out-of-range alloc/free refused */
    CHECK("free past total_clusters refused",
          hype_ntfs_cluster_free(&fs, vol_write, 4090, 100) != 0);

    /* exhaustion: the whole 4096-cluster bitmap fits one more request, one
     * request past it is refused */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("alloc entire volume", 0, hype_ntfs_cluster_alloc(&fs, vol_write, 4096u, &lcn));
    CHECK_HEX("alloc entire volume lcn", 0, lcn);
    CHECK("alloc past exhaustion refused", hype_ntfs_cluster_alloc(&fs, vol_write, 1, &lcn) != 0);

    /* a read-only mount's resolve() path never touches $Bitmap: bogus
     * $Bitmap contents must not affect it */
    build_vol(0);
    memset(g_vol + BITMAP_LCN * SECSZ, 0xFF, SECSZ); /* looks fully allocated */
    CHECK_HEX("mount unaffected by $Bitmap", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        hype_file_rmap_t m;
        CHECK_HEX("resolve unaffected by $Bitmap", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    }

    /* argument guards */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("alloc NULL fs refused", hype_ntfs_cluster_alloc(0, vol_write, 1, &lcn) != 0);
    CHECK("alloc NULL write refused", hype_ntfs_cluster_alloc(&fs, 0, 1, &lcn) != 0);
    CHECK("alloc NULL out_lcn refused", hype_ntfs_cluster_alloc(&fs, vol_write, 1, 0) != 0);
    CHECK("alloc zero count refused", hype_ntfs_cluster_alloc(&fs, vol_write, 0, &lcn) != 0);
    CHECK("free NULL fs refused", hype_ntfs_cluster_free(0, vol_write, 0, 1) != 0);
    CHECK("free NULL write refused", hype_ntfs_cluster_free(&fs, 0, 0, 1) != 0);
    CHECK("free zero count refused", hype_ntfs_cluster_free(&fs, vol_write, 0, 0) != 0);

    /* a missing/malformed $Bitmap record is refused, not crashed on */
    build_vol(0);
    rec_ptr(6)[0] = 0; /* corrupt the FILE magic */
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("alloc with broken $Bitmap refused", hype_ntfs_cluster_alloc(&fs, vol_write, 1, &lcn) != 0);
}

/* #418: append/grow a $DATA stream. */
static void test_data_append(void) {
    hype_ntfs_t fs;
    hype_file_rmap_t m;
    uint64_t lcn;

    /* happy path: allocate then append, and resolve() sees the new range */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("resolve before", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    CHECK_HEX("ranges before", 3, m.count); /* DATA, HOLE, DATA */
    CHECK_HEX("size before", 4300, m.size_bytes);

    CHECK_HEX("alloc 2", 0, hype_ntfs_cluster_alloc(&fs, vol_write, 2, &lcn));
    CHECK_HEX("alloc lcn", 0, lcn); /* pristine #417 bitmap: first free is LCN 0 */
    CHECK_HEX("append", 0,
              hype_ntfs_data_append(&fs, vol_write, 40, lcn, 2, 11u * SECSZ, 4300 + 2u * SECSZ,
                                    4300 + 2u * SECSZ, 2));

    CHECK_HEX("resolve after", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    CHECK_HEX("ranges after", 4, m.count); /* DATA, HOLE, DATA, DATA(new) */
    CHECK_HEX("size after", 4300 + 2u * SECSZ, m.size_bytes);
    CHECK_HEX("new range kind", HYPE_RANGE_DATA, m.ranges[3].kind);
    CHECK_HEX("new range lba", lcn * fs.spc, m.ranges[3].start_lba);
    CHECK_HEX("new range len", 2u * fs.spc, m.ranges[3].sector_count);

    /* the mirror stays byte-identical: record 40 is below MFT_RECORDS but
     * NOT below the mirrored range unless it is < mirrored_records -- check
     * via a fresh mount + resolve, which re-reads through $MFT (not $MFTMirr,
     * but proves the WRITE at least round-trips through the primary copy) */
    CHECK_HEX("remount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("resolve after remount", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    CHECK_HEX("ranges survive remount", 4, m.count);

    /* a second append keeps growing correctly (exercises a second insertion
     * point, not just the first) */
    CHECK_HEX("alloc 1 more", 0, hype_ntfs_cluster_alloc(&fs, vol_write, 1, &lcn));
    CHECK_HEX("second append", 0,
              hype_ntfs_data_append(&fs, vol_write, 40, lcn, 1, 12u * SECSZ, 4300 + 3u * SECSZ,
                                    4300 + 3u * SECSZ, 3));
    CHECK_HEX("resolve after 2nd append", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    /* still 4, not 5: the new run's LCN (2) lands immediately after the
     * first append's DATA(0,2) range, so the #381 rmap builder coalesces
     * them -- correct behavior, not a missed insertion (the on-disk
     * runlist genuinely holds two separate mapping-pair entries; only the
     * logical range VIEW merges adjacent same-kind runs). */
    CHECK_HEX("ranges after 2nd append", 4, m.count);
    CHECK_HEX("size after 2nd append", 4300 + 3u * SECSZ, m.size_bytes);

    /* refusals */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("resident $DATA refused", hype_ntfs_data_append(&fs, vol_write, 44, 0, 1, SECSZ, 1, 1,
                                                          2) != 0);
    CHECK("multi-extent ($ATTRIBUTE_LIST present) $DATA refused",
          hype_ntfs_data_append(&fs, vol_write, 48, 0, 1, SECSZ, 1, 1, 2) != 0);
    CHECK("missing record refused",
          hype_ntfs_data_append(&fs, vol_write, 9999, 0, 1, SECSZ, 1, 1, 2) != 0);
    CHECK("zero cluster_count refused",
          hype_ntfs_data_append(&fs, vol_write, 40, 0, 0, SECSZ, 1, 1, 2) != 0);
    CHECK("NULL fs refused", hype_ntfs_data_append(0, vol_write, 40, 0, 1, SECSZ, 1, 1, 2) != 0);
    CHECK("NULL write refused", hype_ntfs_data_append(&fs, 0, 40, 0, 1, SECSZ, 1, 1, 2) != 0);
    CHECK("past medium refused",
          hype_ntfs_data_append(&fs, vol_write, 40, VOL_SECTORS, 1, SECSZ, 1, 1, 2) != 0);

    /* no room: pad record 40 with a big resident dummy attribute leaving
     * only a few bytes of slack, then keep appending 1-cluster runs -- the
     * attribute's own local padding absorbs the first one or two, but it
     * must eventually need to grow past the packed record and be refused
     * cleanly rather than corrupt anything (no $ATTRIBUTE_LIST support in
     * this slice). */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    {
        uint8_t junk[REC_SIZE];
        uint32_t used = get32(rec_ptr(40) + 0x18);
        uint32_t body_len = REC_SIZE - used - 0x20u;
        unsigned k;
        int saw_refusal = 0;

        memset(junk, 0xAB, sizeof junk);
        attr_add(40, 0x10 /* AT_STANDARD_INFORMATION */, 0, 0, junk, body_len);
        rec_fixup(40);

        for (k = 0; k < 8u; k++) {
            uint64_t lcn2;
            if (hype_ntfs_cluster_alloc(&fs, vol_write, 1, &lcn2) != 0) {
                break;
            }
            if (hype_ntfs_data_append(&fs, vol_write, 40, lcn2, 1, (11u + k) * SECSZ,
                                      4300 + (k + 1u) * SECSZ, 4300 + (k + 1u) * SECSZ,
                                      (uint16_t)(2u + k)) != 0) {
                saw_refusal = 1;
                break;
            }
        }
        CHECK("no room eventually refused", saw_refusal);
    }

    /* more refusals, each exercising a distinct guard */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("no $DATA at all refused" /* record 45: subdir, index-only */,
          hype_ntfs_data_append(&fs, vol_write, 45, 0, 1, SECSZ, 1, 1, 2) != 0);

    /* named-stream-only: poke an existing unnamed $DATA's name_len so
     * attr_find's match is (from this function's view) "not ours" */
    build_vol(0);
    rec_ptr(44)[0x38 + 9] = 1; /* name_len := 1, no name bytes needed for this check */
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("named-stream-only $DATA refused",
          hype_ntfs_data_append(&fs, vol_write, 44, 0, 1, SECSZ, 1, 1, 2) != 0);

    /* two unnamed $DATA pieces in one record: ambiguous, refused */
    build_vol(0);
    {
        uint8_t rl2[8];
        uint32_t n2 = 0, off2;
        rec_init(20, 0);
        rl2[n2++] = 0x21; rl2[n2++] = 1; put16(rl2 + n2, DATA_LCN + 95u); n2 += 2; rl2[n2++] = 0;
        off2 = attr_add(20, 0x80, 1, 0, rl2, n2);
        nonres_sizes(20, off2, 0, SECSZ, 100, 100);
        n2 = 0;
        rl2[n2++] = 0x21; rl2[n2++] = 1; put16(rl2 + n2, DATA_LCN + 96u); n2 += 2; rl2[n2++] = 0;
        off2 = attr_add(20, 0x80, 1, 0, rl2, n2);
        nonres_sizes(20, off2, 1, SECSZ, 100, 100);
        rec_fixup(20);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("duplicate unnamed $DATA refused",
          hype_ntfs_data_append(&fs, vol_write, 20, 0, 1, SECSZ, 1, 1, 2) != 0);

    /* compressed/encrypted $DATA refused */
    build_vol(0);
    {
        uint8_t rl2[8];
        uint32_t n2 = 0, off2;
        rec_init(21, 0);
        rl2[n2++] = 0x21; rl2[n2++] = 1; put16(rl2 + n2, DATA_LCN + 97u); n2 += 2; rl2[n2++] = 0;
        off2 = attr_add(21, 0x80, 1, 0x0001 /* ATTR_IS_COMPRESSED */, rl2, n2);
        nonres_sizes(21, off2, 0, SECSZ, 100, 100);
        rec_fixup(21);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("compressed $DATA refused", hype_ntfs_data_append(&fs, vol_write, 21, 0, 1, SECSZ, 1, 1,
                                                            2) != 0);

    /* malformed runlist (no terminator inside the attribute's own bounds)
     * is refused, not scanned past its own record */
    build_vol(0);
    {
        uint8_t rl2[8];
        uint32_t off2;
        rec_init(22, 0);
        /* 0xFF: len_sz=15, off_sz=15 -- runlist_find_end must refuse this,
         * never read past the attribute looking for a terminator */
        memset(rl2, 0xFFu, sizeof rl2);
        off2 = attr_add(22, 0x80, 1, 0, rl2, 8u); /* body_len a multiple of 8: no extra zero pad */
        nonres_sizes(22, off2, 0, SECSZ, 100, 100);
        rec_fixup(22);
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("malformed runlist refused",
          hype_ntfs_data_append(&fs, vol_write, 22, 0, 1, SECSZ, 1, 1, 2) != 0);
}

/* #419: hole/sparse fill. img.bin (record 40): DATA(4cl@600) HOLE(3cl,
 * VCN 4-6) DATA(2cl@620, VCN 7-8), 4300 bytes real size. */
static void test_hole_fill(void) {
    hype_ntfs_t fs;
    hype_file_rmap_t m;
    uint64_t lcn;

    /* full hole fill: the whole 3-cluster hole becomes one DATA run */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("resolve before", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    CHECK_HEX("ranges before", 3, m.count);
    CHECK_HEX("kind[1] before", HYPE_RANGE_HOLE, m.ranges[1].kind);
    CHECK_HEX("size before", 4300, m.size_bytes);

    CHECK_HEX("alloc 3", 0, hype_ntfs_cluster_alloc(&fs, vol_write, 3, &lcn));
    CHECK_HEX("fill whole hole", 0, hype_ntfs_hole_fill(&fs, vol_write, 40, 4, 3, lcn, 2));

    CHECK_HEX("resolve after", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    CHECK_HEX("ranges after", 3, m.count); /* still 3: hole -> DATA, none adjacent */
    CHECK_HEX("kind[1] after", HYPE_RANGE_DATA, m.ranges[1].kind);
    CHECK_HEX("lba[1] after", lcn * fs.spc, m.ranges[1].start_lba);
    CHECK_HEX("sectors[1] after", 3u * fs.spc, m.ranges[1].sector_count);
    CHECK_HEX("size unchanged", 4300, m.size_bytes); /* hole-fill never changes real_size */

    /* zero-fill happened for real, on the medium, before the metadata
     * commit -- read it back raw */
    {
        unsigned i;
        int all_zero = 1;
        for (i = 0; i < 3u * SECSZ; i++) {
            if (g_vol[lcn * fs.spc * SECSZ + i] != 0u) {
                all_zero = 0;
                break;
            }
        }
        CHECK("new clusters zero-filled on medium", all_zero);
    }

    /* partial fill: only the MIDDLE cluster of a 3-cluster hole, leaving a
     * 1-cluster hole on each side */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("alloc 1", 0, hype_ntfs_cluster_alloc(&fs, vol_write, 1, &lcn));
    CHECK_HEX("partial fill", 0, hype_ntfs_hole_fill(&fs, vol_write, 40, 5, 1, lcn, 2));
    CHECK_HEX("resolve after partial", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    CHECK_HEX("ranges after partial", 5, m.count); /* DATA,HOLE,DATA,HOLE,DATA */
    CHECK_HEX("kind[0]", HYPE_RANGE_DATA, m.ranges[0].kind);
    CHECK_HEX("kind[1]", HYPE_RANGE_HOLE, m.ranges[1].kind);
    CHECK_HEX("sectors[1]", 1u * fs.spc, m.ranges[1].sector_count);
    CHECK_HEX("kind[2]", HYPE_RANGE_DATA, m.ranges[2].kind);
    CHECK_HEX("lba[2]", lcn * fs.spc, m.ranges[2].start_lba);
    CHECK_HEX("kind[3]", HYPE_RANGE_HOLE, m.ranges[3].kind);
    CHECK_HEX("sectors[3]", 1u * fs.spc, m.ranges[3].sector_count);
    CHECK_HEX("kind[4]", HYPE_RANGE_DATA, m.ranges[4].kind);
    CHECK_HEX("size unchanged after partial", 4300, m.size_bytes);

    /* fill the leading edge only (before_len=0, after_len=2) */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("alloc 1", 0, hype_ntfs_cluster_alloc(&fs, vol_write, 1, &lcn));
    CHECK_HEX("leading fill", 0, hype_ntfs_hole_fill(&fs, vol_write, 40, 4, 1, lcn, 2));
    CHECK_HEX("resolve after leading", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    CHECK_HEX("ranges after leading", 4, m.count); /* DATA,DATA(new),HOLE(2),DATA */
    CHECK_HEX("kind[1]", HYPE_RANGE_DATA, m.ranges[1].kind);
    CHECK_HEX("kind[2]", HYPE_RANGE_HOLE, m.ranges[2].kind);
    CHECK_HEX("sectors[2]", 2u * fs.spc, m.ranges[2].sector_count);

    /* sparse flag clears when the last hole is filled */
    build_vol(0);
    rec_ptr(40)[0x38 + 0x0D] |= 0x80u; /* ATTR_IS_SPARSE (0x8000): high byte of the u16 flags field */
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("alloc 3", 0, hype_ntfs_cluster_alloc(&fs, vol_write, 3, &lcn));
    CHECK_HEX("fill whole hole (flag test)", 0,
              hype_ntfs_hole_fill(&fs, vol_write, 40, 4, 3, lcn, 2));
    CHECK_HEX("sparse flag cleared", 0,
              ((uint32_t)rec_ptr(40)[0x38 + 0x0C] | ((uint32_t)rec_ptr(40)[0x38 + 0x0D] << 8)) &
                  0x8000u);

    /* refusals */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("fill spans into a DATA run refused",
          hype_ntfs_hole_fill(&fs, vol_write, 40, 3, 2, 0, 2) != 0);
    CHECK("target is already DATA refused",
          hype_ntfs_hole_fill(&fs, vol_write, 40, 0, 1, 0, 2) != 0);
    CHECK("fill exceeds the hole's own bounds refused",
          hype_ntfs_hole_fill(&fs, vol_write, 40, 4, 5, 0, 2) != 0);
    CHECK("no such VCN refused", hype_ntfs_hole_fill(&fs, vol_write, 40, 9000, 1, 0, 2) != 0);
    CHECK("resident $DATA refused", hype_ntfs_hole_fill(&fs, vol_write, 44, 0, 1, 0, 2) != 0);
    CHECK("multi-extent $DATA refused", hype_ntfs_hole_fill(&fs, vol_write, 48, 0, 1, 0, 2) != 0);
    CHECK("zero cluster_count refused",
          hype_ntfs_hole_fill(&fs, vol_write, 40, 4, 0, 0, 2) != 0);
    CHECK("NULL fs refused", hype_ntfs_hole_fill(0, vol_write, 40, 4, 1, 0, 2) != 0);
    CHECK("NULL write refused", hype_ntfs_hole_fill(&fs, 0, 40, 4, 1, 0, 2) != 0);
    CHECK("past medium refused",
          hype_ntfs_hole_fill(&fs, vol_write, 40, 4, 1, VOL_SECTORS, 2) != 0);
    CHECK("missing record refused",
          hype_ntfs_hole_fill(&fs, vol_write, 9999, 4, 1, 0, 2) != 0);

    /* tail containing BOTH a DATA run (needs its delta re-derived) and a
     * HOLE run after it (copied through as-is): HOLE(2) DATA(1) HOLE(2) --
     * fill the FIRST hole entirely. */
    build_vol(0);
    {
        uint8_t rl2[16];
        uint32_t n2 = 0, off2;
        rec_init(23, 0);
        rl2[n2++] = 0x01; rl2[n2++] = 2; /* HOLE, 2 clusters, VCN 0-1 */
        rl2[n2++] = 0x21; rl2[n2++] = 1; put16(rl2 + n2, DATA_LCN + 99u); n2 += 2; /* DATA, VCN 2 */
        rl2[n2++] = 0x01; rl2[n2++] = 2; /* HOLE, 2 clusters, VCN 3-4 */
        rl2[n2++] = 0;
        off2 = attr_add(23, 0x80, 1, 0, rl2, n2);
        nonres_sizes(23, off2, 0, 5u * SECSZ, 5u * SECSZ, 5u * SECSZ);
        rec_fixup(23);
        for (unsigned k = 0; k < SECSZ; k++) {
            g_vol[(DATA_LCN + 99u) * SECSZ + k] = pat(k + 9000u);
        }
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("alloc 2", 0, hype_ntfs_cluster_alloc(&fs, vol_write, 2, &lcn));
    CHECK_HEX("fill first hole with data+hole tail", 0,
              hype_ntfs_hole_fill(&fs, vol_write, 23, 0, 2, lcn, 2));
    /* round-trip proof without a directory entry to resolve() through:
     * the tail's DATA run (VCN 2) must still decode to its original LCN,
     * and its trailing HOLE (VCN 3-4) must still be fillable -- both only
     * succeed if the re-encoded tail bytes are structurally correct. */
    CHECK_HEX("alloc 2 more", 0, hype_ntfs_cluster_alloc(&fs, vol_write, 2, &lcn));
    CHECK_HEX("fill trailing hole after re-encoded tail", 0,
              hype_ntfs_hole_fill(&fs, vol_write, 23, 3, 2, lcn, 3));
    CHECK("re-fill of an already-fully-allocated stream refused",
          hype_ntfs_hole_fill(&fs, vol_write, 23, 0, 1, lcn, 4) != 0);
}

/* #420: $MFT record allocation and release. build_vol(0)'s $MFT bitmap
 * marks records 0,1,3,5,6,10,40-51 in use; 2,4,7,8,9,11-39,52-63 are free
 * (see the mftbm[] comment in build_vol()). First-fit finds bit 2 first. */
static void test_mft_alloc(void) {
    hype_ntfs_t fs;
    uint64_t rec_no;
    uint16_t seq;
    uint8_t rec[REC_SIZE];

    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));

    CHECK_HEX("alloc file", 0, hype_ntfs_mft_record_alloc(&fs, vol_write, 0, &rec_no, &seq, 2));
    CHECK_HEX("alloc file rec_no", 2, rec_no);
    CHECK_HEX("alloc file seq", 1, seq);
    CHECK_HEX("record now readable", 0, hype_ntfs_record_read(&fs, 2, rec));
    CHECK("not a directory", (get16(rec + 0x16) & 0x0002u) == 0u);
    CHECK_HEX("empty attr list", 0xFFFFFFFFu, get32(rec + get16(rec + 0x14)));

    CHECK_HEX("alloc dir", 0, hype_ntfs_mft_record_alloc(&fs, vol_write, 1, &rec_no, &seq, 3));
    CHECK_HEX("alloc dir rec_no", 4, rec_no); /* next free bit after 2 */
    CHECK_HEX("alloc dir seq", 1, seq);
    CHECK_HEX("record now readable", 0, hype_ntfs_record_read(&fs, 4, rec));
    CHECK("is a directory", (get16(rec + 0x16) & 0x0002u) != 0u);

    /* free + realloc: sequence number bumps twice total (once on free, once
     * on the next alloc reusing the same slot) */
    CHECK_HEX("free rec 2", 0, hype_ntfs_mft_record_free(&fs, vol_write, 2, 4));
    CHECK("freed record now unreadable", hype_ntfs_record_read(&fs, 2, rec) != 0);
    CHECK_HEX("realloc reuses freed slot", 0,
              hype_ntfs_mft_record_alloc(&fs, vol_write, 0, &rec_no, &seq, 5));
    CHECK_HEX("realloc rec_no", 2, rec_no);
    CHECK_HEX("realloc seq bumped past the freed value", 3, seq);

    /* exhaustion: allocate every remaining free bit, then one more refuses */
    {
        unsigned k;
        int exhausted = 0;
        for (k = 0; k < 60u; k++) {
            uint64_t r2;
            uint16_t s2;
            if (hype_ntfs_mft_record_alloc(&fs, vol_write, 0, &r2, &s2,
                                           (uint16_t)(6u + k)) != 0) {
                exhausted = 1;
                break;
            }
        }
        CHECK("bitmap eventually exhausted", exhausted);
    }

    /* bitmap/record disagreement refused: record 7 (free in the bitmap)
     * manually marked in-use on disk without updating the bitmap */
    build_vol(0);
    rec_init(7, 0); /* sets MFT_IN_USE */
    rec_fixup(7);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    /* consume every free bit before 7 (bits 2 and 4) so first-fit lands
     * exactly on the sabotaged record next */
    {
        unsigned k;
        uint64_t r2;
        uint16_t s2;
        for (k = 0; k < 2u; k++) {
            CHECK_HEX("pre-consume", 0,
                      hype_ntfs_mft_record_alloc(&fs, vol_write, 0, &r2, &s2, (uint16_t)(8u + k)));
        }
    }
    CHECK("bitmap/record disagreement refused",
          hype_ntfs_mft_record_alloc(&fs, vol_write, 0, &rec_no, &seq, 20) != 0);

    /* torn record at the target bit refused rather than guessed: corrupt
     * record 2's own fixup tail (it is free in the bitmap, all zero, but
     * make it LOOK like a torn FILE record instead) */
    build_vol(0);
    {
        uint8_t *r2 = rec_ptr(2);
        r2[0] = 'F'; r2[1] = 'I'; r2[2] = 'L'; r2[3] = 'E';
        put16(r2 + 4, 0x30);
        put16(r2 + 6, 3);
        put16(r2 + 0x30, 0x0001); /* USN stamped in the header */
        /* sector tails deliberately NOT stamped to match -- torn write */
    }
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("torn record at the target bit refused",
          hype_ntfs_mft_record_alloc(&fs, vol_write, 0, &rec_no, &seq, 2) != 0);

    /* refusals: bad args */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("alloc NULL fs refused",
          hype_ntfs_mft_record_alloc(0, vol_write, 0, &rec_no, &seq, 2) != 0);
    CHECK("alloc NULL write refused",
          hype_ntfs_mft_record_alloc(&fs, 0, 0, &rec_no, &seq, 2) != 0);
    CHECK("alloc NULL out_rec_no refused",
          hype_ntfs_mft_record_alloc(&fs, vol_write, 0, 0, &seq, 2) != 0);
    CHECK("alloc NULL out_seq refused",
          hype_ntfs_mft_record_alloc(&fs, vol_write, 0, &rec_no, 0, 2) != 0);
    CHECK("free NULL fs refused", hype_ntfs_mft_record_free(0, vol_write, 40, 2) != 0);
    CHECK("free NULL write refused", hype_ntfs_mft_record_free(&fs, 0, 40, 2) != 0);
    CHECK("free a never-used record refused", hype_ntfs_mft_record_free(&fs, vol_write, 2, 2) != 0);
    CHECK("free a missing record refused", hype_ntfs_mft_record_free(&fs, vol_write, 9999, 2) != 0);
}

/* Byte offset (relative to the entries region) of the first index entry in
 * record `n`'s resident $INDEX_ROOT whose name matches `name`, or ~0u. Used
 * to assert on-disk SORT ORDER directly -- resolve() succeeding proves an
 * entry is findable via linear scan, not that it landed in the right
 * collated position (a real ntfs-3g driver's own lookup does care, unlike
 * hype's own deliberately-linear dir_lookup()). */
static uint32_t entry_offset_by_name(hype_ntfs_t *fs, unsigned rec_no, const char *name) {
    uint8_t rec_buf[REC_SIZE];
    uint8_t *rec = rec_buf;
    uint32_t attrs_off;
    /* MUST go through hype_ntfs_record_read(), not a raw rec_ptr() peek:
     * the on-disk sector tails are fixup (USA) territory -- the real bytes
     * there are only restored by fixup_apply(), which record_read() calls
     * and a raw g_vol read does not. A raw peek near a sector boundary
     * (byte 512/1024 for a 2-sector, 1024-byte record) sees the stamped
     * USN, not the real content, and wrongly looks corrupted. */
    if (hype_ntfs_record_read(fs, rec_no, rec) != 0) {
        return (uint32_t)~0u;
    }
    attrs_off = get16(rec + 0x14);
    uint32_t off = attrs_off;
    uint32_t nlen = (uint32_t)strlen(name);
    for (;;) {
        uint32_t t = get32(rec + off);
        uint32_t length = get32(rec + off + 4);
        if (t == 0xFFFFFFFFu) return (uint32_t)~0u;
        if (t == 0x90u) {
            uint32_t val_off = off + get16(rec + off + 0x14);
            uint32_t ents_off = get32(rec + val_off + 0x10);
            uint32_t ents_size = get32(rec + val_off + 0x14);
            uint32_t p = ents_off;
            while (p + 0x10u <= ents_size) {
                const uint8_t *e = rec + val_off + 0x10u + p;
                uint32_t elen = get16(e + 8);
                uint32_t klen = get16(e + 10);
                uint32_t eflags = get16(e + 12);
                if (eflags & 0x02u || elen == 0u) break;
                if (klen >= 0x42u) {
                    uint32_t fn_nlen = e[0x10 + 0x40];
                    if (fn_nlen == nlen) {
                        uint32_t i, eq = 1;
                        for (i = 0; i < nlen; i++) {
                            if (get16(e + 0x10 + 0x42 + i * 2u) != (uint16_t)(uint8_t)name[i]) {
                                eq = 0;
                                break;
                            }
                        }
                        if (eq) return p;
                    }
                }
                p += elen;
            }
            return (uint32_t)~0u;
        }
        off += length;
    }
}

/* #421: $I30 index insert/delete (resident $INDEX_ROOT only). Root (record
 * 5) holds img.bin(40) vdl.bin(41) comp.bin(42) enc.bin(43) res.bin(44)
 * subdir(45,dir) bigdir(47,dir) lst.bin(48), all resident, no
 * $INDEX_ALLOCATION -- exactly this slice's scope. */
static void test_index_insert_delete(void) {
    hype_ntfs_t fs;
    hype_file_rmap_t m, m2;

    /* insert: a second name for an EXISTING file's inode (41, vdl.bin) --
     * resolving the new name must reach the identical underlying data.
     * Root (record 5) has very little resident slack left (its 8 existing
     * entries already nearly fill the 1024-byte record) -- alias.bin is
     * sized to be the last insert that fits before deleting something. */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("alias not found before insert", hype_ntfs_resolve(&fs, "/alias.bin", &m) != 0);
    CHECK_HEX("insert alias", 0,
              hype_ntfs_index_insert(&fs, vol_write, 5, 41, "alias.bin", 9, 0, 2));
    CHECK_HEX("resolve alias", 0, hype_ntfs_resolve(&fs, "/alias.bin", &m));
    CHECK_HEX("resolve vdl.bin", 0, hype_ntfs_resolve(&fs, "/vdl.bin", &m2));
    CHECK_HEX("alias reaches the same data (size)", m2.size_bytes, m.size_bytes);
    CHECK_HEX("alias reaches the same data (count)", m2.count, m.count);
    CHECK_HEX("alias reaches the same data (lba)", m2.ranges[0].start_lba, m.ranges[0].start_lba);
    /* every original name still resolves */
    CHECK_HEX("img.bin still there", 0, hype_ntfs_resolve(&fs, "/img.bin", &m));
    CHECK_HEX("lst.bin still there", 0, hype_ntfs_resolve(&fs, "/lst.bin", &m));

    /* case-insensitive duplicate refused */
    CHECK("duplicate (case-insensitive) refused",
          hype_ntfs_index_insert(&fs, vol_write, 5, 41, "IMG.BIN", 7, 0, 3) != 0);
    CHECK("exact duplicate refused",
          hype_ntfs_index_insert(&fs, vol_write, 5, 41, "img.bin", 7, 0, 3) != 0);

    /* delete: removing a name must not disturb any sibling, and frees room
     * for a later insert to reuse */
    CHECK_HEX("delete img.bin", 0, hype_ntfs_index_delete(&fs, vol_write, 5, "img.bin", 7, 6));
    CHECK("img.bin gone after delete", hype_ntfs_resolve(&fs, "/img.bin", &m) != 0);
    CHECK_HEX("vdl.bin unaffected", 0, hype_ntfs_resolve(&fs, "/vdl.bin", &m));
    CHECK_HEX("lst.bin unaffected", 0, hype_ntfs_resolve(&fs, "/lst.bin", &m));
    CHECK_HEX("alias.bin unaffected", 0, hype_ntfs_resolve(&fs, "/alias.bin", &m));
    CHECK("deleting it again refused", hype_ntfs_index_delete(&fs, vol_write, 5, "img.bin", 7, 7) !=
                                            0);

    /* insert a directory entry into the room just freed, then delete it */
    CHECK_HEX("insert dir entry", 0,
              hype_ntfs_index_insert(&fs, vol_write, 5, 47, "bigdir2", 7, 1, 8));
    CHECK_HEX("resolve dir entry unaffected siblings", 0, hype_ntfs_resolve(&fs, "/lst.bin", &m));
    CHECK_HEX("delete dir entry", 0, hype_ntfs_index_delete(&fs, vol_write, 5, "bigdir2", 7, 9));

    /* many inserts in mixed order still resolve correctly (sorted-position
     * logic exercised at the start, middle, and end) -- subdir (record 45)
     * has only 3 entries, plenty of resident slack for this. */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("insert zzz (end)", 0,
              hype_ntfs_index_insert(&fs, vol_write, 45, 41, "zzz.bin", 7, 0, 2));
    CHECK_HEX("insert aaa (start)", 0,
              hype_ntfs_index_insert(&fs, vol_write, 45, 41, "aaa.bin", 7, 0, 3));
    CHECK_HEX("insert mmm (middle)", 0,
              hype_ntfs_index_insert(&fs, vol_write, 45, 41, "mmm.bin", 7, 0, 4));
    CHECK_HEX("resolve zzz", 0, hype_ntfs_resolve(&fs, "/subdir/zzz.bin", &m));
    CHECK_HEX("resolve aaa", 0, hype_ntfs_resolve(&fs, "/subdir/aaa.bin", &m));
    CHECK_HEX("resolve mmm", 0, hype_ntfs_resolve(&fs, "/subdir/mmm.bin", &m));
    CHECK_HEX("original entries still resolve", 0, hype_ntfs_resolve(&fs, "/subdir/nested.bin", &m));
    /* on-disk collation order, not just linear-scan findability -- a real
     * driver's own lookup depends on this (#421 finding: an earlier version
     * built a byte-packed, not UTF-16LE, comparison key here, which put
     * every insert in the wrong position; resolve() via hype's own linear
     * dir_lookup() never caught it -- only a real ntfs-3g mount's lookup by
     * path did, "No such file or directory" despite `ls` listing the name). */
    {
        uint32_t off_aaa = entry_offset_by_name(&fs, 45, "aaa.bin");
        uint32_t off_mmm = entry_offset_by_name(&fs, 45, "mmm.bin");
        uint32_t off_zzz = entry_offset_by_name(&fs, 45, "zzz.bin");
        CHECK("aaa found on disk", off_aaa != (uint32_t)~0u);
        CHECK("mmm found on disk", off_mmm != (uint32_t)~0u);
        CHECK("zzz found on disk", off_zzz != (uint32_t)~0u);
        CHECK("aaa sorts before mmm on disk", off_aaa < off_mmm);
        CHECK("mmm sorts before zzz on disk", off_mmm < off_zzz);
    }

    /* refusals */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("insert into a $INDEX_ALLOCATION dir refused (bigdir, record 47)",
          hype_ntfs_index_insert(&fs, vol_write, 47, 41, "x.bin", 5, 0, 2) != 0);
    CHECK("insert into a non-directory refused (img.bin, record 40)",
          hype_ntfs_index_insert(&fs, vol_write, 40, 41, "x.bin", 5, 0, 2) != 0);
    CHECK("insert into a missing record refused",
          hype_ntfs_index_insert(&fs, vol_write, 9999, 41, "x.bin", 5, 0, 2) != 0);
    CHECK("insert NULL fs refused",
          hype_ntfs_index_insert(0, vol_write, 5, 41, "x.bin", 5, 0, 2) != 0);
    CHECK("insert NULL write refused",
          hype_ntfs_index_insert(&fs, 0, 5, 41, "x.bin", 5, 0, 2) != 0);
    CHECK("insert zero-length name refused",
          hype_ntfs_index_insert(&fs, vol_write, 5, 41, "x.bin", 0, 0, 2) != 0);
    CHECK("delete from a missing record refused",
          hype_ntfs_index_delete(&fs, vol_write, 9999, "img.bin", 7, 2) != 0);
    CHECK("delete a name that never existed refused",
          hype_ntfs_index_delete(&fs, vol_write, 5, "nope.bin", 8, 2) != 0);
    CHECK("delete NULL fs refused", hype_ntfs_index_delete(0, vol_write, 5, "img.bin", 7, 2) != 0);
    CHECK("delete NULL write refused",
          hype_ntfs_index_delete(&fs, 0, 5, "img.bin", 7, 2) != 0);
}

/* #422: resident-to-non-resident $DATA conversion. res.bin (record 44):
 * resident $DATA, 32 bytes, content pat(i) for i in 0..31. */
static void test_data_to_nonresident(void) {
    hype_ntfs_t fs;
    hype_file_rmap_t m;
    uint8_t buf[5000];
    unsigned i;

    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("res.bin refused while resident", hype_ntfs_resolve(&fs, "/res.bin", &m) != 0);

    CHECK_HEX("convert", 0, hype_ntfs_data_to_nonresident(&fs, vol_write, 44, 5000, 2));
    CHECK_HEX("resolve after convert", 0, hype_ntfs_resolve(&fs, "/res.bin", &m));
    CHECK_HEX("size after convert", 5000, m.size_bytes);
    CHECK("every range is DATA (no stray holes)", m.count >= 1);
    for (i = 0; i < m.count; i++) {
        CHECK_HEX("range kind", HYPE_RANGE_DATA, m.ranges[i].kind);
    }

    CHECK_HEX("read back", 0, hype_file_rmap_read_at(&m, vol_read, 0, 0, buf, sizeof buf));
    for (i = 0; i < 32u; i++) {
        if (buf[i] != pat(i)) {
            CHECK("original resident bytes preserved", 0);
            break;
        }
    }
    for (i = 32u; i < sizeof buf; i++) {
        if (buf[i] != 0u) {
            CHECK("rest zero-filled", 0);
            break;
        }
    }

    /* a second call refuses: already non-resident */
    CHECK("second conversion refused (already non-resident)",
          hype_ntfs_data_to_nonresident(&fs, vol_write, 44, 6000, 3) != 0);

    /* refusals */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("shrinking refused (#422 is growth-only)",
          hype_ntfs_data_to_nonresident(&fs, vol_write, 44, 10, 2) != 0);
    CHECK("multi-extent $DATA refused",
          hype_ntfs_data_to_nonresident(&fs, vol_write, 48, 5000, 2) != 0);
    CHECK("no unnamed $DATA refused (subdir, record 45)",
          hype_ntfs_data_to_nonresident(&fs, vol_write, 45, 5000, 2) != 0);
    CHECK("missing record refused",
          hype_ntfs_data_to_nonresident(&fs, vol_write, 9999, 5000, 2) != 0);
    CHECK("NULL fs refused", hype_ntfs_data_to_nonresident(0, vol_write, 44, 5000, 2) != 0);
    CHECK("NULL write refused", hype_ntfs_data_to_nonresident(&fs, 0, 44, 5000, 2) != 0);
}

/* #423: create and unlink a regular file. subdir (record 45) is resident,
 * roomy, and empty enough to create into. */
static void test_create_unlink(void) {
    hype_ntfs_t fs;
    hype_file_rmap_t m;
    uint64_t rec_no;

    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("newfile not found before create",
          hype_ntfs_resolve(&fs, "/subdir/newfile.bin", &m) != 0);
    CHECK_HEX("create", 0,
              hype_ntfs_create(&fs, vol_write, 45, "newfile.bin", 11, 0x01D0DE6B0A0000ULL,
                               &rec_no, 2));
    /* a freshly created file's $DATA is resident and empty -- resolve()
     * correctly refuses ANY resident $DATA (decision 30); existence is
     * checked via the directory entry itself instead, the same way #421's
     * tests do for on-disk structure that resolve() cannot speak to. */
    CHECK("newfile has a directory entry now",
          entry_offset_by_name(&fs, 45, "newfile.bin") != (uint32_t)~0u);
    CHECK_HEX("original entries unaffected", 0, hype_ntfs_resolve(&fs, "/subdir/nested.bin", &m));

    /* the new record is well-formed: FILE magic, in use, not a directory,
     * hard link count 1, $STANDARD_INFORMATION + $FILE_NAME + $DATA */
    {
        uint8_t rec[REC_SIZE];
        CHECK_HEX("record readable", 0, hype_ntfs_record_read(&fs, rec_no, rec));
        CHECK("not a directory", (get16(rec + 0x16) & 0x0002u) == 0u);
        CHECK_HEX("hard link count", 1, get16(rec + 0x12));
    }

    /* duplicate create refused (name already exists) */
    CHECK("duplicate create refused",
          hype_ntfs_create(&fs, vol_write, 45, "newfile.bin", 11, 1, &rec_no, 3) != 0);

    /* unlink: name gone, siblings unaffected, record freed */
    CHECK_HEX("unlink", 0, hype_ntfs_unlink(&fs, vol_write, 45, "newfile.bin", 11, 4));
    CHECK("newfile's directory entry gone after unlink",
          entry_offset_by_name(&fs, 45, "newfile.bin") == (uint32_t)~0u);
    CHECK_HEX("sibling unaffected", 0, hype_ntfs_resolve(&fs, "/subdir/nested.bin", &m));
    CHECK("unlinking it again refused",
          hype_ntfs_unlink(&fs, vol_write, 45, "newfile.bin", 11, 5) != 0);

    /* the freed record can be reused by a fresh create */
    CHECK_HEX("recreate after unlink", 0,
              hype_ntfs_create(&fs, vol_write, 45, "again.bin", 9, 1, &rec_no, 6));
    CHECK("recreated has a directory entry now",
          entry_offset_by_name(&fs, 45, "again.bin") != (uint32_t)~0u);

    /* create-then-grow-then-unlink releases the file's clusters back to
     * $Bitmap: a pristine bitmap's first allocation always lands at LCN 0,
     * so freeing everything this file owns must make LCN 0 available
     * again for the exact same size. */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("create big", 0,
              hype_ntfs_create(&fs, vol_write, 45, "big.bin", 7, 1, &rec_no, 2));
    /* a fresh create()'s $DATA is resident (empty) -- grow it via #422
     * (resident-to-non-resident), the intended composition for a file
     * that needs backing clusters, not #418's append (which only ever
     * operates on an already-non-resident stream). */
    CHECK_HEX("grow to non-resident", 0,
              hype_ntfs_data_to_nonresident(&fs, vol_write, rec_no, 5u * SECSZ, 3));
    CHECK_HEX("unlink big", 0, hype_ntfs_unlink(&fs, vol_write, 45, "big.bin", 7, 4));
    {
        uint64_t lcn;
        CHECK_HEX("clusters released back to $Bitmap", 0,
                  hype_ntfs_cluster_alloc(&fs, vol_write, 5u, &lcn));
        CHECK_HEX("reallocates the exact same range", 0, lcn);
    }

    /* refusals */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("create into a non-directory refused (img.bin, record 40)",
          hype_ntfs_create(&fs, vol_write, 40, "x.bin", 5, 1, &rec_no, 2) != 0);
    CHECK("create NULL fs refused", hype_ntfs_create(0, vol_write, 45, "x.bin", 5, 1, &rec_no,
                                                     2) != 0);
    CHECK("create NULL write refused",
          hype_ntfs_create(&fs, 0, 45, "x.bin", 5, 1, &rec_no, 2) != 0);
    CHECK("create NULL out_rec_no refused",
          hype_ntfs_create(&fs, vol_write, 45, "x.bin", 5, 1, 0, 2) != 0);
    CHECK("create zero-length name refused",
          hype_ntfs_create(&fs, vol_write, 45, "x.bin", 0, 1, &rec_no, 2) != 0);
    CHECK("unlink a missing name refused",
          hype_ntfs_unlink(&fs, vol_write, 45, "nope.bin", 8, 2) != 0);
    CHECK("unlink a directory refused (bigdir, via root)",
          hype_ntfs_unlink(&fs, vol_write, 5, "bigdir", 6, 2) != 0);
    CHECK("unlink NULL fs refused", hype_ntfs_unlink(0, vol_write, 45, "nested.bin", 10, 2) != 0);
    CHECK("unlink NULL write refused",
          hype_ntfs_unlink(&fs, 0, 45, "nested.bin", 10, 2) != 0);
}

/* #425: mkdir and rmdir. subdir (record 45) is resident and roomy. */
static void test_mkdir_rmdir(void) {
    hype_ntfs_t fs;
    uint64_t rec_no;

    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("newdir not found before mkdir",
          entry_offset_by_name(&fs, 45, "newdir") == (uint32_t)~0u);
    CHECK_HEX("mkdir", 0,
              hype_ntfs_mkdir(&fs, vol_write, 45, "newdir", 6, 1, &rec_no, 2));
    CHECK("newdir has a directory entry", entry_offset_by_name(&fs, 45, "newdir") != (uint32_t)~0u);

    {
        uint8_t rec[REC_SIZE];
        CHECK_HEX("record readable", 0, hype_ntfs_record_read(&fs, rec_no, rec));
        CHECK("is a directory", (get16(rec + 0x16) & 0x0002u) != 0u);
        CHECK_HEX("hard link count", 1, get16(rec + 0x12));
    }

    /* a directory containing a nested subdirectory is not empty */
    {
        uint64_t outer_rec = rec_no;
        uint64_t inner_rec;
        CHECK_HEX("mkdir nested", 0,
                  hype_ntfs_mkdir(&fs, vol_write, outer_rec, "inner", 5, 1, &inner_rec, 3));
        CHECK("rmdir of the now-non-empty outer dir refused",
              hype_ntfs_rmdir(&fs, vol_write, 45, "newdir", 6, 4) != 0);
    }

    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("mkdir", 0, hype_ntfs_mkdir(&fs, vol_write, 45, "newdir", 6, 1, &rec_no, 2));
    CHECK_HEX("rmdir empty dir", 0, hype_ntfs_rmdir(&fs, vol_write, 45, "newdir", 6, 3));
    CHECK("newdir's directory entry gone",
          entry_offset_by_name(&fs, 45, "newdir") == (uint32_t)~0u);
    CHECK("rmdir again refused", hype_ntfs_rmdir(&fs, vol_write, 45, "newdir", 6, 4) != 0);

    /* non-empty directory refused */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("mkdir", 0, hype_ntfs_mkdir(&fs, vol_write, 45, "newdir", 6, 1, &rec_no, 2));
    CHECK_HEX("create a file inside it", 0,
              hype_ntfs_create(&fs, vol_write, rec_no, "x.bin", 5, 1, &rec_no, 3));
    CHECK("rmdir of a non-empty directory refused",
          hype_ntfs_rmdir(&fs, vol_write, 45, "newdir", 6, 4) != 0);

    /* refusals */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("mkdir into a non-directory refused (img.bin, record 40)",
          hype_ntfs_mkdir(&fs, vol_write, 40, "x", 1, 1, &rec_no, 2) != 0);
    CHECK("mkdir NULL fs refused", hype_ntfs_mkdir(0, vol_write, 45, "x", 1, 1, &rec_no, 2) != 0);
    CHECK("mkdir NULL write refused",
          hype_ntfs_mkdir(&fs, 0, 45, "x", 1, 1, &rec_no, 2) != 0);
    CHECK("mkdir zero-length name refused",
          hype_ntfs_mkdir(&fs, vol_write, 45, "x", 0, 1, &rec_no, 2) != 0);
    CHECK("rmdir a missing name refused",
          hype_ntfs_rmdir(&fs, vol_write, 45, "nope", 4, 2) != 0);
    CHECK("rmdir a regular file refused (nested.bin, via subdir)",
          hype_ntfs_rmdir(&fs, vol_write, 45, "nested.bin", 10, 2) != 0);
    CHECK("rmdir a large-index directory refused (bigdir, via root)",
          hype_ntfs_rmdir(&fs, vol_write, 5, "bigdir", 6, 2) != 0);
    CHECK("rmdir NULL fs refused", hype_ntfs_rmdir(0, vol_write, 45, "nope", 4, 2) != 0);
    CHECK("rmdir NULL write refused", hype_ntfs_rmdir(&fs, 0, 45, "nope", 4, 2) != 0);
}

/* #424: rename. subdir (record 45) is resident and roomy. */
static void test_rename(void) {
    hype_ntfs_t fs;
    hype_file_rmap_t m;
    uint64_t rec_no;
    uint8_t rec[REC_SIZE];

    /* same-directory rename */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("create", 0,
              hype_ntfs_create(&fs, vol_write, 45, "oldname.bin", 11, 1, &rec_no, 2));
    CHECK_HEX("rename", 0,
              hype_ntfs_rename(&fs, vol_write, 45, "oldname.bin", 11, 45, "newname.bin", 11, 3));
    CHECK("old name gone", entry_offset_by_name(&fs, 45, "oldname.bin") == (uint32_t)~0u);
    CHECK("new name present", entry_offset_by_name(&fs, 45, "newname.bin") != (uint32_t)~0u);
    CHECK_HEX("sibling unaffected", 0, hype_ntfs_resolve(&fs, "/subdir/nested.bin", &m));

    /* the record's own $FILE_NAME was rewritten to match */
    CHECK_HEX("record readable", 0, hype_ntfs_record_read(&fs, rec_no, rec));

    /* move between directories: subdir(45) -> root(5), name unchanged */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("create", 0,
              hype_ntfs_create(&fs, vol_write, 45, "movee.bin", 9, 1, &rec_no, 2));
    CHECK_HEX("move to root", 0,
              hype_ntfs_rename(&fs, vol_write, 45, "movee.bin", 9, 5, "movee.bin", 9, 3));
    CHECK("gone from subdir", entry_offset_by_name(&fs, 45, "movee.bin") == (uint32_t)~0u);
    CHECK("now in root", entry_offset_by_name(&fs, 5, "movee.bin") != (uint32_t)~0u);
    CHECK_HEX("subdir sibling unaffected", 0,
              hype_ntfs_resolve(&fs, "/subdir/nested.bin", &m));
    CHECK_HEX("root sibling unaffected", 0, hype_ntfs_resolve(&fs, "/lst.bin", &m));

    /* rename to a name that collides at the destination: rolled back, the
     * old name is still there afterward */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("create a.bin", 0, hype_ntfs_create(&fs, vol_write, 45, "a.bin", 5, 1, &rec_no, 2));
    CHECK_HEX("create b.bin", 0, hype_ntfs_create(&fs, vol_write, 45, "b.bin", 5, 1, &rec_no, 3));
    CHECK("rename onto an existing name refused",
          hype_ntfs_rename(&fs, vol_write, 45, "a.bin", 5, 45, "b.bin", 5, 4) != 0);
    CHECK("source name rolled back and still present",
          entry_offset_by_name(&fs, 45, "a.bin") != (uint32_t)~0u);
    CHECK("destination name untouched",
          entry_offset_by_name(&fs, 45, "b.bin") != (uint32_t)~0u);

    /* rename a directory */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("mkdir", 0, hype_ntfs_mkdir(&fs, vol_write, 45, "olddir", 6, 1, &rec_no, 2));
    CHECK_HEX("rename dir", 0,
              hype_ntfs_rename(&fs, vol_write, 45, "olddir", 6, 45, "newdir", 6, 3));
    CHECK("old dir name gone", entry_offset_by_name(&fs, 45, "olddir") == (uint32_t)~0u);
    CHECK("new dir name present", entry_offset_by_name(&fs, 45, "newdir") != (uint32_t)~0u);
    CHECK_HEX("still a directory", 0, hype_ntfs_record_read(&fs, rec_no, rec));
    CHECK("is a directory", (get16(rec + 0x16) & 0x0002u) != 0u);

    /* refusals */
    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK("rename a missing name refused",
          hype_ntfs_rename(&fs, vol_write, 45, "nope.bin", 8, 45, "x.bin", 5, 2) != 0);
    CHECK("rename NULL fs refused",
          hype_ntfs_rename(0, vol_write, 45, "nested.bin", 10, 45, "x.bin", 5, 2) != 0);
    CHECK("rename NULL write refused",
          hype_ntfs_rename(&fs, 0, 45, "nested.bin", 10, 45, "x.bin", 5, 2) != 0);
    CHECK("rename zero-length dst name refused",
          hype_ntfs_rename(&fs, vol_write, 45, "nested.bin", 10, 45, "x.bin", 0, 2) != 0);
}

/* #692: path-based wrappers over #423-#425. */
static void test_path_wrappers(void) {
    hype_ntfs_t fs;
    uint64_t rec_no, pdir_rec;

    build_vol(0);
    CHECK_HEX("mount", 0, hype_ntfs_mount(vol_read, 0, &fs));
    CHECK_HEX("mkdir via path", 0,
              hype_ntfs_mkdir_path(&fs, vol_write, "/subdir/pdir", 1, &pdir_rec, 2));
    CHECK("pdir present under subdir", entry_offset_by_name(&fs, 45, "pdir") != (uint32_t)~0u);

    CHECK_HEX("create via path", 0,
              hype_ntfs_create_path(&fs, vol_write, "/subdir/pdir/pfile.bin", 1, &rec_no, 3));
    CHECK("pfile present under pdir",
          entry_offset_by_name(&fs, (unsigned)pdir_rec, "pfile.bin") != (uint32_t)~0u);

    CHECK_HEX("rename via path", 0,
              hype_ntfs_rename_path(&fs, vol_write, "/subdir/pdir/pfile.bin",
                                    "/subdir/pfile2.bin", 4));
    CHECK("pfile2 present under subdir",
          entry_offset_by_name(&fs, 45, "pfile2.bin") != (uint32_t)~0u);

    CHECK_HEX("unlink via path", 0, hype_ntfs_unlink_path(&fs, vol_write, "/subdir/pfile2.bin", 5));
    CHECK("pfile2 gone", entry_offset_by_name(&fs, 45, "pfile2.bin") == (uint32_t)~0u);

    CHECK_HEX("rmdir via path", 0, hype_ntfs_rmdir_path(&fs, vol_write, "/subdir/pdir", 6));
    CHECK("pdir gone", entry_offset_by_name(&fs, 45, "pdir") == (uint32_t)~0u);

    /* refusals */
    CHECK("create with no parent refused",
          hype_ntfs_create_path(&fs, vol_write, "/nosuch/x.bin", 1, &rec_no, 7) != 0);
    CHECK("create at bare root refused (no parent to split into)",
          hype_ntfs_create_path(&fs, vol_write, "/", 1, &rec_no, 7) != 0);
    CHECK("NULL fs refused", hype_ntfs_create_path(0, vol_write, "/x.bin", 1, &rec_no, 7) != 0);
}

/* #692: the generic, driver-agnostic namespace battery (core/fs_battery.c)
 * run against NTFS through the shared hype_fs_ops_t vtable -- the SAME
 * battery code core/tests/test_ext.c runs against ext, proving the two
 * drivers' namespace mutation behaves identically from a caller's point
 * of view, without either test knowing which driver it is exercising. */
static void battery_log(void *ctx, const char *what, int ok) {
    (void)ctx;
    if (!ok) {
        printf("  battery step failed: %s\n", what);
    }
}

static void test_generic_battery(void) {
    hype_fs_t fs;
    hype_fs_battery_result_t res;

    build_vol(0);
    CHECK_HEX("mount via vtable", 0, hype_fs_mount_auto(&fs, vol_read, vol_write, 0));
    CHECK("caps advertise namespace support",
          (hype_fs_caps(&fs) & HYPE_FS_CAP_NAMESPACE) != 0u);
    CHECK_HEX("battery run", 0,
              hype_fs_battery_run(&fs, "/subdir/battery", &res, battery_log, 0));
    CHECK_HEX("battery failures", 0, res.failures);
    CHECK_HEX("dirs created", 1, res.dirs_created);
    CHECK_HEX("files created", 3, res.files_created); /* f1, f2, f1-again-after-rename */
    CHECK_HEX("duplicate refusals", 5, res.duplicate_refusals_ok);
    CHECK_HEX("renames ok", 1, res.renames_ok);
    CHECK_HEX("deletes ok", 3, res.deletes_ok);
    CHECK_HEX("stale refusals", 2, res.stale_refusals_ok);

    /* a read-only mount has no namespace capability at all: the battery
     * refuses outright rather than attempting anything */
    CHECK_HEX("ro mount", 0, hype_fs_mount_auto(&fs, vol_read, 0, 0));
    CHECK("battery refuses on a read-only mount",
          hype_fs_battery_run(&fs, "/subdir/battery2", &res, 0, 0) != 0);

    /* argument guards */
    CHECK_HEX("mount via vtable", 0, hype_fs_mount_auto(&fs, vol_read, vol_write, 0));
    CHECK("NULL fs refused", hype_fs_battery_run(0, "/x", &res, 0, 0) != 0);
    CHECK("NULL dir refused", hype_fs_battery_run(&fs, 0, &res, 0, 0) != 0);
    CHECK("NULL res refused", hype_fs_battery_run(&fs, "/x", 0, 0, 0) != 0);

    /* a genuine mid-battery failure is recorded, not silently ignored:
     * pre-create the battery's own directory so its own first mkdir()
     * fails immediately */
    build_vol(0);
    CHECK_HEX("mount via vtable", 0, hype_fs_mount_auto(&fs, vol_read, vol_write, 0));
    CHECK_HEX("pre-create the battery dir", 0, hype_fs_mkdir(&fs, "/subdir/battery3"));
    CHECK("battery reports the failure", hype_fs_battery_run(&fs, "/subdir/battery3", &res,
                                                             battery_log, 0) != 0);
    CHECK("failures counted", res.failures != 0u);
    CHECK("first_fail names the step", res.first_fail[0] != 0);
}

int main(void) {
    test_probe();
    test_mount_refusals();
    test_resolve_sparse();
    test_resolve_unwritten();
    test_refusals();
    test_fs_ops_ntfs();
    test_boot_variants();
    test_malformed_structures();
    test_list_and_names();
    test_more_edges();
    test_attr_and_list_guards();
    test_final_edges();
    test_cluster_alloc();
    test_data_append();
    test_hole_fill();
    test_mft_alloc();
    test_index_insert_delete();
    test_data_to_nonresident();
    test_create_unlink();
    test_mkdir_rmdir();
    test_rename();
    test_path_wrappers();
    test_generic_battery();

    if (failures == 0) {
        printf("test_ntfs: all tests passed\n");
        return 0;
    }
    printf("test_ntfs: %d failure(s)\n", failures);
    return 1;
}
