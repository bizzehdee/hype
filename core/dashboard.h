#ifndef HYPE_DASHBOARD_H
#define HYPE_DASHBOARD_H

#include <stdint.h>
#include "vt_screen.h"

/*
 * M8-1: VM management dashboard. Rather than a bespoke pixel renderer, the
 * dashboard is just *another* vt_screen (the same character-grid model TERM-1
 * built): this module formats a per-VM table into a grid, and core/vt_render.c
 * blits it onto the panel exactly like a guest terminal. That keeps the
 * rendering path uniform (one grid model, one blitter) and makes the layout
 * pure + unit-testable -- feed VM info, read back cells, no framebuffer.
 */

typedef struct {
    const char *name;     /* VM display name */
    const char *os_hint;  /* "linux" / "windows" / "bsd" / "" */
    const char *state;    /* "running" / "paused" / "off" / "starting" / ... */
    unsigned cpu_pct;     /* 0-100 recent vCPU utilisation */
    unsigned mem_mb;      /* configured guest RAM */
    uint64_t uptime_s;    /* seconds since this VM last (re)started */
    const char *media;    /* boot media (ISO) short name, or 0 for "-" */
    int focused;          /* nonzero => this row is the currently-focused VM */
} hype_vm_dash_info_t;

/* Render the dashboard into grid `s` (cleared first). host_uptime_s is shown
 * in the header. `cmdline` (the TERM-2 command being typed) and `result` (the
 * last command's result message) render as a footer when non-NULL. Rows beyond
 * the grid height scroll off (the grid's own behaviour); size the grid to the
 * panel. */
void hype_dashboard_render(hype_vt_screen_t *s,
                           const hype_vm_dash_info_t *vms, unsigned n,
                           uint64_t host_uptime_s,
                           const char *cmdline, const char *result);

/* Format `secs` as HH:MM:SS into buf (>= 9 bytes). Exposed for tests. */
void hype_dashboard_fmt_uptime(char *buf, unsigned long long secs);

/*
 * #263: accumulated RUNNING time for one VM.
 *
 * UPTIME used to be recomputed on every render as wall-clock since a fixed
 * perf_boot_start_tsc, so it kept climbing while the VM's state read `off` --
 * nothing subtracted stopped time and nothing re-based the origin. The CPU column
 * immediately above it already gated on lifecycle, so the two neighbours disagreed.
 *
 * "Uptime" here means what an operator means by it, and what virsh/Hyper-V show:
 * time the VM has actually been running, frozen while stopped and continuing on
 * resume. That cannot be derived from a fixed origin -- it has to accumulate.
 *
 * Kept pure and clock-free (the caller passes wall-clock milliseconds) because the
 * formatter was already tested while the DERIVATION had no tests at all, which is
 * exactly why a metric that never stopped counting went unnoticed.
 */
typedef struct {
    unsigned long long accum_ms;   /* running time banked before the current sample */
    unsigned long long last_ms;    /* wall-clock at the previous sample */
    int running;                   /* whether the VM was running as of that sample */
    int started;                   /* whether any sample has been taken yet */
} hype_vm_uptime_t;

void hype_vm_uptime_reset(hype_vm_uptime_t *u);

/*
 * Fold the interval since the previous sample into the total. An interval is credited
 * only when the VM was running at BOTH ends of it, so a stop freezes the figure and a
 * resume continues it; this can under-credit by at most one sample period around a
 * transition, which is preferable to advancing while the state reads `off`.
 * `now_ms` must be non-decreasing; a decreasing reading is ignored rather than
 * credited as a huge interval.
 */
void hype_vm_uptime_sample(hype_vm_uptime_t *u, unsigned long long now_ms, int running);

unsigned long long hype_vm_uptime_ms(const hype_vm_uptime_t *u);

#endif /* HYPE_DASHBOARD_H */
