#ifndef HYPE_CORE_QCOW2_CREATE_H
#define HYPE_CORE_QCOW2_CREATE_H

#include <stdint.h>

/*
 * TERM-11 (#487): the qcow2 image WRITER core/blk_qcow2.h names as missing -- header, refcount
 * table and blocks, L1 and L2 tables, FULLY PREALLOCATED: every data cluster allocated and
 * refcounted up front, so the guest never triggers an allocation at runtime and the on-disk file
 * occupies the full virtual size plus metadata. That is the shape qemu-img itself produces with
 * preallocation=full (minus the data writes qemu delegates to fallocate), and it is exactly what
 * hype's own qcow2 reader/writer (core/blk_qcow2.c) supports: v3, refcount_order 4, 64 KiB
 * clusters, no features.
 *
 * PURE, and shaped for streaming: hype_qcow2_layout() computes the whole image's geometry, and
 * hype_qcow2_create_cluster() renders any single cluster's bytes by index -- so the caller can
 * emit cluster 0..total-1 sequentially through whatever writer it has (a hype_fs append), pace
 * itself for progress reporting, and never hold more than one cluster in memory. Every content
 * decision is here and unit-tested against hype's own reader; the caller only moves bytes.
 */

#define HYPE_QCOW2_CREATE_CLUSTER_BITS 16u /* 64 KiB -- qemu-img's default, blk_qcow2's shape */
#define HYPE_QCOW2_CREATE_CLUSTER_SIZE (1u << HYPE_QCOW2_CREATE_CLUSTER_BITS)

typedef struct {
    uint64_t virtual_bytes;   /* the guest-visible size (cluster-aligned up) */
    uint64_t data_clusters;   /* clusters carrying guest data */
    uint64_t l2_tables;       /* == l1 entries in use */
    uint64_t l1_clusters;     /* clusters holding the L1 table */
    uint64_t rt_clusters;     /* clusters holding the refcount table */
    uint64_t rb_clusters;     /* clusters holding refcount blocks */
    uint64_t total_clusters;  /* the whole file: 1 header + rt + rb + l1 + l2 + data */
    /* first-cluster indexes of each region, in file order */
    uint64_t rt_start;        /* == 1 */
    uint64_t rb_start;
    uint64_t l1_start;
    uint64_t l2_start;
    uint64_t data_start;
} hype_qcow2_layout_t;

/*
 * Computes the layout for `virtual_bytes` (rounded up to a whole cluster; 0 is refused). The
 * refcount metadata is solved to a fixpoint -- refcount blocks must also cover themselves.
 * Returns 0, or -1 for a size of 0 or one whose L1 would not fit the layout's own assumptions.
 */
int hype_qcow2_layout(uint64_t virtual_bytes, hype_qcow2_layout_t *out);

/*
 * Renders cluster `index` (0 .. total_clusters-1) of the image into `buf`
 * (HYPE_QCOW2_CREATE_CLUSTER_SIZE bytes). Deterministic and pure: the same index always renders
 * the same bytes. Data clusters render as zeros. Returns 0, or -1 for an out-of-range index.
 */
int hype_qcow2_create_cluster(const hype_qcow2_layout_t *lo, uint64_t index, uint8_t *buf);

#endif /* HYPE_CORE_QCOW2_CREATE_H */
