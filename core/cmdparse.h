#ifndef HYPE_CMDPARSE_H
#define HYPE_CMDPARSE_H

/*
 * TERM-2: dashboard command parser. Pure string -> command decode (no VM
 * knowledge, no I/O), so it unit-tests in isolation; boot/main.c resolves the
 * arg to a VM and dispatches to the M8 lifecycle events / focus switch.
 *
 * Command set (M8's dashboard-controlled state): list, status, start, stop,
 * resume, shutdown, off (force), focus/switch, help. Verbs accept a few natural
 * aliases; the argument is a VM name or 1-based index.
 */

typedef enum {
    HYPE_CMD_NONE = 0,   /* empty line */
    HYPE_CMD_HELP,
    HYPE_CMD_LIST,
    HYPE_CMD_STATUS,
    HYPE_CMD_START,
    HYPE_CMD_STOP,       /* pause vCPU in place (M8-5) */
    HYPE_CMD_RESUME,
    HYPE_CMD_SHUTDOWN,   /* orderly, guest-driven S5 (M8-6) */
    HYPE_CMD_POWEROFF,   /* force power off (M8-7) */
    HYPE_CMD_FOCUS,      /* switch console focus */
    HYPE_CMD_CONFIRM,    /* M10-5: confirm a pending physical-disk write (arg = drive serial) */
    /* TERM-7 (#443): the first entry in TERM's general host-settings mechanism. arg = "list" (show
     * every resolution the GOP offers), "<W>x<H>" (apply and persist one), or absent (show the
     * current setting). */
    /* #529: HYPE_CMD_RESOLUTION retired with the config key (decision 44). */
    HYPE_CMD_CREATE, /* TERM-10 (#486): interactive VM-creation wizard */
    HYPE_CMD_DELETE, /* TERM-15 (#491): two-step-confirmed VM removal */
    /* TERM-6 (#444): arg = a VM name/index (the same addressing every other per-VM verb uses).
     * Prints that VM's full effective hype.cfg -- every field, tagged (default) or (set) --
     * into the dashboard's multi-line result panel (#460). */
    HYPE_CMD_CONFIG,
    HYPE_CMD_HOST,       /* M9-2 (#175): host-wide reboot/off -- shut every guest down first */
    HYPE_CMD_SET,        /* TERM-14 (#490): set <vm> <key> <value> -- edit config, write back */
    /*
     * #568: capture the screen to a PNG. A SECOND trigger for what was a hotkey only:
     * Right-Ctrl+Right-Alt+Print Screen needs three keys laptops vary on -- Print Screen is
     * often behind Fn and resolved in keyboard firmware, Right-Alt is AltGr on many layouts,
     * and some compact keyboards have no Right-Ctrl at all. On the Intel laptop the chord never
     * fired. A verb works on any keyboard that can type.
     */
    HYPE_CMD_SCREENSHOT,
    HYPE_CMD_UNKNOWN,
} hype_cmd_verb_t;

/*
 * #459: the canonical usage list, and the ONLY place a verb's on-screen spelling lives.
 *
 * There used to be two hand-maintained copies -- the always-visible hint line in
 * core/dashboard.c and the `help` result string in boot/main.c -- and neither was updated when
 * `resolution` (#443) and `config` (#444) were added. The dashboard therefore advertised the
 * pre-TERM-6/7 command set permanently, which is why both features read as missing. Deriving
 * both from this table is what stops the third divergence.
 */
unsigned hype_cmd_usage_count(void);

/* Usage text for entry `index` (e.g. "status <vm>"), or "" when out of range. */
const char *hype_cmd_usage(unsigned index);

#define HYPE_CMD_ARG_MAX 48u

typedef struct {
    hype_cmd_verb_t verb;
    char arg[HYPE_CMD_ARG_MAX];  /* first argument token, "" if none */
    char arg2[HYPE_CMD_ARG_MAX]; /* TERM-14: second argument token (the key) */
    char arg3[HYPE_CMD_ARG_MAX]; /* TERM-14: third argument token (the value) */
    int has_arg;
    int has_arg2;
    int has_arg3;
} hype_cmd_t;

/* Parse one command line. Leading/trailing space ignored; the verb is the first
 * whitespace-delimited token, then up to three argument tokens (further tokens
 * ignored). Only TERM-14's `set` consumes all three today. */
/* Fill-in-place form -- the only one the freestanding build may use: hype_cmd_t is too large
 * to return by value without emitting a memcpy call libc-free code cannot link (AGENTS.md). */
void hype_cmd_parse_at(const char *line, hype_cmd_t *out);

/* By-value convenience for HOSTED callers (the unit tests). static inline so it is only ever
 * materialized in a translation unit that calls it -- never in the EFI image. */
static inline hype_cmd_t hype_cmd_parse(const char *line) {
    hype_cmd_t c;
    hype_cmd_parse_at(line, &c);
    return c;
}

#endif /* HYPE_CMDPARSE_H */
