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
 * in the header. Rows beyond the grid height simply scroll off (the grid's
 * own behaviour); callers size the grid to the panel. */
void hype_dashboard_render(hype_vt_screen_t *s,
                           const hype_vm_dash_info_t *vms, unsigned n,
                           uint64_t host_uptime_s);

/* Format `secs` as HH:MM:SS into buf (>= 9 bytes). Exposed for tests. */
void hype_dashboard_fmt_uptime(char *buf, unsigned long long secs);

#endif /* HYPE_DASHBOARD_H */
