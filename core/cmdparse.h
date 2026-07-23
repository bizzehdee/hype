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
    HYPE_CMD_UNKNOWN,
} hype_cmd_verb_t;

#define HYPE_CMD_ARG_MAX 48u

typedef struct {
    hype_cmd_verb_t verb;
    char arg[HYPE_CMD_ARG_MAX]; /* first argument token, "" if none */
    int has_arg;
} hype_cmd_t;

/* Parse one command line. Leading/trailing space ignored; the verb is the first
 * whitespace-delimited token, the arg the second (further tokens ignored). */
hype_cmd_t hype_cmd_parse(const char *line);

#endif /* HYPE_CMDPARSE_H */
