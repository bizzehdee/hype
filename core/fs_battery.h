#ifndef HYPE_CORE_FS_BATTERY_H
#define HYPE_CORE_FS_BATTERY_H

#include "fs_ops.h"

/*
 * #692: a generic, filesystem-agnostic writer battery. Drives ONLY
 * hype_fs_ops_t's public wrappers (core/fs_ops.h) -- create, unlink,
 * mkdir, rmdir, rename, write_at, read_at, append -- so it does not know
 * or care which driver it is running against. Any driver declaring
 * HYPE_FS_CAP_NAMESPACE (currently ext and NTFS; FAT32/exFAT do not
 * implement namespace mutation) can run the namespace half unchanged.
 *
 * Namespace existence is proven the way every POSIX-style namespace
 * actually guarantees it, not by reading content: a second create()/
 * mkdir() of an existing name must be refused, and unlink()/rmdir() of a
 * name that was never created, or was already removed, must be refused
 * too. This works uniformly even for a driver whose freshly-created file
 * is not yet readable (NTFS's own $DATA starts resident and empty, which
 * hype_ntfs_resolve() correctly refuses per decision 30 until something
 * converts or grows it -- see core/fs_ops.c's ntfs_create() comment).
 *
 * CONTENT is then exercised on top, adaptively: the battery attempts
 * hype_fs_lookup() on a just-created file, and only if that succeeds (the
 * driver's own capability, not a battery assumption) does it write a
 * pattern, read it back byte-exact, and -- if HYPE_FS_CAP_APPEND is
 * advertised -- append more and verify the grown length reads back
 * correctly too. A driver where lookup on a fresh empty file legitimately
 * fails (NTFS, until #692's own documented append-through-the-vtable gap
 * closes) skips content verification for that driver rather than failing
 * -- a real, reported capability difference, not a masked one.
 */

typedef struct {
    unsigned dirs_created;
    unsigned files_created;
    unsigned duplicate_refusals_ok; /* create/mkdir on an existing name correctly refused */
    unsigned renames_ok;
    unsigned deletes_ok; /* unlink/rmdir that should succeed, did */
    unsigned stale_refusals_ok; /* unlink/rmdir/rename on a gone/missing name correctly refused */
    unsigned content_verified;   /* write+read (and append+read, if CAP_APPEND) byte-exact */
    unsigned content_skipped;    /* lookup on the fresh file failed: no content test possible */
    unsigned failures;
    char first_fail[64];
} hype_fs_battery_result_t;

typedef void (*hype_fs_battery_log_fn)(void *ctx, const char *step, int ok);

/*
 * Runs the battery under `dir` (e.g. "/hypebattery" or "\\hypebattery") --
 * the caller picks a path that does not already exist and is reachable
 * from the volume's root through directories the driver already supports.
 * Returns 0 if every step behaved as the battery expects (an intentional
 * refusal counts as success for that step), -1 if the driver lacks
 * HYPE_FS_CAP_NAMESPACE or any step misbehaved.
 */
int hype_fs_battery_run(hype_fs_t *fs, const char *dir, hype_fs_battery_result_t *res,
                        hype_fs_battery_log_fn log, void *logctx);

#endif /* HYPE_CORE_FS_BATTERY_H */
