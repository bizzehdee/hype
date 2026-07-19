#include "clockfacts.h"

/* Key substrings marking the two unverified PERF-1 links + their context.
 * Kept deliberately specific so the buffer captures the decisive lines, not
 * every "clocksource:" mention:
 *   "lpj="                    -> delay-loop calibration (gap #4)
 *   "Switched to clocksource" -> the clocksource Linux actually settled on (#3)
 *   "Marking TSC unstable"    -> whether the TSC was rejected despite invariant
 *   "tsc: Detected"           -> the TSC frequency Linux calibrated
 *   "Refined jiffies"         -> the fallback clocksource, if TSC was dropped */
static const char *const HYPE_CLOCKFACTS_KEYS[] = {
    "lpj=",
    "Switched to clocksource",
    "Marking TSC unstable",
    "tsc: Detected",
    "Refined jiffies",
};
#define HYPE_CLOCKFACTS_NKEYS (sizeof(HYPE_CLOCKFACTS_KEYS) / sizeof(HYPE_CLOCKFACTS_KEYS[0]))

/* Returns 1 if `needle` occurs anywhere in `hay` (empty needle -> 1). */
static int clockfacts_contains(const char *hay, const char *needle) {
    unsigned int i;
    for (i = 0;; i++) {
        unsigned int j = 0;
        while (needle[j] != '\0' && hay[i + j] == needle[j]) {
            j++;
        }
        if (needle[j] == '\0') {
            return 1;
        }
        if (hay[i] == '\0') {
            return 0;
        }
    }
}

static unsigned int clockfacts_strlen(const char *s) {
    unsigned int n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

void hype_clockfacts_reset(hype_clockfacts_t *cf) {
    cf->buf[0] = '\0';
    cf->len = 0;
}

int hype_clockfacts_observe(hype_clockfacts_t *cf, const char *line) {
    unsigned int k, avail, sep, copy, i;
    int matched = 0;

    for (k = 0; k < HYPE_CLOCKFACTS_NKEYS; k++) {
        if (clockfacts_contains(line, HYPE_CLOCKFACTS_KEYS[k])) {
            matched = 1;
            break;
        }
    }
    if (!matched) {
        return 0;
    }
    /* Already captured this exact line? (Boot repeats some clocksource lines.) */
    if (cf->len > 0 && clockfacts_contains(cf->buf, line)) {
        return 0;
    }

    sep = (cf->len > 0) ? 3u : 0u; /* " | " between entries */
    /* Room left for text, keeping one byte for the NUL. */
    if (cf->len + sep + 1u >= HYPE_CLOCKFACTS_CAP) {
        return 0; /* not even the separator fits */
    }
    avail = HYPE_CLOCKFACTS_CAP - 1u - cf->len - sep;
    copy = clockfacts_strlen(line);
    if (copy == 0) {
        return 0;
    }
    if (copy > avail) {
        copy = avail; /* truncate to fit; never overflow */
    }

    if (sep) {
        cf->buf[cf->len++] = ' ';
        cf->buf[cf->len++] = '|';
        cf->buf[cf->len++] = ' ';
    }
    for (i = 0; i < copy; i++) {
        cf->buf[cf->len++] = line[i];
    }
    cf->buf[cf->len] = '\0';
    return 1;
}
