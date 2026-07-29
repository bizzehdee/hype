#include "ext.h"

/*
 * #204: in-place writes to a resolved ext-hosted backing file. See ext.h for
 * the scope and the journal reasoning. Everything here is arithmetic over the
 * extents #203's resolver produced -- no ext metadata is ever written.
 */

#define SECSZ HYPE_FAT_SECTOR_SIZE

/* Superblock fields checked beyond what the resolver already validates. */
#define SB_STATE 0x3Au
#define STATE_VALID 0x0001u /* cleanly unmounted */
#define STATE_ERROR 0x0002u /* the filesystem recorded errors */

static void bcopy(uint8_t *dst, const uint8_t *src, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

int hype_ext_open_rw(hype_fat_read_fn read, hype_fat_write_fn write, void *ctx,
                     const char *path, hype_ext_wfile_t *out) {
    uint8_t sb[1024];
    uint16_t state;

    if (write == 0) {
        return -1;
    }
    /*
     * The resolver validates the superblock's shape; the WRITE gate is the
     * unmount state. A volume that was not cleanly unmounted may have newer
     * metadata sitting in its (unreplayed) journal or simply be inconsistent
     * -- resolving a path through it could map the wrong blocks, and writing
     * through a wrong map corrupts someone's filesystem. Refuse outright;
     * the fix is an fsck/mount cycle on a real OS, not hype guessing.
     */
    if (read(ctx, 2u, 2u, sb) != 0) {
        return -1;
    }
    state = (uint16_t)((uint16_t)sb[SB_STATE] | ((uint16_t)sb[SB_STATE + 1u] << 8));
    if ((state & STATE_VALID) == 0u || (state & STATE_ERROR) != 0u) {
        return -1;
    }
    if (hype_ext_resolve(read, ctx, path, &out->map) != 0) {
        return -1;
    }
    out->read = read;
    out->write = write;
    out->ctx = ctx;
    return 0;
}

/*
 * Maps file-relative sector `fsec` to its volume LBA and reports how many
 * sectors remain contiguous from there in the same extent.
 */
static int locate(const hype_ext_wfile_t *f, uint64_t fsec, uint64_t *out_lba, uint64_t *out_run) {
    uint64_t before = 0;
    unsigned int x;
    for (x = 0; x < f->map.count; x++) {
        if (fsec < before + f->map.extents[x].sector_count) {
            uint64_t into = fsec - before;
            *out_lba = f->map.extents[x].start_lba + into;
            *out_run = f->map.extents[x].sector_count - into;
            return 0;
        }
        before += f->map.extents[x].sector_count;
    }
    return -1; /* past the mapped extents (cannot happen inside size_bytes) */
}

/*
 * Shared body of read_at / write_at. Exactly one of `rbuf` (read into) and
 * `wbuf` (write from) is non-NULL. Ragged head/tail sectors read-modify-write;
 * whole aligned sectors move in one callback call per contiguous run.
 */
static int rw_at(hype_ext_wfile_t *f, uint64_t offset, uint8_t *rbuf, const uint8_t *wbuf,
                 unsigned int len) {
    int writing = (wbuf != 0);

    if (len == 0u) {
        return 0;
    }
    /* The whole range must already exist: this path never grows a file, and an
     * unchecked offset+len is exactly the guest-supplied-length class of bug
     * AGENTS.md forbids. */
    if (offset > f->map.size_bytes || (uint64_t)len > f->map.size_bytes - offset) {
        return -1;
    }
    while (len > 0u) {
        uint64_t lba = 0;
        uint64_t run = 0;
        unsigned int bis = (unsigned int)(offset % SECSZ);
        if (locate(f, offset / SECSZ, &lba, &run) != 0) {
            return -1;
        }
        if (bis != 0u || len < SECSZ) {
            /* A ragged edge: read-modify-write one sector. */
            uint8_t sec[SECSZ];
            unsigned int n = SECSZ - bis;
            if (n > len) {
                n = len;
            }
            if (f->read(f->ctx, lba, 1u, sec) != 0) {
                return -1;
            }
            if (writing) {
                bcopy(sec + bis, wbuf, n);
                if (f->write(f->ctx, lba, 1u, sec) != 0) {
                    return -1;
                }
                wbuf += n;
            } else {
                bcopy(rbuf, sec + bis, n);
                rbuf += n;
            }
            offset += n;
            len -= n;
        } else {
            /* Whole aligned sectors: one bulk transfer per contiguous run. */
            uint64_t nsec = len / SECSZ;
            if (nsec > run) {
                nsec = run;
            }
            if (writing) {
                if (f->write(f->ctx, lba, (uint32_t)nsec, wbuf) != 0) {
                    return -1;
                }
                wbuf += nsec * SECSZ;
            } else {
                if (f->read(f->ctx, lba, (uint32_t)nsec, rbuf) != 0) {
                    return -1;
                }
                rbuf += nsec * SECSZ;
            }
            offset += nsec * SECSZ;
            len -= (unsigned int)(nsec * SECSZ);
        }
    }
    return 0;
}

int hype_ext_write_at(hype_ext_wfile_t *f, uint64_t offset, const void *data, unsigned int len) {
    if (data == 0 || f->write == 0) {
        return -1;
    }
    return rw_at(f, offset, 0, (const uint8_t *)data, len);
}

int hype_ext_read_at(hype_ext_wfile_t *f, uint64_t offset, void *out, unsigned int len) {
    if (out == 0) {
        return -1;
    }
    return rw_at(f, offset, (uint8_t *)out, 0, len);
}
