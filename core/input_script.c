#include "input_script.h"

static int is_space(char c) {
    return (c == ' ' || c == '\t');
}

/* Directive keywords, matched case-sensitively and in full: a prefix match would
 * let "expec" or "fail" (meaning fail-if) through as something else. */
static int keyword_is(const char *s, uint32_t len, const char *kw) {
    uint32_t i = 0;
    while (kw[i] != '\0') {
        if (i >= len || s[i] != kw[i]) {
            return 0;
        }
        i++;
    }
    return (i == len);
}

static int parse_u32(const char *s, uint32_t len, uint32_t *out) {
    uint32_t v = 0;
    uint32_t i;

    if (len == 0) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        /* Reject overflow rather than wrapping: a timeout that silently became a
         * small number would look like a flaky guest, not a bad script. */
        if (v > (0xFFFFFFFFu - (uint32_t)(s[i] - '0')) / 10u) {
            return -1;
        }
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

/*
 * Copy `src` into a directive payload, resolving the four supported escapes.
 * Returns 0, or -1 for an unknown escape / trailing backslash, or -2 if it does
 * not fit.
 */
/*
 * #542: parse `<dx> <dy> [buttons]` from a whitespace-separated argument. Returns how many fields
 * were read (2 or 3 on success, <2 on failure). Deltas are signed decimals in -128..127; buttons is
 * 0-7. A value out of range is a failure rather than a silent clamp -- `sendmouse 500 0` is a script
 * bug, and clamping it to 127 would move the mouse a different distance than the script asked for.
 */
static int parse_mouse_args(const char *a, uint32_t len, int32_t *dx, int32_t *dy,
                            uint32_t *buttons) {
    uint32_t i = 0;
    int field = 0;
    int32_t vals[3] = {0, 0, 0};

    while (field < 3) {
        int neg = 0;
        int digits = 0;
        int32_t v = 0;

        while (i < len && (a[i] == ' ' || a[i] == '\t')) {
            i++;
        }
        if (i >= len) {
            break;
        }
        if (a[i] == '-') {
            neg = 1;
            i++;
        } else if (a[i] == '+') {
            i++;
        }
        while (i < len && a[i] >= '0' && a[i] <= '9') {
            v = v * 10 + (a[i] - '0');
            if (v > 100000) {
                return -1; /* absurd, and stops runaway accumulation */
            }
            digits++;
            i++;
        }
        if (digits == 0) {
            return -1;
        }
        vals[field++] = neg ? -v : v;
    }

    if (field < 2) {
        return field;
    }
    if (vals[0] < -128 || vals[0] > 127 || vals[1] < -128 || vals[1] > 127) {
        return -1;
    }
    if (field == 3 && (vals[2] < 0 || vals[2] > 7)) {
        return -1;
    }
    *dx = vals[0];
    *dy = vals[1];
    *buttons = (field == 3) ? (uint32_t)vals[2] : 0u;
    return field;
}

static int copy_payload(hype_input_directive_t *d, const char *src, uint32_t len) {
    uint32_t i = 0;
    uint32_t n = 0;

    while (i < len) {
        char c = src[i];
        if (c == '\\') {
            if (i + 1u >= len) {
                return -1; /* trailing backslash */
            }
            i++;
            switch (src[i]) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case '\\': c = '\\'; break;
                default: return -1;
            }
        }
        if (n >= HYPE_INPUT_SCRIPT_MAX_ARG) {
            return -2;
        }
        d->text[n] = (uint8_t)c;
        n++;
        i++;
    }
    d->len = n;
    return 0;
}

hype_input_parse_result_t hype_input_script_parse(const char *text, uint32_t len,
                                                  hype_input_script_t *out) {
    hype_input_parse_result_t r;
    uint32_t pos = 0;
    uint32_t line = 0;

    r.status = HYPE_INPUT_PARSE_OK;
    r.line = 0;
    out->count = 0;

    while (pos <= len) {
        uint32_t start = pos;
        uint32_t end;
        uint32_t kw_start, kw_end, arg_start, arg_end;
        hype_input_directive_t *d;
        int takes_number = 0;
        int takes_mouse = 0; /* #542 */
        int rc;

        if (pos == len) {
            break; /* a trailing newline leaves pos == len; nothing more to read */
        }
        while (pos < len && text[pos] != '\n') {
            pos++;
        }
        end = pos;
        if (pos < len) {
            pos++; /* step over the '\n' */
        }
        line++;

        /* CRLF: drop the '\r' so a Windows-authored script is not a parse error and,
         * worse, does not silently give every expect pattern a trailing carriage
         * return that the guest never sends. */
        if (end > start && text[end - 1u] == '\r') {
            end--;
        }
        /* '#' starts a comment. Not honoured mid-payload for `send`, because a shell
         * command may legitimately contain one -- so only a '#' that begins the
         * first token, or follows whitespace before the directive, counts. That is
         * handled by cutting the comment only when it precedes the keyword. */
        {
            uint32_t i = start;
            while (i < end && is_space(text[i])) {
                i++;
            }
            if (i < end && text[i] == '#') {
                continue; /* comment-only line */
            }
        }

        kw_start = start;
        while (kw_start < end && is_space(text[kw_start])) {
            kw_start++;
        }
        if (kw_start == end) {
            continue; /* blank line */
        }
        kw_end = kw_start;
        while (kw_end < end && !is_space(text[kw_end])) {
            kw_end++;
        }
        arg_start = kw_end;
        while (arg_start < end && is_space(text[arg_start])) {
            arg_start++;
        }
        arg_end = end;
        /* Trailing whitespace is stripped for every directive. An expect pattern
         * with an invisible trailing space would never match, and a send payload
         * that needs one can spell it as an escape. */
        while (arg_end > arg_start && is_space(text[arg_end - 1u])) {
            arg_end--;
        }

        if (out->count >= HYPE_INPUT_SCRIPT_MAX_DIRECTIVES) {
            r.status = HYPE_INPUT_PARSE_TOO_MANY_DIRECTIVES;
            r.line = line;
            return r;
        }
        d = &out->d[out->count];
        d->len = 0;
        d->ms = 0;
        d->line = line;

        {
            const char *kw = text + kw_start;
            uint32_t kwlen = kw_end - kw_start;
            if (keyword_is(kw, kwlen, "expect")) {
                d->op = HYPE_INPUT_OP_EXPECT;
            } else if (keyword_is(kw, kwlen, "sendmouse")) {
                d->op = HYPE_INPUT_OP_SENDMOUSE; /* #542 */
                takes_mouse = 1;
            } else if (keyword_is(kw, kwlen, "sendkey")) {
                /* Checked BEFORE "send" would be a prefix problem only if the matcher
                 * were a prefix match; keyword_is compares the whole token, so order is
                 * not load-bearing here -- but keeping sendkey adjacent to send makes
                 * the pair obvious to a reader. */
                d->op = HYPE_INPUT_OP_SENDKEY;
            } else if (keyword_is(kw, kwlen, "send")) {
                d->op = HYPE_INPUT_OP_SEND;
            } else if (keyword_is(kw, kwlen, "delay")) {
                d->op = HYPE_INPUT_OP_DELAY;
                takes_number = 1;
            } else if (keyword_is(kw, kwlen, "timeout")) {
                d->op = HYPE_INPUT_OP_TIMEOUT;
                takes_number = 1;
            } else if (keyword_is(kw, kwlen, "fail-if")) {
                d->op = HYPE_INPUT_OP_FAIL_IF;
            } else if (keyword_is(kw, kwlen, "pass")) {
                d->op = HYPE_INPUT_OP_PASS;
            } else if (keyword_is(kw, kwlen, "fail")) {
                d->op = HYPE_INPUT_OP_FAIL;
            } else {
                r.status = HYPE_INPUT_PARSE_UNKNOWN_DIRECTIVE;
                r.line = line;
                return r;
            }
        }

        if (arg_start >= arg_end) {
            r.status = HYPE_INPUT_PARSE_MISSING_ARG;
            r.line = line;
            return r;
        }

        if (takes_mouse) {
            /*
             * #542: `<dx> <dy> [buttons]`. Stored as the three bytes the device wants --
             * text[0] = status (with the sign bits already folded in), text[1] = dx,
             * text[2] = dy -- so the runner and the routing layer carry no encoding logic.
             */
            int32_t dx = 0, dy = 0;
            uint32_t buttons = 0;
            int nfields = parse_mouse_args(text + arg_start, arg_end - arg_start, &dx, &dy,
                                           &buttons);
            if (nfields < 2) {
                r.status = HYPE_INPUT_PARSE_BAD_NUMBER;
                r.line = line;
                return r;
            }
            d->text[0] = (uint8_t)(0x08u | (buttons & 0x07u) | (dx < 0 ? 0x10u : 0u) |
                                   (dy < 0 ? 0x20u : 0u));
            d->text[1] = (uint8_t)(dx & 0xFF);
            d->text[2] = (uint8_t)(dy & 0xFF);
            d->len = 3u;
        } else if (takes_number) {
            if (parse_u32(text + arg_start, arg_end - arg_start, &d->ms) != 0) {
                r.status = HYPE_INPUT_PARSE_BAD_NUMBER;
                r.line = line;
                return r;
            }
        } else {
            rc = copy_payload(d, text + arg_start, arg_end - arg_start);
            if (rc == -1) {
                r.status = HYPE_INPUT_PARSE_BAD_ESCAPE;
                r.line = line;
                return r;
            }
            if (rc == -2) {
                r.status = HYPE_INPUT_PARSE_ARG_TOO_LONG;
                r.line = line;
                return r;
            }
        }

        out->count++;
    }

    return r;
}

const char *hype_input_parse_status_str(hype_input_parse_status_t st) {
    switch (st) {
        case HYPE_INPUT_PARSE_OK: return "ok";
        case HYPE_INPUT_PARSE_UNKNOWN_DIRECTIVE: return "unknown directive";
        case HYPE_INPUT_PARSE_MISSING_ARG: return "directive needs an argument";
        case HYPE_INPUT_PARSE_BAD_NUMBER: return "not a number (or too large)";
        case HYPE_INPUT_PARSE_BAD_ESCAPE: return "unknown escape (only \\n \\r \\t \\\\)";
        case HYPE_INPUT_PARSE_ARG_TOO_LONG: return "argument too long";
        case HYPE_INPUT_PARSE_TOO_MANY_DIRECTIVES: return "too many directives";
        default: return "unknown error";
    }
}
