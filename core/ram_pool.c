#include "ram_pool.h"

static uint64_t round_up_2mb(uint64_t v) {
    uint64_t r = v + (HYPE_RAM_POOL_ALIGN - 1ull);
    return r & ~(HYPE_RAM_POOL_ALIGN - 1ull);
}

static uint64_t round_down_2mb(uint64_t v) {
    return v & ~(HYPE_RAM_POOL_ALIGN - 1ull);
}

hype_ram_pool_status_t hype_ram_pool_init(hype_ram_pool_t *p, uint64_t base, uint64_t size) {
    unsigned int i;
    if (p == 0) {
        return HYPE_RAM_POOL_ERR_UNINIT;
    }
    if ((base & (HYPE_RAM_POOL_ALIGN - 1ull)) != 0ull) {
        /* Refused rather than rounded up: the caller asked firmware for an aligned block and
         * got something else, which means the reservation is not what it believes it is. */
        return HYPE_RAM_POOL_ERR_BAD_ALIGN;
    }
    p->base = base;
    p->size = round_down_2mb(size);
    p->cursor = base;
    p->carve_count = 0u;
    for (i = 0; i < HYPE_RAM_POOL_MAX_CARVES; i++) {
        p->carves[i].base = 0ull;
        p->carves[i].size = 0ull;
        p->carves[i].owner = HYPE_RAM_POOL_NO_OWNER;
        p->carves[i].kind = 0u;
    }
    return HYPE_RAM_POOL_OK;
}

hype_ram_pool_status_t hype_ram_pool_carve(hype_ram_pool_t *p, uint64_t bytes, unsigned int owner,
                                           unsigned int kind, uint64_t *out_base,
                                           uint64_t *out_shortfall) {
    uint64_t want;
    uint64_t end;
    if (out_shortfall != 0) {
        *out_shortfall = 0ull;
    }
    if (p == 0 || p->size == 0ull) {
        return HYPE_RAM_POOL_ERR_UNINIT;
    }
    if (bytes == 0ull) {
        /* A zero-byte guest is a config or caller bug. Returning a valid-looking base for it
         * would hand the next carve the same address. */
        return HYPE_RAM_POOL_ERR_ZERO;
    }
    if (p->carve_count >= HYPE_RAM_POOL_MAX_CARVES) {
        return HYPE_RAM_POOL_ERR_TOO_MANY;
    }
    want = round_up_2mb(bytes);
    end = p->base + p->size;
    if (p->cursor > end || want > end - p->cursor) {
        if (out_shortfall != 0) {
            uint64_t have = (p->cursor > end) ? 0ull : (end - p->cursor);
            *out_shortfall = want - have;
        }
        return HYPE_RAM_POOL_ERR_EXHAUSTED;
    }
    if (out_base != 0) {
        *out_base = p->cursor;
    }
    p->carves[p->carve_count].base = p->cursor;
    p->carves[p->carve_count].size = want;
    p->carves[p->carve_count].owner = owner;
    p->carves[p->carve_count].kind = kind;
    p->carve_count++;
    p->cursor += want;
    return HYPE_RAM_POOL_OK;
}

const hype_ram_carve_t *hype_ram_pool_find(const hype_ram_pool_t *p, unsigned int owner,
                                           unsigned int kind) {
    unsigned int i;
    if (p == 0) {
        return 0;
    }
    for (i = 0; i < p->carve_count; i++) {
        if (p->carves[i].owner == owner && p->carves[i].kind == kind) {
            return &p->carves[i];
        }
    }
    return 0;
}

uint64_t hype_ram_pool_remaining(const hype_ram_pool_t *p) {
    uint64_t end;
    if (p == 0 || p->size == 0ull) {
        return 0ull;
    }
    end = p->base + p->size;
    return (p->cursor >= end) ? 0ull : (end - p->cursor);
}

uint64_t hype_ram_pool_used(const hype_ram_pool_t *p) {
    if (p == 0) {
        return 0ull;
    }
    return p->cursor - p->base;
}

int hype_ram_pool_any_overlap(const hype_ram_pool_t *p) {
    unsigned int i, j;
    if (p == 0) {
        return 0;
    }
    for (i = 0; i < p->carve_count; i++) {
        for (j = i + 1u; j < p->carve_count; j++) {
            uint64_t a0 = p->carves[i].base, a1 = a0 + p->carves[i].size;
            uint64_t b0 = p->carves[j].base, b1 = b0 + p->carves[j].size;
            if (a0 < b1 && b0 < a1) {
                return 1;
            }
        }
    }
    return 0;
}

int hype_ram_pool_range_is_owned(const hype_ram_pool_t *p, uint64_t base, uint64_t bytes,
                                 unsigned int owner) {
    unsigned int i;
    if (p == 0 || bytes == 0ull) {
        return 0;
    }
    if (base < p->base || base + bytes > p->base + p->size || base + bytes < base) {
        return 0;
    }
    for (i = 0; i < p->carve_count; i++) {
        if (p->carves[i].owner != owner) {
            continue;
        }
        if (base >= p->carves[i].base &&
            base + bytes <= p->carves[i].base + p->carves[i].size) {
            return 1;
        }
    }
    return 0;
}

const char *hype_ram_pool_status_str(hype_ram_pool_status_t st) {
    switch (st) {
        case HYPE_RAM_POOL_OK: return "ok";
        case HYPE_RAM_POOL_ERR_UNINIT: return "pool not initialised";
        case HYPE_RAM_POOL_ERR_ZERO: return "zero-byte carve";
        case HYPE_RAM_POOL_ERR_EXHAUSTED: return "pool exhausted";
        case HYPE_RAM_POOL_ERR_TOO_MANY: return "too many carves";
        case HYPE_RAM_POOL_ERR_BAD_ALIGN: return "pool base is not 2 MB aligned";
    }
    return "unknown";
}
