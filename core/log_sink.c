#include "log_sink.h"
#include "log_split.h"
#include "logbuf.h"

static int sink_open(hype_log_sink_t *s, hype_fat_read_fn read, hype_fat_write_fn write, void *ctx,
                     const char *filename, const hype_rtc_time_t *now, int filter) {
    s->active = 0;
    s->flushed = 0;
    s->filter = filter;
    if (hype_fat32_fs_mount(read, write, ctx, &s->fs) != 0) return HYPE_LOG_SINK_ERR_MOUNT;
    /* Before create(), so the new file's directory entry carries a real date
     * rather than the zeroes that made hype's log show as the Unix epoch. */
    hype_fat32_fs_set_time(&s->fs, now);
    if (hype_fat32_create(&s->fs, filename, &s->file) != 0) return HYPE_LOG_SINK_ERR_CREATE;
    s->active = 1;
    if (hype_log_sink_flush(s) != 0) {
        s->active = 0;
        return HYPE_LOG_SINK_ERR_WRITE;
    }
    return HYPE_LOG_SINK_OK;
}

int hype_log_sink_open(hype_log_sink_t *s, hype_fat_read_fn read, hype_fat_write_fn write,
                       void *ctx, const char *filename, const hype_rtc_time_t *now) {
    return sink_open(s, read, write, ctx, filename, now, HYPE_LOG_SINK_ALL);
}

int hype_log_sink_open_filtered(hype_log_sink_t *s, hype_fat_read_fn read, hype_fat_write_fn write,
                                void *ctx, const char *filename, const hype_rtc_time_t *now,
                                int filter) {
    return sink_open(s, read, write, ctx, filename, now, filter);
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
static int flush_filtered(hype_log_sink_t *s, const char *data, unsigned int len) {
    unsigned int pos = s->flushed;

    while (pos < len) {
        unsigned int nl = pos;
        unsigned int reclen;
        while (nl < len && data[nl] != '\n') nl++;
        if (nl == len) break; /* partial record: leave it for the next flush */
        reclen = nl - pos;
        if (hype_log_record_vm(data + pos, reclen) == s->filter) {
            unsigned int off = (s->filter == HYPE_LOG_SINK_HYPE)
                                   ? 0u
                                   : hype_log_record_body_off(data + pos, reclen);
            /* nl + 1 keeps the newline, so the file stays line-oriented. */
            if (hype_fat32_append(&s->file, data + pos + off, (nl + 1u) - (pos + off)) != 0) {
                return -1;
            }
        }
        pos = nl + 1u;
        s->flushed = pos;
    }
    return 0;
}

int hype_log_sink_flush(hype_log_sink_t *s) {
    unsigned int len;
    if (!s->active) return -1;
    len = hype_logbuf_len();
    if (len <= s->flushed) return 0; /* nothing new since the last flush */
    if (s->filter != HYPE_LOG_SINK_ALL) {
        return flush_filtered(s, hype_logbuf_data(), len);
    }
    if (hype_fat32_append(&s->file, hype_logbuf_data() + s->flushed, len - s->flushed) != 0) {
        return -1;
    }
    s->flushed = len;
    return 0;
}
