#ifndef HYPE_CORE_FS_BATTERY_SPARSE_H
#define HYPE_CORE_FS_BATTERY_SPARSE_H

#include "fs_ops.h"

/*
 * #692: the SPARSE/HOLE half of the fs-agnostic battery -- driven ONLY
 * through hype_fs_ops_t's public wrappers (core/fs_ops.h), for a driver
 * that advertises HYPE_FS_CAP_SPARSE (today: ext and NTFS; FAT32/exFAT
 * cannot represent an internal hole at all, see core/ext.h's and
 * core/ntfs.h's own documented reasoning).
 *
 * The caller's fixture must supply an EXISTING file at `path` that
 * genuinely contains at least one HOLE or UNWRITTEN range (map_ranges()
 * reporting an all-DATA file is treated as a fixture bug, not a driver
 * capability gap, and fails loudly rather than silently skipping).
 *
 * Proves, uniformly across any HYPE_FS_CAP_SPARSE driver:
 *   - map_ranges() reports the sparse range as HOLE or UNWRITTEN, never DATA;
 *   - reading across it returns genuine zero bytes, not whatever the medium
 *     happens to hold;
 *   - writing INTO it is handled one of two ways, both legitimate and
 *     BOTH recorded (never silently assumed to be the other): the driver
 *     either fills it (ext's own #384/#385 hole-allocating writer) and the
 *     write reads back correctly afterward, or it refuses the write
 *     outright before touching anything (NTFS's decision-30 contract,
 *     honest because filling a hole through this vtable would need a
 *     record number the generic write handle does not carry -- see
 *     core/fs_ops.c's ntfs_create() comment). Exactly one of those two
 *     outcomes must occur; neither happening at all is a failure.
 *
 * NOT built into hype.efi -- test infrastructure, not a boot-time feature.
 */

typedef struct {
    unsigned hole_found;         /* map_ranges() reported a HOLE/UNWRITTEN range */
    unsigned hole_reads_zero;    /* reading across it returned genuine zero bytes */
    unsigned hole_filled_ok;     /* write into the hole succeeded and read back correctly */
    unsigned hole_write_refused; /* write into the hole was refused outright (also legitimate) */
    unsigned failures;
    char first_fail[64];
} hype_fs_battery_sparse_result_t;

typedef void (*hype_fs_battery_sparse_log_fn)(void *ctx, const char *step, int ok);

/*
 * Returns 0 if every step behaved as expected (either sparse-write outcome
 * counts as success), -1 if the driver lacks HYPE_FS_CAP_SPARSE, `path`
 * has no sparse range at all, or any step misbehaved.
 */
int hype_fs_battery_sparse_run(hype_fs_t *fs, const char *path,
                               hype_fs_battery_sparse_result_t *res,
                               hype_fs_battery_sparse_log_fn log, void *logctx);

#endif /* HYPE_CORE_FS_BATTERY_SPARSE_H */
