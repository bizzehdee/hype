#include "cmdparse.h"

static int is_ws(char c) { return c == ' ' || c == '\t'; }

/* Case-insensitive compare of a token [tok, tok+len) against a NUL word. */
static int tok_eq(const char *tok, unsigned len, const char *word) {
    unsigned i = 0;
    for (; i < len; i++) {
        char a = tok[i];
        char b = word[i];
        if (b == '\0') return 0;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return word[i] == '\0';
}

static hype_cmd_verb_t verb_of(const char *tok, unsigned len) {
    if (len == 0) return HYPE_CMD_NONE;
    if (tok_eq(tok, len, "help") || tok_eq(tok, len, "?"))            return HYPE_CMD_HELP;
    if (tok_eq(tok, len, "list") || tok_eq(tok, len, "ls"))          return HYPE_CMD_LIST;
    if (tok_eq(tok, len, "status") || tok_eq(tok, len, "stat"))      return HYPE_CMD_STATUS;
    if (tok_eq(tok, len, "start") || tok_eq(tok, len, "boot"))       return HYPE_CMD_START;
    if (tok_eq(tok, len, "stop") || tok_eq(tok, len, "pause"))       return HYPE_CMD_STOP;
    if (tok_eq(tok, len, "resume") || tok_eq(tok, len, "cont"))      return HYPE_CMD_RESUME;
    if (tok_eq(tok, len, "shutdown") || tok_eq(tok, len, "poweroff")) return HYPE_CMD_SHUTDOWN;
    if (tok_eq(tok, len, "off") || tok_eq(tok, len, "kill") || tok_eq(tok, len, "force")) return HYPE_CMD_POWEROFF;
    if (tok_eq(tok, len, "focus") || tok_eq(tok, len, "switch") || tok_eq(tok, len, "sw")) return HYPE_CMD_FOCUS;
    if (tok_eq(tok, len, "confirm")) return HYPE_CMD_CONFIRM;
    /* #529: `resolution`/`res` retired with the config key -- hype picks the mode nearest
     * 1920x1080 at boot, and SetMode is Boot Services so a runtime command could not work. */
    if (tok_eq(tok, len, "create")) return HYPE_CMD_CREATE; /* TERM-10 (#486) */
    if (tok_eq(tok, len, "delete")) return HYPE_CMD_DELETE; /* TERM-15 (#491) */
    if (tok_eq(tok, len, "mkdisk")) return HYPE_CMD_MKDISK; /* TERM-11 (#487) */
    if (tok_eq(tok, len, "attach")) return HYPE_CMD_ATTACH; /* TERM-12 (#488) */
    if (tok_eq(tok, len, "detach")) return HYPE_CMD_DETACH; /* TERM-12 (#488) */
    if (tok_eq(tok, len, "screenshot")) return HYPE_CMD_SCREENSHOT; /* #568 */
    if (tok_eq(tok, len, "dump")) return HYPE_CMD_DUMP; /* #611 */
    if (tok_eq(tok, len, "config") || tok_eq(tok, len, "cfg")) return HYPE_CMD_CONFIG;
    if (tok_eq(tok, len, "host")) return HYPE_CMD_HOST; /* M9-2 (#175): host reboot|off */
    if (tok_eq(tok, len, "set")) return HYPE_CMD_SET;   /* TERM-14 (#490) */
    return HYPE_CMD_UNKNOWN;
}

void hype_cmd_parse_at(const char *line, hype_cmd_t *out) {
    hype_cmd_t *cp = out;
#define c (*cp)
    char *dst[4];
    int *has[4];
    unsigned t;
    c.verb = HYPE_CMD_NONE;
    c.arg[0] = '\0';
    c.arg2[0] = '\0';
    c.arg3[0] = '\0';
    c.arg4[0] = '\0';
    c.has_arg = 0;
    c.has_arg2 = 0;
    c.has_arg3 = 0;
    c.has_arg4 = 0;

    if (!line) return;

    unsigned i = 0;
    while (line[i] && is_ws(line[i])) i++;      /* skip leading ws */

    unsigned vstart = i;
    while (line[i] && !is_ws(line[i])) i++;     /* verb token */
    unsigned vlen = i - vstart;
    c.verb = verb_of(&line[vstart], vlen);

    dst[0] = c.arg;
    dst[1] = c.arg2;
    dst[2] = c.arg3;
    dst[3] = c.arg4;
    has[0] = &c.has_arg;
    has[1] = &c.has_arg2;
    has[2] = &c.has_arg3;
    has[3] = &c.has_arg4;
    for (t = 0; t < 4u; t++) {
        while (line[i] && is_ws(line[i])) i++;  /* skip ws before this token */
        unsigned astart = i;
        while (line[i] && !is_ws(line[i])) i++;
        unsigned alen = i - astart;
        if (alen == 0) break;
        *has[t] = 1;
        unsigned n = (alen < HYPE_CMD_ARG_MAX - 1) ? alen : HYPE_CMD_ARG_MAX - 1;
        for (unsigned k = 0; k < n; k++) dst[t][k] = line[astart + k];
        dst[t][n] = '\0';
    }
#undef c
}


/*
 * #459: one table, both readers. Order matches the verb_of() chain above so a new verb is added
 * in two adjacent places in one file, not in three files two of which are easy to miss.
 */
static const char *const g_cmd_usage[] = {
    "help",
    "list",
    "status <vm>",
    "start <vm>",
    "stop <vm>",
    "resume <vm>",
    "shutdown <vm>",
    "off <vm>",
    "focus <vm>",
    "confirm <serial>",
    "create",
    "delete <vm>",
    "mkdisk <disk-serial> <path> <GiB> [raw|qcow2]",
    "attach <vm> <usb-msc|sata|usb-phys>:<path-or-serial>",
    "detach <vm> <device-id>",
    "config <vm>",
    "host <reboot|off>",
    "set <vm> <key> <value>",
    "screenshot",
    "dump <vm>",
};

unsigned hype_cmd_usage_count(void) {
    return (unsigned)(sizeof(g_cmd_usage) / sizeof(g_cmd_usage[0]));
}

const char *hype_cmd_usage(unsigned index) {
    return (index < hype_cmd_usage_count()) ? g_cmd_usage[index] : "";
}
