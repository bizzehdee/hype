#ifndef HYPE_CORE_LOG_SINK_H
#define HYPE_CORE_LOG_SINK_H

#include <stdint.h>
#include "fat_write_fs.h"

/*
 * #230 (USB debug-log sink): stream hype's in-RAM capture buffer (core/logbuf.c)
 * to a file on a FAT32 volume, so a real-hardware debug run leaves a complete
 * \hype-fulllog.txt on the USB stick it booted from -- the RT-3 NV-variable tail
 * stays as a backup, but this carries the WHOLE log, not just the last few KB.
 *
 * The volume is reached through an injected read+write sector-callback pair
 * (hype_fat_read_fn / hype_fat_write_fn) so the block path (USB MSC via blk_usb,
 * plus a partition-base offset) is the caller's concern and this module stays
 * pure and unit-testable. Open once, then flush repeatedly: each flush appends
 * only the logbuf bytes written since the previous flush, so it can be called
 * periodically during a run and again at shutdown to capture late output.
 */

typedef struct {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t file;
    unsigned int flushed; /* logbuf bytes already streamed to the file */
    int active;           /* 1 once the volume mounted and the file was created */
} hype_log_sink_t;

/*
 * Mounts the FAT32 volume, creates (truncating) `filename` (8.3) in its root,
 * and streams whatever the logbuf already holds. Returns 0 on success, -1 if the
 * volume is not FAT32 or any I/O fails (the sink is left inactive).
 */
int hype_log_sink_open(hype_log_sink_t *s, hype_fat_read_fn read, hype_fat_write_fn write,
                       void *ctx, const char *filename);

/*
 * Appends the logbuf bytes captured since the previous flush (or open). A no-op
 * returning 0 if nothing new. Returns -1 if the sink is inactive or a write
 * fails. Safe to call as often as desired.
 */
int hype_log_sink_flush(hype_log_sink_t *s);

#endif /* HYPE_CORE_LOG_SINK_H */
