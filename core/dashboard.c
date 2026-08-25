#include "dashboard.h"
#include "cmdparse.h"
#include "format.h"
#include "strutil.h"

void hype_dash_text_reset(hype_dash_text_t *t) {
    if (t == (hype_dash_text_t *)0) {
        return;
    }
    t->count = 0;
    t->dropped = 0;
    t->line[0][0] = '\0';
}

void hype_dash_text_add(hype_dash_text_t *t, const char *s) {
    if (t == (hype_dash_text_t *)0) {
        return;
    }
    if (t->count >= HYPE_DASH_TEXT_LINES) {
        t->dropped++;
        return;
    }
    (void)hype_strlcpy(t->line[t->count], s ? s : "", HYPE_DASH_TEXT_COLS);
    t->count++;
}

/* Append `s` to line[*len..], left-justified and space-padded to `width`, then
 * one separator space; `s` is truncated at `width`. Callers pass non-null `s`
 * (via ternaries) and fixed column widths whose sum (~86) is far below the
 * caller's line buffer, so no per-char bounds guard is needed here. */
static void put_field(char *line, unsigned *len, const char *s, unsigned width) {
    unsigned w = 0;
    for (; s[w] && w < width; w++) line[(*len)++] = s[w];
    for (; w < width; w++) line[(*len)++] = ' ';
    line[(*len)++] = ' ';
}

void hype_vm_uptime_reset(hype_vm_uptime_t *u) {
    if (u == (hype_vm_uptime_t *)0) {
        return;
    }
    u->accum_ms = 0;
    u->last_ms = 0;
    u->running = 0;
    u->started = 0;
}

void hype_vm_uptime_sample(hype_vm_uptime_t *u, unsigned long long now_ms, int running) {
    if (u == (hype_vm_uptime_t *)0) {
        return;
    }
    if (!u->started) {
        /* First sample establishes the origin only -- there is no interval to credit
         * yet, and crediting `now_ms` here would bank all of host boot as VM uptime. */
        u->started = 1;
        u->last_ms = now_ms;
        u->running = running ? 1 : 0;
        return;
    }
    /* Credit only when the VM was running at BOTH ends of the interval. Crediting on
     * the start state alone banks an interval the VM stopped part-way through, which
     * is the reported symptom (the figure advancing while the state reads `off`).
     * This can under-credit by at most one sample period around a transition -- the
     * right trade for a dashboard, and never wrong in the direction users noticed. */
    if (now_ms > u->last_ms && u->running && running) {
        u->accum_ms += now_ms - u->last_ms;
    }
    u->last_ms = now_ms;
    u->running = running ? 1 : 0;
}

unsigned long long hype_vm_uptime_ms(const hype_vm_uptime_t *u) {
    return (u == (const hype_vm_uptime_t *)0) ? 0ULL : u->accum_ms;
}

void hype_vm_cpu_reset(hype_vm_cpu_t *c) {
    unsigned int i;
    if (c == (hype_vm_cpu_t *)0) {
        return;
    }
    c->last_busy = 0;
    c->last_wall = 0;
    c->last_commit_wall = 0;
    for (i = 0; i < HYPE_VM_CPU_RING_MAX; i++) {
        c->ring_busy[i] = 0;
        c->ring_wall[i] = 0;
    }
    c->ring_count = 0;
    c->ring_next = 0;
    c->pct = 0;
    c->started = 0;
}

void hype_vm_cpu_sample(hype_vm_cpu_t *c, unsigned long long busy_total,
                        unsigned long long wall_total, unsigned long long window) {
    unsigned long long decim;
    unsigned int oldest_idx;
    unsigned long long oldest_busy, oldest_wall;

    if (c == (hype_vm_cpu_t *)0) {
        return;
    }
    if (!c->started) {
        /*
         * First sample only establishes the baseline: with no previous reading there is
         * no window to average over, and treating the totals as a window would report
         * the lifetime mean -- exactly the behaviour this replaces. It also seeds ring
         * slot 0, so the very NEXT call already has one checkpoint to measure against
         * instead of needing a third call before any percentage is available at all.
         */
        c->started = 1;
        c->last_busy = busy_total;
        c->last_wall = wall_total;
        c->last_commit_wall = wall_total;
        c->ring_busy[0] = busy_total;
        c->ring_wall[0] = wall_total;
        c->ring_count = 1;
        c->ring_next = 1;
        return;
    }
    if (busy_total < c->last_busy || wall_total < c->last_wall) {
        /* A counter went backwards (reset or wrap): rebase and keep the last reading
         * rather than computing a nonsense window. */
        c->last_busy = busy_total;
        c->last_wall = wall_total;
        return;
    }
    c->last_busy = busy_total;
    c->last_wall = wall_total;

    /* The oldest populated slot: index 0 while the ring hasn't wrapped yet, otherwise
     * the slot about to be overwritten next. Computed BEFORE this sample's own commit
     * decision below, so a fresh commit never measures against itself. */
    oldest_idx = (c->ring_count < HYPE_VM_CPU_RING_MAX) ? 0u : (c->ring_next % HYPE_VM_CPU_RING_MAX);
    oldest_busy = c->ring_busy[oldest_idx];
    oldest_wall = c->ring_wall[oldest_idx];
    if (wall_total > oldest_wall) {
        unsigned long long dbusy = busy_total - oldest_busy;
        unsigned long long dwall = wall_total - oldest_wall;
        c->pct = (unsigned)((dbusy >= dwall) ? 100u : ((dbusy * 100u) / dwall));
    }
    /* else: no elapsed time against the oldest checkpoint (this IS that checkpoint's
     * own instant) -- keep the previous percentage rather than dividing by zero. */

    if (window == 0u) {
        window = 1u;
    }
    decim = window / HYPE_VM_CPU_RING_MAX;
    if (decim == 0u) {
        decim = 1u;
    }
    /* Commit a new checkpoint for FUTURE calls at most once per `decim` -- decoupling the
     * ring's resolution from however often this function itself is called, which can be
     * far finer than the window (FW-1's real caller samples on every VM exit). */
    if (wall_total - c->last_commit_wall >= decim) {
        unsigned int idx = c->ring_next % HYPE_VM_CPU_RING_MAX;
        c->ring_busy[idx] = busy_total;
        c->ring_wall[idx] = wall_total;
        c->ring_next++;
        if (c->ring_count < HYPE_VM_CPU_RING_MAX) {
            c->ring_count++;
        }
        c->last_commit_wall = wall_total;
    }
}

unsigned hype_vm_cpu_pct(const hype_vm_cpu_t *c) {
    return (c == (const hype_vm_cpu_t *)0) ? 0u : c->pct;
}

/* #396: availability is a per-VM byte array (available[i] != 0), not a 32-bit
 * mask -- the mask capped the VM count at 32. */
static int term_vm_available(unsigned int index, const unsigned char *available, unsigned int nvm) {
    return index < nvm && available != 0 && available[index] != 0u;
}

int hype_term_focus_validate(int view, const unsigned char *available, unsigned int nvm) {
    if (view < 0) return -1;
    return term_vm_available((unsigned int)view, available, nvm) ? view : -1;
}

int hype_term_focus_find_name(const char *name, const char *const *names, unsigned int nvm) {
    unsigned int i;

    if (name == 0 || name[0] == '\0' || names == 0) return -1;
    for (i = 0u; i < nvm; i++) {
        const char *candidate = names[i];
        unsigned int j = 0u;
        if (candidate == 0) continue;
        while (name[j] != '\0' && name[j] == candidate[j]) j++;
        if (name[j] == '\0' && candidate[j] == '\0') return (int)i;
    }
    return -1;
}

int hype_term_focus_apply(int current, hype_term_focus_action_t action,
                          unsigned int jump_index, const unsigned char *available,
                          unsigned int nvm) {
    unsigned int i;
    current = hype_term_focus_validate(current, available, nvm);
    switch (action) {
        case HYPE_TERM_FOCUS_NEXT:
            for (i = (unsigned int)(current + 1); i < nvm; i++) {
                if (term_vm_available(i, available, nvm)) return (int)i;
            }
            return -1;
        case HYPE_TERM_FOCUS_PREV:
            if (current < 0) {
                i = nvm;
            } else {
                i = (unsigned int)current;
            }
            while (i > 0u) {
                i--;
                if (term_vm_available(i, available, nvm)) return (int)i;
            }
            return -1;
        case HYPE_TERM_FOCUS_JUMP:
            return term_vm_available(jump_index, available, nvm)
                       ? (int)jump_index
                       : current;
        case HYPE_TERM_FOCUS_TOGGLE_DASHBOARD:
            if (current >= 0) return -1;
            for (i = 0u; i < nvm; i++) {
                if (term_vm_available(i, available, nvm)) return (int)i;
            }
            return -1;
        case HYPE_TERM_FOCUS_NONE:
        default:
            return current;
    }
}

void hype_dashboard_fmt_uptime(char *buf, unsigned long long secs) {
    unsigned long long h = secs / 3600ull;
    unsigned m = (unsigned)((secs / 60ull) % 60ull);
    unsigned sec = (unsigned)(secs % 60ull);
    /* Clamp hours field to 2 digits' worth of display sanity (99h+ shown as-is
     * via %llu; the common case is small). */
    hype_snprintf(buf, 16, "%llu:%s%u:%s%u",
                  h, (m < 10) ? "0" : "", m, (sec < 10) ? "0" : "", sec);
    /* pad the hours to 2 digits for column alignment when < 10 */
    if (h < 10) {
        char tmp[16];
        hype_strlcpy(tmp, buf, sizeof(tmp));
        hype_snprintf(buf, 16, "0%s", tmp);
    }
}

static void emit_line(hype_vt_screen_t *s, const char *line) {
    hype_vt_screen_write(s, (const uint8_t *)line, (unsigned)hype_strlen(line));
    hype_vt_screen_write(s, (const uint8_t *)"\r\n", 2);
}

void hype_dashboard_render(hype_vt_screen_t *s,
                           const hype_vm_dash_info_t *vms, unsigned n,
                           uint64_t host_uptime_s,
                           const char *cmdline, const hype_dash_text_t *result,
                           const char *alert, const char *version) {
    char line[160];
    char up[16];
    /* #460: rows consumed so far, so the result panel knows how much of the grid is left.
     * Counted rather than derived, because the hint block below wraps to an unknown height. */
    unsigned rows_used = 0;

    /* Home + clear the grid (reuse the VT interpreter's own ED/CUP). */
    hype_vt_screen_write(s, (const uint8_t *)"\x1b[H\x1b[2J", 7);

    hype_dashboard_fmt_uptime(up, host_uptime_s);
    if (version != (const char *)0 && version[0] != '\0') {
        hype_snprintf(line, sizeof(line), "hype - VM dashboard        host up %s        %s", up,
                      version);
    } else {
        hype_snprintf(line, sizeof(line), "hype - VM dashboard        host up %s", up);
    }
    emit_line(s, line);
    if (alert != (const char *)0 && alert[0] != '\0') {
        emit_line(s, alert);
        rows_used++;
    }
    emit_line(s, "");

    /* header row */
    {
        unsigned len = 0;
        put_field(line, &len, "#",      2);
        put_field(line, &len, "NAME",   14);
        put_field(line, &len, "OS",      7);
        put_field(line, &len, "STATE",   9);
        put_field(line, &len, "CPU",     4);
        put_field(line, &len, "MEM",     7);
        put_field(line, &len, "UPTIME",  9);
        put_field(line, &len, "MEDIA",  16);
        line[len] = '\0';
        emit_line(s, line);
    }

    for (unsigned i = 0; i < n; i++) {
        const hype_vm_dash_info_t *v = &vms[i];
        unsigned len = 0;
        char idx[8], cpu[8], mem[12];

        hype_snprintf(idx, sizeof(idx), "%s%u", v->focused ? ">" : "", i + 1u);
        hype_snprintf(cpu, sizeof(cpu), "%u%%", v->cpu_pct);
        hype_snprintf(mem, sizeof(mem), "%uM", v->mem_mb);
        hype_dashboard_fmt_uptime(up, v->uptime_s);

        put_field(line, &len, idx,         2);
        put_field(line, &len, v->name ? v->name : "?", 14);
        put_field(line, &len, v->os_hint ? v->os_hint : "-", 7);
        put_field(line, &len, v->state ? v->state : "?", 9);
        put_field(line, &len, cpu,         4);
        put_field(line, &len, mem,         7);
        put_field(line, &len, up,          9);
        put_field(line, &len, v->media ? v->media : "-", 16);
        line[len] = '\0';
        emit_line(s, line);
    }

    /* TERM-2 footer: the command hint, the prompt (with a '_' caret), then the last result. */
    emit_line(s, "");
    /* title, blank, column header, n VM rows, this blank. */
    rows_used += n + 4u;

    /*
     * #459: the hint is built from hype_cmd_usage(), not hand-written here. The hand-written
     * version silently fell two verbs behind and made TERM-6/TERM-7 look unimplemented.
     * Wrapped to the grid width so a longer command set grows a line instead of vanishing off
     * the right edge -- the same failure in a different direction.
     */
    {
        unsigned i, count = hype_cmd_usage_count();
        unsigned len = 0;
        unsigned width = (s->cols > 8u && s->cols < sizeof(line)) ? s->cols - 1u
                                                                  : (unsigned)sizeof(line) - 1u;
        hype_strlcpy(line, "type: ", sizeof(line));
        len = (unsigned)hype_strlen(line);
        for (i = 0; i < count; i++) {
            const char *u = hype_cmd_usage(i);
            unsigned ul = (unsigned)hype_strlen(u);
            if (len + ul + 3u > width) {
                line[len] = '\0';
                emit_line(s, line);
                rows_used++;
                len = 6u; /* re-indent under "type: " */
                for (unsigned k = 0; k < len; k++) line[k] = ' ';
            } else if (i > 0) {
                line[len++] = ' ';
                line[len++] = '|';
                line[len++] = ' ';
            }
            for (unsigned k = 0; k < ul; k++) line[len++] = u[k];
        }
        line[len] = '\0';
        emit_line(s, line);
        rows_used++;
    }

    hype_snprintf(line, sizeof(line), "hype> %s_", cmdline ? cmdline : "");
    emit_line(s, line);
    rows_used++;

    if (result != (const hype_dash_text_t *)0 && result->count > 0u) {
        /* One row is reserved for the overflow notice, so a clipped result can always say so. */
        unsigned room = (s->rows > rows_used + 1u) ? s->rows - rows_used - 1u : 0u;
        unsigned shown = (result->count < room) ? result->count : room;
        unsigned i;
        for (i = 0; i < shown; i++) {
            emit_line(s, result->line[i]);
        }
        if (shown < result->count || result->dropped > 0u) {
            hype_snprintf(line, sizeof(line),
                          "-- %u more line(s) not shown (grid has %u rows) --",
                          (result->count - shown) + result->dropped, s->rows);
            emit_line(s, line);
        }
    }
}
