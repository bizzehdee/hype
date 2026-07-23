#include "phys_confirm.h"
#include "strutil.h"

/* --- tiny freestanding string-builder (no libc, no float) --- */

typedef struct {
    char *buf;
    unsigned int cap; /* including the NUL slot */
    unsigned int len;
} sb_t;

static void sb_init(sb_t *b, char *buf, unsigned int n) {
    b->buf = buf;
    b->cap = n;
    b->len = 0;
    if (n) buf[0] = '\0';
}

static void sb_puts(sb_t *b, const char *s) {
    if (!s) return;
    while (*s && b->len + 1 < b->cap) {
        b->buf[b->len++] = *s++;
    }
    if (b->cap) b->buf[b->len] = '\0';
}

static void sb_putu(sb_t *b, unsigned long long v) {
    char tmp[24];
    unsigned int i = 0;
    if (v == 0) {
        sb_puts(b, "0");
        return;
    }
    while (v && i < sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i) {
        char one[2];
        one[0] = tmp[--i];
        one[1] = '\0';
        sb_puts(b, one);
    }
}

/*
 * Appends a human-readable size. Picks the largest unit that yields a whole
 * part >= 1 and prints one decimal place (integer math only, banker's-free
 * truncation), e.g. 500107862016 -> "465.6 GiB". Bytes below 1 KiB print as
 * "<n> B".
 */
static void sb_put_size(sb_t *b, uint64_t bytes) {
    static const char *const units[] = { "KiB", "MiB", "GiB", "TiB", "PiB" };
    unsigned int u;
    uint64_t scale;

    if (bytes < 1024ULL) {
        sb_putu(b, bytes);
        sb_puts(b, " B");
        return;
    }
    /* find the largest unit whose scale still leaves a non-zero whole part */
    u = 0;
    scale = 1024ULL;
    while (u + 1 < (unsigned)(sizeof(units) / sizeof(units[0])) &&
           bytes >= scale * 1024ULL) {
        scale *= 1024ULL;
        u++;
    }
    {
        /* whole.tenths, truncated */
        uint64_t whole = bytes / scale;
        uint64_t rem = bytes - whole * scale;
        uint64_t tenths = (rem * 10ULL) / scale;
        sb_putu(b, whole);
        sb_puts(b, ".");
        sb_putu(b, tenths);
        sb_puts(b, " ");
        sb_puts(b, units[u]);
    }
}

/* --- state machine --- */

void hype_phys_confirm_request(hype_phys_confirm_t *c, const char *vm_name,
                               const char *model, const char *serial,
                               uint64_t size_bytes) {
    if (!c) return;
    hype_strlcpy(c->vm_name, vm_name ? vm_name : "", sizeof(c->vm_name));
    hype_strlcpy(c->model, model ? model : "", sizeof(c->model));
    hype_strlcpy(c->serial, serial ? serial : "", sizeof(c->serial));
    c->size_bytes = size_bytes;
    c->attempts = 0;
    c->state = HYPE_PHYS_CONFIRM_PENDING;
}

hype_phys_confirm_submit_t hype_phys_confirm_submit(hype_phys_confirm_t *c,
                                                    const char *typed_serial) {
    char tmp[HYPE_PHYS_CONFIRM_SERIAL_MAX * 2];
    const char *trimmed;

    if (!c || c->state != HYPE_PHYS_CONFIRM_PENDING) {
        return HYPE_PHYS_CONFIRM_SUBMIT_NONE_PENDING;
    }
    if (!typed_serial) {
        c->attempts++;
        return HYPE_PHYS_CONFIRM_SUBMIT_MISMATCH;
    }

    /* trim the typed token in a local copy (input is const) */
    hype_strlcpy(tmp, typed_serial, sizeof(tmp));
    trimmed = hype_str_trim(tmp);

    if (trimmed[0] != '\0' && hype_streq(trimmed, c->serial)) {
        c->state = HYPE_PHYS_CONFIRM_ACCEPTED;
        return HYPE_PHYS_CONFIRM_SUBMIT_ACCEPTED;
    }
    c->attempts++;
    return HYPE_PHYS_CONFIRM_SUBMIT_MISMATCH;
}

int hype_phys_confirm_is_accepted(const hype_phys_confirm_t *c) {
    return c && c->state == HYPE_PHYS_CONFIRM_ACCEPTED;
}

void hype_phys_confirm_reset(hype_phys_confirm_t *c) {
    if (!c) return;
    c->state = HYPE_PHYS_CONFIRM_IDLE;
    c->vm_name[0] = '\0';
    c->model[0] = '\0';
    c->serial[0] = '\0';
    c->size_bytes = 0;
    c->attempts = 0;
}

const char *hype_phys_confirm_prompt(const hype_phys_confirm_t *c, char *buf, unsigned int n) {
    sb_t b;
    sb_init(&b, buf, n);
    if (!c) return buf;

    switch (c->state) {
    case HYPE_PHYS_CONFIRM_PENDING:
        sb_puts(&b, "!! PHYSICAL WRITE -- VM '");
        sb_puts(&b, c->vm_name);
        sb_puts(&b, "' -> ");
        sb_puts(&b, c->model[0] ? c->model : "(unknown model)");
        sb_puts(&b, " sn ");
        sb_puts(&b, c->serial[0] ? c->serial : "(none)");
        sb_puts(&b, " ");
        sb_put_size(&b, c->size_bytes);
        sb_puts(&b, " -- ALL DATA WILL BE DESTROYED. To proceed type: confirm ");
        sb_puts(&b, c->serial);
        break;
    case HYPE_PHYS_CONFIRM_ACCEPTED:
        sb_puts(&b, "physical write CONFIRMED for VM '");
        sb_puts(&b, c->vm_name);
        sb_puts(&b, "' (sn ");
        sb_puts(&b, c->serial);
        sb_puts(&b, ")");
        break;
    case HYPE_PHYS_CONFIRM_IDLE:
    default:
        break;
    }
    return buf;
}
