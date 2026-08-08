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

/*
 * #338: which records this sink takes. HYPE_LOG_SINK_ALL reproduces the
 * original behaviour exactly -- every byte, unfiltered, and NOT split on record
 * boundaries -- so \HYPEFULL.LOG remains byte-for-byte what it always was.
 * Any other value routes per record (see core/log_split.h).
 */
#define HYPE_LOG_SINK_ALL (-2)  /* combined stream: everything, verbatim */
#define HYPE_LOG_SINK_HYPE (-1) /* hype's own output only (== HYPE_LOG_VM_HYPE) */
/* >= 0: that VM index's guest serial only */

typedef struct {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t file;
    unsigned int flushed; /* logbuf bytes already streamed to the file */
    int active;           /* 1 once the volume mounted and the file was created */
    int filter;           /* #338: HYPE_LOG_SINK_ALL / _HYPE / a VM index */
    int ordered;          /* prefix each record with its capture-buffer offset */
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
 * #338: as hype_log_sink_open(), but takes only the records `filter` selects.
 * A filtered sink flushes whole records, so it never emits a half-written line
 * and never re-emits one: a trailing partial record is left in the buffer for
 * the next flush.
 */
int hype_log_sink_open_filtered(hype_log_sink_t *s, hype_fat_read_fn read, hype_fat_write_fn write,
                                void *ctx, const char *filename, const hype_rtc_time_t *now,
                                int filter);

/*
 * Prefix every record this sink writes with its offset in the capture buffer, as
 * "[00012345] ".
 *
 * This exists so the combined log can be retired without losing what it was actually
 * for. Splitting by source keeps each stream's own order but destroys the ordering
 * BETWEEN hype and a guest -- and that interleaving is what several past
 * investigations turned on. The capture-buffer offset is a total order over every
 * record ever written, so sorting the split files by it reconstructs the combined
 * stream exactly.
 *
 * Ten bytes per record against duplicating every byte of every record: the offset is
 * roughly a tenth the cost of the file it replaces.
 *
 * Off by default -- callers that keep a combined log do not need it.
 */
void hype_log_sink_set_ordered(hype_log_sink_t *s, int ordered);

/*
 * As hype_log_sink_open_filtered(), with ordering on from the FIRST record.
 *
 * Prefer this over open_filtered() + set_ordered(): open() streams whatever the capture
 * buffer already holds, so turning ordering on afterwards leaves that backlog unstamped,
 * and a merge tool drops unstamped records silently because they do not match the record
 * pattern. Measured at 65 of 453 records -- all of them boot-time -- before this existed.
 */
int hype_log_sink_open_ordered(hype_log_sink_t *s, hype_fat_read_fn read, hype_fat_write_fn write,
                               void *ctx, const char *filename, const hype_rtc_time_t *now,
                               int filter);

/*
 * Appends the logbuf bytes captured since the previous flush (or open). A no-op
 * returning 0 if nothing new. Returns -1 if the sink is inactive or a write
 * fails. Safe to call as often as desired.
 */
/* #338: how many logbuf bytes have reached the file. Lets the drain loop report a growing
 * gap between "captured" and "written" in the log itself, rather than leaving a stalled sink
 * to be inferred from a port-access count after the fact. */
static inline unsigned int hype_log_sink_flushed(const hype_log_sink_t *s) {
    return s->flushed;
}

int hype_log_sink_flush(hype_log_sink_t *s);

#endif /* HYPE_CORE_LOG_SINK_H */
