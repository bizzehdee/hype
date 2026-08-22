#include "file_range.h"

#define SECSZ ((uint64_t)HYPE_BLK_SECTOR_SIZE)

static void bzero8(uint8_t *p, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        p[i] = 0;
    }
}

static void bcopy8(uint8_t *dst, const uint8_t *src, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static int kind_valid(uint32_t kind) {
    return kind == HYPE_RANGE_DATA || kind == HYPE_RANGE_HOLE || kind == HYPE_RANGE_UNWRITTEN;
}

void hype_file_rmap_init(hype_file_rmap_t *m, uint64_t size_bytes) {
    m->count = 0;
    m->size_bytes = size_bytes;
    m->too_fragmented = 0;
}

int hype_file_rmap_append(hype_file_rmap_t *m, hype_range_kind_t kind, uint64_t start_lba,
                          uint64_t sector_count) {
    hype_file_range_t *prev;

    if (!kind_valid((uint32_t)kind) || sector_count == 0) {
        return -1;
    }
    if (kind == HYPE_RANGE_HOLE) {
        start_lba = 0;
    } else if (start_lba + sector_count < start_lba) {
        return -1; /* LBA run wraps the 64-bit space: never a real allocation */
    }

    if (m->count > 0) {
        prev = &m->ranges[m->count - 1];
        if (prev->kind == (uint32_t)kind &&
            (kind == HYPE_RANGE_HOLE || prev->start_lba + prev->sector_count == start_lba)) {
            if (prev->sector_count + sector_count < prev->sector_count) {
                return -1;
            }
            prev->sector_count += sector_count;
            return 0;
        }
    }

    if (m->count >= HYPE_FILE_MAX_RANGES) {
        m->too_fragmented = 1;
        return -1;
    }
    m->ranges[m->count].kind = (uint32_t)kind;
    m->ranges[m->count].start_lba = start_lba;
    m->ranges[m->count].sector_count = sector_count;
    m->count++;
    return 0;
}

int hype_file_rmap_validate(const hype_file_rmap_t *m, uint64_t media_sectors) {
    uint64_t covered = 0;
    uint64_t need_sectors;
    unsigned i;

    if (m->count > HYPE_FILE_MAX_RANGES) {
        return -1;
    }
    for (i = 0; i < m->count; i++) {
        const hype_file_range_t *r = &m->ranges[i];
        if (!kind_valid(r->kind) || r->sector_count == 0) {
            return -1;
        }
        if (r->kind != HYPE_RANGE_HOLE) {
            if (r->start_lba + r->sector_count < r->start_lba) {
                return -1; /* overflow */
            }
            if (r->start_lba + r->sector_count > media_sectors) {
                return -1; /* allocation past the end of the medium */
            }
        }
        if (covered + r->sector_count < covered) {
            return -1; /* logical size overflows */
        }
        covered += r->sector_count;
    }

    /* Exact coverage: enough sectors for every byte, and no whole sector
     * beyond the last byte -- a longer map hides either corruption (FAT) or a
     * producer bug, and a shorter one loses bytes. */
    need_sectors = (m->size_bytes + SECSZ - 1) / SECSZ;
    if (covered != need_sectors) {
        return -1;
    }
    return 0;
}

int hype_file_rmap_from_extents(const hype_file_map_t *src, hype_file_rmap_t *out) {
    uint64_t covered = 0;
    unsigned i;

    hype_file_rmap_init(out, src->size_bytes);
    out->too_fragmented = src->too_fragmented;
    if (src->count > HYPE_FILE_MAX_EXTENTS) {
        return -1;
    }
    for (i = 0; i < src->count; i++) {
        if (hype_file_rmap_append(out, HYPE_RANGE_DATA, src->extents[i].start_lba,
                                  src->extents[i].sector_count) != 0) {
            return -1;
        }
        covered += src->extents[i].sector_count;
    }
    /* A physical map that does not reach size_bytes is a malformed allocation
     * chain (FAT32/exFAT can never be sparse) -- refuse, never pad with HOLE. */
    if (covered * SECSZ < src->size_bytes) {
        return -1;
    }
    return 0;
}

int hype_file_map_from_rmap(const hype_file_rmap_t *src, hype_file_map_t *out) {
    unsigned i;

    out->count = 0;
    out->size_bytes = src->size_bytes;
    out->too_fragmented = src->too_fragmented;
    if (src->count > HYPE_FILE_MAX_RANGES) {
        return -1;
    }
    for (i = 0; i < src->count; i++) {
        const hype_file_range_t *r = &src->ranges[i];
        if (r->kind != HYPE_RANGE_DATA) {
            return -1;
        }
        if (out->count >= HYPE_FILE_MAX_EXTENTS) {
            out->too_fragmented = 1;
            return -1;
        }
        out->extents[out->count].start_lba = r->start_lba;
        out->extents[out->count].sector_count = r->sector_count;
        out->count++;
    }
    return 0;
}

int hype_file_rmap_locate(const hype_file_rmap_t *m, uint64_t off, hype_range_kind_t *out_kind,
                          uint64_t *out_lba, uint32_t *out_head, uint64_t *out_run) {
    uint64_t base = 0; /* logical byte offset where the current range starts */
    unsigned i;

    if (off >= m->size_bytes) {
        return -1;
    }
    for (i = 0; i < m->count; i++) {
        const hype_file_range_t *r = &m->ranges[i];
        uint64_t rbytes = r->sector_count * SECSZ;
        if (rbytes / SECSZ != r->sector_count || base + rbytes < base) {
            return -1; /* malformed map: byte size overflows */
        }
        if (off < base + rbytes) {
            uint64_t into = off - base; /* byte offset into this range */
            uint64_t run = rbytes - into;
            if (base + rbytes > m->size_bytes) {
                run = m->size_bytes - off; /* final range: cap at the exact size */
            }
            *out_kind = (hype_range_kind_t)r->kind;
            *out_lba = (r->kind == HYPE_RANGE_HOLE) ? 0 : r->start_lba + into / SECSZ;
            *out_head = (uint32_t)(into % SECSZ);
            *out_run = run;
            return 0;
        }
        base += rbytes;
    }
    return -1; /* off < size_bytes but the ranges do not cover it: malformed */
}

int hype_file_rmap_read_at(const hype_file_rmap_t *m, hype_blk_read_fn read, void *ctx,
                           uint64_t offset, void *dst, unsigned int len) {
    uint8_t *out = (uint8_t *)dst;
    uint8_t sec[HYPE_BLK_SECTOR_SIZE];

    if (offset + len < offset || offset + len > m->size_bytes) {
        return -1; /* refused, not clamped (AGENTS.md bounds rule) */
    }
    if (read == 0 && len > 0) {
        /* A pure-hole read needs no medium, but refusing a NULL callback up
         * front beats failing only when a DATA range is eventually hit. */
        return -1;
    }

    while (len > 0) {
        hype_range_kind_t kind;
        uint64_t lba, run;
        uint32_t head;
        unsigned int n;

        if (hype_file_rmap_locate(m, offset, &kind, &lba, &head, &run) != 0) {
            return -1;
        }
        n = (run < (uint64_t)len) ? (unsigned int)run : len;

        if (kind != HYPE_RANGE_DATA) {
            bzero8(out, n);
        } else if (head == 0 && n >= HYPE_BLK_SECTOR_SIZE) {
            /* Bulk: whole aligned sectors in one transfer. */
            uint32_t whole = (uint32_t)(n / HYPE_BLK_SECTOR_SIZE);
            if (read(ctx, lba, whole, out) != 0) {
                return -1;
            }
            n = whole * HYPE_BLK_SECTOR_SIZE;
        } else {
            /* Ragged head or short tail: bounce one sector. */
            if (read(ctx, lba, 1, sec) != 0) {
                return -1;
            }
            if ((uint64_t)head + n > SECSZ) {
                n = (unsigned int)(SECSZ - head);
            }
            bcopy8(out, sec + head, n);
        }

        out += n;
        offset += n;
        len -= n;
    }
    return 0;
}

int hype_file_rmap_write_at(const hype_file_rmap_t *m, hype_blk_read_fn read,
                            hype_blk_write_fn write, void *ctx, uint64_t offset,
                            const void *src, unsigned int len) {
    const uint8_t *in = (const uint8_t *)src;
    uint8_t sec[HYPE_BLK_SECTOR_SIZE];
    uint64_t probe, remaining;

    if (len == 0) {
        return 0;
    }
    if (offset + len < offset || offset + len > m->size_bytes || read == 0 || write == 0) {
        return -1;
    }

    /* Validate the WHOLE span is DATA before writing anything. */
    probe = offset;
    remaining = len;
    while (remaining > 0) {
        hype_range_kind_t kind;
        uint64_t lba, run;
        uint32_t head;
        if (hype_file_rmap_locate(m, probe, &kind, &lba, &head, &run) != 0) {
            return -1;
        }
        if (kind != HYPE_RANGE_DATA) {
            return -1; /* a HOLE needs allocation; UNWRITTEN needs a VDL advance */
        }
        if (run >= remaining) {
            break;
        }
        probe += run;
        remaining -= run;
    }

    while (len > 0) {
        hype_range_kind_t kind;
        uint64_t lba, run;
        uint32_t head;
        unsigned int n;

        if (hype_file_rmap_locate(m, offset, &kind, &lba, &head, &run) != 0 ||
            kind != HYPE_RANGE_DATA) {
            return -1;
        }
        n = (run < (uint64_t)len) ? (unsigned int)run : len;
        if (head == 0 && n >= HYPE_BLK_SECTOR_SIZE) {
            uint32_t whole = (uint32_t)(n / HYPE_BLK_SECTOR_SIZE);
            if (write(ctx, lba, whole, in) != 0) {
                return -1;
            }
            n = whole * HYPE_BLK_SECTOR_SIZE;
        } else {
            if (read(ctx, lba, 1, sec) != 0) {
                return -1;
            }
            if ((uint64_t)head + n > SECSZ) {
                n = (unsigned int)(SECSZ - head);
            }
            bcopy8(sec + head, in, n);
            if (write(ctx, lba, 1, sec) != 0) {
                return -1;
            }
        }
        in += n;
        offset += n;
        len -= n;
    }
    return 0;
}
