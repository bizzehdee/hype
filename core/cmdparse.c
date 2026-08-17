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
    if (tok_eq(tok, len, "resolution") || tok_eq(tok, len, "res")) return HYPE_CMD_RESOLUTION;
    if (tok_eq(tok, len, "config") || tok_eq(tok, len, "cfg")) return HYPE_CMD_CONFIG;
    if (tok_eq(tok, len, "host")) return HYPE_CMD_HOST; /* M9-2 (#175): host reboot|off */
    return HYPE_CMD_UNKNOWN;
}

hype_cmd_t hype_cmd_parse(const char *line) {
    hype_cmd_t c;
    c.verb = HYPE_CMD_NONE;
    c.arg[0] = '\0';
    c.has_arg = 0;

    if (!line) return c;

    unsigned i = 0;
    while (line[i] && is_ws(line[i])) i++;      /* skip leading ws */

    unsigned vstart = i;
    while (line[i] && !is_ws(line[i])) i++;     /* verb token */
    unsigned vlen = i - vstart;
    c.verb = verb_of(&line[vstart], vlen);

    while (line[i] && is_ws(line[i])) i++;      /* skip ws before arg */
    unsigned astart = i;
    while (line[i] && !is_ws(line[i])) i++;     /* arg token */
    unsigned alen = i - astart;
    if (alen > 0) {
        c.has_arg = 1;
        unsigned n = (alen < HYPE_CMD_ARG_MAX - 1) ? alen : HYPE_CMD_ARG_MAX - 1;
        for (unsigned k = 0; k < n; k++) c.arg[k] = line[astart + k];
        c.arg[n] = '\0';
    }
    return c;
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
    "resolution [list|<W>x<H>]",
    "config <vm>",
    "host <reboot|off>",
};

unsigned hype_cmd_usage_count(void) {
    return (unsigned)(sizeof(g_cmd_usage) / sizeof(g_cmd_usage[0]));
}

const char *hype_cmd_usage(unsigned index) {
    return (index < hype_cmd_usage_count()) ? g_cmd_usage[index] : "";
}
