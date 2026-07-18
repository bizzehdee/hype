#include "logbuf.h"

/* One contiguous, page-aligned region: magic header immediately ahead of
 * the data, so a scanner (hype_logbuf_find) locates the magic at a fixed
 * offset before the bytes. Page alignment (RT-1d) puts the magic on a 4 KB
 * boundary in physical RAM -- UEFI loads the image page-aligned -- so the
 * RT-1b next-boot scan can step by HYPE_LOGBUF_SCAN_ALIGN instead of 8
 * bytes. Lives in BSS (zero at load); hype_logbuf_reset() stamps the magic
 * before any logging -- see efi_main. */
static hype_logbuf_t g_logbuf __attribute__((aligned(HYPE_LOGBUF_SCAN_ALIGN)));

void hype_logbuf_reset(void) {
    g_logbuf.magic = HYPE_LOGBUF_MAGIC;
    g_logbuf.version = HYPE_LOGBUF_VERSION;
    g_logbuf.len = 0;
    g_logbuf.truncated = 0;
    g_logbuf.checksum = 0;
}

void hype_logbuf_append(const char *s) {
    if (s == 0) {
        return;
    }
    while (*s != '\0') {
        if (g_logbuf.len >= HYPE_LOGBUF_CAPACITY) {
            g_logbuf.truncated = 1;
            return;
        }
        g_logbuf.data[g_logbuf.len++] = *s;
        g_logbuf.checksum += (unsigned char)*s;
        s++;
    }
}

const char *hype_logbuf_data(void) {
    return g_logbuf.data;
}

unsigned int hype_logbuf_len(void) {
    return g_logbuf.len;
}

int hype_logbuf_truncated(void) {
    return g_logbuf.truncated;
}

const hype_logbuf_t *hype_logbuf_get(void) {
    return &g_logbuf;
}

int hype_logbuf_validate(const hype_logbuf_t *hdr) {
    uint32_t sum = 0;
    uint32_t i;

    if (hdr == 0) {
        return 0;
    }
    if (hdr->magic != HYPE_LOGBUF_MAGIC || hdr->version != HYPE_LOGBUF_VERSION) {
        return 0;
    }
    if (hdr->len > HYPE_LOGBUF_CAPACITY) {
        return 0;
    }
    for (i = 0; i < hdr->len; i++) {
        sum += (unsigned char)hdr->data[i];
    }
    return (sum == hdr->checksum) ? 1 : 0;
}

const hype_logbuf_t *hype_logbuf_find(const void *base, unsigned long size, unsigned long stride) {
    const unsigned char *p;
    unsigned long off;
    /* The header must fit before we can even read its fields: magic(8) +
     * version(4) + len(4) + truncated(4) + checksum(4). */
    const unsigned long header_prefix = 8u + 4u + 4u + 4u + 4u;

    /* 8 is the minimum at which the 8-byte magic is readable; a caller
     * asking for less is clamped up rather than reading misaligned/OOB. */
    if (stride < 8u) {
        stride = 8u;
    }
    if (base == 0 || size < header_prefix) {
        return 0;
    }
    p = (const unsigned char *)base;
    /* The magic only ever starts on a `stride` boundary relative to `base`
     * (the buffer is page-aligned and `base` is a page-aligned RAM region),
     * so stepping by `stride` keeps a large real-RAM sweep cheap. */
    for (off = 0; off + header_prefix <= size; off += stride) {
        const hype_logbuf_t *cand = (const hype_logbuf_t *)(const void *)(p + off);
        if (cand->magic != HYPE_LOGBUF_MAGIC) {
            continue;
        }
        /* Magic matched -- ensure the claimed data actually fits inside the
         * scanned region before validating (never read past [base,base+size)). */
        if (cand->len > HYPE_LOGBUF_CAPACITY) {
            continue;
        }
        if (off + header_prefix + (unsigned long)cand->len > size) {
            continue;
        }
        if (hype_logbuf_validate(cand)) {
            return cand;
        }
    }
    return 0;
}
