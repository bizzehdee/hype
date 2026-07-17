#include "logbuf.h"

static char g_logbuf[HYPE_LOGBUF_CAPACITY];
static unsigned int g_logbuf_len = 0;
static int g_logbuf_truncated = 0;

void hype_logbuf_reset(void) {
    g_logbuf_len = 0;
    g_logbuf_truncated = 0;
}

void hype_logbuf_append(const char *s) {
    if (s == 0) {
        return;
    }
    while (*s != '\0') {
        if (g_logbuf_len >= HYPE_LOGBUF_CAPACITY) {
            g_logbuf_truncated = 1;
            return;
        }
        g_logbuf[g_logbuf_len++] = *s++;
    }
}

const char *hype_logbuf_data(void) {
    return g_logbuf;
}

unsigned int hype_logbuf_len(void) {
    return g_logbuf_len;
}

int hype_logbuf_truncated(void) {
    return g_logbuf_truncated;
}
