/*
 * #550: M5-2 (ATA disk behind AHCI), ported out of boot/main.c as a guest-side driver.
 *
 * Shares its bring-up with #548's ahci.c through micro_ahci.h -- the same command list, received
 * FIS area, command header and PRDT. What differs is the command: ahci.c issues IDENTIFY, which
 * proves the HBA transfers device metadata; this issues WRITE DMA EXT then READ DMA EXT, which
 * proves the ATA disk model moves guest DATA to and from a real backing file.
 *
 * The in-binary version read and wrote a buffer its own launch code had prepared, so nothing it
 * did could distinguish a working disk from a working memcpy. Here the file comes from
 * `[disk.*] backing = file` and the harness re-reads it on the HOST after the run, which is the
 * only way to tell storage from a cache with a storage-shaped interface (#343).
 *
 * The write happens BEFORE the read into a different buffer, and the pattern is position- and
 * LBA-dependent, so a device returning a constant, the previous sector, or the buffer the guest
 * already had cannot match by accident.
 *
 * cmdline (#546): `sector=N`. Default 1 rather than 0, so a run cannot overwrite a partition
 * table on an image someone cares about.
 */
#include "micro_ahci.h"

#define NAME "atadisk"

#define SECTOR_SIZE 512u

static unsigned long long parse_uint(const char *s) {
    unsigned long long v = 0ull;
    if (s == 0) {
        return 0ull;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10ull + (unsigned long long)(*s - '0');
        s++;
    }
    return v;
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    const char *cl = micro_cmdline(zero_page_gpa);
    volatile uint8_t *buf = (volatile uint8_t *)(uintptr_t)AHCI_DATA_GPA;
    uint64_t sector = 1ull;
    int port;
    unsigned i;

    micro_puts("\n");

    if (cl != 0) {
        const char *v = micro_cmdline_value(cl, "sector");
        if (v != 0) {
            sector = parse_uint(v);
        }
    }

    if (ahci_attach(NAME) != 0) {
        micro_fail(NAME, "no AHCI HBA on the bus, or its ABAR is too small");
        micro_halt();
    }
    /* A SATA disk specifically: the rig also attaches an ISO, so "any device" finds the ATAPI
     * drive on port 0 first. */
    port = ahci_find_device(NAME, AHCI_SIG_ATA, 0);
    if (port < 0) {
        micro_fail(NAME, "no implemented port carries a SATA disk -- this test needs a [disk.*] "
                         "with bus = ahci-sata");
        micro_halt();
    }

    ahci_port_start((unsigned)port);

    /* ---- write a known pattern ---------------------------------------------------------- */
    for (i = 0; i < SECTOR_SIZE; i++) {
        buf[i] = (uint8_t)(0x5Au ^ (uint8_t)i ^ (uint8_t)(sector & 0xFFu));
    }
    if (ahci_issue(NAME, (unsigned)port, ATA_CMD_WRITE_DMA_EXT, sector, 1u, SECTOR_SIZE, 1) != 0) {
        micro_fail(NAME, "WRITE DMA EXT did not complete cleanly");
        micro_halt();
    }

    /*
     * Poison the buffer before reading back. Without this a device that transfers nothing leaves
     * the written pattern in place and the comparison passes -- the exact false pass this whole
     * epic exists to remove.
     */
    for (i = 0; i < SECTOR_SIZE; i++) {
        buf[i] = 0xCCu;
    }
    if (ahci_issue(NAME, (unsigned)port, ATA_CMD_READ_DMA_EXT, sector, 1u, SECTOR_SIZE, 0) != 0) {
        micro_fail(NAME, "READ DMA EXT did not complete cleanly");
        micro_halt();
    }

    for (i = 0; i < SECTOR_SIZE; i++) {
        uint8_t want = (uint8_t)(0x5Au ^ (uint8_t)i ^ (uint8_t)(sector & 0xFFu));
        if (buf[i] != want) {
            micro_puts("micro/" NAME ": byte ");
            micro_put_uint(i);
            micro_puts(" read ");
            micro_put_hex(buf[i]);
            micro_puts(" wrote ");
            micro_put_hex(want);
            if (buf[i] == 0xCCu) {
                micro_puts(" (still the poison -- the read transferred nothing)");
            }
            micro_puts("\n");
            micro_fail(NAME, "the sector did not read back what was written");
            micro_halt();
        }
    }

    micro_puts("micro/" NAME ": sector ");
    micro_put_uint(sector);
    micro_puts(" written and read back byte-for-byte (");
    micro_put_uint(SECTOR_SIZE);
    micro_puts(" bytes, pattern 0x5A^i^lba)\n");
    micro_puts("micro/" NAME ": the guest cannot see the backing FILE -- the harness compares it "
               "on the host, which is what distinguishes storage from a cache\n");
    micro_pass(NAME);
    micro_halt();
}
