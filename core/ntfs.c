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

/* First free run of `count` contiguous clusters, scanning from cluster 0 in
 * BITMAP_CHUNK_BYTES-sized reads. Returns 0 and fills *out_start, or -1 if
 * no run that large exists. */
static int bitmap_find_free(hype_ntfs_t *fs, uint64_t count, uint64_t *out_start) {
    uint8_t buf[BITMAP_CHUNK_BYTES];
    uint64_t bit = 0;
    uint64_t total_bytes = (fs->total_clusters + 7u) / 8u;
    uint64_t run_start = 0, run_len = 0;
    int in_run = 0;

    while (bit < fs->total_clusters) {
        uint64_t byte_off = bit / 8u;
        uint64_t remaining_bytes = total_bytes - byte_off;
        uint32_t chunk_bytes =
            remaining_bytes < BITMAP_CHUNK_BYTES ? (uint32_t)remaining_bytes : BITMAP_CHUNK_BYTES;
        uint64_t chunk_bits = (uint64_t)chunk_bytes * 8u;
        uint64_t i;

        if (hype_file_rmap_read_at(&fs->bitmap, fs->read, fs->ctx, byte_off, buf, chunk_bytes) !=
            0) {
            return -1;
        }
        for (i = 0; i < chunk_bits && bit < fs->total_clusters; i++, bit++) {
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

/* True iff every bit in [start_bit, start_bit+count) equals `want_used`. */
static int bitmap_run_is(hype_ntfs_t *fs, uint64_t start_bit, uint64_t count, int want_used) {
    uint8_t buf[BITMAP_CHUNK_BYTES];
    uint64_t bit = start_bit;
    uint64_t end = start_bit + count;
    uint64_t total_bytes = (fs->total_clusters + 7u) / 8u;

    while (bit < end) {
        uint64_t byte_off = bit / 8u;
        uint64_t remaining_bytes = total_bytes - byte_off;
        uint32_t chunk_bytes =
            remaining_bytes < BITMAP_CHUNK_BYTES ? (uint32_t)remaining_bytes : BITMAP_CHUNK_BYTES;
        uint64_t chunk_bits = (uint64_t)chunk_bytes * 8u;
        uint64_t base = bit;
        uint64_t i;

        if (hype_file_rmap_read_at(&fs->bitmap, fs->read, fs->ctx, byte_off, buf, chunk_bytes) !=
            0) {
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

/* Sets or clears every bit in [start_bit, start_bit+count). Ragged leading
 * and trailing bytes go through single-byte read-modify-write; whole bytes
 * in between are written directly (no read needed -- the value doesn't
 * depend on what was there). */
static int bitmap_set_run(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t start_bit,
                          uint64_t count, int used) {
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

            if (hype_file_rmap_read_at(&fs->bitmap, fs->read, fs->ctx, byte_off, &b, 1u) != 0) {
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
            if (hype_file_rmap_write_at(&fs->bitmap, fs->read, write, fs->ctx, byte_off, &b, 1u) !=
                0) {
                return -1;
            }
            bit += n;
        } else {
            uint8_t chunk[BITMAP_CHUNK_BYTES];
            uint64_t whole_bytes = (end - bit) / 8u;
            uint32_t n =
                whole_bytes < BITMAP_CHUNK_BYTES ? (uint32_t)whole_bytes : BITMAP_CHUNK_BYTES;
            bfill8(chunk, used ? 0xFFu : 0x00u, n);
            if (hype_file_rmap_write_at(&fs->bitmap, fs->read, write, fs->ctx, byte_off, chunk,
                                        n) != 0) {
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
    if (bitmap_find_free(fs, count, &start) != 0) {
        return -1;
    }
    if (bitmap_set_run(fs, write, start, count, 1) != 0) {
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
    if (!bitmap_run_is(fs, lcn, count, 1)) {
        return -1; /* not fully allocated: caller bug or an already-inconsistent bitmap */
    }
    return bitmap_set_run(fs, write, lcn, count, 0);
}
