/*
 * #543: M4-4's fw_cfg test, ported out of boot/main.c.
 *
 * fw_cfg is reached entirely through PORT I/O, so this port needs no BAR, no ECAM and no device
 * placement -- the guest just talks to 0x510/0x511/0x514/0x518. That makes it the cheapest of the
 * device ports and a good one to take before the MMIO ones.
 *
 * What it gains over the in-binary version:
 *
 *  1. It reads hype's REAL fw_cfg registry -- the files fw_1_setup_fw_cfg() publishes for an actual
 *     guest (etc/table-loader, etc/e820, the ACPI tables, SMBIOS) -- rather than a private registry
 *     the test built for itself with one file in it.
 *  2. It reads the same file BOTH WAYS, by classic PIO and by DMA, and requires the two to agree
 *     byte for byte. Neither path is trusted as an oracle for the other; a device that returns
 *     plausible-but-different bytes through its two interfaces fails, which is not something either
 *     path alone can detect.
 *  3. It walks the file directory and reports what is actually registered, so a file that silently
 *     stopped being published shows up as a missing name rather than as a guest that hangs waiting
 *     for it.
 *
 * The in-binary version was SKIPPED ON VMX, with this note: "the fw_cfg DMA writes the result into
 * guest RAM correctly (host-verified), but the GUEST then reads that buffer as 0 -- hype's write
 * isn't visible to the guest's later read." That test's buffers were static arrays inside hype's own
 * image, identity-mapped; a configured VM's RAM is a carve mapped through NPT/EPT, which is a
 * different coherency question. Whether the port passes on VMX is therefore a real result either
 * way, and is stated in the ticket rather than assumed here.
 */
#include "micro.h"

#define NAME "fwcfg"

#define FW_CFG_PORT_SEL 0x510u
#define FW_CFG_PORT_DATA 0x511u
#define FW_CFG_PORT_DMA_HI 0x514u
#define FW_CFG_PORT_DMA_LO 0x518u

#define FW_CFG_KEY_SIGNATURE 0x0000u
#define FW_CFG_KEY_ID 0x0001u
#define FW_CFG_KEY_FILE_DIR 0x0019u

#define FW_CFG_DMA_CTL_ERROR (1u << 0)
#define FW_CFG_DMA_CTL_READ (1u << 1)
#define FW_CFG_DMA_CTL_SELECT (1u << 3)

#define FILE_NAME_MAX 56u
#define MAX_FILES 8u

/* Scratch, well clear of the payload at 16 MB and of the low-memory layout. */
#define DMA_STRUCT_GPA 0x300000ull /* 3 MB -- the 16-byte access struct */
#define DMA_DEST_GPA 0x310000ull   /* the DMA destination */
#define PIO_DEST_GPA 0x320000ull   /* the classic-interface copy, for comparison */
#define SCRATCH_MAX 4096u

static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline void outl(uint16_t port, uint32_t v) {
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t bswap32(uint32_t v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}
static inline uint64_t bswap64(uint64_t v) {
    return ((uint64_t)bswap32((uint32_t)v) << 32) | (uint64_t)bswap32((uint32_t)(v >> 32));
}

static void fw_select(uint16_t key) { outw(FW_CFG_PORT_SEL, key); }

/* Classic interface: select once, then read consecutive bytes from the data port. */
static void fw_read_pio(uint16_t key, uint8_t *dst, uint32_t len) {
    uint32_t i;
    fw_select(key);
    for (i = 0; i < len; i++) {
        dst[i] = micro_inb(FW_CFG_PORT_DATA);
    }
}

/*
 * DMA interface. The access struct is {control BE32, length BE32, address BE64}; writing the high
 * half of its address to 0x514 stages, and writing the low half to 0x518 triggers. The device
 * clears Control on completion, which the guest polls as an ordinary memory read -- so this also
 * checks that hype's write to the struct is visible to the guest, which is the coherency question
 * that kept this test off VMX in its in-binary form.
 */
static int fw_read_dma(uint16_t key, uint64_t dest_gpa, uint32_t len) {
    volatile uint32_t *ctl = (volatile uint32_t *)(uintptr_t)DMA_STRUCT_GPA;
    volatile uint32_t *length = (volatile uint32_t *)(uintptr_t)(DMA_STRUCT_GPA + 4ull);
    volatile uint64_t *addr = (volatile uint64_t *)(uintptr_t)(DMA_STRUCT_GPA + 8ull);
    uint32_t control = FW_CFG_DMA_CTL_READ | FW_CFG_DMA_CTL_SELECT | ((uint32_t)key << 16);
    unsigned long long spins = 0;

    *ctl = bswap32(control);
    *length = bswap32(len);
    *addr = bswap64(dest_gpa);

    outl(FW_CFG_PORT_DMA_HI, bswap32((uint32_t)(DMA_STRUCT_GPA >> 32)));
    outl(FW_CFG_PORT_DMA_LO, bswap32((uint32_t)DMA_STRUCT_GPA));

    /* Bounded. An unbounded poll on a transfer that never completes is a wedge with no verdict,
     * which the harness would report as a missing verdict -- true, but far less useful than saying
     * the transfer did not complete. */
    while (*ctl != 0u) {
        if (++spins > 50000000ull) {
            micro_puts("micro/" NAME ": DMA control never cleared, last value ");
            micro_put_hex(bswap32(*ctl));
            micro_puts("\n");
            return -1;
        }
        if ((bswap32(*ctl) & FW_CFG_DMA_CTL_ERROR) != 0u) {
            micro_puts("micro/" NAME ": DMA reported an error in Control\n");
            return -1;
        }
    }
    return 0;
}

typedef struct {
    uint32_t size;
    uint16_t select;
    char name[FILE_NAME_MAX + 1];
} fwfile_t;

static fwfile_t g_files[MAX_FILES];
static unsigned g_file_count;

static int name_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    uint8_t sig[4];
    uint8_t idbuf[4];
    uint32_t features;
    uint8_t *dma_dst = (uint8_t *)(uintptr_t)DMA_DEST_GPA;
    uint8_t *pio_dst = (uint8_t *)(uintptr_t)PIO_DEST_GPA;

    (void)zero_page_gpa;
    micro_puts("\n");

    /* The signature is the one thing that says a fw_cfg device is there at all. */
    fw_read_pio(FW_CFG_KEY_SIGNATURE, sig, 4u);
    micro_puts("micro/" NAME ": signature '");
    {
        char s[5];
        s[0] = (char)sig[0]; s[1] = (char)sig[1]; s[2] = (char)sig[2]; s[3] = (char)sig[3];
        s[4] = '\0';
        micro_puts(s);
    }
    micro_puts("'\n");
    if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U') {
        micro_fail(NAME, "no QEMU fw_cfg signature at port 0x511");
        micro_halt();
    }

    fw_read_pio(FW_CFG_KEY_ID, idbuf, 4u);
    features = (uint32_t)idbuf[0] | ((uint32_t)idbuf[1] << 8) | ((uint32_t)idbuf[2] << 16) |
               ((uint32_t)idbuf[3] << 24);
    micro_puts("micro/" NAME ": id/features ");
    micro_put_hex(features);
    micro_puts((features & 0x2u) ? " (DMA supported)\n" : " (NO DMA)\n");
    if ((features & 0x2u) == 0u) {
        micro_fail(NAME, "the device does not advertise DMA, which hype implements");
        micro_halt();
    }

    /* The file directory: a big-endian count, then 64-byte entries. */
    {
        uint8_t hdr[4];
        uint32_t count, i;

        fw_select(FW_CFG_KEY_FILE_DIR);
        hdr[0] = micro_inb(FW_CFG_PORT_DATA);
        hdr[1] = micro_inb(FW_CFG_PORT_DATA);
        hdr[2] = micro_inb(FW_CFG_PORT_DATA);
        hdr[3] = micro_inb(FW_CFG_PORT_DATA);
        count = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) |
                (uint32_t)hdr[3];

        micro_puts("micro/" NAME ": file directory lists ");
        micro_put_uint(count);
        micro_puts(" file(s)\n");
        if (count == 0u || count > MAX_FILES) {
            micro_fail(NAME, "the file directory count is zero or beyond what hype can register");
            micro_halt();
        }
        /* The directory is one contiguous item, so the entries follow the count on the SAME
         * selection -- re-selecting would restart at the count. */
        for (i = 0; i < count; i++) {
            uint8_t e[64];
            unsigned j;
            for (j = 0; j < 64u; j++) {
                e[j] = micro_inb(FW_CFG_PORT_DATA);
            }
            g_files[i].size = ((uint32_t)e[0] << 24) | ((uint32_t)e[1] << 16) |
                              ((uint32_t)e[2] << 8) | (uint32_t)e[3];
            g_files[i].select = (uint16_t)(((uint16_t)e[4] << 8) | (uint16_t)e[5]);
            for (j = 0; j < FILE_NAME_MAX; j++) {
                g_files[i].name[j] = (char)e[8 + j];
            }
            g_files[i].name[FILE_NAME_MAX] = '\0';

            micro_puts("micro/" NAME ":   '");
            micro_puts(g_files[i].name);
            micro_puts("' select=");
            micro_put_hex(g_files[i].select);
            micro_puts(" size=");
            micro_put_uint(g_files[i].size);
            micro_puts("\n");
        }
        g_file_count = count;
    }

    /*
     * Read one real file both ways and require agreement. etc/e820 is chosen because hype builds it
     * from THIS VM's configured RAM, so its content is config-derived rather than constant -- a
     * device returning a stale or shared buffer shows up as bytes that do not match this VM.
     */
    {
        unsigned i, found = MAX_FILES;
        uint32_t len;

        for (i = 0; i < g_file_count; i++) {
            if (name_eq(g_files[i].name, "etc/e820")) {
                found = i;
                break;
            }
        }
        if (found == MAX_FILES) {
            micro_fail(NAME, "etc/e820 is not in the file directory -- hype publishes it for every "
                             "guest, so its absence is a regression, not a configuration");
            micro_halt();
        }
        len = g_files[found].size;
        if (len == 0u || len > SCRATCH_MAX) {
            micro_fail(NAME, "etc/e820 has an implausible size");
            micro_halt();
        }

        for (i = 0; i < len; i++) {
            dma_dst[i] = 0xAAu; /* so a transfer that writes nothing is not mistaken for zeros */
            pio_dst[i] = 0x55u;
        }

        fw_read_pio(g_files[found].select, pio_dst, len);
        if (fw_read_dma(g_files[found].select, DMA_DEST_GPA, len) != 0) {
            micro_fail(NAME, "the DMA read of etc/e820 did not complete");
            micro_halt();
        }

        micro_puts("micro/" NAME ": etc/e820 read ");
        micro_put_uint(len);
        micro_puts(" bytes by PIO and by DMA\n");

        for (i = 0; i < len; i++) {
            if (dma_dst[i] != pio_dst[i]) {
                micro_puts("micro/" NAME ": byte ");
                micro_put_uint(i);
                micro_puts(" differs: pio=");
                micro_put_hex(pio_dst[i]);
                micro_puts(" dma=");
                micro_put_hex(dma_dst[i]);
                micro_puts("\n");
                micro_fail(NAME, "the classic and DMA interfaces returned different bytes for the "
                                 "same file");
                micro_halt();
            }
        }

        /* Neither path may have simply left the fill pattern in place. */
        if (dma_dst[0] == 0xAAu && pio_dst[0] == 0x55u) {
            micro_fail(NAME, "both buffers still hold their fill bytes -- nothing was transferred");
            micro_halt();
        }

        /*
         * An e820 table is an array of 20-byte {addr, size, type} entries, so its length must be a
         * multiple of 20 and its first entry must be usable RAM at address 0. Checking the CONTENT,
         * not just that bytes arrived -- a device returning the wrong file would otherwise pass.
         */
        if ((len % 20u) != 0u) {
            micro_fail(NAME, "etc/e820's length is not a multiple of a 20-byte entry");
            micro_halt();
        }
        {
            uint64_t addr = *(const uint64_t *)(uintptr_t)DMA_DEST_GPA;
            uint64_t size = *(const uint64_t *)(uintptr_t)(DMA_DEST_GPA + 8ull);
            uint32_t type = *(const uint32_t *)(uintptr_t)(DMA_DEST_GPA + 16ull);

            micro_puts("micro/" NAME ": e820[0] addr=");
            micro_put_hex(addr);
            micro_puts(" size=");
            micro_put_uint(size / (1024ull * 1024ull));
            micro_puts(" MiB type=");
            micro_put_uint(type);
            micro_puts("\n");
            if (addr != 0ull || type != 1u || size == 0ull) {
                micro_fail(NAME, "e820[0] is not usable RAM starting at 0");
                micro_halt();
            }
        }
    }

    micro_pass(NAME);
    micro_halt();
}
