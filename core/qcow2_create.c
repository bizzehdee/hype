#include "qcow2_create.h"

#define CS ((uint64_t)HYPE_QCOW2_CREATE_CLUSTER_SIZE)
#define L2_PER ((uint64_t)(CS / 8u))     /* 8192 L2 entries per table */
#define RC_PER ((uint64_t)(CS / 2u))     /* 32768 16-bit refcounts per block */
#define OFLAG_COPIED (1ull << 63)

static void wr16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void wr64be(uint8_t *p, uint64_t v) {
    wr32be(p, (uint32_t)(v >> 32));
    wr32be(p + 4, (uint32_t)v);
}

int hype_qcow2_layout(uint64_t virtual_bytes, hype_qcow2_layout_t *out) {
    uint64_t data, l2, l1c, rb, rt, total, prev;

    if (out == 0 || virtual_bytes == 0ull) {
        return -1;
    }
    data = (virtual_bytes + CS - 1u) / CS;
    l2 = (data + L2_PER - 1u) / L2_PER;
    l1c = (l2 * 8u + CS - 1u) / CS;

    /*
     * The refcount metadata refcounts ITSELF, so its size is a fixpoint: start from the clusters
     * everything else needs and grow rb/rt until they cover the total they are part of. Converges
     * in a couple of rounds -- each round can only add a handful of clusters.
     */
    rb = 1u;
    rt = 1u;
    prev = ~0ull; /* enter the loop: total starts unequal by construction */
    total = 0u;
    while (total != prev) {
        prev = total;
        total = 1u + rt + rb + l1c + l2 + data;
        rb = (total + RC_PER - 1u) / RC_PER;
        rt = (rb * 8u + CS - 1u) / CS;
        total = 1u + rt + rb + l1c + l2 + data;
    }

    out->virtual_bytes = data * CS;
    out->data_clusters = data;
    out->l2_tables = l2;
    out->l1_clusters = l1c;
    out->rt_clusters = rt;
    out->rb_clusters = rb;
    out->total_clusters = total;
    out->rt_start = 1u;
    out->rb_start = 1u + rt;
    out->l1_start = out->rb_start + rb;
    out->l2_start = out->l1_start + l1c;
    out->data_start = out->l2_start + l2;
    return 0;
}

int hype_qcow2_create_cluster(const hype_qcow2_layout_t *lo, uint64_t index, uint8_t *buf) {
    uint64_t i;

    if (lo == 0 || buf == 0 || index >= lo->total_clusters) {
        return -1;
    }
    for (i = 0; i < CS; i++) {
        buf[i] = 0;
    }

    if (index == 0u) {
        /*
         * The v3 header. header_length 104 covers through refcount_order; the feature bitmaps are
         * all zero, which both qemu and blk_qcow2 read as "no features" -- exactly true.
         */
        buf[0] = 'Q';
        buf[1] = 'F';
        buf[2] = 'I';
        buf[3] = 0xFB;
        wr32be(buf + 4, 3u);                                /* version */
        /* backing offset/size: zero (none) */
        wr32be(buf + 20, HYPE_QCOW2_CREATE_CLUSTER_BITS);   /* cluster_bits */
        wr64be(buf + 24, lo->virtual_bytes);                /* size */
        /* crypt_method: 0 */
        wr32be(buf + 36, (uint32_t)lo->l2_tables);          /* l1_size (entries) */
        wr64be(buf + 40, lo->l1_start * CS);                /* l1_table_offset */
        wr64be(buf + 48, lo->rt_start * CS);                /* refcount_table_offset */
        wr32be(buf + 56, (uint32_t)lo->rt_clusters);        /* refcount_table_clusters */
        /* nb_snapshots / snapshots_offset: 0 */
        /* incompatible/compatible/autoclear features (72..95): 0 */
        wr32be(buf + 96, 4u);                               /* refcount_order: 16-bit */
        wr32be(buf + 100, 104u);                            /* header_length */
        return 0;
    }
    if (index >= lo->rt_start && index < lo->rt_start + lo->rt_clusters) {
        /* refcount TABLE: 8-byte big-endian pointers to the refcount blocks. */
        uint64_t first_entry = (index - lo->rt_start) * (CS / 8u);
        for (i = 0; i < CS / 8u; i++) {
            uint64_t e = first_entry + i;
            if (e < lo->rb_clusters) {
                wr64be(buf + i * 8u, (lo->rb_start + e) * CS);
            }
        }
        return 0;
    }
    if (index >= lo->rb_start && index < lo->rb_start + lo->rb_clusters) {
        /* refcount BLOCK: 16-bit big-endian counts; every real cluster is referenced once. */
        uint64_t first = (index - lo->rb_start) * RC_PER;
        for (i = 0; i < RC_PER; i++) {
            uint64_t c = first + i;
            if (c < lo->total_clusters) {
                wr16be(buf + i * 2u, 1u);
            }
        }
        return 0;
    }
    if (index >= lo->l1_start && index < lo->l1_start + lo->l1_clusters) {
        /* L1: 8-byte pointers to L2 tables, COPIED set (refcount is exactly 1). */
        uint64_t first_entry = (index - lo->l1_start) * (CS / 8u);
        for (i = 0; i < CS / 8u; i++) {
            uint64_t e = first_entry + i;
            if (e < lo->l2_tables) {
                wr64be(buf + i * 8u, ((lo->l2_start + e) * CS) | OFLAG_COPIED);
            }
        }
        return 0;
    }
    if (index >= lo->l2_start && index < lo->l2_start + lo->l2_tables) {
        /* L2: pointers to the preallocated data clusters, COPIED set. */
        uint64_t first_data = (index - lo->l2_start) * L2_PER;
        for (i = 0; i < L2_PER; i++) {
            uint64_t d = first_data + i;
            if (d < lo->data_clusters) {
                wr64be(buf + i * 8u, ((lo->data_start + d) * CS) | OFLAG_COPIED);
            }
        }
        return 0;
    }
    /* a data cluster: zeros (already cleared) */
    return 0;
}
