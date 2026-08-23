#include "ntfs.h"
#include "lebytes.h"

/* See ntfs.h and plan.md §10 decision 30. Research provenance:
 * research/linux-ntfs-docs (kernel Documentation/filesystems/ntfs.rst) and
 * the ntfs-3g layout headers -- field offsets cited inline. */

#define SECSZ HYPE_BLK_SECTOR_SIZE

/* MFT attribute type codes */
#define AT_STANDARD_INFORMATION 0x10u
#define AT_ATTRIBUTE_LIST 0x20u
#define AT_FILE_NAME 0x30u
#define AT_VOLUME_INFORMATION 0x70u
#define AT_DATA 0x80u
#define AT_INDEX_ROOT 0x90u
#define AT_INDEX_ALLOCATION 0xA0u
#define AT_BITMAP 0xB0u
#define AT_END 0xFFFFFFFFu

/* attribute-header flags (u16 at offset 0x0C) */
#define ATTR_IS_COMPRESSED 0x0001u
#define ATTR_IS_ENCRYPTED 0x4000u
#define ATTR_IS_SPARSE 0x8000u

/* MFT record flags (u16 at offset 0x16) */
#define MFT_IN_USE 0x0001u
#define MFT_IS_DIR 0x0002u

/* well-known MFT record numbers */
#define REC_MFT 0u
#define REC_MFTMIRR 1u
#define REC_VOLUME 3u
#define REC_ROOT 5u
#define REC_UPCASE 10u
#define REC_BITMAP 6u

#define VOLUME_IS_DIRTY 0x0001u

/* FILE_NAME namespaces */
#define NS_DOS 2u

static void bcopy8(uint8_t *dst, const uint8_t *src, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) dst[i] = src[i];
}

/* ---- fixups ----------------------------------------------------------- */

/*
 * Un-apply the update sequence array of a multi-sector record ("FILE" or
 * "INDX"): the last two bytes of every 512-byte sector must equal the USN,
 * and are restored from the array. A mismatch is a TORN WRITE -- refuse. The
 * classic trap is skipping this and reading plausible garbage where the real
 * bytes should be.
 */
static int fixup_apply(uint8_t *rec, uint32_t rec_bytes) {
    uint32_t usa_off = hype_rd16(rec + 4);
    uint32_t usa_count = hype_rd16(rec + 6); /* 1 (the USN) + one per sector */
    uint32_t sectors = rec_bytes / SECSZ;
    uint16_t usn;
    uint32_t s;

    if (usa_count < 2u || usa_count - 1u < sectors) return -1;
    if (usa_off + usa_count * 2u > rec_bytes) return -1;
    usn = hype_rd16(rec + usa_off);
    for (s = 0; s < sectors; s++) {
        uint8_t *tail = rec + (s + 1u) * SECSZ - 2u;
        if (hype_rd16(tail) != usn) return -1; /* torn write */
        bcopy8(tail, rec + usa_off + (s + 1u) * 2u, 2u);
    }
    return 0;
}

/* ---- runlists ---------------------------------------------------------- */

/*
 * Decode one runlist into the range map, in CLUSTER units converted to
 * 512-byte sectors. `vcn_cursor` carries the logical position across
 * $ATTRIBUTE_LIST pieces; sparse runs (offset size 0) become HOLE.
 */
static int runlist_decode(const hype_ntfs_t *fs, const uint8_t *rl, uint32_t rl_bytes,
                          hype_file_rmap_t *out, uint64_t *lcn_cursor) {
    uint32_t p = 0;
    int64_t lcn = (int64_t)*lcn_cursor;

    while (p < rl_bytes && rl[p] != 0u) {
        uint32_t len_sz = rl[p] & 0x0Fu;
        uint32_t off_sz = (rl[p] >> 4) & 0x0Fu;
        uint64_t run_len = 0;
        int64_t off = 0;
        uint32_t i;

        p++;
        if (len_sz == 0u || len_sz > 8u || off_sz > 8u) return -1;
        if (p + len_sz + off_sz > rl_bytes) return -1;
        for (i = 0; i < len_sz; i++) run_len |= (uint64_t)rl[p + i] << (8u * i);
        p += len_sz;
        if (run_len == 0u) return -1;
        if (off_sz != 0u) {
            for (i = 0; i < off_sz; i++) off |= (int64_t)((uint64_t)rl[p + i] << (8u * i));
            /* sign-extend the top byte */
            if (rl[p + off_sz - 1u] & 0x80u) off -= (int64_t)((uint64_t)1u << (8u * off_sz));
            p += off_sz;
        }

        if (run_len > (~0ull) / fs->spc) return -1;
        if (off_sz == 0u) {
            /* sparse run: no clusters allocated */
            if (hype_file_rmap_append(out, HYPE_RANGE_HOLE, 0, run_len * fs->spc) != 0) {
                return -1;
            }
        } else {
            lcn += off;
            if (lcn < 0) return -1;
            if (((uint64_t)lcn + run_len) * fs->spc > fs->total_sectors) return -1;
            if (hype_file_rmap_append(out, HYPE_RANGE_DATA, (uint64_t)lcn * fs->spc,
                                      run_len * fs->spc) != 0) {
                return -1;
            }
        }
    }
    *lcn_cursor = (uint64_t)lcn;
    return 0;
}

/* ---- attribute walking -------------------------------------------------- */

typedef struct {
    uint32_t type;
    uint32_t length;      /* whole attribute record */
    int non_resident;
    uint32_t name_len;    /* UTF-16 code units */
    uint32_t name_off;
    uint16_t flags;
    /* resident */
    uint32_t val_len;
    uint32_t val_off;
    /* non-resident */
    uint64_t start_vcn;
    uint32_t rl_off;
    uint64_t alloc_size;
    uint64_t real_size;
    uint64_t init_size;
} ntfs_attr_t;

static int attr_parse(const uint8_t *rec, uint32_t rec_bytes, uint32_t off, ntfs_attr_t *a) {
    if (off + 16u > rec_bytes) return -1;
    a->type = hype_rd32(rec + off);
    if (a->type == AT_END) return 1; /* terminator */
    a->length = hype_rd32(rec + off + 4);
    if (a->length < 24u || off + a->length > rec_bytes || (a->length & 7u) != 0u) return -1;
    a->non_resident = rec[off + 8];
    a->name_len = rec[off + 9];
    a->name_off = hype_rd16(rec + off + 0x0A);
    a->flags = hype_rd16(rec + off + 0x0C);
    if (a->non_resident) {
        if (a->length < 0x40u) return -1;
        a->start_vcn = hype_rd64(rec + off + 0x10);
        a->rl_off = hype_rd16(rec + off + 0x20);
        a->alloc_size = hype_rd64(rec + off + 0x28);
        a->real_size = hype_rd64(rec + off + 0x30);
        a->init_size = hype_rd64(rec + off + 0x38);
        if (a->rl_off >= a->length) return -1;
        a->val_len = 0;
        a->val_off = 0;
    } else {
        a->val_len = hype_rd32(rec + off + 0x10);
        a->val_off = hype_rd16(rec + off + 0x14);
        if (a->val_off + (uint64_t)a->val_len > a->length) return -1;
        a->start_vcn = 0;
        a->rl_off = 0;
        a->alloc_size = a->real_size = a->init_size = a->val_len;
    }
    return 0;
}

/* Iterate the record's attributes; returns the offset of the next attribute
 * of `type` at or after *cursor, filling *a. 0 found, 1 exhausted, -1 bad. */
static int attr_find(const uint8_t *rec, uint32_t rec_bytes, uint32_t type, uint32_t *cursor,
                     ntfs_attr_t *a) {
    uint32_t off = *cursor;
    if (off == 0u) {
        if (hype_rd16(rec + 0x14) >= rec_bytes) return -1;
        off = hype_rd16(rec + 0x14); /* attrs offset */
    }
    for (;;) {
        int rc = attr_parse(rec, rec_bytes, off, a);
        if (rc == 1) return 1;
        if (rc != 0) return -1;
        if (a->type == type) {
            *cursor = off + a->length;
            return 0;
        }
        off += a->length;
    }
}

/* ---- MFT record access -------------------------------------------------- */

/* Read + fixup MFT record `n` through the $MFT map. */
static int record_read(hype_ntfs_t *fs, uint64_t n, uint8_t *rec) {
    uint64_t off = n * fs->mft_record_size;
    if (fs->mft.size_bytes == 0u) {
        /* bootstrap: record 0 lives at mft_lcn, before the map exists */
        uint64_t lba = fs->mft_lcn * fs->spc + (off / SECSZ);
        if (fs->read(fs->ctx, lba, fs->mft_record_size / SECSZ, rec) != 0) return -1;
    } else {
        if (hype_file_rmap_read_at(&fs->mft, fs->read, fs->ctx, off, rec,
                                   fs->mft_record_size) != 0) {
            return -1;
        }
    }
    if (rec[0] != 'F' || rec[1] != 'I' || rec[2] != 'L' || rec[3] != 'E') return -1;
    if (fixup_apply(rec, fs->mft_record_size) != 0) return -1;
    if (!(hype_rd16(rec + 0x16) & MFT_IN_USE)) return -1;
    return 0;
}

/*
 * Collect a (possibly multi-record) attribute's runlist into `out`. Handles
 * $ATTRIBUTE_LIST: pieces of the attribute living in extension records are
 * visited in VCN order (the list is kept sorted on disk). The attribute list
 * itself may be resident or non-resident. Depth is one -- an extension
 * record's own attribute list is a corruption, refused.
 */
#define NTFS_ATTRLIST_MAX 4096u

static int attr_runs_from_record(hype_ntfs_t *fs, const uint8_t *rec, uint32_t type,
                                 hype_file_rmap_t *out, uint64_t *lcn_cursor, uint64_t *sizes);

static int stream_map(hype_ntfs_t *fs, uint64_t rec_no, uint32_t type, hype_file_rmap_t *out,
                      uint64_t *out_real, uint64_t *out_init) {
    static uint8_t rec[HYPE_NTFS_MAX_RECORD];
    static uint8_t list[NTFS_ATTRLIST_MAX];
    static uint8_t xrec[HYPE_NTFS_MAX_RECORD]; /* extension record */
    ntfs_attr_t a;
    uint32_t cur = 0;
    uint64_t lcn_cursor = 0;
    uint64_t sizes[2] = {0, 0}; /* real, init -- from the FIRST piece */
    int rc;

    hype_file_rmap_init(out, 0);
    if (record_read(fs, rec_no, rec) != 0) return -1;

    /* $ATTRIBUTE_LIST present? */
    rc = attr_find(rec, fs->mft_record_size, AT_ATTRIBUTE_LIST, &cur, &a);
    if (rc < 0) return -1;
    if (rc == 1) {
        /* the simple case: the whole attribute lives here */
        if (attr_runs_from_record(fs, rec, type, out, &lcn_cursor, sizes) != 0) return -1;
    } else {
        uint32_t list_len, p;
        uint64_t prev_ref = ~0ull;
        uint32_t attr_off = cur - a.length;
        if (a.non_resident) {
            /* the list itself overflowed the record: read it through its own
             * runlist (a single-record attribute -- a multi-record attribute
             * list would need a list to find the list, which is corruption) */
            static hype_file_rmap_t lm;
            uint64_t lc = 0;
            hype_file_rmap_init(&lm, 0);
            if (runlist_decode(fs, rec + attr_off + a.rl_off, a.length - a.rl_off, &lm, &lc) !=
                0) {
                return -1;
            }
            lm.size_bytes = a.real_size;
            if (a.real_size == 0u || a.real_size > NTFS_ATTRLIST_MAX) return -1;
            if (hype_file_rmap_read_at(&lm, fs->read, fs->ctx, 0, list,
                                       (unsigned int)a.real_size) != 0) {
                return -1;
            }
            list_len = (uint32_t)a.real_size;
        } else {
            if (a.val_len > NTFS_ATTRLIST_MAX) return -1;
            bcopy8(list, rec + attr_off + a.val_off, a.val_len);
            list_len = a.val_len;
        }
        for (p = 0; p + 0x1A <= list_len;) {
            uint32_t etype = hype_rd32(list + p);
            uint32_t elen = hype_rd16(list + p + 4);
            uint64_t eref = hype_rd64(list + p + 0x10) & 0x0000FFFFFFFFFFFFull;
            if (elen < 0x1A || p + elen > list_len) return -1;
            if (etype == type) {
                const uint8_t *src = rec;
                if (eref != rec_no) {
                    if (eref == prev_ref) {
                        /* consecutive pieces in one extension record are
                         * handled by attr_runs_from_record scanning all
                         * matching attributes -- skip the duplicate visit */
                        p += elen;
                        continue;
                    }
                    if (record_read(fs, eref, xrec) != 0) return -1;
                    src = xrec;
                    prev_ref = eref;
                } else if (prev_ref == rec_no) {
                    p += elen;
                    continue;
                } else {
                    prev_ref = rec_no;
                }
                if (attr_runs_from_record(fs, src, type, out, &lcn_cursor, sizes) != 0) {
                    return -1;
                }
            }
            p += elen;
        }
    }

    out->size_bytes = sizes[0];
    if (out_real) *out_real = sizes[0];
    if (out_init) *out_init = sizes[1];
    return 0;
}

/* Append every piece of `type` (unnamed only) found in this record. */
static int attr_runs_from_record(hype_ntfs_t *fs, const uint8_t *rec, uint32_t type,
                                 hype_file_rmap_t *out, uint64_t *lcn_cursor, uint64_t *sizes) {
    ntfs_attr_t a;
    uint32_t cur = 0;
    int rc;
    int found = 0;

    while ((rc = attr_find(rec, fs->mft_record_size, type, &cur, &a)) == 0) {
        uint32_t attr_off = cur - a.length;
        if (type == AT_DATA && a.name_len != 0u) continue; /* named stream: not ours */
        found = 1;
        if (a.flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED)) return -1; /* refused */
        if (!a.non_resident) return -1; /* resident $DATA: refused (decision 30) */
        if (a.start_vcn == 0u) {
            sizes[0] = a.real_size;
            sizes[1] = a.init_size;
        }
        /* Each attribute EXTENT's mapping pairs are self-contained: the first
         * delta is relative to LCN 0, not to the previous extent's last LCN
         * (ntfs-3g decompresses every extent from zero and merges by VCN). */
        *lcn_cursor = 0;
        if (runlist_decode(fs, rec + attr_off + a.rl_off, a.length - a.rl_off, out,
                           lcn_cursor) != 0) {
            return -1;
        }
    }
    if (rc < 0) return -1;
    return found ? 0 : -1;
}

/* ---- name handling ------------------------------------------------------ */

/*
 * Compare an on-disk UTF-16LE name against an ASCII path component,
 * case-insensitively through the cached $UpCase prefix. 1 match, 0 no match.
 * The decision-24/30 "fold exactly or not at all" rule holds structurally: a
 * query component is a byte string, so every query code point is < 256 and
 * inside the cache by construction; an on-disk code point PAST the cache can
 * never equal a query byte, so it is a non-match, never a guessed fold.
 */
static int name_eq(const hype_ntfs_t *fs, const uint8_t *utf16, uint32_t units, const char *ascii,
                   uint32_t alen) {
    uint32_t i;
    if (units != alen) return 0;
    for (i = 0; i < units; i++) {
        uint16_t c = hype_rd16(utf16 + i * 2u);
        uint16_t d = (uint16_t)(uint8_t)ascii[i];
        if (c >= HYPE_NTFS_UPCASE_CACHE) return 0; /* on-disk name can't be OUR name */
        if (fs->upcase[c] != fs->upcase[d]) return 0;
    }
    return 1;
}

/* ---- directory lookup ---------------------------------------------------- */

/*
 * Scan one block of index entries (from $INDEX_ROOT's value or an INDX
 * block) for `name`. Returns 1 found (*out_ref = MFT reference, *out_isdir),
 * 0 not found, -1 broken/unfoldable.
 */
static int entries_scan(const hype_ntfs_t *fs, const uint8_t *ents, uint32_t bytes,
                        const char *name, uint32_t nlen, uint64_t *out_ref, int *out_isdir) {
    uint32_t p = 0;
    while (p + 0x10u <= bytes) {
        uint64_t ref = hype_rd64(ents + p) & 0x0000FFFFFFFFFFFFull;
        uint32_t elen = hype_rd16(ents + p + 8);
        uint32_t klen = hype_rd16(ents + p + 10);
        uint32_t eflags = hype_rd16(ents + p + 12);
        if (elen < 0x10u || p + elen > bytes) return -1;
        if (eflags & 0x02u) return 0; /* last entry (no key) */
        if (klen >= 0x42u && p + 0x10u + klen <= bytes) {
            const uint8_t *fn = ents + p + 0x10u; /* FILE_NAME attribute value */
            uint32_t fn_nlen = fn[0x40];
            uint8_t ns = fn[0x41];
            uint64_t fnflags = hype_rd32(fn + 0x38);
            if (ns != NS_DOS && 0x42u + fn_nlen * 2u <= klen) {
                if (name_eq(fs, fn + 0x42, fn_nlen, name, nlen) == 1) {
                    *out_ref = ref;
                    *out_isdir = (fnflags & 0x10000000u) ? 1 : 0; /* FILE_ATTR_I30_INDEX */
                    return 1;
                }
            }
        }
        p += elen;
    }
    return 0;
}

/* An index header: entries offset u32, total size u32, allocated u32, flags. */
static int index_header_scan(const hype_ntfs_t *fs, const uint8_t *ih, uint32_t avail,
                             const char *name, uint32_t nlen, uint64_t *out_ref,
                             int *out_isdir) {
    uint32_t ents_off = hype_rd32(ih);
    uint32_t ents_size = hype_rd32(ih + 4);
    if (ents_off > ents_size || ents_size > avail) return -1;
    return entries_scan(fs, ih + ents_off, ents_size - ents_off, name, nlen, out_ref, out_isdir);
}

/*
 * Look `name` up in directory record `dir_rec`. Linear scan: $INDEX_ROOT's
 * entries, then every in-use INDX block of $INDEX_ALLOCATION (gated by the
 * $BITMAP attribute, because a freed block still holds stale entries). The
 * B+tree structure is deliberately not descended -- a linear scan of the
 * allocated blocks visits every live entry (the ext htree precedent), and
 * lookup here is setup-time, not a hot path.
 */
static int dir_lookup(hype_ntfs_t *fs, uint64_t dir_rec, const char *name, uint32_t nlen,
                      uint64_t *out_ref, int *out_isdir) {
    static uint8_t rec[HYPE_NTFS_MAX_RECORD];
    static uint8_t indx[HYPE_NTFS_MAX_RECORD];
    static uint8_t bitmap[512];
    ntfs_attr_t a;
    uint32_t cur;
    uint32_t block_size = 0;
    int rc;

    if (record_read(fs, dir_rec, rec) != 0) return -1;
    if (!(hype_rd16(rec + 0x16) & MFT_IS_DIR)) return -1;

    /* $INDEX_ROOT (must be the $I30 filename index, resident by definition) */
    cur = 0;
    rc = attr_find(rec, fs->mft_record_size, AT_INDEX_ROOT, &cur, &a);
    if (rc != 0 || a.non_resident) return -1;
    {
        const uint8_t *ir = rec + (cur - a.length) + a.val_off;
        if (a.val_len < 0x20u) return -1;
        if (hype_rd32(ir) != AT_FILE_NAME) return -1; /* not the filename index */
        block_size = hype_rd32(ir + 8);
        if (block_size < SECSZ || block_size > HYPE_NTFS_MAX_RECORD ||
            (block_size & (block_size - 1u)) != 0u) {
            return -1;
        }
        rc = index_header_scan(fs, ir + 0x10, a.val_len - 0x10u, name, nlen, out_ref, out_isdir);
        if (rc != 0) return rc; /* found or broken */
    }

    /* $INDEX_ALLOCATION + $BITMAP: the overflow blocks */
    {
        /* static: >4 KiB frames need the __chkstk probe the freestanding
         * build lacks (the #366 precedent). Lookup is setup-time, BSP-only. */
        static hype_file_rmap_t am;
        uint64_t real = 0;
        uint64_t nblocks, b;
        uint32_t bm_len = 0;

        cur = 0;
        rc = attr_find(rec, fs->mft_record_size, AT_INDEX_ALLOCATION, &cur, &a);
        if (rc == 1) return 0; /* small directory: root only */
        if (rc < 0) return -1;
        if (stream_map(fs, dir_rec, AT_INDEX_ALLOCATION, &am, &real, 0) != 0) return -1;

        cur = 0;
        rc = attr_find(rec, fs->mft_record_size, AT_BITMAP, &cur, &a);
        if (rc != 0) return -1; /* allocation without a bitmap is broken */
        if (a.non_resident || a.val_len > sizeof bitmap) return -1;
        bcopy8(bitmap, rec + (cur - a.length) + a.val_off, a.val_len);
        bm_len = a.val_len;

        nblocks = real / block_size;
        for (b = 0; b < nblocks; b++) {
            if (b / 8u >= bm_len || !(bitmap[b / 8u] & (1u << (b % 8u)))) continue;
            if (hype_file_rmap_read_at(&am, fs->read, fs->ctx, b * block_size, indx,
                                       block_size) != 0) {
                return -1;
            }
            if (indx[0] != 'I' || indx[1] != 'N' || indx[2] != 'D' || indx[3] != 'X') return -1;
            if (fixup_apply(indx, block_size) != 0) return -1;
            /* the index header sits at 0x18 (after the INDX record header) */
            rc = index_header_scan(fs, indx + 0x18, block_size - 0x18u, name, nlen, out_ref,
                                   out_isdir);
            if (rc != 0) return rc;
        }
    }
    return 0;
}

/* ---- mount + resolve ----------------------------------------------------- */

static int boot_parse(hype_blk_read_fn read, void *ctx, uint32_t *out_spc, uint64_t *out_sectors,
                      uint64_t *out_mft_lcn, uint32_t *out_rec_size) {
    uint8_t bs[SECSZ];
    static const char oem[8] = {'N', 'T', 'F', 'S', ' ', ' ', ' ', ' '};
    unsigned i;
    uint32_t spc;
    int8_t cpr;

    if (read(ctx, 0, 1, bs) != 0) return -1;
    for (i = 0; i < 8u; i++) {
        if (bs[3 + i] != (uint8_t)oem[i]) return -1; /* incl. BitLocker "-FVE-FS-": refused */
    }
    if (hype_rd16(bs + 0x0B) != SECSZ) return -1; /* 512-byte logical sectors only */
    if (bs[510] != 0x55u || bs[511] != 0xAAu) return -1;
    spc = bs[0x0D];
    if (spc == 0u || spc > 128u || (spc & (spc - 1u)) != 0u) return -1;
    *out_spc = spc;
    *out_sectors = hype_rd64(bs + 0x28);
    *out_mft_lcn = hype_rd64(bs + 0x30);
    if (*out_sectors == 0u || *out_mft_lcn == 0u ||
        *out_mft_lcn >= *out_sectors / spc) {
        return -1;
    }
    cpr = (int8_t)bs[0x40]; /* clusters per MFT record, or -log2(bytes) */
    if (cpr > 0) {
        uint64_t sz = (uint64_t)cpr * spc * SECSZ;
        if (sz < SECSZ || sz > HYPE_NTFS_MAX_RECORD) return -1;
        *out_rec_size = (uint32_t)sz;
    } else {
        int shift = -cpr;
        if (shift < 9 || shift > 12) return -1; /* 512..4096 */
        *out_rec_size = 1u << shift;
    }
    return 0;
}

int hype_ntfs_probe(hype_blk_read_fn read, void *ctx) {
    uint32_t spc, rs;
    uint64_t sectors, mft;
    return boot_parse(read, ctx, &spc, &sectors, &mft, &rs);
}

int hype_ntfs_mount(hype_blk_read_fn read, void *ctx, hype_ntfs_t *out) {
    static uint8_t rec[HYPE_NTFS_MAX_RECORD];
    uint64_t real = 0, init = 0;
    unsigned i;

    out->read = read;
    out->ctx = ctx;
    out->upcase_loaded = 0;
    out->bitmap_loaded = 0;
    hype_file_rmap_init(&out->mft, 0);
    if (boot_parse(read, ctx, &out->spc, &out->total_sectors, &out->mft_lcn,
                   &out->mft_record_size) != 0) {
        return -1;
    }

    /* $MFT record 0: its own $DATA runlist maps every other record. */
    if (stream_map(out, REC_MFT, AT_DATA, &out->mft, &real, &init) != 0) return -1;
    out->mft.size_bytes = real;
    if (real < 16u * out->mft_record_size) return -1; /* must cover the system files */

    /* $Volume: refuse a dirty volume before trusting anything else. */
    {
        ntfs_attr_t a;
        uint32_t cur = 0;
        int rc;
        if (record_read(out, REC_VOLUME, rec) != 0) return -1;
        rc = attr_find(rec, out->mft_record_size, AT_VOLUME_INFORMATION, &cur, &a);
        if (rc != 0 || a.non_resident || a.val_len < 12u) return -1;
        if (hype_rd16(rec + (cur - a.length) + a.val_off + 10) & VOLUME_IS_DIRTY) {
            return -1; /* fast startup / hibernation / crash: metadata not trustworthy */
        }
    }

    /* $UpCase: cache the first 256 code points; sanity-check instead of
     * guessing (ASCII letters must fold, everything below 'a' identity-ish). */
    {
        static hype_file_rmap_t um; /* static: __chkstk, as above */
        uint8_t buf[HYPE_NTFS_UPCASE_CACHE * 2u];
        if (stream_map(out, REC_UPCASE, AT_DATA, &um, &real, &init) != 0) return -1;
        um.size_bytes = real;
        if (real < sizeof buf) return -1;
        if (hype_file_rmap_read_at(&um, read, ctx, 0, buf, sizeof buf) != 0) return -1;
        for (i = 0; i < HYPE_NTFS_UPCASE_CACHE; i++) {
            out->upcase[i] = hype_rd16(buf + i * 2u);
        }
        for (i = 'a'; i <= 'z'; i++) {
            if (out->upcase[i] != (uint16_t)(i - 'a' + 'A')) return -1;
        }
        for (i = 0; i < 'a'; i++) {
            if (out->upcase[i] != i) return -1; /* identity below 'a' on real tables */
        }
        out->upcase_loaded = 1;
    }
    return 0;
}

/* Trim the map's coverage to exactly ceil(size/512) sectors: NTFS allocates
 * whole clusters, so a stream's last cluster usually extends past the byte
 * size. That slack is not part of the file's logical content, and the #381
 * validator (rightly) refuses maps that cover sectors no byte can reach. */
static void map_trim(hype_file_rmap_t *m) {
    uint64_t need = (m->size_bytes + SECSZ - 1u) / SECSZ;
    uint64_t covered = 0;
    unsigned i;
    for (i = 0; i < m->count; i++) {
        if (covered >= need) {
            m->count = i;
            return;
        }
        if (covered + m->ranges[i].sector_count > need) {
            m->ranges[i].sector_count = need - covered;
        }
        covered += m->ranges[i].sector_count;
    }
}

int hype_ntfs_resolve(hype_ntfs_t *fs, const char *path, hype_file_rmap_t *out) {
    uint64_t rec_no = REC_ROOT;
    uint32_t p = 0;
    int isdir = 1;
    uint64_t real = 0, init = 0;

    if (fs->upcase_loaded == 0 || path == 0) return -1;
    while (path[p] == '/' || path[p] == '\\') p++;
    if (path[p] == 0) return -1; /* the root itself is a directory */

    while (path[p] != 0) {
        uint32_t start = p, nlen;
        uint64_t ref = 0;
        int rc;
        while (path[p] != 0 && path[p] != '/' && path[p] != '\\') p++;
        nlen = p - start;
        while (path[p] == '/' || path[p] == '\\') p++;
        if (nlen == 0u || nlen > 255u) return -1;
        if (!isdir) return -1; /* a path component under a FILE */
        rc = dir_lookup(fs, rec_no, path + start, nlen, &ref, &isdir);
        if (rc != 1) return -1;
        rec_no = ref;
    }
    if (isdir) return -1; /* resolves to a directory */

    if (stream_map(fs, rec_no, AT_DATA, out, &real, &init) != 0) return -1;

    /*
     * Split the allocated tail past the initialized size into UNWRITTEN:
     * those clusters exist but were never written, and their media contents
     * are stale. Walk the map, re-emitting with the boundary applied.
     */
    if (init < real) {
        static hype_file_rmap_t rebuilt;
        uint64_t pos = 0; /* logical byte cursor */
        unsigned r;
        hype_file_rmap_init(&rebuilt, real);
        for (r = 0; r < out->count; r++) {
            uint64_t rb = out->ranges[r].sector_count * (uint64_t)SECSZ;
            hype_range_kind_t k = (hype_range_kind_t)out->ranges[r].kind;
            if (k == HYPE_RANGE_HOLE || pos + rb <= init) {
                if (hype_file_rmap_append(&rebuilt, k, out->ranges[r].start_lba,
                                          out->ranges[r].sector_count) != 0) {
                    return -1;
                }
            } else if (pos >= init) {
                if (hype_file_rmap_append(&rebuilt, HYPE_RANGE_UNWRITTEN,
                                          out->ranges[r].start_lba,
                                          out->ranges[r].sector_count) != 0) {
                    return -1;
                }
            } else {
                /* the boundary falls inside this run: split at a sector edge */
                uint64_t init_secs = ((init - pos) + SECSZ - 1u) / SECSZ;
                if (hype_file_rmap_append(&rebuilt, HYPE_RANGE_DATA, out->ranges[r].start_lba,
                                          init_secs) != 0) {
                    return -1;
                }
                if (out->ranges[r].sector_count > init_secs &&
                    hype_file_rmap_append(&rebuilt, HYPE_RANGE_UNWRITTEN,
                                          out->ranges[r].start_lba + init_secs,
                                          out->ranges[r].sector_count - init_secs) != 0) {
                    return -1;
                }
            }
            pos += rb;
        }
        rebuilt.too_fragmented = out->too_fragmented;
        /* field-by-field copy (freestanding: no struct assignment) */
        hype_file_rmap_init(out, rebuilt.size_bytes);
        for (r = 0; r < rebuilt.count; r++) {
            out->ranges[r].kind = rebuilt.ranges[r].kind;
            out->ranges[r].start_lba = rebuilt.ranges[r].start_lba;
            out->ranges[r].sector_count = rebuilt.ranges[r].sector_count;
        }
        out->count = rebuilt.count;
        out->too_fragmented = rebuilt.too_fragmented;
    }

    out->size_bytes = real;
    map_trim(out);
    return hype_file_rmap_validate(out, fs->total_sectors);
}

/* ---- write-side record access (#416, exported for core/ntfs_journal.c) - */

/*
 * Stamp the update sequence array before writing `rec` to the medium: pick
 * `usn`, write it into the last 2 bytes of every sector the header's
 * usa_off/usa_count already describe, and save each sector's TRUE tail bytes
 * into the USA -- the exact inverse of fixup_apply() above. The header must
 * already be valid (usa_off/usa_count set), which holds for any record this
 * module first read (and therefore fixup_apply'd) through hype_ntfs_record_read().
 * `usn` must never be 0 or 0xFFFF: 0 means "never fixed up" and some readers
 * treat 0xFFFF as a sentinel; callers cycle through 1..0xFFFE.
 */
void hype_ntfs_fixup_stamp(uint8_t *rec, uint32_t rec_bytes, uint16_t usn) {
    uint32_t usa_off = hype_rd16(rec + 4);
    uint32_t usa_count = hype_rd16(rec + 6);
    uint32_t sectors = rec_bytes / SECSZ;
    uint32_t s;

    hype_wr16(rec + usa_off, usn);
    for (s = 0; s < sectors && s + 1u < usa_count; s++) {
        uint8_t *tail = rec + (s + 1u) * SECSZ - 2u;
        hype_wr16(rec + usa_off + (s + 1u) * 2u, hype_rd16(tail));
        hype_wr16(tail, usn);
    }
}

/* Public wrapper over the existing static record_read(): read + fixup-verify
 * MFT record `n` into `rec` (caller-sized to fs->mft_record_size). */
int hype_ntfs_record_read(hype_ntfs_t *fs, uint64_t n, uint8_t *rec) {
    return record_read(fs, n, rec);
}

/*
 * Write MFT record `n` back to the medium: re-stamp its fixups with `usn`
 * (see hype_ntfs_fixup_stamp) and write the WHOLE record through fs->mft's
 * range map -- always record-aligned, so hype_file_rmap_write_at never hits
 * a ragged edge here. `rec` must already hold the record's live bytes
 * (typically: hype_ntfs_record_read() this same record, mutate it, then
 * call this) -- this function does not merge partial changes for you.
 */
/*
 * #416: reads $VOLUME_INFORMATION's dirty bit (the same field hype_ntfs_mount
 * already refuses read-only mounts on). Returns 1 (dirty), 0 (clean), or -1
 * on a structural failure (record 3 missing, no $VOLUME_INFORMATION, etc.).
 */
int hype_ntfs_volume_dirty_get(hype_ntfs_t *fs) {
    uint8_t rec[HYPE_NTFS_MAX_RECORD];
    ntfs_attr_t a;
    uint32_t cur = 0;

    if (record_read(fs, REC_VOLUME, rec) != 0) return -1;
    if (attr_find(rec, fs->mft_record_size, AT_VOLUME_INFORMATION, &cur, &a) != 0) return -1;
    if (a.non_resident || a.val_len < 12u) return -1;
    return (hype_rd16(rec + (cur - a.length) + a.val_off + 10) & VOLUME_IS_DIRTY) ? 1 : 0;
}

/*
 * #416: sets or clears $VOLUME_INFORMATION's dirty bit and writes record 3
 * back through hype_ntfs_record_write(). This is the whole of #416's
 * "journal" contract with the rest of the world (plan.md §10 decision 64):
 * setting it before a writable session's first mutation, and clearing it
 * only after every pending write from that session has reached the medium,
 * is what tells any conforming NTFS driver (Windows or ntfs-3g) whether the
 * volume needs a chkdsk after an interrupted hype session -- without hype
 * ever writing a genuine $LogFile transaction record.
 */
int hype_ntfs_volume_dirty_set(hype_ntfs_t *fs, hype_blk_write_fn write, int dirty,
                               uint16_t usn) {
    uint8_t rec[HYPE_NTFS_MAX_RECORD];
    ntfs_attr_t a;
    uint32_t cur = 0;
    uint16_t flags;
    uint32_t val_off;

    if (record_read(fs, REC_VOLUME, rec) != 0) return -1;
    if (attr_find(rec, fs->mft_record_size, AT_VOLUME_INFORMATION, &cur, &a) != 0) return -1;
    if (a.non_resident || a.val_len < 12u) return -1;

    val_off = (cur - a.length) + a.val_off;
    flags = hype_rd16(rec + val_off + 10);
    if (dirty) {
        flags |= VOLUME_IS_DIRTY;
    } else {
        flags &= (uint16_t)~VOLUME_IS_DIRTY;
    }
    hype_wr16(rec + val_off + 10, flags);
    return hype_ntfs_record_write(fs, write, REC_VOLUME, rec, usn);
}

/*
 * $MFTMirr (record 1) holds a byte-identical backup of the low-numbered
 * "system file" records -- real ntfs-3g and chkdsk both refuse the WHOLE
 * volume outright if $MFTMirr disagrees with $MFT ("$MFTMirr does not match
 * $MFT (record N)"), discovered empirically running tools/416 against a real
 * mkntfs volume: writing record 3 ($Volume) through $MFT alone, with no
 * mirror update, corrupted the volume by that measure even though the
 * primary $MFT copy was perfectly self-consistent. Every write in the
 * mirrored range must land in both copies, or not at all.
 */
static int mirror_record_if_needed(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t n,
                                   const uint8_t *rec) {
    hype_file_rmap_t mirr;
    uint64_t real = 0, init = 0;
    uint64_t mirrored_records;

    if (stream_map(fs, REC_MFTMIRR, AT_DATA, &mirr, &real, &init) != 0) {
        return -1; /* no $MFTMirr at all is a structural problem, not "nothing to mirror" */
    }
    mirr.size_bytes = real;
    mirrored_records = real / fs->mft_record_size;
    if (n >= mirrored_records) {
        return 0; /* this record isn't one $MFTMirr backs up -- nothing to do */
    }
    return hype_file_rmap_write_at(&mirr, fs->read, write, fs->ctx, n * fs->mft_record_size, rec,
                                   fs->mft_record_size);
}

int hype_ntfs_record_write(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t n, uint8_t *rec,
                           uint16_t usn) {
    uint64_t off = n * fs->mft_record_size;
    if (usn == 0u || usn == 0xFFFFu) {
        return -1;
    }
    hype_ntfs_fixup_stamp(rec, fs->mft_record_size, usn);
    if (hype_file_rmap_write_at(&fs->mft, fs->read, write, fs->ctx, off, rec,
                                fs->mft_record_size) != 0) {
        return -1;
    }
    return mirror_record_if_needed(fs, write, n, rec);
}

/* ---- #417: $Bitmap cluster allocation ----------------------------------- */

#define BITMAP_CHUNK_BYTES 512u

static void bfill8(uint8_t *dst, uint8_t v, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) dst[i] = v;
}

static int bitmap_ensure_loaded(hype_ntfs_t *fs) {
    uint64_t real = 0, init = 0;
    uint64_t need_bytes;

    if (fs->bitmap_loaded) {
        return 0;
    }
    if (fs->spc == 0u) {
        return -1; /* unmounted */
    }
    if (stream_map(fs, REC_BITMAP, AT_DATA, &fs->bitmap, &real, &init) != 0) {
        return -1;
    }
    fs->bitmap.size_bytes = real;
    fs->total_clusters = fs->total_sectors / fs->spc;
    need_bytes = (fs->total_clusters + 7u) / 8u;
    if (fs->total_clusters == 0u || real < need_bytes) {
        return -1; /* $Bitmap too small to cover every cluster on the volume */
    }
    fs->bitmap_loaded = 1;
    return 0;
}

/* First free run of `count` contiguous bits in `m` (any bitmap-shaped
 * stream: #417's $Bitmap file OR #420's $MFT-record bitmap), scanning from
 * bit 0 across `total_bits`, in BITMAP_CHUNK_BYTES-sized reads. Returns 0
 * and fills *out_start, or -1 if no run that large exists. */
static int bitmap_find_free(hype_ntfs_t *fs, hype_file_rmap_t *m, uint64_t total_bits,
                            uint64_t count, uint64_t *out_start) {
    uint8_t buf[BITMAP_CHUNK_BYTES];
    uint64_t bit = 0;
    uint64_t total_bytes = (total_bits + 7u) / 8u;
    uint64_t run_start = 0, run_len = 0;
    int in_run = 0;

    while (bit < total_bits) {
        uint64_t byte_off = bit / 8u;
        uint64_t remaining_bytes = total_bytes - byte_off;
        uint32_t chunk_bytes =
            remaining_bytes < BITMAP_CHUNK_BYTES ? (uint32_t)remaining_bytes : BITMAP_CHUNK_BYTES;
        uint64_t chunk_bits = (uint64_t)chunk_bytes * 8u;
        uint64_t i;

        if (hype_file_rmap_read_at(m, fs->read, fs->ctx, byte_off, buf, chunk_bytes) != 0) {
            return -1;
        }
        for (i = 0; i < chunk_bits && bit < total_bits; i++, bit++) {
            uint32_t byte_i = (uint32_t)(i / 8u);
            uint32_t bit_i = (uint32_t)(i % 8u);
            int used = (buf[byte_i] >> bit_i) & 1u;
            if (!used) {
                if (!in_run) {
                    run_start = bit;
                    in_run = 1;
                    run_len = 0;
                }
                run_len++;
                if (run_len >= count) {
                    *out_start = run_start;
                    return 0;
                }
            } else {
                in_run = 0;
                run_len = 0;
            }
        }
    }
    return -1;
}

/* True iff every bit in [start_bit, start_bit+count) of `m` (bounded by
 * `total_bits`) equals `want_used`. */
static int bitmap_run_is(hype_ntfs_t *fs, hype_file_rmap_t *m, uint64_t total_bits,
                         uint64_t start_bit, uint64_t count, int want_used) {
    uint8_t buf[BITMAP_CHUNK_BYTES];
    uint64_t bit = start_bit;
    uint64_t end = start_bit + count;
    uint64_t total_bytes = (total_bits + 7u) / 8u;

    while (bit < end) {
        uint64_t byte_off = bit / 8u;
        uint64_t remaining_bytes = total_bytes - byte_off;
        uint32_t chunk_bytes =
            remaining_bytes < BITMAP_CHUNK_BYTES ? (uint32_t)remaining_bytes : BITMAP_CHUNK_BYTES;
        uint64_t chunk_bits = (uint64_t)chunk_bytes * 8u;
        uint64_t base = bit;
        uint64_t i;

        if (hype_file_rmap_read_at(m, fs->read, fs->ctx, byte_off, buf, chunk_bytes) != 0) {
            return 0;
        }
        for (i = base - byte_off * 8u; i < chunk_bits && bit < end; i++, bit++) {
            uint32_t byte_i = (uint32_t)(i / 8u);
            uint32_t bit_i = (uint32_t)(i % 8u);
            int used = (buf[byte_i] >> bit_i) & 1u;
            if ((used != 0) != (want_used != 0)) {
                return 0;
            }
        }
    }
    return 1;
}

/* Sets or clears every bit in [start_bit, start_bit+count) of `m`. Ragged
 * leading and trailing bytes go through single-byte read-modify-write;
 * whole bytes in between are written directly (no read needed -- the value
 * doesn't depend on what was there). */
static int bitmap_set_run(hype_ntfs_t *fs, hype_file_rmap_t *m, hype_blk_write_fn write,
                          uint64_t start_bit, uint64_t count, int used) {
    uint64_t bit = start_bit;
    uint64_t end = start_bit + count;

    while (bit < end) {
        uint64_t byte_off = bit / 8u;
        uint32_t bit_in_byte = (uint32_t)(bit % 8u);
        uint64_t bits_left_in_byte = 8u - bit_in_byte;
        uint64_t bits_here = end - bit;

        if (bit_in_byte != 0u || bits_here < 8u) {
            uint8_t b;
            uint32_t k;
            uint64_t n = bits_here < bits_left_in_byte ? bits_here : bits_left_in_byte;

            if (hype_file_rmap_read_at(m, fs->read, fs->ctx, byte_off, &b, 1u) != 0) {
                return -1;
            }
            for (k = 0; k < n; k++) {
                uint32_t bi = bit_in_byte + k;
                if (used) {
                    b = (uint8_t)(b | (1u << bi));
                } else {
                    b = (uint8_t)(b & ~(1u << bi));
                }
            }
            if (hype_file_rmap_write_at(m, fs->read, write, fs->ctx, byte_off, &b, 1u) != 0) {
                return -1;
            }
            bit += n;
        } else {
            uint8_t chunk[BITMAP_CHUNK_BYTES];
            uint64_t whole_bytes = (end - bit) / 8u;
            uint32_t n =
                whole_bytes < BITMAP_CHUNK_BYTES ? (uint32_t)whole_bytes : BITMAP_CHUNK_BYTES;
            bfill8(chunk, used ? 0xFFu : 0x00u, n);
            if (hype_file_rmap_write_at(m, fs->read, write, fs->ctx, byte_off, chunk, n) != 0) {
                return -1;
            }
            bit += (uint64_t)n * 8u;
        }
    }
    return 0;
}

int hype_ntfs_cluster_alloc(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t count,
                            uint64_t *out_lcn) {
    uint64_t start;

    if (fs == 0 || write == 0 || out_lcn == 0 || count == 0) {
        return -1;
    }
    if (bitmap_ensure_loaded(fs) != 0) {
        return -1;
    }
    if (bitmap_find_free(fs, &fs->bitmap, fs->total_clusters, count, &start) != 0) {
        return -1;
    }
    if (bitmap_set_run(fs, &fs->bitmap, write, start, count, 1) != 0) {
        return -1;
    }
    *out_lcn = start;
    return 0;
}

int hype_ntfs_cluster_free(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t lcn, uint64_t count) {
    if (fs == 0 || write == 0 || count == 0) {
        return -1;
    }
    if (bitmap_ensure_loaded(fs) != 0) {
        return -1;
    }
    if (lcn + count < lcn || lcn + count > fs->total_clusters) {
        return -1;
    }
    if (!bitmap_run_is(fs, &fs->bitmap, fs->total_clusters, lcn, count, 1)) {
        return -1; /* not fully allocated: caller bug or an already-inconsistent bitmap */
    }
    return bitmap_set_run(fs, &fs->bitmap, write, lcn, count, 0);
}

/* ---- #418: append/grow a $DATA stream ----------------------------------- */

/* Walks a raw mapping-pairs byte range the same way runlist_decode() does,
 * but only to find where the terminator (0x00) sits and what the final
 * absolute LCN cursor is -- the two things an append needs and a full
 * decode-to-rmap does not expose. */
static int runlist_find_end(const uint8_t *rl, uint32_t rl_bytes, uint32_t *out_end,
                            int64_t *out_lcn_cursor) {
    uint32_t p = 0;
    int64_t lcn = 0;

    while (p < rl_bytes && rl[p] != 0u) {
        uint32_t len_sz = rl[p] & 0x0Fu;
        uint32_t off_sz = (rl[p] >> 4) & 0x0Fu;
        int64_t off = 0;
        uint32_t i;

        p++;
        if (len_sz == 0u || len_sz > 8u || off_sz > 8u) return -1;
        if (p + len_sz + off_sz > rl_bytes) return -1;
        p += len_sz;
        if (off_sz != 0u) {
            for (i = 0; i < off_sz; i++) off |= (int64_t)((uint64_t)rl[p + i] << (8u * i));
            if (rl[p + off_sz - 1u] & 0x80u) off -= (int64_t)((uint64_t)1u << (8u * off_sz));
            p += off_sz;
            lcn += off;
        }
    }
    if (p >= rl_bytes) {
        return -1; /* ran off the end without ever finding a terminator */
    }
    *out_end = p;
    *out_lcn_cursor = lcn;
    return 0;
}

/* Fewest bytes (1..8) needed to hold unsigned `v` little-endian. */
static uint32_t min_bytes_unsigned(uint64_t v) {
    uint32_t n = 1;
    v >>= 8;
    while (v != 0u && n < 8u) {
        n++;
        v >>= 8;
    }
    return n;
}

/* Fewest bytes (1..8) needed to hold signed `v` two's-complement
 * little-endian such that sign-extending byte n-1 reproduces `v` exactly. */
static uint32_t min_bytes_signed(int64_t v) {
    uint32_t n;
    for (n = 1; n < 8u; n++) {
        int64_t lo = -((int64_t)1 << (8u * n - 1u));
        int64_t hi = ((int64_t)1 << (8u * n - 1u)) - 1;
        if (v >= lo && v <= hi) {
            return n;
        }
    }
    return 8u;
}

static uint32_t round_up_8(uint32_t v) { return (v + 7u) & ~7u; }

int hype_ntfs_data_append(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t rec_no, uint64_t lcn,
                          uint64_t cluster_count, uint64_t new_alloc_size, uint64_t new_real_size,
                          uint64_t new_init_size, uint16_t usn) {
    static uint8_t rec[HYPE_NTFS_MAX_RECORD];
    ntfs_attr_t a;
    ntfs_attr_t dup;
    uint32_t cur = 0, dup_cur;
    uint32_t attr_off;
    uint32_t rl_end;
    int64_t lcn_cursor;
    int64_t lcn_delta;
    uint32_t len_sz, off_sz, newrun_len;
    uint8_t newrun[1u + 8u + 8u];
    uint32_t content_end_before, new_content_len, new_attr_length, delta;
    uint32_t bytes_used;
    uint32_t i;

    if (fs == 0 || write == 0 || cluster_count == 0u) {
        return -1;
    }
    if (record_read(fs, rec_no, rec) != 0) {
        return -1;
    }
    /* Out of scope: a $DATA stream already split across records. */
    cur = 0;
    if (attr_find(rec, fs->mft_record_size, AT_ATTRIBUTE_LIST, &cur, &dup) == 0) {
        return -1;
    }
    cur = 0;
    if (attr_find(rec, fs->mft_record_size, AT_DATA, &cur, &a) != 0) {
        return -1; /* no unnamed $DATA here at all */
    }
    if (a.name_len != 0u) {
        return -1; /* only a named stream exists -- not the one we grow */
    }
    attr_off = cur - a.length;
    /* Refuse a second unnamed $DATA piece in the same record: ambiguous. */
    dup_cur = cur;
    if (attr_find(rec, fs->mft_record_size, AT_DATA, &dup_cur, &dup) == 0 && dup.name_len == 0u) {
        return -1;
    }
    if (!a.non_resident) {
        return -1; /* resident: #422's job */
    }
    if (a.flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED)) {
        return -1;
    }
    if (lcn + cluster_count < lcn || (lcn + cluster_count) * fs->spc > fs->total_sectors) {
        return -1; /* overflow or past the medium */
    }

    if (runlist_find_end(rec + attr_off + a.rl_off, a.length - a.rl_off, &rl_end, &lcn_cursor) !=
        0) {
        return -1;
    }
    lcn_delta = (int64_t)lcn - lcn_cursor;

    len_sz = min_bytes_unsigned(cluster_count);
    off_sz = min_bytes_signed(lcn_delta);
    newrun_len = 1u + len_sz + off_sz;
    if (newrun_len > sizeof newrun) {
        return -1;
    }

    newrun[0] = (uint8_t)(len_sz | (off_sz << 4));
    for (i = 0; i < len_sz; i++) {
        newrun[1u + i] = (uint8_t)(cluster_count >> (8u * i));
    }
    for (i = 0; i < off_sz; i++) {
        newrun[1u + len_sz + i] = (uint8_t)((uint64_t)lcn_delta >> (8u * i));
    }

    content_end_before = a.rl_off + rl_end; /* offset of the old terminator == start of new content */
    new_content_len = content_end_before + newrun_len + 1u; /* new run + its terminator */
    new_attr_length = round_up_8(new_content_len);
    if (new_attr_length < a.length) {
        new_attr_length = a.length; /* never shrink: the next attribute already starts here */
    }
    delta = new_attr_length - a.length;

    bytes_used = hype_rd32(rec + 0x18);
    if (bytes_used < attr_off + a.length || bytes_used > fs->mft_record_size) {
        return -1; /* record already inconsistent */
    }
    if (delta != 0u) {
        if (bytes_used + delta > fs->mft_record_size) {
            return -1; /* no room: would need an $ATTRIBUTE_LIST, out of scope */
        }
        for (i = bytes_used; i > attr_off + a.length; i--) {
            rec[i - 1u + delta] = rec[i - 1u];
        }
        hype_wr32(rec + 0x18, bytes_used + delta);
    }
    hype_wr32(rec + attr_off + 4, new_attr_length);

    /* Write the new run, a fresh terminator, and zero-pad to the attribute's
     * (possibly now larger) end -- the old trailing padding's exact bytes
     * carry no meaning, so overwriting them is correct, not merely tolerated. */
    for (i = 0; i < newrun_len; i++) {
        rec[attr_off + a.rl_off + rl_end + i] = newrun[i];
    }
    for (i = a.rl_off + rl_end + newrun_len; i < new_attr_length; i++) {
        rec[attr_off + i] = 0u;
    }

    /*
     * Highest VCN (+0x18, between start_vcn and the mapping-pairs offset --
     * present but never read by our OWN runlist_decode, which derives
     * everything from the mapping pairs themselves; a real driver validates
     * it against the runlist's actual VCN coverage and refuses the WHOLE
     * attribute, from VCN 0, if it disagrees. Confirmed empirically: leaving
     * this stale on a genuine ntfs-3g volume made ntfs-3g refuse to read
     * even the ORIGINAL, untouched bytes of the file with EIO, despite the
     * runlist and every size field being individually well-formed -- see
     * research/README.md's NTFS $DATA append entry (#418).
     */
    hype_wr64(rec + attr_off + 0x18, hype_rd64(rec + attr_off + 0x18) + cluster_count);
    hype_wr64(rec + attr_off + 0x28, new_alloc_size);
    hype_wr64(rec + attr_off + 0x30, new_real_size);
    hype_wr64(rec + attr_off + 0x38, new_init_size);

    return hype_ntfs_record_write(fs, write, rec_no, rec, usn);
}

/* ---- #419: hole/sparse fill ---------------------------------------------- */

#define HOLE_FILL_MAX_TAIL_RUNS 32u

typedef struct {
    int is_hole;
    uint64_t run_len;  /* clusters */
    uint64_t abs_lcn;  /* meaningful only if !is_hole */
} tail_run_t;

/* Zero-fills `count` clusters starting at `lcn`, BEFORE any metadata makes
 * them visible: a crash here still leaves the run sparse (reads as zero
 * anyway), never a readable stale byte. */
static int hole_fill_zero_clusters(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t lcn,
                                   uint64_t count) {
    uint8_t chunk[BITMAP_CHUNK_BYTES];
    uint64_t sector = lcn * fs->spc;
    uint64_t sectors_left = count * fs->spc;

    bfill8(chunk, 0u, sizeof chunk);
    while (sectors_left != 0u) {
        uint32_t n = sectors_left < (BITMAP_CHUNK_BYTES / SECSZ)
                         ? (uint32_t)sectors_left
                         : (BITMAP_CHUNK_BYTES / SECSZ);
        if (write(fs->ctx, sector, n, chunk) != 0) {
            return -1;
        }
        sector += n;
        sectors_left -= n;
    }
    return 0;
}

int hype_ntfs_hole_fill(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t rec_no,
                        uint64_t fill_start_vcn, uint64_t cluster_count, uint64_t lcn,
                        uint16_t usn) {
    static uint8_t rec[HYPE_NTFS_MAX_RECORD];
    ntfs_attr_t a, dup;
    uint32_t cur = 0, dup_cur;
    uint32_t attr_off;
    const uint8_t *rl;
    uint32_t rl_bytes;
    uint32_t p;
    uint64_t vcn = 0;
    int64_t lcn_cursor = 0;
    uint32_t run_start_p;
    uint64_t run_start_vcn = 0;
    uint64_t run_len = 0;
    int run_is_hole = 0;
    int found = 0;
    tail_run_t tail[HOLE_FILL_MAX_TAIL_RUNS];
    unsigned tail_n = 0;
    uint64_t before_len, after_len;
    uint8_t out[8u + HOLE_FILL_MAX_TAIL_RUNS * (1u + 8u + 8u) + 1u];
    uint32_t out_len = 0u;
    int64_t enc_cursor;
    unsigned i;
    uint32_t content_prefix_len, new_content_len, new_attr_length, delta;
    uint32_t bytes_used;
    int any_hole_left = 0;
    uint64_t prefix_data_clusters = 0;
    uint64_t tail_data_clusters = 0;
    uint64_t total_data_clusters;

    if (fs == 0 || write == 0 || cluster_count == 0u) {
        return -1;
    }
    if (record_read(fs, rec_no, rec) != 0) {
        return -1;
    }
    cur = 0;
    if (attr_find(rec, fs->mft_record_size, AT_ATTRIBUTE_LIST, &cur, &dup) == 0) {
        return -1;
    }
    cur = 0;
    if (attr_find(rec, fs->mft_record_size, AT_DATA, &cur, &a) != 0) {
        return -1;
    }
    if (a.name_len != 0u) {
        return -1;
    }
    attr_off = cur - a.length;
    dup_cur = cur;
    if (attr_find(rec, fs->mft_record_size, AT_DATA, &dup_cur, &dup) == 0 && dup.name_len == 0u) {
        return -1;
    }
    if (!a.non_resident) {
        return -1;
    }
    if (a.flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED)) {
        return -1;
    }
    if (fill_start_vcn + cluster_count < fill_start_vcn) {
        return -1;
    }
    if (lcn + cluster_count < lcn || (lcn + cluster_count) * fs->spc > fs->total_sectors) {
        return -1;
    }

    /* Walk runs from the start, tracking VCN/LCN cursors, until the one
     * containing [fill_start_vcn, fill_start_vcn+cluster_count) is found. */
    rl = rec + attr_off + a.rl_off;
    rl_bytes = a.length - a.rl_off;
    p = 0;
    while (p < rl_bytes && rl[p] != 0u) {
        uint32_t len_sz = rl[p] & 0x0Fu;
        uint32_t off_sz = (rl[p] >> 4) & 0x0Fu;
        uint64_t this_len = 0;
        int64_t off = 0;

        run_start_p = p;
        run_start_vcn = vcn;
        p++;
        if (len_sz == 0u || len_sz > 8u || off_sz > 8u || p + len_sz + off_sz > rl_bytes) {
            return -1;
        }
        for (i = 0; i < len_sz; i++) this_len |= (uint64_t)rl[p + i] << (8u * i);
        p += len_sz;
        if (this_len == 0u) {
            return -1;
        }
        if (off_sz != 0u) {
            for (i = 0; i < off_sz; i++) off |= (int64_t)((uint64_t)rl[p + i] << (8u * i));
            if (rl[p + off_sz - 1u] & 0x80u) off -= (int64_t)((uint64_t)1u << (8u * off_sz));
            p += off_sz;
        }
        run_len = this_len;
        run_is_hole = (off_sz == 0u);
        if (!run_is_hole) {
            lcn_cursor += off;
        }

        if (fill_start_vcn >= run_start_vcn && fill_start_vcn < run_start_vcn + run_len) {
            if (!run_is_hole) {
                return -1; /* target is already allocated, not a hole */
            }
            if (fill_start_vcn + cluster_count > run_start_vcn + run_len) {
                return -1; /* fill spans past this one hole run: refused */
            }
            found = 1;
            break;
        }
        if (!run_is_hole) {
            prefix_data_clusters += run_len;
        }
        vcn += run_len;
    }
    if (!found) {
        return -1; /* no run covers the requested VCN range */
    }
    /* lcn_cursor here is exactly the cursor value BEFORE the target hole
     * (the hole itself never touched it) -- the correct baseline for the
     * new DATA run's own delta below. */
    enc_cursor = lcn_cursor;

    /* Decode every run AFTER the target hole into `tail`, so the whole
     * remainder can be re-encoded: each tail run's absolute LCN is fixed
     * (physical layout doesn't change), but a DATA run's ENCODED delta is
     * relative to whatever the cursor was going in, so tail runs still need
     * their absolute LCN recorded even though only the first tail DATA run
     * (right after this hole) will actually get a different delta. */
    while (p < rl_bytes && rl[p] != 0u) {
        uint32_t len_sz = rl[p] & 0x0Fu;
        uint32_t off_sz = (rl[p] >> 4) & 0x0Fu;
        uint64_t this_len = 0;
        int64_t off = 0;

        p++;
        if (len_sz == 0u || len_sz > 8u || off_sz > 8u || p + len_sz + off_sz > rl_bytes) {
            return -1;
        }
        for (i = 0; i < len_sz; i++) this_len |= (uint64_t)rl[p + i] << (8u * i);
        p += len_sz;
        if (this_len == 0u) {
            return -1;
        }
        if (off_sz != 0u) {
            for (i = 0; i < off_sz; i++) off |= (int64_t)((uint64_t)rl[p + i] << (8u * i));
            if (rl[p + off_sz - 1u] & 0x80u) off -= (int64_t)((uint64_t)1u << (8u * off_sz));
            p += off_sz;
        }
        if (tail_n >= HOLE_FILL_MAX_TAIL_RUNS) {
            return -1;
        }
        tail[tail_n].is_hole = (off_sz == 0u);
        tail[tail_n].run_len = this_len;
        if (!tail[tail_n].is_hole) {
            lcn_cursor += off;
            tail[tail_n].abs_lcn = (uint64_t)lcn_cursor;
            tail_data_clusters += this_len;
        }
        tail_n++;
    }
    if (p >= rl_bytes) {
        return -1; /* no terminator */
    }

    before_len = fill_start_vcn - run_start_vcn;
    after_len = (run_start_vcn + run_len) - (fill_start_vcn + cluster_count);

    /* Encode: [HOLE before]? [DATA fill]? [HOLE after]? [tail runs...] [term] */
    if (before_len != 0u) {
        out[out_len++] = 0x01u; /* len_sz=1, off_sz=0: sparse */
        if (before_len > 0xFFu) {
            return -1; /* this slice keeps the encoder simple: refuse an
                          oversized split hole rather than multi-byte-length
                          sparse runs (real files rarely need it here) */
        }
        out[out_len++] = (uint8_t)before_len;
        any_hole_left = 1;
    }
    {
        int64_t delta_lcn = (int64_t)lcn - enc_cursor;
        uint32_t len_sz = min_bytes_unsigned(cluster_count);
        uint32_t off_sz = min_bytes_signed(delta_lcn);
        if (out_len + 1u + len_sz + off_sz > sizeof out) {
            return -1;
        }
        out[out_len++] = (uint8_t)(len_sz | (off_sz << 4));
        for (i = 0; i < len_sz; i++) out[out_len++] = (uint8_t)(cluster_count >> (8u * i));
        for (i = 0; i < off_sz; i++) out[out_len++] = (uint8_t)((uint64_t)delta_lcn >> (8u * i));
        enc_cursor = (int64_t)lcn;
    }
    if (after_len != 0u) {
        if (after_len > 0xFFu) {
            return -1;
        }
        out[out_len++] = 0x01u;
        out[out_len++] = (uint8_t)after_len;
        any_hole_left = 1;
    }
    for (i = 0; i < tail_n; i++) {
        if (tail[i].is_hole) {
            if (tail[i].run_len > 0xFFu) {
                return -1;
            }
            if (out_len + 2u > sizeof out) {
                return -1;
            }
            out[out_len++] = 0x01u;
            out[out_len++] = (uint8_t)tail[i].run_len;
            any_hole_left = 1;
        } else {
            int64_t delta_lcn = (int64_t)tail[i].abs_lcn - enc_cursor;
            uint32_t len_sz = min_bytes_unsigned(tail[i].run_len);
            uint32_t off_sz = min_bytes_signed(delta_lcn);
            if (out_len + 1u + len_sz + off_sz > sizeof out) {
                return -1;
            }
            out[out_len++] = (uint8_t)(len_sz | (off_sz << 4));
            for (unsigned k = 0; k < len_sz; k++) {
                out[out_len++] = (uint8_t)(tail[i].run_len >> (8u * k));
            }
            for (unsigned k = 0; k < off_sz; k++) {
                out[out_len++] = (uint8_t)((uint64_t)delta_lcn >> (8u * k));
            }
            enc_cursor = (int64_t)tail[i].abs_lcn;
        }
    }
    if (out_len + 1u > sizeof out) {
        return -1;
    }
    out[out_len++] = 0u; /* terminator */

    content_prefix_len = a.rl_off + run_start_p;
    new_content_len = content_prefix_len + out_len;
    new_attr_length = round_up_8(new_content_len);
    if (new_attr_length < a.length) {
        new_attr_length = a.length;
    }
    delta = new_attr_length - a.length;

    bytes_used = hype_rd32(rec + 0x18);
    if (bytes_used < attr_off + a.length || bytes_used > fs->mft_record_size) {
        return -1;
    }
    if (delta != 0u) {
        if (bytes_used + delta > fs->mft_record_size) {
            return -1; /* no room: would need an $ATTRIBUTE_LIST, out of scope */
        }
        for (i = bytes_used; i > attr_off + a.length; i--) {
            rec[i - 1u + delta] = rec[i - 1u];
        }
        hype_wr32(rec + 0x18, bytes_used + delta);
    }
    hype_wr32(rec + attr_off + 4, new_attr_length);

    for (i = 0; i < out_len; i++) {
        rec[attr_off + content_prefix_len + i] = out[i];
    }
    for (i = content_prefix_len + out_len; i < new_attr_length; i++) {
        rec[attr_off + i] = 0u;
    }

    /* highest_vcn is unchanged: filling a hole never changes the stream's
     * total VCN coverage, only what backs part of it (#418's bug, this
     * function cannot repeat it since the total run-length sum is
     * unchanged: before_len + cluster_count + after_len == run_len). */

    if (any_hole_left == 0) {
        uint16_t flags = hype_rd16(rec + attr_off + 0x0C);
        flags = (uint16_t)(flags & ~ATTR_IS_SPARSE);
        hype_wr16(rec + attr_off + 0x0C, flags);
    }
    /*
     * AllocatedSize is recomputed from scratch as (every DATA run's cluster
     * count) * cluster bytes -- NOT derived by adding to the OLD on-disk
     * AllocatedSize field. Confirmed empirically necessary: a genuine
     * ntfs-3g-created sparse file had its on-disk AllocatedSize already
     * equal to the FULL logical size even before any hole was filled (it
     * tracks true physical backing separately, in a derived "compressed
     * size" ntfsinfo computes from the runlist, not in this field) -- so
     * "old + newly filled bytes" silently double-counts on such a file.
     * Recomputing from the actual runlist is correct regardless of what
     * convention produced the file.
     */
    total_data_clusters = prefix_data_clusters + cluster_count + tail_data_clusters;
    hype_wr64(rec + attr_off + 0x28, total_data_clusters * (uint64_t)fs->spc * SECSZ);

    if (hole_fill_zero_clusters(fs, write, lcn, cluster_count) != 0) {
        return -1;
    }
    return hype_ntfs_record_write(fs, write, rec_no, rec, usn);
}

/* ---- #420: $MFT record allocation ---------------------------------------- */

/* Initializes a fresh, empty FILE record for `rec_no`: magic, fixup array
 * sized for `rec_size`, sequence number, MFT_IN_USE (+ MFT_IS_DIR), and an
 * empty attribute list (just the end marker) -- the base attributes any
 * actual file needs are #423/#425's job to add. */
static void mft_record_init_empty(uint8_t *rec, uint32_t rec_size, uint64_t rec_no, uint16_t seq,
                                  int is_dir) {
    uint32_t usa_off = 0x30u;
    uint32_t usa_count = (rec_size / SECSZ) + 1u;
    uint32_t attrs_off = round_up_8(usa_off + usa_count * 2u);
    uint32_t i;

    for (i = 0; i < rec_size; i++) {
        rec[i] = 0u;
    }
    rec[0] = 'F'; rec[1] = 'I'; rec[2] = 'L'; rec[3] = 'E';
    hype_wr16(rec + 4, (uint16_t)usa_off);
    hype_wr16(rec + 6, (uint16_t)usa_count);
    hype_wr16(rec + 0x10, seq);
    hype_wr16(rec + 0x12, 0u); /* hard link count */
    hype_wr16(rec + 0x14, (uint16_t)attrs_off);
    hype_wr16(rec + 0x16, (uint16_t)(MFT_IN_USE | (is_dir ? MFT_IS_DIR : 0u)));
    hype_wr32(rec + 0x18, attrs_off + 8u); /* bytes in use: attrs area + end marker */
    hype_wr32(rec + 0x1C, rec_size);       /* bytes allocated */
    hype_wr64(rec + 0x20, 0u);             /* base file record: 0, this IS a base record */
    hype_wr16(rec + 0x28, 0u);             /* next attribute instance */
    hype_wr32(rec + 0x2C, (uint32_t)rec_no); /* self-reference, NTFS 3.1+ */
    hype_wr32(rec + attrs_off, 0xFFFFFFFFu);  /* end marker: no attributes yet */
}

int hype_ntfs_mft_record_alloc(hype_ntfs_t *fs, hype_blk_write_fn write, int is_dir,
                               uint64_t *out_rec_no, uint16_t *out_seq, uint16_t usn) {
    hype_file_rmap_t bmap;
    uint64_t real = 0, init = 0;
    uint64_t total_bits;
    uint64_t bit;
    uint8_t rec[HYPE_NTFS_MAX_RECORD];
    uint16_t old_seq, new_seq;

    if (fs == 0 || write == 0 || out_rec_no == 0 || out_seq == 0) {
        return -1;
    }
    if (stream_map(fs, REC_MFT, AT_BITMAP, &bmap, &real, &init) != 0) {
        return -1; /* missing, resident, or otherwise malformed: refused */
    }
    bmap.size_bytes = real;
    total_bits = init * 8u;
    if (total_bits == 0u) {
        return -1;
    }

    if (bitmap_find_free(fs, &bmap, total_bits, 1u, &bit) != 0) {
        /* $MFT's bitmap has no free bit within its already-initialized
         * region. Growing $MFT (and/or its $BITMAP) through the cluster
         * allocator is out of scope for this slice -- refuse honestly. */
        return -1;
    }
    if (bit >= fs->mft.size_bytes / fs->mft_record_size) {
        return -1; /* bitmap claims a slot $MFT's own $DATA doesn't cover */
    }

    /* Cross-check: the record must genuinely be free on disk, not merely
     * absent from the bitmap by mistake. record_read() itself requires
     * MFT_IN_USE, so a clean "not in use" read fails it too -- read the raw
     * bytes directly to tell "genuinely free" apart from "corrupt", and to
     * recover any previous sequence number to bump. */
    if (hype_file_rmap_read_at(&fs->mft, fs->read, fs->ctx, bit * fs->mft_record_size, rec,
                               fs->mft_record_size) != 0) {
        return -1;
    }
    old_seq = 0u;
    if (rec[0] == 'F' && rec[1] == 'I' && rec[2] == 'L' && rec[3] == 'E') {
        if (fixup_apply(rec, fs->mft_record_size) != 0) {
            return -1; /* torn write on a slot the bitmap claims is free */
        }
        if (hype_rd16(rec + 0x16) & MFT_IN_USE) {
            return -1; /* bitmap/record disagree: refused, not guessed */
        }
        old_seq = hype_rd16(rec + 0x10);
    }
    new_seq = (uint16_t)(old_seq + 1u);
    if (new_seq == 0u) {
        new_seq = 1u; /* 0 is reserved for "never used" */
    }

    mft_record_init_empty(rec, fs->mft_record_size, bit, new_seq, is_dir);
    if (hype_ntfs_record_write(fs, write, bit, rec, usn) != 0) {
        return -1;
    }
    if (bitmap_set_run(fs, &bmap, write, bit, 1u, 1) != 0) {
        return -1;
    }
    *out_rec_no = bit;
    *out_seq = new_seq;
    return 0;
}

int hype_ntfs_mft_record_free(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t rec_no,
                              uint16_t usn) {
    uint8_t rec[HYPE_NTFS_MAX_RECORD];
    hype_file_rmap_t bmap;
    uint64_t real = 0, init = 0;
    uint16_t seq;

    if (fs == 0 || write == 0) {
        return -1;
    }
    if (record_read(fs, rec_no, rec) != 0) {
        return -1; /* not currently a valid, in-use record */
    }
    seq = (uint16_t)(hype_rd16(rec + 0x10) + 1u);
    if (seq == 0u) {
        seq = 1u;
    }
    hype_wr16(rec + 0x10, seq);
    hype_wr16(rec + 0x16, (uint16_t)(hype_rd16(rec + 0x16) & (uint16_t)~MFT_IN_USE));
    if (hype_ntfs_record_write(fs, write, rec_no, rec, usn) != 0) {
        return -1;
    }

    if (stream_map(fs, REC_MFT, AT_BITMAP, &bmap, &real, &init) != 0) {
        return -1;
    }
    bmap.size_bytes = real;
    if (rec_no >= init * 8u) {
        return -1; /* out of the bitmap's tracked range: inconsistent */
    }
    return bitmap_set_run(fs, &bmap, write, rec_no, 1u, 0);
}

/* ---- #421: $I30 index insert/delete (resident $INDEX_ROOT only) --------- */

/* Three-way $UpCase collation: case-insensitive first, then (only on a
 * full case-insensitive tie) raw code units, so two names differing only
 * by case still get a deterministic, chkdsk-acceptable total order. -1/0/1. */
static int index_collate(const hype_ntfs_t *fs, const uint8_t *a_utf16, uint32_t a_units,
                         const uint8_t *b_utf16, uint32_t b_units) {
    uint32_t n = a_units < b_units ? a_units : b_units;
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint16_t ca = hype_rd16(a_utf16 + i * 2u);
        uint16_t cb = hype_rd16(b_utf16 + i * 2u);
        uint16_t ua = ca < HYPE_NTFS_UPCASE_CACHE ? fs->upcase[ca] : ca;
        uint16_t ub = cb < HYPE_NTFS_UPCASE_CACHE ? fs->upcase[cb] : cb;
        if (ua != ub) {
            return ua < ub ? -1 : 1;
        }
    }
    if (a_units != b_units) {
        return a_units < b_units ? -1 : 1;
    }
    for (i = 0; i < n; i++) {
        uint16_t ca = hype_rd16(a_utf16 + i * 2u);
        uint16_t cb = hype_rd16(b_utf16 + i * 2u);
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
    }
    return 0;
}

#define IDX_ENTRY_LAST 0x02u

/* Builds one $FILE_NAME index entry (header + key) for `name`, WIN32
 * namespace. Returns the entry's total (8-aligned) length. */
static uint32_t index_build_entry(uint8_t *out, uint64_t mft_ref, uint64_t parent_ref,
                                  const char *name, uint32_t name_len, int is_dir) {
    uint32_t klen = 0x42u + name_len * 2u;
    uint32_t elen = round_up_8(0x10u + klen);
    uint32_t i;

    for (i = 0; i < elen; i++) {
        out[i] = 0u;
    }
    hype_wr64(out + 0, mft_ref);
    hype_wr16(out + 8, (uint16_t)elen);
    hype_wr16(out + 10, (uint16_t)klen);
    hype_wr16(out + 12, 0u); /* not the last entry */
    hype_wr64(out + 0x10, parent_ref & 0x0000FFFFFFFFFFFFull);
    hype_wr32(out + 0x10 + 0x38, is_dir ? 0x10000000u : 0u); /* FILE_ATTR_I30_INDEX */
    out[0x10 + 0x40] = (uint8_t)name_len;
    out[0x10 + 0x41] = 1u; /* WIN32 namespace */
    for (i = 0; i < name_len; i++) {
        hype_wr16(out + 0x10 + 0x42 + i * 2u, (uint16_t)(uint8_t)name[i]);
    }
    return elen;
}

/* Locates $INDEX_ROOT's $FILE_NAME index in `rec`, refusing anything this
 * slice does not maintain (non-resident, wrong indexed attribute, or an
 * $INDEX_ALLOCATION already present -- a B+tree this slice does not
 * descend or grow). On success: *out_attr_off is the attribute's own
 * offset, *out_val_off the value's offset from record start, *out_val_len
 * its current length, out_ents_off/out_ents_size the index header's own
 * fields (both relative to the index header, i.e. value offset + 0x10). */
static int index_root_locate(hype_ntfs_t *fs, uint8_t *rec, uint32_t *out_attr_off,
                             uint32_t *out_val_off, uint32_t *out_val_len,
                             uint32_t *out_ents_off, uint32_t *out_ents_size) {
    ntfs_attr_t a, dup;
    uint32_t cur = 0;
    const uint8_t *ir;

    if (!(hype_rd16(rec + 0x16) & MFT_IS_DIR)) {
        return -1;
    }
    cur = 0;
    if (attr_find(rec, fs->mft_record_size, AT_INDEX_ALLOCATION, &cur, &dup) == 0) {
        return -1; /* out of scope: a B+tree already exists */
    }
    cur = 0;
    if (attr_find(rec, fs->mft_record_size, AT_INDEX_ROOT, &cur, &a) != 0) {
        return -1;
    }
    if (a.non_resident || a.val_len < 0x20u) {
        return -1;
    }
    *out_attr_off = cur - a.length;
    *out_val_off = *out_attr_off + a.val_off;
    *out_val_len = a.val_len;
    ir = rec + *out_val_off;
    if (hype_rd32(ir) != AT_FILE_NAME) {
        return -1;
    }
    *out_ents_off = hype_rd32(ir + 0x10);
    *out_ents_size = hype_rd32(ir + 0x14);
    if (ir[0x10 + 12] & 0x01u) {
        return -1; /* has-children flag: an $INDEX_ALLOCATION is expected */
    }
    if (*out_ents_off > *out_ents_size || 0x10u + *out_ents_size > *out_val_len) {
        return -1;
    }
    return 0;
}

int hype_ntfs_index_insert(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t dir_rec,
                           uint64_t mft_ref, const char *name, uint32_t name_len, int is_dir,
                           uint16_t usn) {
    static uint8_t rec[HYPE_NTFS_MAX_RECORD];
    uint32_t attr_off, val_off, val_len, ents_off, ents_size;
    uint8_t new_entry[0x10u + 0x42u + 510u];
    uint8_t new_key[510u];
    uint32_t new_elen;
    uint32_t p, insert_at;
    uint32_t new_val_len, new_attr_length, old_attr_length, delta;
    uint32_t bytes_used;
    uint32_t i;

    if (fs == 0 || write == 0 || name == 0 || name_len == 0u || name_len > 255u) {
        return -1;
    }
    if (record_read(fs, dir_rec, rec) != 0) {
        return -1;
    }
    if (index_root_locate(fs, rec, &attr_off, &val_off, &val_len, &ents_off, &ents_size) != 0) {
        return -1;
    }
    /* index_collate() reads UTF-16LE code units (like every on-disk key),
     * so `name` must be widened here, not byte-packed -- every byte of an
     * ASCII `name` is already < HYPE_NTFS_UPCASE_CACHE (256) by
     * construction, decision 24's "fold exactly or not at all" rule holds
     * structurally, the same reasoning name_eq() relies on. */
    for (i = 0; i < name_len; i++) {
        hype_wr16(new_key + i * 2u, (uint16_t)(uint8_t)name[i]);
    }

    /* Walk existing entries in order, finding the sorted insertion point
     * and refusing a case-insensitive duplicate. */
    p = 0;
    insert_at = (uint32_t)~0u;
    while (p + 0x10u <= ents_size - ents_off) {
        const uint8_t *e = rec + val_off + 0x10u + ents_off + p;
        uint32_t elen = hype_rd16(e + 8);
        uint32_t klen = hype_rd16(e + 10);
        uint32_t eflags = hype_rd16(e + 12);
        if (elen < 0x10u || p + elen > ents_size - ents_off) {
            return -1;
        }
        if (eflags & IDX_ENTRY_LAST) {
            if (insert_at == (uint32_t)~0u) {
                insert_at = p;
            }
            break;
        }
        if (klen >= 0x42u && 0x10u + klen <= elen) {
            const uint8_t *fn = e + 0x10u;
            uint32_t fn_nlen = fn[0x40];
            if (0x42u + fn_nlen * 2u <= klen) {
                if (name_eq(fs, fn + 0x42, fn_nlen, name, name_len)) {
                    return -1; /* duplicate name (case-insensitive): refused, like a real create() */
                }
                if (insert_at == (uint32_t)~0u &&
                    index_collate(fs, new_key, name_len, fn + 0x42, fn_nlen) < 0) {
                    insert_at = p;
                }
            }
        }
        p += elen;
    }
    if (insert_at == (uint32_t)~0u) {
        return -1; /* no terminator found: malformed index */
    }

    new_elen = index_build_entry(new_entry, mft_ref, dir_rec, name, name_len, is_dir);
    if (new_elen > sizeof new_entry) {
        return -1;
    }

    new_val_len = val_len + new_elen;
    old_attr_length = 0;
    {
        ntfs_attr_t a;
        uint32_t cur = 0;
        if (attr_find(rec, fs->mft_record_size, AT_INDEX_ROOT, &cur, &a) != 0) {
            return -1;
        }
        old_attr_length = a.length;
    }
    /*
     * NOT a fixed 0x18: $INDEX_ROOT (any "indexed" resident attribute) has
     * an extra 8-byte indexed-flag/reserved field between the standard
     * resident header and its value, so a.val_off is 0x20 here, not 0x18.
     * Confirmed empirically on a real mkntfs/ntfs-3g directory -- assuming
     * 0x18 undercounted the attribute's true header size by 8 bytes,
     * corrupting the splice math even though every individual entry byte
     * was correct. Always use the attribute's OWN val_off, never assume.
     */
    new_attr_length = round_up_8((val_off - attr_off) + new_val_len);
    if (new_attr_length < old_attr_length) {
        new_attr_length = old_attr_length;
    }
    delta = new_attr_length - old_attr_length;

    bytes_used = hype_rd32(rec + 0x18);
    if (bytes_used < attr_off + old_attr_length || bytes_used > fs->mft_record_size) {
        return -1;
    }
    if (delta != 0u) {
        if (bytes_used + delta > fs->mft_record_size) {
            return -1; /* no room: this is exactly the "needs $INDEX_ALLOCATION" case */
        }
        for (i = bytes_used; i > attr_off + old_attr_length; i--) {
            rec[i - 1u + delta] = rec[i - 1u];
        }
        hype_wr32(rec + 0x18, bytes_used + delta);
    }
    hype_wr32(rec + attr_off + 4, new_attr_length);
    hype_wr32(rec + attr_off + 0x10, new_val_len); /* resident val_len */

    /* Shift entries from the insertion point onward (still within the OLD
     * val_len bytes, which are all still valid at their old offsets since
     * the shift above only moved bytes AFTER the whole attribute) forward
     * by new_elen, then splice the new entry into the gap. */
    {
        uint32_t region_start = val_off + 0x10u + ents_off;
        uint32_t region_old_len = ents_size - ents_off;
        for (i = region_old_len; i > insert_at; i--) {
            rec[region_start + i - 1u + new_elen] = rec[region_start + i - 1u];
        }
        for (i = 0; i < new_elen; i++) {
            rec[region_start + insert_at + i] = new_entry[i];
        }
    }
    hype_wr32(rec + val_off + 0x10 + 4, ents_size + new_elen);  /* entries_size */
    hype_wr32(rec + val_off + 0x10 + 8, ents_size + new_elen);  /* allocated: kept tight */

    return hype_ntfs_record_write(fs, write, dir_rec, rec, usn);
}

int hype_ntfs_index_delete(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t dir_rec,
                           const char *name, uint32_t name_len, uint16_t usn) {
    static uint8_t rec[HYPE_NTFS_MAX_RECORD];
    uint32_t attr_off, val_off, val_len, ents_off, ents_size;
    uint32_t p;
    uint32_t found_off = (uint32_t)~0u, found_elen = 0;
    uint32_t new_val_len, old_attr_length;
    uint32_t i;

    if (fs == 0 || write == 0 || name == 0 || name_len == 0u) {
        return -1;
    }
    if (record_read(fs, dir_rec, rec) != 0) {
        return -1;
    }
    if (index_root_locate(fs, rec, &attr_off, &val_off, &val_len, &ents_off, &ents_size) != 0) {
        return -1;
    }

    p = 0;
    while (p + 0x10u <= ents_size - ents_off) {
        const uint8_t *e = rec + val_off + 0x10u + ents_off + p;
        uint32_t elen = hype_rd16(e + 8);
        uint32_t klen = hype_rd16(e + 10);
        uint32_t eflags = hype_rd16(e + 12);
        if (elen < 0x10u || p + elen > ents_size - ents_off) {
            return -1;
        }
        if (eflags & IDX_ENTRY_LAST) {
            break;
        }
        if (klen >= 0x42u && 0x10u + klen <= elen) {
            const uint8_t *fn = e + 0x10u;
            uint32_t fn_nlen = fn[0x40];
            if (0x42u + fn_nlen * 2u <= klen && fn_nlen == name_len) {
                if (name_eq(fs, fn + 0x42, fn_nlen, name, name_len)) {
                    found_off = p;
                    found_elen = elen;
                    break;
                }
            }
        }
        p += elen;
    }
    if (found_off == (uint32_t)~0u) {
        return -1; /* no such name */
    }

    /* Shift everything after the removed entry back over it. */
    {
        uint32_t region_start = val_off + 0x10u + ents_off;
        uint32_t region_old_len = ents_size - ents_off;
        for (i = found_off + found_elen; i < region_old_len; i++) {
            rec[region_start + i - found_elen] = rec[region_start + i];
        }
    }

    new_val_len = val_len - found_elen;
    {
        ntfs_attr_t a;
        uint32_t cur = 0;
        if (attr_find(rec, fs->mft_record_size, AT_INDEX_ROOT, &cur, &a) != 0) {
            return -1;
        }
        old_attr_length = a.length;
    }
    /*
     * The attribute's own declared length is NEVER shrunk here (the next
     * attribute already starts right after it, exactly like #418/#419) --
     * deleting only ever frees content bytes within that same space, so no
     * record-wide shift is ever needed.
     */
    hype_wr32(rec + attr_off + 0x10, new_val_len);
    hype_wr32(rec + val_off + 0x10 + 4, ents_size - found_elen);
    hype_wr32(rec + val_off + 0x10 + 8, ents_size - found_elen);
    /* zero the vacated tail for cleanliness (not required for correctness:
     * it sits past the new val_len, which nothing reads) */
    for (i = (val_off - attr_off) + new_val_len; i < old_attr_length; i++) {
        rec[attr_off + i] = 0u;
    }

    return hype_ntfs_record_write(fs, write, dir_rec, rec, usn);
}


/* ---- #422: resident-to-non-resident $DATA conversion --------------------- */

/*
 * Replaces the byte range [attr_off, attr_off+old_length) with a new
 * region of new_length bytes, shifting every byte from attr_off+old_length
 * to bytes_used accordingly (forward if growing, backward if shrinking --
 * unlike #418/#419/#421, which only ever grow). Updates the record's
 * bytes-in-use field. The caller writes the new attribute's own content
 * into [attr_off, attr_off+new_length) afterward; this only makes room.
 * Returns 0, or -1 if it would not fit in the record.
 */
static int record_resize_attr_region(uint8_t *rec, uint32_t mft_record_size, uint32_t attr_off,
                                     uint32_t old_length, uint32_t new_length) {
    uint32_t bytes_used = hype_rd32(rec + 0x18);
    uint32_t tail_len;
    uint32_t i;

    if (bytes_used < attr_off + old_length || bytes_used > mft_record_size) {
        return -1;
    }
    tail_len = bytes_used - (attr_off + old_length);
    if (new_length > old_length) {
        uint32_t grow = new_length - old_length;
        if (bytes_used + grow > mft_record_size) {
            return -1;
        }
        for (i = tail_len; i > 0; i--) {
            rec[attr_off + new_length + i - 1u] = rec[attr_off + old_length + i - 1u];
        }
        hype_wr32(rec + 0x18, bytes_used + grow);
    } else if (new_length < old_length) {
        uint32_t shrink = old_length - new_length;
        for (i = 0; i < tail_len; i++) {
            rec[attr_off + new_length + i] = rec[attr_off + old_length + i];
        }
        hype_wr32(rec + 0x18, bytes_used - shrink);
    }
    return 0;
}

int hype_ntfs_data_to_nonresident(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t rec_no,
                                  uint64_t new_size, uint16_t usn) {
    static uint8_t rec[HYPE_NTFS_MAX_RECORD];
    ntfs_attr_t a, dup;
    uint32_t cur = 0, dup_cur;
    uint32_t attr_off;
    uint64_t cluster_bytes;
    uint64_t clusters_needed;
    uint64_t lcn;
    uint32_t len_sz, off_sz, rl_len;
    uint8_t rl[1u + 8u + 8u + 1u];
    uint32_t old_attr_length, new_attr_length;
    uint32_t old_val_len;
    uint32_t old_val_off;
    uint32_t i;

    if (fs == 0 || write == 0) {
        return -1;
    }
    if (record_read(fs, rec_no, rec) != 0) {
        return -1;
    }
    cur = 0;
    if (attr_find(rec, fs->mft_record_size, AT_ATTRIBUTE_LIST, &cur, &dup) == 0) {
        return -1;
    }
    cur = 0;
    if (attr_find(rec, fs->mft_record_size, AT_DATA, &cur, &a) != 0) {
        return -1;
    }
    if (a.name_len != 0u) {
        return -1;
    }
    attr_off = cur - a.length;
    dup_cur = cur;
    if (attr_find(rec, fs->mft_record_size, AT_DATA, &dup_cur, &dup) == 0 && dup.name_len == 0u) {
        return -1;
    }
    if (a.non_resident) {
        return -1; /* already non-resident: nothing for this function to do */
    }
    if (a.flags & (ATTR_IS_COMPRESSED | ATTR_IS_ENCRYPTED)) {
        return -1;
    }
    old_val_len = a.val_len;
    old_val_off = a.val_off;
    if (new_size < old_val_len) {
        return -1; /* growth path only: #422 does not truncate */
    }

    cluster_bytes = (uint64_t)fs->spc * SECSZ;
    clusters_needed = (new_size + cluster_bytes - 1u) / cluster_bytes;
    if (clusters_needed == 0u) {
        clusters_needed = 1u; /* a zero-byte non-resident stream still owns one cluster */
    }

    if (hype_ntfs_cluster_alloc(fs, write, clusters_needed, &lcn) != 0) {
        return -1;
    }

    len_sz = min_bytes_unsigned(clusters_needed);
    off_sz = min_bytes_signed((int64_t)lcn);
    rl_len = 1u + len_sz + off_sz + 1u; /* + terminator */
    if (rl_len > sizeof rl) {
        (void)hype_ntfs_cluster_free(fs, write, lcn, clusters_needed);
        return -1;
    }
    rl[0] = (uint8_t)(len_sz | (off_sz << 4));
    for (i = 0; i < len_sz; i++) {
        rl[1u + i] = (uint8_t)(clusters_needed >> (8u * i));
    }
    for (i = 0; i < off_sz; i++) {
        rl[1u + len_sz + i] = (uint8_t)((uint64_t)lcn >> (8u * i));
    }
    rl[1u + len_sz + off_sz] = 0u; /* terminator */

    old_attr_length = a.length;
    new_attr_length = round_up_8(0x40u + rl_len);

    /* Write the real bytes to the medium BEFORE the attribute is replaced:
     * old resident content, zero-padded to new_size, then zero-padded
     * again through the rest of the allocation. */
    {
        uint8_t chunk[BITMAP_CHUNK_BYTES];
        uint64_t sector = lcn * fs->spc;
        uint64_t total_sectors = clusters_needed * fs->spc;
        uint64_t resident_sectors = (old_val_len + SECSZ - 1u) / SECSZ;
        uint32_t k;

        for (i = 0; i < total_sectors; i++) {
            uint32_t n = (uint32_t)(SECSZ < sizeof chunk ? SECSZ : sizeof chunk);
            bfill8(chunk, 0u, n);
            if (i < resident_sectors) {
                uint64_t base = i * SECSZ;
                uint32_t take = (uint32_t)(old_val_len > base ? old_val_len - base : 0u);
                if (take > SECSZ) {
                    take = SECSZ;
                }
                for (k = 0; k < take; k++) {
                    chunk[k] = rec[attr_off + old_val_off + base + k];
                }
            }
            if (write(fs->ctx, sector + i, 1u, chunk) != 0) {
                (void)hype_ntfs_cluster_free(fs, write, lcn, clusters_needed);
                return -1;
            }
        }
    }

    if (record_resize_attr_region(rec, fs->mft_record_size, attr_off, old_attr_length,
                                  new_attr_length) != 0) {
        (void)hype_ntfs_cluster_free(fs, write, lcn, clusters_needed);
        return -1; /* no room: vanishingly unlikely, checked anyway */
    }

    for (i = 0; i < new_attr_length; i++) {
        rec[attr_off + i] = 0u;
    }
    hype_wr32(rec + attr_off + 0, AT_DATA);
    hype_wr32(rec + attr_off + 4, new_attr_length);
    rec[attr_off + 8] = 1u; /* non-resident */
    hype_wr16(rec + attr_off + 0x0C, 0u); /* flags: plain, not sparse/compressed */
    hype_wr64(rec + attr_off + 0x10, 0u); /* start_vcn */
    hype_wr64(rec + attr_off + 0x18, clusters_needed - 1u); /* highest_vcn */
    hype_wr16(rec + attr_off + 0x20, 0x40u); /* mapping pairs offset */
    hype_wr64(rec + attr_off + 0x28, clusters_needed * cluster_bytes); /* allocated size */
    hype_wr64(rec + attr_off + 0x30, new_size);                        /* real size */
    hype_wr64(rec + attr_off + 0x38, new_size);                        /* initialized size */
    for (i = 0; i < rl_len; i++) {
        rec[attr_off + 0x40 + i] = rl[i];
    }

    return hype_ntfs_record_write(fs, write, rec_no, rec, usn);
}


/* ---- #423: create and unlink a regular file ------------------------------ */

/* Appends a new RESIDENT attribute at the end of the record's attribute
 * chain (right where the current end-of-attributes marker sits), refusing
 * if it would not fit. *out_attr_off is the new attribute's own offset. */
static int record_attr_append_resident(uint8_t *rec, uint32_t mft_record_size, uint32_t type,
                                       const uint8_t *value, uint32_t value_len,
                                       uint32_t *out_attr_off) {
    uint32_t attrs_off = hype_rd16(rec + 0x14);
    uint32_t new_len = round_up_8(0x18u + value_len);
    uint32_t end_marker_pos = attrs_off;
    uint32_t i;

    for (;;) {
        ntfs_attr_t tmp;
        int rc = attr_parse(rec, mft_record_size, end_marker_pos, &tmp);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            return -1;
        }
        end_marker_pos += tmp.length;
    }
    if (end_marker_pos + new_len + 4u > mft_record_size) {
        return -1; /* no room: real create() into a packed record fails cleanly */
    }

    for (i = 0; i < new_len; i++) {
        rec[end_marker_pos + i] = 0u;
    }
    hype_wr32(rec + end_marker_pos + 0, type);
    hype_wr32(rec + end_marker_pos + 4, new_len);
    rec[end_marker_pos + 8] = 0u; /* resident */
    hype_wr32(rec + end_marker_pos + 0x10, value_len);
    hype_wr16(rec + end_marker_pos + 0x14, 0x18u); /* val_off */
    for (i = 0; i < value_len; i++) {
        rec[end_marker_pos + 0x18u + i] = value[i];
    }
    hype_wr32(rec + end_marker_pos + new_len, 0xFFFFFFFFu); /* new end marker */
    hype_wr32(rec + 0x18, end_marker_pos + new_len + 4u);
    *out_attr_off = end_marker_pos;
    return 0;
}

int hype_ntfs_create(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t parent_dir_rec,
                     const char *name, uint32_t name_len, uint64_t timestamp_filetime,
                     uint64_t *out_rec_no, uint16_t usn) {
    uint8_t rec[HYPE_NTFS_MAX_RECORD];
    uint8_t si[0x30];
    uint8_t fn[0x42u + 255u * 2u];
    uint32_t fn_len;
    uint64_t rec_no;
    uint16_t seq;
    uint32_t attr_off;
    unsigned i;

    if (fs == 0 || write == 0 || name == 0 || name_len == 0u || name_len > 255u ||
        out_rec_no == 0) {
        return -1;
    }
    if (hype_ntfs_mft_record_alloc(fs, write, 0, &rec_no, &seq, usn) != 0) {
        return -1;
    }
    if (record_read(fs, rec_no, rec) != 0) {
        (void)hype_ntfs_mft_record_free(fs, write, rec_no, usn);
        return -1;
    }

    for (i = 0; i < sizeof si; i++) {
        si[i] = 0u;
    }
    hype_wr64(si + 0x00, timestamp_filetime); /* creation time */
    hype_wr64(si + 0x08, timestamp_filetime); /* modification time */
    hype_wr64(si + 0x10, timestamp_filetime); /* MFT change time */
    hype_wr64(si + 0x18, timestamp_filetime); /* access time */
    hype_wr32(si + 0x20, 0x20u);              /* FILE_ATTRIBUTE_ARCHIVE */
    if (record_attr_append_resident(rec, fs->mft_record_size, AT_STANDARD_INFORMATION, si,
                                    sizeof si, &attr_off) != 0) {
        (void)hype_ntfs_mft_record_free(fs, write, rec_no, usn);
        return -1;
    }

    fn_len = 0x42u + name_len * 2u;
    for (i = 0; i < fn_len; i++) {
        fn[i] = 0u;
    }
    hype_wr64(fn + 0x00, parent_dir_rec & 0x0000FFFFFFFFFFFFull);
    hype_wr64(fn + 0x08, timestamp_filetime);
    hype_wr64(fn + 0x10, timestamp_filetime);
    hype_wr64(fn + 0x18, timestamp_filetime);
    hype_wr64(fn + 0x20, timestamp_filetime);
    hype_wr32(fn + 0x38, 0x20u); /* FILE_ATTRIBUTE_ARCHIVE */
    fn[0x40] = (uint8_t)name_len;
    fn[0x41] = 1u; /* WIN32 namespace */
    for (i = 0; i < name_len; i++) {
        hype_wr16(fn + 0x42 + i * 2u, (uint16_t)(uint8_t)name[i]);
    }
    if (record_attr_append_resident(rec, fs->mft_record_size, AT_FILE_NAME, fn, fn_len,
                                    &attr_off) != 0) {
        (void)hype_ntfs_mft_record_free(fs, write, rec_no, usn);
        return -1;
    }

    if (record_attr_append_resident(rec, fs->mft_record_size, AT_DATA, 0, 0u, &attr_off) != 0) {
        (void)hype_ntfs_mft_record_free(fs, write, rec_no, usn);
        return -1;
    }
    hype_wr16(rec + 0x12, 1u); /* hard link count */

    if (hype_ntfs_record_write(fs, write, rec_no, rec, usn) != 0) {
        (void)hype_ntfs_mft_record_free(fs, write, rec_no, usn);
        return -1;
    }
    if (hype_ntfs_index_insert(fs, write, parent_dir_rec, rec_no, name, name_len, 0, usn) != 0) {
        (void)hype_ntfs_mft_record_free(fs, write, rec_no, usn);
        return -1;
    }

    *out_rec_no = rec_no;
    return 0;
}

/* Releases every DATA range's clusters for record `rec_no`'s unnamed
 * $DATA, if any -- resident owns none, and an $ATTRIBUTE_LIST-split
 * stream is refused (mirrors the write-side scope boundary throughout
 * this epic; unlink() only ever meets what hype_ntfs_create() built). */
static int release_data_clusters(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t rec_no) {
    uint8_t rec[HYPE_NTFS_MAX_RECORD];
    ntfs_attr_t a, dup;
    uint32_t cur = 0;
    static hype_file_rmap_t m;
    uint64_t lcn_cursor = 0;
    unsigned i;

    if (record_read(fs, rec_no, rec) != 0) {
        return -1;
    }
    cur = 0;
    if (attr_find(rec, fs->mft_record_size, AT_ATTRIBUTE_LIST, &cur, &dup) == 0) {
        return -1;
    }
    cur = 0;
    if (attr_find(rec, fs->mft_record_size, AT_DATA, &cur, &a) != 0) {
        return -1; /* every hype_ntfs_create()'d file has one */
    }
    if (!a.non_resident) {
        return 0; /* resident: no clusters to release */
    }
    hype_file_rmap_init(&m, 0);
    if (runlist_decode(fs, rec + (cur - a.length) + a.rl_off, a.length - a.rl_off, &m,
                       &lcn_cursor) != 0) {
        return -1;
    }
    for (i = 0; i < m.count; i++) {
        if (m.ranges[i].kind == HYPE_RANGE_DATA) {
            uint64_t lcn = m.ranges[i].start_lba / fs->spc;
            uint64_t count = m.ranges[i].sector_count / fs->spc;
            if (hype_ntfs_cluster_free(fs, write, lcn, count) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

int hype_ntfs_unlink(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t parent_dir_rec,
                     const char *name, uint32_t name_len, uint16_t usn) {
    uint8_t rec[HYPE_NTFS_MAX_RECORD];
    uint64_t ref;
    int isdir;
    uint16_t links;

    if (fs == 0 || write == 0 || name == 0 || name_len == 0u) {
        return -1;
    }
    if (dir_lookup(fs, parent_dir_rec, name, name_len, &ref, &isdir) != 1) {
        return -1; /* no such name */
    }
    if (isdir) {
        return -1; /* unlink is for regular files; #425 handles directories */
    }
    if (hype_ntfs_index_delete(fs, write, parent_dir_rec, name, name_len, usn) != 0) {
        return -1;
    }

    if (record_read(fs, ref, rec) != 0) {
        return -1; /* index said it existed; the record itself is broken */
    }
    links = hype_rd16(rec + 0x12);
    if (links > 0u) {
        links--;
    }
    hype_wr16(rec + 0x12, links);
    if (hype_ntfs_record_write(fs, write, ref, rec, usn) != 0) {
        return -1;
    }
    if (links != 0u) {
        return 0; /* another name still references this record */
    }

    if (release_data_clusters(fs, write, ref) != 0) {
        return -1;
    }
    return hype_ntfs_mft_record_free(fs, write, ref, usn);
}


/* ---- #425: mkdir and rmdir ------------------------------------------------ */

/* Like record_attr_append_resident(), but for a NAMED resident attribute
 * ($I30, the only name any writer in this module ever needs to produce):
 * name bytes sit between the standard header and the value, so val_off is
 * 0x18 + name_bytes, matching what a real $INDEX_ROOT looks like (#421's
 * val_off-0x20 finding). */
static int record_attr_append_named_resident(uint8_t *rec, uint32_t mft_record_size,
                                              uint32_t type, const uint16_t *name_utf16,
                                              uint32_t name_units, const uint8_t *value,
                                              uint32_t value_len, uint32_t *out_attr_off) {
    uint32_t attrs_off = hype_rd16(rec + 0x14);
    uint32_t name_bytes = name_units * 2u;
    /* No padding between the name and the value: confirmed on a real
     * mkntfs $INDEX_ROOT (#421), val_off sits immediately after the name. */
    uint32_t val_off_rel = 0x18u + name_bytes;
    uint32_t new_len = round_up_8(val_off_rel + value_len);
    uint32_t end_marker_pos = attrs_off;
    uint32_t i;

    for (;;) {
        ntfs_attr_t tmp;
        int rc = attr_parse(rec, mft_record_size, end_marker_pos, &tmp);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            return -1;
        }
        end_marker_pos += tmp.length;
    }
    if (end_marker_pos + new_len + 4u > mft_record_size) {
        return -1;
    }

    for (i = 0; i < new_len; i++) {
        rec[end_marker_pos + i] = 0u;
    }
    hype_wr32(rec + end_marker_pos + 0, type);
    hype_wr32(rec + end_marker_pos + 4, new_len);
    rec[end_marker_pos + 8] = 0u; /* resident */
    rec[end_marker_pos + 9] = (uint8_t)name_units;
    hype_wr16(rec + end_marker_pos + 0x0A, 0x18u); /* name offset */
    hype_wr32(rec + end_marker_pos + 0x10, value_len);
    hype_wr16(rec + end_marker_pos + 0x14, (uint16_t)val_off_rel);
    for (i = 0; i < name_units; i++) {
        hype_wr16(rec + end_marker_pos + 0x18u + i * 2u, name_utf16[i]);
    }
    for (i = 0; i < value_len; i++) {
        rec[end_marker_pos + val_off_rel + i] = value[i];
    }
    hype_wr32(rec + end_marker_pos + new_len, 0xFFFFFFFFu);
    hype_wr32(rec + 0x18, end_marker_pos + new_len + 4u);
    *out_attr_off = end_marker_pos;
    return 0;
}

int hype_ntfs_mkdir(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t parent_dir_rec,
                    const char *name, uint32_t name_len, uint64_t timestamp_filetime,
                    uint64_t *out_rec_no, uint16_t usn) {
    static const uint16_t i30_name[4] = {'$', 'I', '3', '0'};
    uint8_t rec[HYPE_NTFS_MAX_RECORD];
    uint8_t si[0x30];
    uint8_t fn[0x42u + 255u * 2u];
    uint8_t ir[0x10u + 0x10u + 24u]; /* root header + index header + terminator */
    uint32_t fn_len;
    uint64_t rec_no;
    uint16_t seq;
    uint32_t attr_off;
    unsigned i;

    if (fs == 0 || write == 0 || name == 0 || name_len == 0u || name_len > 255u ||
        out_rec_no == 0) {
        return -1;
    }
    if (hype_ntfs_mft_record_alloc(fs, write, 1, &rec_no, &seq, usn) != 0) {
        return -1;
    }
    if (record_read(fs, rec_no, rec) != 0) {
        (void)hype_ntfs_mft_record_free(fs, write, rec_no, usn);
        return -1;
    }

    for (i = 0; i < sizeof si; i++) {
        si[i] = 0u;
    }
    hype_wr64(si + 0x00, timestamp_filetime);
    hype_wr64(si + 0x08, timestamp_filetime);
    hype_wr64(si + 0x10, timestamp_filetime);
    hype_wr64(si + 0x18, timestamp_filetime);
    hype_wr32(si + 0x20, 0x10u); /* FILE_ATTRIBUTE_DIRECTORY */
    if (record_attr_append_resident(rec, fs->mft_record_size, AT_STANDARD_INFORMATION, si,
                                    sizeof si, &attr_off) != 0) {
        (void)hype_ntfs_mft_record_free(fs, write, rec_no, usn);
        return -1;
    }

    fn_len = 0x42u + name_len * 2u;
    for (i = 0; i < fn_len; i++) {
        fn[i] = 0u;
    }
    hype_wr64(fn + 0x00, parent_dir_rec & 0x0000FFFFFFFFFFFFull);
    hype_wr64(fn + 0x08, timestamp_filetime);
    hype_wr64(fn + 0x10, timestamp_filetime);
    hype_wr64(fn + 0x18, timestamp_filetime);
    hype_wr64(fn + 0x20, timestamp_filetime);
    hype_wr32(fn + 0x38, 0x10000010u); /* FILE_ATTRIBUTE_DIRECTORY | I30_INDEX */
    fn[0x40] = (uint8_t)name_len;
    fn[0x41] = 1u;
    for (i = 0; i < name_len; i++) {
        hype_wr16(fn + 0x42 + i * 2u, (uint16_t)(uint8_t)name[i]);
    }
    if (record_attr_append_resident(rec, fs->mft_record_size, AT_FILE_NAME, fn, fn_len,
                                    &attr_off) != 0) {
        (void)hype_ntfs_mft_record_free(fs, write, rec_no, usn);
        return -1;
    }

    for (i = 0; i < sizeof ir; i++) {
        ir[i] = 0u;
    }
    hype_wr32(ir + 0x00, AT_FILE_NAME);
    hype_wr32(ir + 0x04, 1u); /* COLLATION_FILE_NAME */
    hype_wr32(ir + 0x08, 4096u);
    ir[0x0C] = 1u;
    hype_wr32(ir + 0x10 + 0x00, 0x10u); /* entries offset, relative to the index header */
    hype_wr32(ir + 0x10 + 0x04, 0x10u + 24u); /* entries size: header + terminator */
    hype_wr32(ir + 0x10 + 0x08, 0x10u + 24u); /* allocated */
    ir[0x10 + 0x0C] = 0u;                     /* no children */
    hype_wr16(ir + 0x10 + 0x10 + 8, 0x18u);   /* terminator entry length */
    hype_wr16(ir + 0x10 + 0x10 + 12, 0x02u);  /* IDX_ENTRY_LAST */
    if (record_attr_append_named_resident(rec, fs->mft_record_size, AT_INDEX_ROOT, i30_name, 4u,
                                          ir, sizeof ir, &attr_off) != 0) {
        (void)hype_ntfs_mft_record_free(fs, write, rec_no, usn);
        return -1;
    }
    hype_wr16(rec + 0x12, 1u); /* hard link count */

    if (hype_ntfs_record_write(fs, write, rec_no, rec, usn) != 0) {
        (void)hype_ntfs_mft_record_free(fs, write, rec_no, usn);
        return -1;
    }
    if (hype_ntfs_index_insert(fs, write, parent_dir_rec, rec_no, name, name_len, 1, usn) != 0) {
        (void)hype_ntfs_mft_record_free(fs, write, rec_no, usn);
        return -1;
    }

    *out_rec_no = rec_no;
    return 0;
}

int hype_ntfs_rmdir(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t parent_dir_rec,
                    const char *name, uint32_t name_len, uint16_t usn) {
    uint8_t rec[HYPE_NTFS_MAX_RECORD];
    uint64_t ref;
    int isdir;
    uint32_t attr_off, val_off, val_len, ents_off, ents_size;

    if (fs == 0 || write == 0 || name == 0 || name_len == 0u) {
        return -1;
    }
    if (dir_lookup(fs, parent_dir_rec, name, name_len, &ref, &isdir) != 1) {
        return -1;
    }
    if (!isdir) {
        return -1; /* rmdir is for directories; #423 handles regular files */
    }
    if (record_read(fs, ref, rec) != 0) {
        return -1;
    }
    if (index_root_locate(fs, rec, &attr_off, &val_off, &val_len, &ents_off, &ents_size) != 0) {
        return -1; /* not resident-only, or not a directory index: out of scope */
    }
    /* Empty means the FIRST entry in the index is already the terminator
     * (IDX_ENTRY_LAST) -- not merely "ents_size == ents_off", which would
     * wrongly demand a zero-byte terminator that never exists on disk. */
    if (ents_off + 0x10u > ents_size ||
        !(hype_rd16(rec + val_off + 0x10u + ents_off + 12) & IDX_ENTRY_LAST)) {
        return -1; /* non-empty directory: refused, like a real rmdir() */
    }

    if (hype_ntfs_index_delete(fs, write, parent_dir_rec, name, name_len, usn) != 0) {
        return -1;
    }
    return hype_ntfs_mft_record_free(fs, write, ref, usn);
}


/* ---- #424: rename ---------------------------------------------------------- */

/* Rewrites record `rec_no`'s own (first, unnamed WIN32) $FILE_NAME
 * attribute in place: new name, new parent reference. Resident, so this
 * is a resize-and-rebuild like #422's non-resident replace, just for a
 * much smaller, always-resident attribute. */
static int record_update_filename(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t rec_no,
                                  uint64_t new_parent_ref, const char *new_name,
                                  uint32_t new_name_len, uint16_t usn) {
    uint8_t rec[HYPE_NTFS_MAX_RECORD];
    ntfs_attr_t a;
    uint32_t cur = 0;
    uint32_t attr_off;
    uint8_t fn[0x42u + 255u * 2u];
    uint32_t new_val_len, old_attr_length, new_attr_length;
    uint32_t old_flags;
    uint32_t i;

    if (record_read(fs, rec_no, rec) != 0) {
        return -1;
    }
    if (attr_find(rec, fs->mft_record_size, AT_FILE_NAME, &cur, &a) != 0) {
        return -1;
    }
    if (a.non_resident || a.val_len < 0x42u) {
        return -1;
    }
    attr_off = cur - a.length;
    old_flags = hype_rd32(rec + attr_off + a.val_off + 0x38);

    new_val_len = 0x42u + new_name_len * 2u;
    for (i = 0; i < new_val_len; i++) {
        fn[i] = 0u;
    }
    hype_wr64(fn + 0x00, new_parent_ref & 0x0000FFFFFFFFFFFFull);
    hype_wr32(fn + 0x38, old_flags);
    fn[0x40] = (uint8_t)new_name_len;
    fn[0x41] = 1u;
    for (i = 0; i < new_name_len; i++) {
        hype_wr16(fn + 0x42 + i * 2u, (uint16_t)(uint8_t)new_name[i]);
    }

    old_attr_length = a.length;
    new_attr_length = round_up_8(a.val_off + new_val_len);
    if (record_resize_attr_region(rec, fs->mft_record_size, attr_off, old_attr_length,
                                  new_attr_length) != 0) {
        return -1;
    }
    for (i = 0; i < new_attr_length; i++) {
        rec[attr_off + i] = 0u;
    }
    hype_wr32(rec + attr_off + 0, AT_FILE_NAME);
    hype_wr32(rec + attr_off + 4, new_attr_length);
    hype_wr32(rec + attr_off + 0x10, new_val_len);
    hype_wr16(rec + attr_off + 0x14, (uint16_t)a.val_off);
    for (i = 0; i < new_val_len; i++) {
        rec[attr_off + a.val_off + i] = fn[i];
    }

    return hype_ntfs_record_write(fs, write, rec_no, rec, usn);
}

int hype_ntfs_rename(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t src_parent,
                     const char *src_name, uint32_t src_name_len, uint64_t dst_parent,
                     const char *dst_name, uint32_t dst_name_len, uint16_t usn) {
    uint64_t ref;
    int isdir;

    if (fs == 0 || write == 0 || src_name == 0 || src_name_len == 0u || dst_name == 0 ||
        dst_name_len == 0u) {
        return -1;
    }
    if (dir_lookup(fs, src_parent, src_name, src_name_len, &ref, &isdir) != 1) {
        return -1;
    }

    if (hype_ntfs_index_delete(fs, write, src_parent, src_name, src_name_len, usn) != 0) {
        return -1;
    }
    if (hype_ntfs_index_insert(fs, write, dst_parent, ref, dst_name, dst_name_len, isdir, usn) !=
        0) {
        /* never leave it in neither directory: put the old entry back */
        (void)hype_ntfs_index_insert(fs, write, src_parent, ref, src_name, src_name_len, isdir,
                                     usn);
        return -1;
    }

    if (record_update_filename(fs, write, ref, dst_parent, dst_name, dst_name_len, usn) != 0) {
        /* the index already reflects the move; the record's own $FILE_NAME
         * is now stale (old name/parent) but the file is still reachable
         * under its NEW name (dir_lookup() only ever reads through the
         * index, never the target record's own $FILE_NAME) -- inconsistent
         * but not lost, and a real driver's own consistency checker (this
         * ticket's chkdsk bar) flags a $FILE_NAME/index mismatch as
         * repairable, not corrupt. */
        return -1;
    }
    return 0;
}
