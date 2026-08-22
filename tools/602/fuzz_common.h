#ifndef HYPE_TOOLS_602_FUZZ_COMMON_H
#define HYPE_TOOLS_602_FUZZ_COMMON_H

#include <stdint.h>
#include <string.h>

/*
 * #602: a tiny byte-cursor shared by every libFuzzer harness in this directory.
 *
 * Every harness turns one opaque `(data, size)` blob into a sequence of typed
 * fields (register offsets, sizes, values, raw buffer contents). A cursor that
 * degrades gracefully past the end of the input -- returning 0 rather than
 * reading out of bounds -- means a truncated/mutated input still drives SOME
 * call into the device model instead of being thrown away by the harness
 * itself, which is what libFuzzer's coverage feedback needs to make progress.
 */
typedef struct {
    const uint8_t *p;
    size_t left;
} hype_fuzz_cursor_t;

static inline void hype_fuzz_cursor_init(hype_fuzz_cursor_t *c, const uint8_t *data, size_t size) {
    c->p = data;
    c->left = size;
}

static inline size_t hype_fuzz_cursor_remaining(const hype_fuzz_cursor_t *c) {
    return c->left;
}

static inline uint8_t hype_fuzz_u8(hype_fuzz_cursor_t *c) {
    uint8_t v;
    if (c->left == 0) return 0;
    v = c->p[0];
    c->p++;
    c->left--;
    return v;
}

static inline uint16_t hype_fuzz_u16(hype_fuzz_cursor_t *c) {
    uint16_t v = (uint16_t)hype_fuzz_u8(c);
    v |= (uint16_t)hype_fuzz_u8(c) << 8;
    return v;
}

static inline uint32_t hype_fuzz_u32(hype_fuzz_cursor_t *c) {
    uint32_t v = (uint32_t)hype_fuzz_u16(c);
    v |= (uint32_t)hype_fuzz_u16(c) << 16;
    return v;
}

static inline uint64_t hype_fuzz_u64(hype_fuzz_cursor_t *c) {
    uint64_t v = (uint64_t)hype_fuzz_u32(c);
    v |= (uint64_t)hype_fuzz_u32(c) << 32;
    return v;
}

/* Copies up to `dst_len` bytes of whatever the input has left into `dst`,
 * zero-filling the rest. Never reads past the input, never writes past
 * `dst_len` -- the guest-RAM backing buffers this fills are fixed-size
 * statics, and short/empty input must still produce a deterministic buffer. */
static inline void hype_fuzz_fill(hype_fuzz_cursor_t *c, uint8_t *dst, size_t dst_len) {
    size_t n = c->left < dst_len ? c->left : dst_len;
    if (n != 0) {
        memcpy(dst, c->p, n);
        c->p += n;
        c->left -= n;
    }
    if (n < dst_len) {
        memset(dst + n, 0, dst_len - n);
    }
}

#endif /* HYPE_TOOLS_602_FUZZ_COMMON_H */
