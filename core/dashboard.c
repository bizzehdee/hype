#include "dashboard.h"
#include "format.h"
#include "strutil.h"

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
    if (c == (hype_vm_cpu_t *)0) {
        return;
    }
    c->last_busy = 0;
    c->last_wall = 0;
    c->pct = 0;
    c->started = 0;
}

void hype_vm_cpu_sample(hype_vm_cpu_t *c, unsigned long long busy_total,
                        unsigned long long wall_total) {
    unsigned long long dbusy, dwall;
    if (c == (hype_vm_cpu_t *)0) {
        return;
    }
    if (!c->started) {
        /* First sample only establishes the baseline: with no previous reading there
         * is no window to average over, and treating the totals as a window would
         * report the lifetime mean -- exactly the behaviour this replaces. */
        c->started = 1;
        c->last_busy = busy_total;
        c->last_wall = wall_total;
        return;
    }
    if (busy_total < c->last_busy || wall_total < c->last_wall) {
        /* A counter went backwards (reset or wrap): rebase and keep the last reading
         * rather than computing a nonsense window. */
        c->last_busy = busy_total;
        c->last_wall = wall_total;
        return;
    }
    dbusy = busy_total - c->last_busy;
    dwall = wall_total - c->last_wall;
    c->last_busy = busy_total;
    c->last_wall = wall_total;
    if (dwall == 0u) {
        return; /* no window elapsed; keep the previous percentage */
    }
    c->pct = (unsigned)((dbusy >= dwall) ? 100u : ((dbusy * 100u) / dwall));
}

unsigned hype_vm_cpu_pct(const hype_vm_cpu_t *c) {
    return (c == (const hype_vm_cpu_t *)0) ? 0u : c->pct;
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
                           const char *cmdline, const char *result) {
    char line[160];
    char up[16];

    /* Home + clear the grid (reuse the VT interpreter's own ED/CUP). */
    hype_vt_screen_write(s, (const uint8_t *)"\x1b[H\x1b[2J", 7);

    hype_dashboard_fmt_uptime(up, host_uptime_s);
    hype_snprintf(line, sizeof(line), "hype - VM dashboard        host up %s", up);
    emit_line(s, line);
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

    /* TERM-2 footer: the command prompt (with a '_' caret) + last result. */
    emit_line(s, "");
    emit_line(s, "type: help | list | status|start|stop|resume|shutdown|off|focus <vm>");
    hype_snprintf(line, sizeof(line), "hype> %s_", cmdline ? cmdline : "");
    emit_line(s, line);
    if (result && result[0]) {
        emit_line(s, result);
    }
}
