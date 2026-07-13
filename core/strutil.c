#include "strutil.h"

unsigned long long hype_strlen(const char *s) {
    unsigned long long n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int hype_streq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int hype_strneq(const char *a, const char *b, unsigned long long n) {
    unsigned long long i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == '\0') {
            return 1;
        }
    }
    return 1;
}

unsigned long long hype_strlcpy(char *dst, const char *src, unsigned long long dst_size) {
    unsigned long long len = hype_strlen(src);
    unsigned long long copy_len;

    if (dst_size == 0) {
        return len;
    }

    copy_len = (len < dst_size - 1) ? len : dst_size - 1;
    {
        unsigned long long i;
        for (i = 0; i < copy_len; i++) {
            dst[i] = src[i];
        }
    }
    dst[copy_len] = '\0';
    return len;
}

int hype_is_digit(char c) {
    return c >= '0' && c <= '9';
}

int hype_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int hype_parse_uint(const char *s, unsigned long long *out) {
    unsigned long long v = 0;
    int saw_digit = 0;

    while (hype_is_space(*s)) {
        s++;
    }
    if (*s == '\0') {
        return -1;
    }
    while (hype_is_digit(*s)) {
        unsigned long long d = (unsigned long long)(*s - '0');
        if (v > (0xFFFFFFFFFFFFFFFFULL - d) / 10ULL) {
            return -1; /* overflow */
        }
        v = v * 10 + d;
        saw_digit = 1;
        s++;
    }
    while (hype_is_space(*s)) {
        s++;
    }
    if (!saw_digit || *s != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

char *hype_str_trim(char *s) {
    unsigned long long len;
    unsigned long long end;

    while (hype_is_space(*s)) {
        s++;
    }
    len = hype_strlen(s);
    end = len;
    while (end > 0 && hype_is_space(s[end - 1])) {
        end--;
    }
    s[end] = '\0';
    return s;
}
