#include "run_state.h"

#include "format.h"
#include "strutil.h"

/*
 * The written form, deliberately plain text:
 *
 *   # hype run-state record -- written at host shutdown, read at the next startup [#176 #177]
 *   version = 1
 *   reason = reboot
 *   vm alpine = running
 *   vm build = stopped
 *
 * Text rather than a packed struct because the whole value of this file is diagnostic: when a host
 * comes back up with the wrong machines running, the first question is what the record said, and
 * that question has to be answerable by reading it. A binary blob would need a tool that only
 * exists on a working host.
 */

void hype_run_state_init(hype_run_state_t *st, hype_run_state_reason_t reason) {
    unsigned int i;
    st->version = HYPE_RUN_STATE_VERSION;
    st->reason = reason;
    st->count = 0;
    st->malformed = 0;
    st->unknown_keys = 0;
    st->dropped = 0;
    for (i = 0; i < HYPE_RUN_STATE_MAX_VMS; i++) {
        st->vms[i].name[0] = '\0';
        st->vms[i].running = 0;
    }
}

static const char *reason_name(hype_run_state_reason_t r) {
    if (r == HYPE_RUN_STATE_REASON_REBOOT) return "reboot";
    if (r == HYPE_RUN_STATE_REASON_OFF) return "off";
    return "unknown";
}

int hype_run_state_add(hype_run_state_t *st, const char *name, int running) {
    if (st->count >= HYPE_RUN_STATE_MAX_VMS) {
        st->dropped++;
        return -1;
    }
    hype_strlcpy(st->vms[st->count].name, name, sizeof(st->vms[st->count].name));
    st->vms[st->count].running = running ? 1 : 0;
    st->count++;
    return 0;
}

int hype_run_state_lookup(const hype_run_state_t *st, const char *name) {
    unsigned int i;
    /*
     * A refused record has no opinion about anything. Answering from a fragment is how a host
     * comes up with the wrong set of machines running and no way to tell.
     */
    if (st->malformed) {
        return HYPE_RUN_STATE_UNKNOWN;
    }
    if (name == 0 || name[0] == '\0') {
        return HYPE_RUN_STATE_UNKNOWN;
    }
    for (i = 0; i < st->count; i++) {
        if (hype_streq(st->vms[i].name, name)) {
            return st->vms[i].running ? HYPE_RUN_STATE_RUNNING : HYPE_RUN_STATE_STOPPED;
        }
    }
    return HYPE_RUN_STATE_UNKNOWN;
}

int hype_run_state_serialize(const hype_run_state_t *st, char *out, unsigned int cap,
                             unsigned int *len) {
    unsigned int n = 0, i;
    int w;

    if (out == 0 || cap == 0) {
        return -1;
    }
    /*
     * Every hype_snprintf return is checked against the REMAINING space, not just for being
     * positive. hype_snprintf reports the length it WOULD have written, so a value >= the space
     * left means this line was cut -- and a record whose last `vm` line was cut in half claims a
     * machine was stopped when it was running.
     */
    w = hype_snprintf(out + n, cap - n,
                      "# hype run-state record -- written at host shutdown, read at the next "
                      "startup [#176 #177]\n");
    if (w < 0 || (unsigned int)w >= cap - n) return -1;
    n += (unsigned int)w;

    w = hype_snprintf(out + n, cap - n, "version = %u\n", st->version);
    if (w < 0 || (unsigned int)w >= cap - n) return -1;
    n += (unsigned int)w;

    w = hype_snprintf(out + n, cap - n, "reason = %s\n", reason_name(st->reason));
    if (w < 0 || (unsigned int)w >= cap - n) return -1;
    n += (unsigned int)w;

    for (i = 0; i < st->count; i++) {
        /* An unnamed VM cannot be looked up again, so writing it would be writing a line that can
         * only ever be ignored. Skipped, and it is not a failure of the record. */
        if (st->vms[i].name[0] == '\0') {
            continue;
        }
        w = hype_snprintf(out + n, cap - n, "vm %s = %s\n", st->vms[i].name,
                          st->vms[i].running ? "running" : "stopped");
        if (w < 0 || (unsigned int)w >= cap - n) return -1;
        n += (unsigned int)w;
    }
    if (len != 0) {
        *len = n;
    }
    return 0;
}

int hype_run_state_parse(const char *text, unsigned int len, hype_run_state_t *out) {
    unsigned int i = 0;
    int have_version = 0;

    hype_run_state_init(out, HYPE_RUN_STATE_REASON_UNKNOWN);
    out->version = 0;
    if (text == 0 || len == 0) {
        out->malformed = 1;
        return -1;
    }

    while (i < len) {
        char line[64 + HYPE_CFG_NAME_MAX];
        unsigned int j = 0;
        char *s, *eq, *key, *val;

        while (i < len && text[i] != '\n' && j + 1u < sizeof(line)) {
            if (text[i] != '\r') {
                line[j++] = text[i];
            }
            i++;
        }
        /* Skip whatever did not fit plus the newline itself. A line longer than the buffer is a
         * line this build does not understand, and it is treated as one rather than as the prefix
         * that happened to fit -- a truncated `vm somename` would name a different VM. */
        {
            int overlong = (i < len && text[i] != '\n');
            while (i < len && text[i] != '\n') i++;
            if (i < len) i++;
            line[j] = '\0';
            if (overlong) {
                out->unknown_keys++;
                continue;
            }
        }

        s = hype_str_trim(line);
        if (s[0] == '\0' || s[0] == '#') {
            continue;
        }
        eq = s;
        while (*eq != '\0' && *eq != '=') eq++;
        if (*eq != '=') {
            /* A non-empty, non-comment line with no `=` is not a key at all. That is a damaged
             * file, not an unknown key, so it refuses the record. */
            out->malformed = 1;
            break;
        }
        *eq = '\0';
        key = hype_str_trim(s);
        val = hype_str_trim(eq + 1);

        if (hype_streq(key, "version")) {
            unsigned long long v = 0;
            int ok = (hype_parse_uint(val, &v) == 0);
            out->version = ok ? (unsigned int)v : 0u;
            if (!ok || v != (unsigned long long)HYPE_RUN_STATE_VERSION) {
                /*
                 * A version this build does not know refuses the WHOLE record. Reading the lines
                 * that happen to still parse would produce a half-restored host, and a
                 * half-restored host is indistinguishable from a correctly restored one until an
                 * operator notices a machine missing.
                 */
                out->malformed = 1;
                break;
            }
            have_version = 1;
        } else if (hype_streq(key, "reason")) {
            if (hype_streq(val, "reboot")) {
                out->reason = HYPE_RUN_STATE_REASON_REBOOT;
            } else if (hype_streq(val, "off")) {
                out->reason = HYPE_RUN_STATE_REASON_OFF;
            } else {
                out->reason = HYPE_RUN_STATE_REASON_UNKNOWN;
            }
        } else if (hype_strneq(key, "vm ", 3) || hype_strneq(key, "vm\t", 3)) {
            char *nm = hype_str_trim(key + 3);
            int running;
            if (nm[0] == '\0') {
                out->malformed = 1;
                break;
            }
            if (hype_streq(val, "running")) {
                running = 1;
            } else if (hype_streq(val, "stopped")) {
                running = 0;
            } else {
                /* Neither state. Refusing rather than guessing: the two answers lead to opposite
                 * actions on the next boot. */
                out->malformed = 1;
                break;
            }
            (void)hype_run_state_add(out, nm, running);
        } else if (hype_streq(key, "vm")) {
            /* A `vm` line with no name cannot be looked up, so it is a damaged record rather than
             * a key from a newer build. */
            out->malformed = 1;
            break;
        } else {
            out->unknown_keys++;
        }
    }

    /*
     * No version line at all is a refusal too. An unversioned file is either not ours or was
     * written by something that did not finish, and both readings say the same thing: do not act
     * on it.
     */
    if (!have_version) {
        out->malformed = 1;
    }
    if (out->malformed) {
        out->count = 0;
        return -1;
    }
    return 0;
}
