#ifndef HYPE_CORE_LOG_SINK_H
#define HYPE_CORE_LOG_SINK_H

#include <stdint.h>
#include "fat_write_fs.h"
#include "rtc.h"

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
 * Distinct failure stages. Every one of these used to collapse into -1, so a
 * volume that mounted and a file that was created could still be reported as
 * "no mountable FAT32 volume" when it was the WRITE that failed -- observed on
 * real hardware, where a 0-byte HYPEFULL.LOG on the stick proved both the mount
 * and the create had in fact succeeded. Losing the log is bad; misdescribing why
 * is worse, because it sends the next person to the wrong layer.
 */
#define HYPE_LOG_SINK_OK 0
#define HYPE_LOG_SINK_ERR_MOUNT (-1)  /* not a FAT32 volume at this base LBA */
#define HYPE_LOG_SINK_ERR_CREATE (-2) /* mounted, but the file could not be created */
#define HYPE_LOG_SINK_ERR_WRITE (-3)  /* created, but the first append failed (block I/O) */

/*
 * Mounts the FAT32 volume, creates (truncating) `filename` (8.3) in its root,
 * and streams whatever the logbuf already holds. Returns HYPE_LOG_SINK_OK, or one
 * of the ERR codes above -- which stage failed is the useful part. The sink is
 * left inactive on any failure.
 */
/* `now` stamps the created file's directory entry; pass 0 for none (the
 * timestamps are then zeroed, as they were before the RTC existed). */
int hype_log_sink_open(hype_log_sink_t *s, hype_fat_read_fn read, hype_fat_write_fn write,
                       void *ctx, const char *filename, const hype_rtc_time_t *now);

/*
 * Appends the logbuf bytes captured since the previous flush (or open). A no-op
 * returning 0 if nothing new. Returns -1 if the sink is inactive or a write
 * fails. Safe to call as often as desired.
 */
int hype_log_sink_flush(hype_log_sink_t *s);

#endif /* HYPE_CORE_LOG_SINK_H */
