/*
 * fw_cfg access from inside a guest: ports, the file directory, and both transfer paths.
 *
 * Extracted from tests/micro/fwcfg.c when ramfb.c needed the same directory walk and the DMA
 * engine in the other direction (#549). Two callers, so this is not a speculative abstraction --
 * and the wire details (big-endian everything, the stage-then-trigger port pair, the 64-byte
 * directory entry) are exactly the kind of thing that goes subtly wrong twice if written twice.
 *
 * Header-only and `static`, because each microtest links as its own freestanding binary and the
 * suite build compiles each test twice.
 */
#ifndef MICRO_FWCFG_H
#define MICRO_FWCFG_H

#include "micro.h"

#define FW_CFG_PORT_SEL 0x510u
#define FW_CFG_PORT_DATA 0x511u
#define FW_CFG_PORT_DMA_HI 0x514u
#define FW_CFG_PORT_DMA_LO 0x518u

#define FW_CFG_KEY_SIGNATURE 0x0000u
#define FW_CFG_KEY_ID 0x0001u
#define FW_CFG_KEY_FILE_DIR 0x0019u

#define FW_CFG_DMA_CTL_ERROR (1u << 0)
#define FW_CFG_DMA_CTL_READ (1u << 1)
#define FW_CFG_DMA_CTL_SKIP (1u << 2)
#define FW_CFG_DMA_CTL_SELECT (1u << 3)
#define FW_CFG_DMA_CTL_WRITE (1u << 4)

#define FW_CFG_FILE_NAME_MAX 56u
#define FW_CFG_MAX_FILES 12u

/* Scratch, well clear of the payload at 16 MB and of the low-memory layout. */
#define FW_DMA_STRUCT_GPA 0x300000ull /* the 16-byte access struct */

static inline void fw_outw(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline void fw_outl(uint16_t port, uint32_t v) {
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t fw_bswap32(uint32_t v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}
static inline uint64_t fw_bswap64(uint64_t v) {
    return ((uint64_t)fw_bswap32((uint32_t)v) << 32) | (uint64_t)fw_bswap32((uint32_t)(v >> 32));
}

static inline void fw_select(uint16_t key) { fw_outw(FW_CFG_PORT_SEL, key); }

/* Classic interface: select once, then read consecutive bytes from the data port. */
static inline void fw_read_pio(uint16_t key, uint8_t *dst, uint32_t len) {
    uint32_t i;
    fw_select(key);
    for (i = 0; i < len; i++) {
        dst[i] = micro_inb(FW_CFG_PORT_DATA);
    }
}

/*
 * The DMA engine, both directions. The access struct is {control BE32, length BE32, address
 * BE64}; writing the high half of its address to 0x514 stages, and writing the low half to 0x518
 * triggers. The device clears Control on completion, which the guest polls as an ordinary memory
 * read -- so this also checks that hype's write to the struct is visible to the guest, the
 * coherency question that kept the fw_cfg test off VMX in its in-binary form.
 *
 * `name` appears in failure text so a caller's verdict names itself.
 */
static inline int fw_dma(const char *name, uint16_t key, uint64_t gpa, uint32_t len, int write) {
    volatile uint32_t *ctl = (volatile uint32_t *)(uintptr_t)FW_DMA_STRUCT_GPA;
    volatile uint32_t *length = (volatile uint32_t *)(uintptr_t)(FW_DMA_STRUCT_GPA + 4ull);
    volatile uint64_t *addr = (volatile uint64_t *)(uintptr_t)(FW_DMA_STRUCT_GPA + 8ull);
    uint32_t control = (write ? FW_CFG_DMA_CTL_WRITE : FW_CFG_DMA_CTL_READ) |
                       FW_CFG_DMA_CTL_SELECT | ((uint32_t)key << 16);
    unsigned long long spins = 0;

    *ctl = fw_bswap32(control);
    *length = fw_bswap32(len);
    *addr = fw_bswap64(gpa);

    fw_outl(FW_CFG_PORT_DMA_HI, fw_bswap32((uint32_t)(FW_DMA_STRUCT_GPA >> 32)));
    fw_outl(FW_CFG_PORT_DMA_LO, fw_bswap32((uint32_t)FW_DMA_STRUCT_GPA));

    /* Bounded. An unbounded poll on a transfer that never completes is a wedge with no verdict,
     * which reports as a missing verdict -- true, but far less useful than naming the stall. */
    while (*ctl != 0u) {
        if (++spins > 50000000ull) {
            micro_puts("micro/");
            micro_puts(name);
            micro_puts(": DMA control never cleared, last value ");
            micro_put_hex(fw_bswap32(*ctl));
            micro_puts("\n");
            return -1;
        }
        if ((fw_bswap32(*ctl) & FW_CFG_DMA_CTL_ERROR) != 0u) {
            micro_puts("micro/");
            micro_puts(name);
            micro_puts(": DMA reported an error in Control\n");
            return -1;
        }
    }
    return 0;
}

static inline int fw_read_dma(const char *name, uint16_t key, uint64_t dest_gpa, uint32_t len) {
    return fw_dma(name, key, dest_gpa, len, 0);
}
static inline int fw_write_dma(const char *name, uint16_t key, uint64_t src_gpa, uint32_t len) {
    return fw_dma(name, key, src_gpa, len, 1);
}

typedef struct {
    uint32_t size;
    uint16_t select;
    char name[FW_CFG_FILE_NAME_MAX + 1];
} fw_file_t;

static fw_file_t g_fw_files[FW_CFG_MAX_FILES];
static unsigned g_fw_file_count;

static inline int fw_name_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/*
 * Read the file directory: a big-endian count, then 64-byte entries of
 * {size BE32, select BE16, reserved BE16, name[56]}. Returns how many were recorded, capped at
 * FW_CFG_MAX_FILES -- the cap is reported by the caller rather than silently truncating a walk.
 */
static inline unsigned fw_read_dir(uint32_t *out_total) {
    uint8_t hdr[4];
    uint32_t count, i;

    g_fw_file_count = 0;
    fw_select(FW_CFG_KEY_FILE_DIR);
    hdr[0] = micro_inb(FW_CFG_PORT_DATA);
    hdr[1] = micro_inb(FW_CFG_PORT_DATA);
    hdr[2] = micro_inb(FW_CFG_PORT_DATA);
    hdr[3] = micro_inb(FW_CFG_PORT_DATA);
    count = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) |
            (uint32_t)hdr[3];
    if (out_total != 0) {
        *out_total = count;
    }
    for (i = 0; i < count; i++) {
        uint8_t e[64];
        uint32_t k;
        for (k = 0; k < 64u; k++) {
            e[k] = micro_inb(FW_CFG_PORT_DATA);
        }
        if (g_fw_file_count < FW_CFG_MAX_FILES) {
            fw_file_t *f = &g_fw_files[g_fw_file_count];
            unsigned n;
            f->size = ((uint32_t)e[0] << 24) | ((uint32_t)e[1] << 16) | ((uint32_t)e[2] << 8) |
                      (uint32_t)e[3];
            f->select = (uint16_t)(((uint32_t)e[4] << 8) | (uint32_t)e[5]);
            for (n = 0; n < FW_CFG_FILE_NAME_MAX; n++) {
                f->name[n] = (char)e[8 + n];
            }
            f->name[FW_CFG_FILE_NAME_MAX] = '\0';
            g_fw_file_count++;
        }
    }
    return g_fw_file_count;
}

/* The entry named `want`, or 0. */
static inline const fw_file_t *fw_find(const char *want) {
    unsigned i;
    for (i = 0; i < g_fw_file_count; i++) {
        if (fw_name_eq(g_fw_files[i].name, want)) {
            return &g_fw_files[i];
        }
    }
    return 0;
}

#endif /* MICRO_FWCFG_H */
