#include "log_sink.h"
#include "log_split.h"
#include "logbuf.h"

void hype_log_sink_set_ordered(hype_log_sink_t *s, int ordered) {
    if (s != 0) s->ordered = ordered;
}

/* "[00012345] " -- fixed width so the files sort lexically as well as numerically,
 * which means a plain `sort` merges them correctly with no tooling. */
static unsigned int order_prefix(char *buf, unsigned int off) {
    unsigned int i;
    buf[0] = '[';
    for (i = 0; i < 8u; i++) {
        buf[8u - i] = (char)('0' + (off % 10u));
        off /= 10u;
    }
    buf[9] = ']';
    buf[10] = ' ';
    return 11u;
}

static int sink_start(hype_log_sink_t *s, hype_fat32_fs_t *fs, const char *filename,
                      const hype_rtc_time_t *now, int filter, int ordered) {
    s->active = 0;
    s->flushed = 0;
    s->filter = filter;
    /*
     * Set BEFORE the open-time flush below, not after. open() writes everything already in the
     * capture buffer, so switching ordering on afterwards left that backlog unstamped -- 65 of 453
     * records in a QEMU run, 14%, which a merge tool drops SILENTLY because they simply do not
     * match the record pattern. That is the whole justification for retiring the combined log
     * quietly failing for the earliest records, which are the boot ones.
     */
    s->ordered = ordered;
    /* Before create(), so the new file's directory entry carries a real date
     * rather than the zeroes that made hype's log show as the Unix epoch. */
    hype_fat32_fs_set_time(fs, now);
    if (hype_fat32_create(fs, filename, &s->file) != 0) return HYPE_LOG_SINK_ERR_CREATE;
    s->active = 1;
    if (hype_log_sink_flush(s) != 0) {
        s->active = 0;
        return HYPE_LOG_SINK_ERR_WRITE;
    }
    return HYPE_LOG_SINK_OK;
}

static int sink_open(hype_log_sink_t *s, hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                     const char *filename, const hype_rtc_time_t *now, int filter, int ordered,
                     hype_blk_sync_fn sync) {
    s->active = 0;
    if (hype_fat32_fs_mount(read, write, ctx, &s->fs) != 0) return HYPE_LOG_SINK_ERR_MOUNT;
    hype_fat32_fs_set_sync(&s->fs, sync);
    return sink_start(s, &s->fs, filename, now, filter, ordered);
}

int hype_log_sink_open(hype_log_sink_t *s, hype_blk_read_fn read, hype_blk_write_fn write,
                       void *ctx, const char *filename, const hype_rtc_time_t *now) {
    return sink_open(s, read, write, ctx, filename, now, HYPE_LOG_SINK_ALL, 0,
                     (hype_blk_sync_fn)0);
}

int hype_log_sink_open_filtered(hype_log_sink_t *s, hype_blk_read_fn read, hype_blk_write_fn write,
                                void *ctx, const char *filename, const hype_rtc_time_t *now,
                                int filter) {
    return sink_open(s, read, write, ctx, filename, now, filter, 0, (hype_blk_sync_fn)0);
}

int hype_log_sink_open_ordered(hype_log_sink_t *s, hype_blk_read_fn read, hype_blk_write_fn write,
                               void *ctx, const char *filename, const hype_rtc_time_t *now,
                               int filter) {
    return sink_open(s, read, write, ctx, filename, now, filter, 1, (hype_blk_sync_fn)0);
}

int hype_log_sink_open_ordered_durable(hype_log_sink_t *s, hype_blk_read_fn read,
                                       hype_blk_write_fn write, hype_blk_sync_fn sync, void *ctx,
                                       const char *filename, const hype_rtc_time_t *now, int filter) {
    return sink_open(s, read, write, ctx, filename, now, filter, 1, sync);
}

int hype_log_sink_open_shared_ordered(hype_log_sink_t *s, hype_fat32_fs_t *fs,
                                      const char *filename, const hype_rtc_time_t *now,
                                      int filter) {
    if (fs == (hype_fat32_fs_t *)0) return HYPE_LOG_SINK_ERR_MOUNT;
    return sink_start(s, fs, filename, now, filter, 1);
}

/*
 * #338: record-wise drain for a filtered sink.
 *
 * Advances `flushed` only past records that are COMPLETE in the buffer, so a
 * record still being appended is left for the next flush instead of being
 * written in two halves (and, worse, classified from a prefix that has not
 * arrived yet). `flushed` therefore lags the combined sink's by at most one
 * record.
 *
 * A skipped record still advances the cursor -- the sink must not re-examine
 * bytes it has already decided about.
 */
/*
 * #374: one batch turns N ordered records from 2*N FAT appends (prefix, body)
 * into one. hype_fat32_append() commits file metadata on every call, and the
 * real USB run measured thousands of one-sector transfers while draining a
 * roughly 100 KiB boot log. The sink is BSP-owned, so one module scratch
 * buffer is sufficient and avoids adding 4 KiB to every sink instance.
 */
#define HYPE_LOG_SINK_BATCH_BYTES 4096u
static char g_filtered_batch[HYPE_LOG_SINK_BATCH_BYTES];

static void batch_copy(char *dst, const char *src, unsigned int len) {
    unsigned int i;
    for (i = 0; i < len; i++) dst[i] = src[i];
}

static int flush_filtered(hype_log_sink_t *s, const char *data, unsigned int len,
                          unsigned int max_source_bytes) {
    unsigned int pos = s->flushed;
    unsigned int start = pos;
    unsigned int commit_pos = pos;
    unsigned int batch_len = 0u;

    while (pos < len) {
        unsigned int nl = pos;
        unsigned int reclen;
        unsigned int off = 0u;
        unsigned int plen = 0u;
        unsigned int body_len;
        char pfx[12];
        while (nl < len && data[nl] != '\n') nl++;
        if (nl == len) break; /* partial record: leave it for the next flush */
        reclen = nl - pos;
        if (hype_log_record_vm(data + pos, reclen) == s->filter) {
            off = (s->filter == HYPE_LOG_SINK_HYPE)
                      ? 0u
                      : hype_log_record_body_off(data + pos, reclen);
            if (s->ordered) {
                plen = order_prefix(pfx, pos);
            }
            body_len = (nl + 1u) - (pos + off); /* keep the newline */
            if (plen + body_len > HYPE_LOG_SINK_BATCH_BYTES) {
                /* A single unusually long record must not wedge the cursor.
                 * Flush the accumulated batch first, then commit this record
                 * directly. This path is bounded to one complete record. */
                if (batch_len != 0u) break;
                if (plen != 0u && hype_fat32_append(&s->file, pfx, plen) != 0) return -1;
                if (hype_fat32_append(&s->file, data + pos + off, body_len) != 0) return -1;
            } else {
                if (batch_len + plen + body_len > HYPE_LOG_SINK_BATCH_BYTES) break;
                if (plen != 0u) {
                    batch_copy(g_filtered_batch + batch_len, pfx, plen);
                    batch_len += plen;
                }
                batch_copy(g_filtered_batch + batch_len, data + pos + off, body_len);
                batch_len += body_len;
            }
        }
        pos = nl + 1u;
        commit_pos = pos;
        /* Process at least one complete record. This prevents a small budget
         * from making no progress when the next record exceeds it. */
        if (pos - start >= max_source_bytes) break;
    }
    if (batch_len != 0u) {
        if (hype_fat32_append(&s->file, g_filtered_batch, batch_len) != 0) return -1;
    }
    s->flushed = commit_pos;
    return 0;
}

int hype_log_sink_flush_budget(hype_log_sink_t *s, unsigned int max_source_bytes) {
    unsigned int len;
    if (!s->active) return -1;
    if (max_source_bytes == 0u) return 0;
    len = hype_logbuf_len();
    if (len <= s->flushed) return 0; /* nothing new since the last flush */
    if (s->filter != HYPE_LOG_SINK_ALL) {
        return flush_filtered(s, hype_logbuf_data(), len, max_source_bytes);
    }
    {
        unsigned int pending = len - s->flushed;
        unsigned int n = (pending > max_source_bytes) ? max_source_bytes : pending;
        if (hype_fat32_append(&s->file, hype_logbuf_data() + s->flushed, n) != 0) {
            return -1;
        }
        s->flushed += n;
    }
    return 0;
}

int hype_log_sink_flush(hype_log_sink_t *s) {
    unsigned int before;
    do {
        before = s->flushed;
        if (hype_log_sink_flush_budget(s, ~0u) != 0) return -1;
        /* A filtered sink stops at a trailing partial record. */
    } while (s->flushed != before && s->flushed < hype_logbuf_len());
    return 0;
}
