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
    g_logbuf.reclaimed = 0;
}

/*
 * #338: the append lock.
 *
 * g_logbuf.len was advanced with no synchronisation, so two cores appending at once did not merely
 * interleave -- they LOST BYTES: both read the same len, both wrote there, one won. That silently
 * corrupts \HYPE.LOG and the per-VM USB logs, which are the persistent artefacts
 * that exist precisely for serial-less real-hardware debugging, where there is no second copy.
 *
 * A whole record is appended under one acquisition so it cannot be split by another core. This is a
 * plain test-and-set spinlock: the critical section is a short memcpy-ish loop and there is nothing
 * to sleep on in a hypervisor's log path.
 */
static volatile int g_logbuf_lock;

static void logbuf_lock(void) {
    while (__atomic_exchange_n(&g_logbuf_lock, 1, __ATOMIC_ACQUIRE) != 0) {
        __builtin_ia32_pause();
    }
}

static void logbuf_unlock(void) {
    __atomic_store_n(&g_logbuf_lock, 0, __ATOMIC_RELEASE);
}

/*
 * #338: for the panic path. hype_fatal() may run on a core that already holds the lock (it can be
 * called FROM inside a logging call, or from a fault taken there), and blocking would turn a readable
 * panic into a silent hang -- the exact failure core/halt.c already carries scar tissue about. So the
 * panic path takes the lock if it can and writes anyway if it cannot: a torn panic message is still
 * infinitely more useful than no panic message.
 */
void hype_logbuf_append_unlocked(const char *s) {
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

void hype_logbuf_lock(void) {
    logbuf_lock();
}

int hype_logbuf_try_lock(void) {
    return __atomic_exchange_n(&g_logbuf_lock, 1, __ATOMIC_ACQUIRE) == 0;
}

void hype_logbuf_unlock(void) {
    logbuf_unlock();
}

void hype_logbuf_append(const char *s) {
    logbuf_lock();
    hype_logbuf_append_unlocked(s);
    logbuf_unlock();
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

/*
 * #585: drop the already-written prefix and slide the rest down.
 *
 * The checksum is maintained by SUBTRACTING the departing bytes rather than recomputed over what
 * remains. Recomputing would walk up to 8 MiB on every reclaim, on the BSP, in the middle of the
 * drain loop that also renders the dashboard -- and the whole reason this exists is a run long
 * enough for that to happen thousands of times. Subtraction is exact for a rolling sum.
 *
 * `truncated` is deliberately NOT cleared. It means "output was lost", and reclaiming does not
 * un-lose it: a run that hit capacity before any sink existed still produced an incomplete log, and
 * a later reclaim must not make that look fine.
 */
unsigned int hype_logbuf_reclaim_unlocked(unsigned int upto) {
    unsigned int i;

    if (upto > g_logbuf.len) {
        upto = g_logbuf.len;
    }
    if (upto == 0u) {
        return 0u;
    }
    for (i = 0; i < upto; i++) {
        g_logbuf.checksum -= (unsigned char)g_logbuf.data[i];
    }
    /* Forward copy is correct for a downward move even when the ranges overlap: the destination
     * index is always below the source, so a byte is read before anything writes over it. */
    for (i = upto; i < g_logbuf.len; i++) {
        g_logbuf.data[i - upto] = g_logbuf.data[i];
    }
    g_logbuf.len -= upto;
    g_logbuf.reclaimed += (uint64_t)upto;
    return upto;
}

uint64_t hype_logbuf_reclaimed(void) {
    return g_logbuf.reclaimed;
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
    /*
     * Bytes ahead of data[], taken from the struct rather than added up by hand. The hand-written
     * sum said 8+4+4+4+4 and #585 added a uint64_t to the header, so it would have understated the
     * offset by 8 (plus padding) and let the scan validate a candidate whose data ran past the
     * region it had checked. A layout fact belongs to the compiler.
     */
    const unsigned long header_prefix = (unsigned long)__builtin_offsetof(hype_logbuf_t, data);

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
