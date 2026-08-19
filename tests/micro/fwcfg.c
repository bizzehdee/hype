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
/*
 * The fw_cfg wire details -- ports, big-endian everything, the stage-then-trigger port pair, the
 * 64-byte directory entry, both DMA directions -- live in micro_fwcfg.h, shared with ramfb.c
 * (#549). They were written here first; moving them out when a second caller appeared is why
 * there is only one copy of them to get wrong.
 */
#include "micro_fwcfg.h"

#define NAME "fwcfg"

/* Scratch, clear of the payload at 16 MB, of the DMA access struct at 3 MB, and of each other. */
#define DMA_DEST_GPA 0x310000ull /* the DMA destination */
#define PIO_DEST_GPA 0x320000ull /* the classic-interface copy, for comparison */
#define SCRATCH_MAX 4096u

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
        uint32_t count;
        unsigned recorded = fw_read_dir(&count), i;

        micro_puts("micro/" NAME ": file directory lists ");
        micro_put_uint(count);
        micro_puts(" file(s)\n");
        if (count == 0u) {
            micro_fail(NAME, "the file directory count is zero");
            micro_halt();
        }
        if (count > recorded) {
            micro_fail(NAME, "the file directory lists more files than the walk can record -- "
                             "raise FW_CFG_MAX_FILES rather than reading a truncated directory");
            micro_halt();
        }
        for (i = 0; i < recorded; i++) {
            micro_puts("micro/" NAME ":   '");
            micro_puts(g_fw_files[i].name);
            micro_puts("' select=");
            micro_put_hex(g_fw_files[i].select);
            micro_puts(" size=");
            micro_put_uint(g_fw_files[i].size);
            micro_puts("\n");
        }
    }

    /*
     * Read one real file both ways and require agreement. etc/e820 is chosen because hype builds it
     * from THIS VM's configured RAM, so its content is config-derived rather than constant -- a
     * device returning a stale or shared buffer shows up as bytes that do not match this VM.
     */
    {
        const fw_file_t *e820 = fw_find("etc/e820");
        unsigned i;
        uint32_t len;

        if (e820 == 0) {
            micro_fail(NAME, "etc/e820 is not in the file directory -- hype publishes it for every "
                             "guest, so its absence is a regression, not a configuration");
            micro_halt();
        }
        len = e820->size;
        if (len == 0u || len > SCRATCH_MAX) {
            micro_fail(NAME, "etc/e820 has an implausible size");
            micro_halt();
        }

        for (i = 0; i < len; i++) {
            dma_dst[i] = 0xAAu; /* so a transfer that writes nothing is not mistaken for zeros */
            pio_dst[i] = 0x55u;
        }

        fw_read_pio(e820->select, pio_dst, len);
        if (fw_read_dma(NAME, e820->select, DMA_DEST_GPA, len) != 0) {
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
