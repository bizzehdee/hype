#include "blk_qcow2.h"

/*
 * #200: qcow2 format layer. See blk_qcow2.h for the scope and for what is refused.
 */

#define SECSZ HYPE_BLK_SECTOR_SIZE

/* L1/L2 entry layout (qcow2 spec). The offset field is bits 9..55, so the mask both
 * extracts the offset and asserts its 512-byte alignment. */
#define QCOW2_ENTRY_OFFSET_MASK 0x00FFFFFFFFFFFE00ULL
#define QCOW2_ENTRY_COPIED (1ULL << 63)
#define QCOW2_ENTRY_COMPRESSED (1ULL << 62)
#define QCOW2_L2_ZERO 1ULL /* standard cluster reads as all zeroes */

/* Header field offsets. */
#define QCOW2_OFF_MAGIC 0u
#define QCOW2_OFF_VERSION 4u
#define QCOW2_OFF_BACKING_OFFSET 8u
#define QCOW2_OFF_BACKING_SIZE 16u
#define QCOW2_OFF_CLUSTER_BITS 20u
#define QCOW2_OFF_SIZE 24u
#define QCOW2_OFF_CRYPT 32u
#define QCOW2_OFF_L1_SIZE 36u
#define QCOW2_OFF_L1_OFFSET 40u
#define QCOW2_OFF_REFCOUNT_TABLE_OFFSET 48u
#define QCOW2_OFF_REFCOUNT_TABLE_CLUSTERS 56u
#define QCOW2_OFF_NB_SNAPSHOTS 60u
#define QCOW2_OFF_INCOMPAT 72u
#define QCOW2_OFF_REFCOUNT_ORDER 96u

/* The only refcount width this file writes. qcow2 v2 has no refcount_order field and
 * is defined to use 4; v3 images from qemu-img default to it. */
#define QCOW2_REQUIRED_REFCOUNT_ORDER 4u

/*
 * A refcount block cluster may itself need a refcount, whose block may not exist
 * either. In practice that nests once. The cap turns a malformed image that would
 * otherwise recurse without bound into a refusal.
 */
#define QCOW2_RC_MAX_DEPTH 3u

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

static void put_be64(uint8_t *p, uint64_t v) {
    unsigned int i;
    for (i = 0; i < 8u; i++) {
        p[i] = (uint8_t)(v >> (56 - 8 * i));
    }
}

/* Load the sector containing `off` into q->sec. */
static int load_sector(hype_qcow2_t *q, uint64_t off) {
    return hype_blk_backend_read(q->file, off / SECSZ, 1, q->sec);
}

static int store_sector(hype_qcow2_t *q, uint64_t off) {
    return hype_blk_backend_write(q->file, off / SECSZ, 1, q->sec);
}

/* Read one 64-bit big-endian table entry at an arbitrary file offset. */
static int read_entry64(hype_qcow2_t *q, uint64_t off, uint64_t *out) {
    if (load_sector(q, off) != 0) {
        return -1;
    }
    *out = be64(&q->sec[off % SECSZ]);
    return 0;
}

/* Read-modify-write one 64-bit big-endian table entry. */
static int write_entry64(hype_qcow2_t *q, uint64_t off, uint64_t value) {
    if (load_sector(q, off) != 0) {
        return -1;
    }
    put_be64(&q->sec[off % SECSZ], value);
    return store_sector(q, off);
}

/* 16-bit refcount entries per refcount block. */
static uint64_t rc_entries_per_block(const hype_qcow2_t *q) {
    return q->cluster_size / 2u;
}

/* 64-bit entries in the refcount table. */
static uint64_t rc_table_entries(const hype_qcow2_t *q) {
    return ((uint64_t)q->refcount_table_clusters * q->cluster_size) / 8u;
}

static int zero_cluster(hype_qcow2_t *q, uint64_t cluster_idx) {
    uint64_t base = cluster_idx << q->cluster_bits;
    uint64_t n = q->cluster_size / SECSZ;
    uint64_t i;

    for (i = 0; i < SECSZ; i++) {
        q->sec[i] = 0;
    }
    for (i = 0; i < n; i++) {
        if (hype_blk_backend_write(q->file, (base / SECSZ) + i, 1, q->sec) != 0) {
            return -1;
        }
    }
    return 0;
}

/*
 * Hand out the next free cluster. The ceiling is the underlying backend's capacity:
 * this layer cannot extend the file (the raw-image backend below it is a fixed extent
 * list), so a full image is a hard refusal rather than a write that lands outside the
 * image.
 */
static int alloc_cluster(hype_qcow2_t *q, uint64_t *out_idx) {
    if (q->next_free_cluster >= q->file_clusters) {
        return -1;
    }
    *out_idx = q->next_free_cluster;
    q->next_free_cluster++;
    return 0;
}

static int rc_set(hype_qcow2_t *q, uint64_t cluster_idx, uint16_t value, unsigned int depth);

/* Point the refcount table at a newly allocated block cluster covering `tbl_idx`. */
static int rc_add_block(hype_qcow2_t *q, uint64_t tbl_idx, uint64_t *out_block,
                        unsigned int depth) {
    uint64_t blk = 0;

    if (alloc_cluster(q, &blk) != 0) {
        return -1;
    }
    if (zero_cluster(q, blk) != 0) {
        return -1;
    }
    if (write_entry64(q, q->refcount_table_offset + tbl_idx * 8u, blk << q->cluster_bits) != 0) {
        return -1;
    }
    /* The block cluster is itself allocated space and must be accounted for, or a later
     * qemu-img check reports a leaked cluster. Its own entry may live in the block just
     * created, which is why this is ordered after the table entry is installed. */
    if (rc_set(q, blk, 1, depth + 1u) != 0) {
        return -1;
    }
    *out_block = blk;
    return 0;
}

static int rc_set(hype_qcow2_t *q, uint64_t cluster_idx, uint16_t value, unsigned int depth) {
    uint64_t per = rc_entries_per_block(q);
    uint64_t tbl_idx = cluster_idx / per;
    uint64_t in_blk = cluster_idx % per;
    uint64_t tbl_entry = 0;
    uint64_t blk_off;
    uint64_t entry_off;

    if (depth >= QCOW2_RC_MAX_DEPTH) {
        return -1;
    }
    if (tbl_idx >= rc_table_entries(q)) {
        return -1; /* would need a bigger refcount table -- a prep-time mistake */
    }
    if (read_entry64(q, q->refcount_table_offset + tbl_idx * 8u, &tbl_entry) != 0) {
        return -1;
    }
    blk_off = tbl_entry & QCOW2_ENTRY_OFFSET_MASK;
    if (blk_off == 0u) {
        uint64_t blk = 0;
        if (rc_add_block(q, tbl_idx, &blk, depth) != 0) {
            return -1;
        }
        blk_off = blk << q->cluster_bits;
    }

    entry_off = blk_off + in_blk * 2u;
    if (load_sector(q, entry_off) != 0) {
        return -1;
    }
    q->sec[entry_off % SECSZ] = (uint8_t)(value >> 8);
    q->sec[(entry_off % SECSZ) + 1u] = (uint8_t)(value & 0xFFu);
    return store_sector(q, entry_off);
}

/*
 * Establish the allocation cursor by walking the refcount map: every allocated cluster
 * has a nonzero refcount, so one past the highest nonzero entry is the first cluster
 * that is safe to hand out.
 *
 * Derived rather than stored because qcow2 has no end-of-data marker -- qemu infers it
 * from the file length, which is useless here: the backing file is deliberately
 * pre-allocated larger than the qcow2 content (see #199), so its length says nothing
 * about what is in use.
 */
static int scan_next_free(hype_qcow2_t *q) {
    uint64_t tbl_entries = rc_table_entries(q);
    uint64_t per = rc_entries_per_block(q);
    uint64_t highest = 0;
    uint64_t t;

    for (t = 0; t < tbl_entries; t++) {
        uint64_t tbl_entry = 0;
        uint64_t blk_off;
        uint64_t sec_i;

        if (read_entry64(q, q->refcount_table_offset + t * 8u, &tbl_entry) != 0) {
            return -1;
        }
        blk_off = tbl_entry & QCOW2_ENTRY_OFFSET_MASK;
        if (blk_off == 0u) {
            continue;
        }
        for (sec_i = 0; sec_i < q->cluster_size / SECSZ; sec_i++) {
            unsigned int i;
            if (hype_blk_backend_read(q->file, (blk_off / SECSZ) + sec_i, 1, q->sec) != 0) {
                return -1;
            }
            for (i = 0; i < SECSZ; i += 2u) {
                if (q->sec[i] != 0u || q->sec[i + 1u] != 0u) {
                    uint64_t idx = t * per + sec_i * (SECSZ / 2u) + (i / 2u);
                    if (idx >= highest) {
                        highest = idx + 1u;
                    }
                }
            }
        }
    }
    /*
     * Floor the cursor past the structures the header itself points at. The scan alone
     * would trust the refcount map completely, so an image whose map understates what is
     * in use -- a truncated file, a bad transfer, anything -- would hand out cluster 0
     * and the very next allocation would overwrite the HEADER. Refusing to allocate
     * below the metadata costs a couple of clusters and removes that whole class of
     * outcome.
     */
    {
        uint64_t floor = 1u; /* cluster 0 is the header */
        uint64_t l1_end = (q->l1_offset + (uint64_t)q->l1_size * 8u + q->cluster_size - 1u) >>
                          q->cluster_bits;
        uint64_t rt_end =
            (q->refcount_table_offset >> q->cluster_bits) + (uint64_t)q->refcount_table_clusters;
        if (l1_end > floor) {
            floor = l1_end;
        }
        if (rt_end > floor) {
            floor = rt_end;
        }
        if (highest < floor) {
            highest = floor;
        }
    }
    q->next_free_cluster = highest;
    return 0;
}

int hype_qcow2_map(hype_qcow2_t *q, uint64_t guest_offset, uint64_t *out_file_offset) {
    uint64_t l1_index = guest_offset >> (q->cluster_bits + q->l2_bits);
    uint64_t l2_index = (guest_offset >> q->cluster_bits) & ((1ULL << q->l2_bits) - 1u);
    uint64_t l1_entry = 0;
    uint64_t l2_entry = 0;
    uint64_t l2_off;
    uint64_t host;

    if (l1_index >= (uint64_t)q->l1_size) {
        return 0; /* past the mapped L1 -- unallocated, not an error */
    }
    if (read_entry64(q, q->l1_offset + l1_index * 8u, &l1_entry) != 0) {
        return -1;
    }
    l2_off = l1_entry & QCOW2_ENTRY_OFFSET_MASK;
    if (l2_off == 0u) {
        return 0;
    }
    if (read_entry64(q, l2_off + l2_index * 8u, &l2_entry) != 0) {
        return -1;
    }
    if ((l2_entry & QCOW2_ENTRY_COMPRESSED) != 0u) {
        return -1; /* no decompressor here -- refuse rather than serve garbage */
    }
    if ((l2_entry & QCOW2_L2_ZERO) != 0u) {
        return 0; /* explicitly all-zeroes; same handling as unallocated */
    }
    host = l2_entry & QCOW2_ENTRY_OFFSET_MASK;
    if (host == 0u) {
        return 0;
    }
    *out_file_offset = host + (guest_offset & (q->cluster_size - 1u));
    return 1;
}

/* Sectors from `guest_offset` to the end of its cluster, capped at `want`. */
static uint32_t run_in_cluster(const hype_qcow2_t *q, uint64_t guest_offset, uint32_t want) {
    uint64_t left = (q->cluster_size - (guest_offset & (q->cluster_size - 1u))) / SECSZ;
    return (uint64_t)want < left ? want : (uint32_t)left;
}

static void zero_buf(uint8_t *p, uint64_t n) {
    uint64_t i;
    for (i = 0; i < n; i++) {
        p[i] = 0;
    }
}

static int qcow2_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    hype_qcow2_t *q = (hype_qcow2_t *)ctx;
    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;

    while (done < count) {
        uint64_t guest_off = (lba + done) * (uint64_t)SECSZ;
        uint64_t file_off = 0;
        uint32_t n = run_in_cluster(q, guest_off, count - done);
        int mapped = hype_qcow2_map(q, guest_off, &file_off);

        if (mapped < 0) {
            return -1;
        }
        if (mapped == 1) {
            if (hype_blk_backend_read(q->file, file_off / SECSZ, n, out + (uint64_t)done * SECSZ) !=
                0) {
                return -1;
            }
        } else if (q->backing != 0) {
            /* Unallocated: the chain below owns this range. Addressed in GUEST sectors,
             * because a backing image is a disk of the same virtual geometry. */
            if (hype_blk_backend_read((hype_blk_backend_t *)q->backing, lba + done, n,
                                      out + (uint64_t)done * SECSZ) != 0) {
                return -1;
            }
        } else {
            zero_buf(out + (uint64_t)done * SECSZ, (uint64_t)n * SECSZ);
        }
        done += n;
    }
    return 0;
}

/*
 * Make the cluster holding `guest_off` writable in place, allocating the L2 table and
 * the data cluster as needed, and return its file offset for `guest_off`.
 *
 * A newly allocated cluster is fully populated before the caller's payload lands on it
 * -- zeroed, or copied from the backing image -- because the payload may cover only
 * part of it and the rest must read back as what it read before, not as whatever the
 * pre-allocated file happened to contain.
 */
static int ensure_allocated(hype_qcow2_t *q, uint64_t guest_off, uint64_t *out_file_offset) {
    uint64_t l1_index = guest_off >> (q->cluster_bits + q->l2_bits);
    uint64_t l2_index = (guest_off >> q->cluster_bits) & ((1ULL << q->l2_bits) - 1u);
    uint64_t l1_entry = 0;
    uint64_t l2_entry = 0;
    uint64_t l2_off;
    uint64_t data_cluster = 0;
    int fresh_l2 = 0;

    if (l1_index >= (uint64_t)q->l1_size) {
        return -1; /* outside the virtual disk; the bounds gate should have caught it */
    }
    if (read_entry64(q, q->l1_offset + l1_index * 8u, &l1_entry) != 0) {
        return -1;
    }
    l2_off = l1_entry & QCOW2_ENTRY_OFFSET_MASK;
    if (l2_off != 0u && (l1_entry & QCOW2_ENTRY_COPIED) == 0u) {
        /* Shared with a snapshot: rewriting this L2 table would rewrite the snapshot's
         * view too. hype takes no snapshots, so this means the image came from
         * elsewhere -- refuse rather than damage it. */
        return -1;
    }
    if (l2_off == 0u) {
        uint64_t l2_cluster = 0;
        if (alloc_cluster(q, &l2_cluster) != 0) {
            return -1;
        }
        if (zero_cluster(q, l2_cluster) != 0) {
            return -1;
        }
        if (rc_set(q, l2_cluster, 1, 0) != 0) {
            return -1;
        }
        l2_off = l2_cluster << q->cluster_bits;
        if (write_entry64(q, q->l1_offset + l1_index * 8u, l2_off | QCOW2_ENTRY_COPIED) != 0) {
            return -1;
        }
        fresh_l2 = 1;
    }

    if (!fresh_l2) {
        if (read_entry64(q, l2_off + l2_index * 8u, &l2_entry) != 0) {
            return -1;
        }
        if ((l2_entry & QCOW2_ENTRY_COMPRESSED) != 0u) {
            return -1;
        }
        if ((l2_entry & QCOW2_ENTRY_OFFSET_MASK) != 0u && (l2_entry & QCOW2_L2_ZERO) == 0u) {
            if ((l2_entry & QCOW2_ENTRY_COPIED) == 0u) {
                return -1; /* shared data cluster -- see the L2 case above */
            }
            *out_file_offset =
                (l2_entry & QCOW2_ENTRY_OFFSET_MASK) + (guest_off & (q->cluster_size - 1u));
            return 0;
        }
    }

    if (alloc_cluster(q, &data_cluster) != 0) {
        return -1;
    }
    if (q->backing != 0 && (l2_entry & QCOW2_L2_ZERO) == 0u) {
        /* Copy-on-write from the chain. Whole cluster, one sector at a time -- there is
         * no cluster-sized buffer here by design. */
        uint64_t cluster_base_guest = guest_off & ~(q->cluster_size - 1u);
        uint64_t i;
        for (i = 0; i < q->cluster_size / SECSZ; i++) {
            if (hype_blk_backend_read((hype_blk_backend_t *)q->backing,
                                      cluster_base_guest / SECSZ + i, 1, q->sec) != 0) {
                return -1;
            }
            if (hype_blk_backend_write(q->file, ((data_cluster << q->cluster_bits) / SECSZ) + i, 1,
                                       q->sec) != 0) {
                return -1;
            }
        }
    } else if (zero_cluster(q, data_cluster) != 0) {
        return -1;
    }
    if (rc_set(q, data_cluster, 1, 0) != 0) {
        return -1;
    }
    if (write_entry64(q, l2_off + l2_index * 8u,
                      (data_cluster << q->cluster_bits) | QCOW2_ENTRY_COPIED) != 0) {
        return -1;
    }
    *out_file_offset = (data_cluster << q->cluster_bits) + (guest_off & (q->cluster_size - 1u));
    return 0;
}

static int qcow2_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    hype_qcow2_t *q = (hype_qcow2_t *)ctx;
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t done = 0;

    while (done < count) {
        uint64_t guest_off = (lba + done) * (uint64_t)SECSZ;
        uint64_t file_off = 0;
        uint32_t n = run_in_cluster(q, guest_off, count - done);

        if (ensure_allocated(q, guest_off, &file_off) != 0) {
            return -1;
        }
        if (hype_blk_backend_write(q->file, file_off / SECSZ, n,
                                   in + (uint64_t)done * SECSZ) != 0) {
            return -1;
        }
        done += n;
    }
    return 0;
}

int hype_qcow2_init(hype_qcow2_t *q, hype_blk_backend_t *be, hype_blk_backend_t *file,
                    const hype_blk_backend_t *backing) {
    uint32_t backing_size;
    uint64_t needed_l1;

    if (q == 0 || be == 0 || file == 0) {
        return -1;
    }
    q->file = file;
    q->backing = backing;

    if (hype_blk_backend_read(file, 0, 1, q->sec) != 0) {
        return -1;
    }
    if (be32(&q->sec[QCOW2_OFF_MAGIC]) != HYPE_QCOW2_MAGIC) {
        return -1;
    }
    q->version = be32(&q->sec[QCOW2_OFF_VERSION]);
    if (q->version != 2u && q->version != 3u) {
        return -1;
    }
    q->cluster_bits = be32(&q->sec[QCOW2_OFF_CLUSTER_BITS]);
    if (q->cluster_bits < HYPE_QCOW2_MIN_CLUSTER_BITS ||
        q->cluster_bits > HYPE_QCOW2_MAX_CLUSTER_BITS) {
        return -1;
    }
    q->cluster_size = 1ULL << q->cluster_bits;
    q->l2_bits = q->cluster_bits - 3u; /* 8 bytes per L2 entry */
    q->virtual_size = be64(&q->sec[QCOW2_OFF_SIZE]);
    if (q->virtual_size < SECSZ) {
        return -1; /* nothing a guest could use as a disk */
    }
    if (be32(&q->sec[QCOW2_OFF_CRYPT]) != 0u) {
        return -1; /* encrypted: no key handling here */
    }
    if (be32(&q->sec[QCOW2_OFF_NB_SNAPSHOTS]) != 0u) {
        /* Snapshots share clusters with the active layer; writing in place would alter
         * them. Refused at init rather than per-cluster so the operator learns at
         * attach time, not mid-install. */
        return -1;
    }
    q->l1_size = be32(&q->sec[QCOW2_OFF_L1_SIZE]);
    q->l1_offset = be64(&q->sec[QCOW2_OFF_L1_OFFSET]);
    q->refcount_table_offset = be64(&q->sec[QCOW2_OFF_REFCOUNT_TABLE_OFFSET]);
    q->refcount_table_clusters = be32(&q->sec[QCOW2_OFF_REFCOUNT_TABLE_CLUSTERS]);
    if (q->l1_offset == 0u || q->refcount_table_offset == 0u ||
        q->refcount_table_clusters == 0u) {
        return -1;
    }
    if ((q->l1_offset & (q->cluster_size - 1u)) != 0u ||
        (q->refcount_table_offset & (q->cluster_size - 1u)) != 0u) {
        return -1; /* both must be cluster-aligned */
    }

    /* The L1 must cover the whole virtual size, or a guest write to the tail would land
     * outside the table and be silently dropped. */
    needed_l1 = (q->virtual_size + (1ULL << (q->cluster_bits + q->l2_bits)) - 1u) >>
                (q->cluster_bits + q->l2_bits);
    if ((uint64_t)q->l1_size < needed_l1) {
        return -1;
    }

    backing_size = be32(&q->sec[QCOW2_OFF_BACKING_SIZE]);
    if (backing_size != 0u && backing == 0) {
        return -1; /* chain named but not supplied -- see the header */
    }

    if (q->version >= 3u) {
        if (be64(&q->sec[QCOW2_OFF_INCOMPAT]) != 0u) {
            return -1; /* extended L2 entries, dirty bit, corrupt bit, ... */
        }
        if (be32(&q->sec[QCOW2_OFF_REFCOUNT_ORDER]) != QCOW2_REQUIRED_REFCOUNT_ORDER) {
            return -1;
        }
    }

    q->file_clusters = (file->total_sectors * (uint64_t)SECSZ) >> q->cluster_bits;
    if (q->file_clusters == 0u) {
        return -1;
    }
    if (scan_next_free(q) != 0) {
        return -1;
    }

    be->read = qcow2_read;
    be->write = (file->write != 0) ? qcow2_write : 0;
    be->ctx = q;
    be->total_sectors = q->virtual_size / SECSZ;
    return 0;
}
