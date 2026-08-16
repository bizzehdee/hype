#include "gop_mode.h"
#include "strutil.h"

int hype_gop_mode_find(const hype_gop_mode_t *modes, unsigned int count, uint32_t width,
                       uint32_t height) {
    unsigned int i;
    for (i = 0; i < count; i++) {
        if (modes[i].width == width && modes[i].height == height) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * #465: parse "<W>x<H>".
 *
 * This lived inline in boot/main.c and was wrong in a way no amount of reading caught: it found
 * the 'x', then handed hype_parse_uint() the WHOLE string for the width. That function requires
 * the entire string to be digits (strutil.h), so "1920x1080" failed at the 'x' every time and
 * the command answered "expected <W>x<H>" to input that was already exactly <W>x<H>. Every
 * WxH ever typed was rejected.
 *
 * Pure and here rather than inline there, so it is unit-testable -- which is the only reason
 * the bug survived: nothing exercised it.
 */
int hype_gop_mode_parse_wxh(const char *s, uint32_t *out_width, uint32_t *out_height) {
    unsigned int i, sep = 0;
    unsigned long long w = 0, h = 0;
    char wbuf[16];

    if (s == 0 || out_width == 0 || out_height == 0) {
        return -1;
    }
    while (s[sep] != '\0' && s[sep] != 'x' && s[sep] != 'X') {
        sep++;
    }
    /* No separator, nothing before it, or a width too long to be a real resolution. */
    if (s[sep] == '\0' || sep == 0u || sep >= sizeof(wbuf)) {
        return -1;
    }
    for (i = 0; i < sep; i++) {
        wbuf[i] = s[i];
    }
    wbuf[sep] = '\0';

    if (hype_parse_uint(wbuf, &w) != 0 || hype_parse_uint(s + sep + 1u, &h) != 0) {
        return -1;
    }
    /* Reject 0 and anything a GOP mode field cannot hold, so callers never see a truncated
     * value that silently matches the wrong mode. */
    if (w == 0ull || h == 0ull || w > 0xFFFFFFFFull || h > 0xFFFFFFFFull) {
        return -1;
    }
    *out_width = (uint32_t)w;
    *out_height = (uint32_t)h;
    return 0;
}
