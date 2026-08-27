#include "log_sink.h"
#include "log_split.h"
#include "logbuf.h"
#include "format.h"

void hype_log_sink_set_ordered(hype_log_sink_t *s, int ordered) {
    if (s != 0) s->ordered = ordered;
}

/*
 * #643: HYPE.LOG (and every per-VM log) was truncated at each boot. Several validation
 * protocols need two consecutive boots compared against each other -- a marker written on
 * boot 1, read back on boot 2 -- and a truncating sink destroys boot 1's own evidence with
 * nothing in the surviving log saying so. Keep a small bounded window of prior boots instead:
 * not unbounded growth, the volume is finite and this is #596's writer, the constrained path.
 */
#define HYPE_LOG_SINK_MAX_GENERATIONS 4u

/* "HYPE.LOG" -> "HYPE.<gen>.LOG": the generation number goes before the final extension, not
 * appended after it, so a directory listing still groups a log's generations by name and an
 * operator can tell HYPE.1.LOG is an older HYPE.LOG at a glance. A name with no extension just
 * gets the suffix appended. */
static void gen_filename(char *out, unsigned out_sz, const char *filename, unsigned gen) {
    const char *dot = 0;
    const char *p;
    unsigned prefix_len;
    char prefix[40];
    for (p = filename; *p != '\0'; p++) {
        if (*p == '.') dot = p;
    }
    if (dot == 0) {
        hype_snprintf(out, out_sz, "%s.%u", filename, gen);
        return;
    }
    prefix_len = (unsigned)(dot - filename);
    if (prefix_len >= sizeof(prefix)) prefix_len = sizeof(prefix) - 1u;
    for (p = filename; (unsigned)(p - filename) < prefix_len; p++) {
        prefix[p - filename] = *p;
    }
    prefix[prefix_len] = '\0';
    hype_snprintf(out, out_sz, "%s.%u%s", prefix, gen, dot);
}

/*
 * Shift existing generations up by one and evict the oldest, so the about-to-be-created
 * `filename` never destroys what a prior boot wrote. Processed from the oldest slot down to
 * the newest: hype_fat32_rename() (and exFAT's equivalent) never replaces an existing
 * destination, so each rename's target must already be vacant -- evicting the oldest first,
 * then working downward, guarantees that by construction. A missing source at any step
 * fails harmlessly (there simply aren't that many prior boots yet) and rotation continues.
 */
static void rotate_generations(hype_fs_t *fs, const char *filename) {
    char from[48], to[48];
    unsigned gen;
    gen_filename(to, sizeof(to), filename, HYPE_LOG_SINK_MAX_GENERATIONS);
    (void)hype_fs_unlink(fs, to);
    for (gen = HYPE_LOG_SINK_MAX_GENERATIONS - 1u; gen >= 1u; gen--) {
        gen_filename(from, sizeof(from), filename, gen);
        gen_filename(to, sizeof(to), filename, gen + 1u);
        (void)hype_fs_rename(fs, from, to);
    }
    gen_filename(to, sizeof(to), filename, 1u);
    (void)hype_fs_rename(fs, filename, to);
}

/* "[00012345] " -- fixed width so the files sort lexically as well as numerically,
 * which means a plain `sort` merges them correctly with no tooling. */
/*
 * #338's total order over every record ever written, and #585's reason it had to widen.
 *
 * The offset must be ABSOLUTE. Once the capture buffer reclaims its already-written prefix, a
 * buffer index restarts near zero, so indices would repeat within one run and a merge tool sorting
 * by them would interleave hour six with hour one. hype_logbuf_reclaimed() is what makes it
 * absolute.
 *
 * Ten digits, not eight. Eight wraps at 100 MB, and an overnight run at the measured ~3.2 KB/s
 * passes that in about nine hours -- so the field would have wrapped inside the very run reclaim
 * exists to enable, which is a worse failure than the one it fixes because it looks like ordinary
 * data. Ten digits reach 10 GB, i.e. five weeks at that rate. Any tool matching the prefix has to
 * accept 10 digits.
 */
static unsigned int order_prefix(char *buf, uint64_t off) {
    unsigned int i;
    buf[0] = '[';
    for (i = 0; i < 10u; i++) {
        buf[10u - i] = (char)('0' + (unsigned int)(off % 10u));
        off /= 10u;
    }
    buf[11] = ']';
    buf[12] = ' ';
    return 13u;
}

static int sink_start(hype_log_sink_t *s, hype_fs_t *fs, const char *filename,
                      const hype_rtc_time_t *now, int filter, int ordered) {
    s->active = 0;
    /* #747: HERE, not in sink_open() -- every open funnels through this function, including
     * the shared-fs one, and clearing it in only one of the two callers is how the shared
     * path would have quietly stayed gone forever after a re-attach. */
    s->device_gone = 0;
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
    hype_fs_set_time(fs, now);
    /* #643: age out any prior boot's log under this name before creating a fresh one, so a
     * two-boot validation protocol has both boots' evidence to read. Runs under the same
     * caller-held guard as create() itself -- a rename is a filesystem mutation exactly like
     * the append it precedes (decision 57). */
    rotate_generations(fs, filename);
    if (hype_fs_create(fs, filename, &s->file) != 0) return HYPE_LOG_SINK_ERR_CREATE;
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
    /* #293: probe the registry instead of assuming FAT32. The gate is what the
     * sink needs -- create + append on THIS mount -- not a driver name. */
    if (hype_fs_mount_auto(&s->fs, read, write, ctx) != 0) return HYPE_LOG_SINK_ERR_MOUNT;
    if ((hype_fs_caps(&s->fs) & (HYPE_FS_CAP_APPEND | HYPE_FS_CAP_NAMESPACE)) !=
        (HYPE_FS_CAP_APPEND | HYPE_FS_CAP_NAMESPACE)) {
        return HYPE_LOG_SINK_ERR_MOUNT;
    }
    hype_fs_set_barrier(&s->fs, sync);
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

int hype_log_sink_open_shared_ordered(hype_log_sink_t *s, hype_fs_t *fs,
                                      const char *filename, const hype_rtc_time_t *now,
                                      int filter) {
    if (fs == (hype_fs_t *)0) return HYPE_LOG_SINK_ERR_MOUNT;
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
        char pfx[16];
        while (nl < len && data[nl] != '\n') nl++;
        if (nl == len) break; /* partial record: leave it for the next flush */
        reclen = nl - pos;
        if (hype_log_record_vm(data + pos, reclen) == s->filter) {
            off = (s->filter == HYPE_LOG_SINK_HYPE)
                      ? 0u
                      : hype_log_record_body_off(data + pos, reclen);
            if (s->ordered) {
                plen = order_prefix(pfx, hype_logbuf_reclaimed() + (uint64_t)pos);
            }
            body_len = (nl + 1u) - (pos + off); /* keep the newline */
            if (plen + body_len > HYPE_LOG_SINK_BATCH_BYTES) {
                /* A single unusually long record must not wedge the cursor.
                 * Flush the accumulated batch first, then commit this record
                 * directly. This path is bounded to one complete record. */
                if (batch_len != 0u) break;
                if (plen != 0u && hype_fs_append(&s->file, pfx, plen) != 0) return -1;
                if (hype_fs_append(&s->file, data + pos + off, body_len) != 0) return -1;
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
        if (hype_fs_append(&s->file, g_filtered_batch, batch_len) != 0) return -1;
    }
    s->flushed = commit_pos;
    return 0;
}

void hype_log_sink_mark_device_gone(hype_log_sink_t *s) {
    if (s != (hype_log_sink_t *)0) {
        s->device_gone = 1;
    }
}

int hype_log_sink_device_gone(const hype_log_sink_t *s) {
    return (s != (const hype_log_sink_t *)0) && s->device_gone;
}

int hype_log_sink_flush_budget(hype_log_sink_t *s, unsigned int max_source_bytes) {
#ifdef HYPE_596_JOURNAL
    {
        void hype_596_note(uint32_t tag, uint32_t a, uint32_t b);
        hype_596_note('F', (uint32_t)(uintptr_t)__builtin_return_address(0),
                      (uint32_t)s->filter);
    }
#endif
    unsigned int len;
    /*
     * #747: checked BEFORE `active`, and before anything reaches the filesystem. A gone
     * volume is not an inactive sink -- it mounted, it has a file, and its in-memory FAT
     * state describes a medium that is no longer there. Every further append would be a
     * write into that state, which is exactly the torn chain #596 is about.
     */
    if (s->device_gone) return HYPE_LOG_SINK_ERR_GONE;
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
        if (hype_fs_append(&s->file, hype_logbuf_data() + s->flushed, n) != 0) {
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
