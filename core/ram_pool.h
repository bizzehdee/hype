#ifndef HYPE_CORE_RAM_POOL_H
#define HYPE_CORE_RAM_POOL_H

#include <stdint.h>

/*
 * RAM-3 (#449): one guest-memory pool, reserved once, carved many times.
 *
 * plan.md section 10 decision 37. AllocatePages has no post-ExitBootServices equivalent, so
 * reserving memory is the one thing that genuinely cannot move to Phase 1. Reserving it at a
 * size taken from hype.cfg is what forced the CONFIG to be read by firmware, and that is what
 * made Phase 0 large. So Phase 0 reserves one pool sized from the memory map, and Phase 1 --
 * which can read the config through hype's own storage stack -- carves it.
 *
 * Two properties this module owns, both load-bearing rather than incidental:
 *
 *  - **2 MB alignment.** hype_npt_map_range() encodes the host-physical base into PS=1 2 MB
 *    PDEs whose low 21 bits must be zero. A carve that is merely page-aligned maps a guest at
 *    the wrong address.
 *  - **No two carves overlap.** Section 6i already asserts this for cpu_set and target_disk;
 *    with one shared pool it becomes a property of the allocator, so it is tested here.
 *
 * Zeroing is the CALLER's job and section 6f's hard invariant: pool memory is reused across VM
 * restarts, so a missed zero hands a previous guest's RAM to the next one. The allocator
 * reports whether a carve is fresh or a reuse so the caller cannot skip it by accident.
 */

#define HYPE_RAM_POOL_ALIGN 0x200000ull /* 2 MB, the NPT/EPT large-page requirement */
#define HYPE_RAM_POOL_MAX_CARVES 64u

typedef enum {
    HYPE_RAM_POOL_OK = 0,
    HYPE_RAM_POOL_ERR_UNINIT = -1,     /* pool never initialised */
    HYPE_RAM_POOL_ERR_ZERO = -2,       /* a zero-byte carve is a caller bug, not a no-op */
    HYPE_RAM_POOL_ERR_EXHAUSTED = -3,  /* not enough room left; shortfall is reported */
    HYPE_RAM_POOL_ERR_TOO_MANY = -4,   /* carve table full */
    HYPE_RAM_POOL_ERR_BAD_ALIGN = -5   /* base is not 2 MB aligned */
} hype_ram_pool_status_t;

typedef struct {
    uint64_t base;
    uint64_t size;
    unsigned int owner;  /* VM index, or HYPE_RAM_POOL_NO_OWNER */
    unsigned int kind;   /* caller-defined tag: guest RAM, firmware, vdisk, ... */
} hype_ram_carve_t;

#define HYPE_RAM_POOL_NO_OWNER 0xFFFFFFFFu

typedef struct {
    uint64_t base;
    uint64_t size;
    uint64_t cursor; /* bump position, always 2 MB aligned */
    unsigned int carve_count;
    hype_ram_carve_t carves[HYPE_RAM_POOL_MAX_CARVES];
} hype_ram_pool_t;

/* `base` must be 2 MB aligned; `size` is rounded DOWN to a 2 MB multiple. */
hype_ram_pool_status_t hype_ram_pool_init(hype_ram_pool_t *p, uint64_t base, uint64_t size);

/*
 * Carve `bytes` (rounded up to 2 MB) for `owner`/`kind`. On success *out_base holds a 2 MB
 * aligned host-physical base. On HYPE_RAM_POOL_ERR_EXHAUSTED *out_shortfall holds how many
 * bytes were missing, so the caller can name the VM and the number (#290's diagnostic bar).
 */
hype_ram_pool_status_t hype_ram_pool_carve(hype_ram_pool_t *p, uint64_t bytes, unsigned int owner,
                                           unsigned int kind, uint64_t *out_base,
                                           uint64_t *out_shortfall);

/* The carve already made for this owner/kind, or 0. A VM restart re-uses its carve rather than
 * taking a second one -- and must re-zero it. */
const hype_ram_carve_t *hype_ram_pool_find(const hype_ram_pool_t *p, unsigned int owner,
                                           unsigned int kind);

uint64_t hype_ram_pool_remaining(const hype_ram_pool_t *p);
uint64_t hype_ram_pool_used(const hype_ram_pool_t *p);

/* 1 if any two carves overlap. Always 0 by construction; exists so the property is TESTED
 * rather than assumed, and so a future non-bump allocator cannot quietly break it. */
int hype_ram_pool_any_overlap(const hype_ram_pool_t *p);

/* 1 if [base,base+bytes) lies wholly inside the pool and inside one carve owned by `owner`.
 * The check a caller makes before handing a range to a guest. */
int hype_ram_pool_range_is_owned(const hype_ram_pool_t *p, uint64_t base, uint64_t bytes,
                                 unsigned int owner);

const char *hype_ram_pool_status_str(hype_ram_pool_status_t st);

#endif /* HYPE_CORE_RAM_POOL_H */
