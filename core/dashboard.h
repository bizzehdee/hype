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

/*
 * #460: the last command's output, as LINES rather than one string.
 *
 * It used to be a single `char[96]` in boot/main.c. Two features outgrew it: `help` is 116
 * characters and was cut mid-token (taking `config <vm>` off the screen entirely, #459), and
 * TERM-6's `config <vm>` is a whole VM's configuration -- which is why that shipped writing to
 * the debug log with `-- see log` on screen. On a cold-boot-only, serial-less machine "see log"
 * means power down and move the USB stick, so the answer was effectively unreadable.
 *
 * Sized to hold the largest thing a command produces (a VM's full effective config) on a grid
 * that is 100+ rows tall in every mode this project renders at. Overflow is reported, never
 * silent -- see hype_dash_text_add.
 */
#define HYPE_DASH_TEXT_LINES 48u
#define HYPE_DASH_TEXT_COLS 128u

typedef struct {
    char line[HYPE_DASH_TEXT_LINES][HYPE_DASH_TEXT_COLS];
    unsigned count;   /* lines held */
    unsigned dropped; /* lines refused because the buffer was full */
} hype_dash_text_t;

void hype_dash_text_reset(hype_dash_text_t *t);

/* Append one line, truncated at HYPE_DASH_TEXT_COLS-1. Past capacity the line is refused and
 * `dropped` counts it, so the renderer can say so instead of quietly showing a partial answer. */
void hype_dash_text_add(hype_dash_text_t *t, const char *s);

/*
 * Render the dashboard into grid `s` (cleared first). host_uptime_s is shown in the header.
 * `cmdline` (the TERM-2 command being typed) and `result` (the last command's output) render as
 * a footer when non-NULL. Result lines that do not fit the grid are reported by count rather
 * than dropped silently.
 *
 * #461: `alert`, when non-NULL, renders directly under the header, before anything else. It
 * exists for the one thing the dashboard could not previously say -- that a core has died. A
 * core that takes an unhandled fault halts alone, so the remaining cores keep rendering a
 * perfectly healthy-looking table for a VM whose vCPU is gone.
 */
void hype_dashboard_render(hype_vt_screen_t *s,
                           const hype_vm_dash_info_t *vms, unsigned n,
                           uint64_t host_uptime_s,
                           const char *cmdline, const hype_dash_text_t *result,
                           const char *alert);

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

/*
 * #264: per-VM CPU%, measured rather than inferred.
 *
 * The old figure was 100 minus the fraction of wall-clock spent waiting in HLT. Two
 * things made that structurally incapable of showing anything but 0% or 100%: the
 * guest never executed HLT at all (#256 -- it idled via an un-intercepted MWAIT), so
 * the idle term was always zero and the result always exactly 100; and the render
 * substituted a literal 0 whenever the VM was not RUNNING. The metric was not coarse,
 * it was measuring something that could not move.
 *
 * This measures busy time DIRECTLY: time actually spent inside the guest (which the
 * exit-cost instrumentation already accumulates as VMRUN time) against wall-clock.
 * Both inputs are monotonic cumulative counters in the same unit, and the percentage
 * is computed from the DELTA between consecutive samples, so it is a sliding window
 * that responds to load instead of converging on a lifetime mean.
 *
 * Deliberately unit-agnostic (TSC ticks, ns, ms -- whatever the caller uses), because
 * the ratio is all that matters and that keeps this pure and testable. As with the
 * uptime accumulator, the formatter was already tested while the DERIVATION had no
 * tests, which is how a permanently-constant metric survived.
 */
/*
 * #429: this used to compare only the two most recent samples, which flickered wildly
 * (0% <-> 96%) once the caller started sampling on every VM-exit rather than once per
 * render tick -- a window of "since the last call" shrinks to whatever tiny slice of
 * work happened between two exits, not anything a human would call a rate.
 *
 * Fixed by decimating commits into a small ring spanning the requested `window` (in the
 * caller's own unit -- TSC ticks for FW-1's real caller), independent of how often
 * hype_vm_cpu_sample() itself is called: a commit lands at most once per `window /
 * HYPE_VM_CPU_RING_MAX`, so the ring always covers roughly `window` regardless of call
 * cadence. The percentage is busy/wall between the OLDEST populated ring entry and the
 * latest raw sample, which is what makes it "decay over N seconds" rather than snap.
 */
#define HYPE_VM_CPU_RING_MAX 32u

typedef struct {
    unsigned long long last_busy; /* most recent raw cumulative sample */
    unsigned long long last_wall;
    unsigned long long ring_busy[HYPE_VM_CPU_RING_MAX];
    unsigned long long ring_wall[HYPE_VM_CPU_RING_MAX];
    unsigned long long last_commit_wall; /* wall value of the most recently committed entry */
    unsigned int ring_count;             /* populated ring entries, <= HYPE_VM_CPU_RING_MAX */
    unsigned int ring_next;              /* index the next commit writes (mod RING_MAX) */
    unsigned pct;                        /* most recent window's percentage, 0..100 */
    int started;
} hype_vm_cpu_t;

void hype_vm_cpu_reset(hype_vm_cpu_t *c);

/*
 * Fold one sample. `busy_total` and `wall_total` are cumulative and must be in the same
 * unit as `window` (the averaging window's span in that same unit -- e.g. TSC ticks for
 * N seconds at the host's TSC frequency). A window with no elapsed wall time since the
 * oldest populated ring entry, or either counter going backwards, leaves the previous
 * percentage in place rather than producing a spike. The result is clamped to 100 so
 * measurement jitter cannot report an impossible figure. `window == 0` is treated as 1
 * (the smallest meaningful window), matching TERM-7/#429's own "no less than 1 second"
 * floor at the config layer -- this function does not know what a second is, so the
 * floor has to be enforced again here for any caller that skips the config parser.
 */
void hype_vm_cpu_sample(hype_vm_cpu_t *c, unsigned long long busy_total,
                        unsigned long long wall_total, unsigned long long window);

unsigned hype_vm_cpu_pct(const hype_vm_cpu_t *c);

/* Pure terminal-focus policy. `available` is a per-VM byte array (available[i] != 0) of VMs whose runtime
 * display and input state is initialized. Dashboard is -1. Cycling skips
 * unavailable VMs; a direct jump to one leaves focus unchanged. */
typedef enum {
    HYPE_TERM_FOCUS_NONE = 0,
    HYPE_TERM_FOCUS_NEXT,
    HYPE_TERM_FOCUS_PREV,
    HYPE_TERM_FOCUS_JUMP,
    HYPE_TERM_FOCUS_TOGGLE_DASHBOARD
} hype_term_focus_action_t;

int hype_term_focus_validate(int view, const unsigned char *available, unsigned int nvm);
int hype_term_focus_apply(int current, hype_term_focus_action_t action,
                          unsigned int jump_index, const unsigned char *available,
                          unsigned int nvm);

/* Resolve a configured VM name to its index without making availability part of
 * the answer.  Startup uses this before vCPUs are dispatched, then waits for
 * hype_term_focus_validate() to say the requested view is safe to render. */
int hype_term_focus_find_name(const char *name, const char *const *names, unsigned int nvm);

#endif /* HYPE_DASHBOARD_H */
