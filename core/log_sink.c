#include "log_sink.h"
#include "logbuf.h"

int hype_log_sink_open(hype_log_sink_t *s, hype_fat_read_fn read, hype_fat_write_fn write,
                       void *ctx, const char *filename, const hype_rtc_time_t *now) {
    s->active = 0;
    s->flushed = 0;
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

int hype_log_sink_flush(hype_log_sink_t *s) {
    unsigned int len;
    if (!s->active) return -1;
    len = hype_logbuf_len();
    if (len <= s->flushed) return 0; /* nothing new since the last flush */
    if (hype_fat32_append(&s->file, hype_logbuf_data() + s->flushed, len - s->flushed) != 0) {
        return -1;
    }
    s->flushed = len;
    return 0;
}
