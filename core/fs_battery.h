#ifndef HYPE_CORE_FS_BATTERY_H
#define HYPE_CORE_FS_BATTERY_H

#include "fs_ops.h"

/*
 * #692: the GENERIC half of the fs-agnostic battery -- content read/write/
 * append on an ALREADY-EXISTING file, driven ONLY through hype_fs_ops_t's
 * public wrappers (core/fs_ops.h). Every step is capability-adaptive
 * (skipped, not failed, when a driver's own caps say it does not apply),
 * so the SAME call proves whatever a given driver actually supports:
 * FAT32/exFAT/ext all advertise both write_at and append; NTFS advertises
 * write_at only (its append slot is deliberately NULL -- growth needs a
 * handle carrying the MFT record number, #692's own documented follow-up).
 * FAT32/exFAT additionally have no namespace mutation at all to CREATE a
 * file in the first place -- see core/fs_battery_ntfs_ext.h for the
 * create/unlink/mkdir/rmdir/rename half only ext and NTFS can run, and
 * core/fs_battery_sparse.h for HOLE/UNWRITTEN-specific behavior.
 *
 * Deliberately takes an EXISTING path rather than creating one itself:
 * that keeps this module honestly runnable against FAT32/exFAT fixtures
 * (which have no create()) as well as ext/NTFS ones, with no special-casing
 * for which driver it is talking to. Every step is capability-adaptive
 * (skipped, not failed, when the driver's own caps say it does not apply)
 * so the SAME call proves whatever a given driver actually supports.
 *
 * NOT built into hype.efi (see core/tests/run.sh's own LIB_DIRS glob,
 * which links every core .c file into the host test binaries regardless
 * of the top-level Makefile's CORE_SRCS list) -- this is test
 * infrastructure, not a boot-time feature.
 */

typedef struct {
    unsigned read_ok;          /* lookup + read_at succeeded */
    unsigned write_verified;   /* write_at + read-back byte-exact */
    unsigned append_verified;  /* append + read-back (whole file) byte-exact */
    unsigned skipped_no_write; /* read-only mount, or driver lacks WRITE_INPLACE */
    unsigned skipped_no_append; /* driver lacks HYPE_FS_CAP_APPEND */
    unsigned failures;
    char first_fail[64];
} hype_fs_battery_result_t;

typedef void (*hype_fs_battery_log_fn)(void *ctx, const char *step, int ok);

/*
 * Runs against `path`, which MUST already exist and be readable (this is a
 * content battery, not a namespace one -- the caller's own fixture creates
 * the file). Returns 0 if every step behaved as expected (a capability the
 * driver does not have counts as a skip, not a failure), -1 if the initial
 * lookup/read fails or any exercised step misbehaves.
 */
int hype_fs_battery_run(hype_fs_t *fs, const char *path, hype_fs_battery_result_t *res,
                        hype_fs_battery_log_fn log, void *logctx);

#endif /* HYPE_CORE_FS_BATTERY_H */
