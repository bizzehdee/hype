/*
 * #547: PCI-1 and PCI-2, ported out of boot/main.c as one guest-side bus walk.
 *
 * The in-binary tests registered their OWN hype_pci_t with a host bridge and a FAKE AHCI-class
 * device, put the ECAM window at a private 4 GB address, and had the host assert what the guest
 * read back out of a result buffer. PCI-2 then swapped the fake device for the real one, which is
 * the whole difference between the two tests.
 *
 * A kernel-boot VM has NO firmware, so nothing has enumerated the bus or programmed a single BAR
 * before the guest's first instruction. That makes this port a REAL bus walk over hype's real device
 * model -- ECAM at 0xE0000000, a Q35 host bridge at dev 0, an ICH9 LPC at dev 31, the AHCI function
 * at dev 2 -- and it covers three things the in-binary versions structurally could not:
 *
 *  1. The ECAM path itself, from the guest. The old tests were handed a device at a hardcoded
 *     address; the config accesses here are the ones a real guest's bus scan performs.
 *  2. The BAR sizing PROTOCOL against the real function -- write all-ones, read back the writable
 *     mask, restore -- rather than against a fake device whose size the test itself had set.
 *  3. The capability list, which nothing tested at all. hype attaches an MSI capability to the AHCI
 *     function (#512's delivery path depends on it), and a guest that cannot find it falls back to
 *     INTx without saying so.
 *
 * Scope is CONFIG SPACE only. Touching the BAR's MMIO is the AHCI test's job (#548); a test that did
 * both could not say which half failed.
 */
#include "micro_pci.h"

#define NAME "pci"

/* boot/main.c's own constants for the real model. */
#define MCH_VENDOR 0x8086u
#define MCH_DEVICE 0x29C0u
#define LPC_VENDOR 0x8086u
#define LPC_DEVICE 0x2918u
#define HYPE_VENDOR 0xFFFEu /* HYPE_PCI_VENDOR_ID_HYPE */
#define AHCI_DEVICE 0x0005u
#define AHCI_BAR 5u

/* A device number hype never populates, for the absent-device convention. */
#define ABSENT_DEV 7u

#define PCI_CAP_PTR 0x34u
#define PCI_CAP_ID_MSI 0x05u

static int fail_count;

static void expect_u32(const char *what, uint32_t got, uint32_t want) {
    micro_puts("micro/" NAME ": ");
    micro_puts(what);
    micro_puts(" = ");
    micro_put_hex(got);
    if (got != want) {
        micro_puts(" -- EXPECTED ");
        micro_put_hex(want);
        fail_count++;
    }
    micro_puts("\n");
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    uint32_t id, cls, bar_mask, bar_back;
    unsigned dev;

    (void)zero_page_gpa;
    micro_puts("\n");

    /* A bus scan, reported. What is PRESENT is itself information: a device that silently stopped
     * being registered would otherwise show up only as a later assertion failing. */
    micro_puts("micro/" NAME ": bus 0 scan:\n");
    for (dev = 0u; dev < 32u; dev++) {
        if (!micro_pci_present(dev)) {
            continue;
        }
        id = micro_pci_read32(dev, MICRO_PCI_VENDOR_ID);
        cls = micro_pci_read32(dev, MICRO_PCI_CLASS_REV);
        micro_puts("micro/" NAME ":   dev ");
        micro_put_uint(dev);
        micro_puts(" id=");
        micro_put_hex(id);
        micro_puts(" class=");
        micro_put_hex(cls >> 8);
        micro_puts("\n");
    }

    /* The host bridge. */
    expect_u32("dev0 vendor/device", micro_pci_read32(MICRO_PCI_DEV_MCH, MICRO_PCI_VENDOR_ID),
               ((uint32_t)MCH_DEVICE << 16) | MCH_VENDOR);
    expect_u32("dev0 class", micro_pci_read32(MICRO_PCI_DEV_MCH, MICRO_PCI_CLASS_REV) >> 16,
               0x0600u);

    /* The LPC bridge. */
    expect_u32("dev31 vendor/device", micro_pci_read32(MICRO_PCI_DEV_LPC, MICRO_PCI_VENDOR_ID),
               ((uint32_t)LPC_DEVICE << 16) | LPC_VENDOR);
    expect_u32("dev31 class", micro_pci_read32(MICRO_PCI_DEV_LPC, MICRO_PCI_CLASS_REV) >> 16,
               0x0601u);

    /* The AHCI function -- the real one, which is what PCI-2 existed to reach. */
    expect_u32("dev2 vendor/device", micro_pci_read32(MICRO_PCI_DEV_AHCI, MICRO_PCI_VENDOR_ID),
               ((uint32_t)AHCI_DEVICE << 16) | HYPE_VENDOR);
    expect_u32("dev2 class", micro_pci_read32(MICRO_PCI_DEV_AHCI, MICRO_PCI_CLASS_REV) >> 8,
               0x010601u);

    /*
     * The absent-device convention EVERY real bus walk relies on: a function that is not there reads
     * as all-ones. A model returning 0 instead would make a scanning guest believe device 7 exists
     * with vendor 0, which is how a bus walk goes wrong quietly.
     */
    expect_u32("absent dev7 reads all-ones", micro_pci_read32(ABSENT_DEV, MICRO_PCI_VENDOR_ID),
               0xFFFFFFFFu);

    /*
     * BAR sizing on the real function. Write all-ones, read the writable mask back, restore. hype
     * gives the AHCI ABAR a 4 KB window, so the mask must be 0xFFFFF000.
     */
    bar_mask = micro_pci_read32(MICRO_PCI_DEV_AHCI, MICRO_PCI_BAR0 + AHCI_BAR * 4u);
    micro_pci_write32(MICRO_PCI_DEV_AHCI, MICRO_PCI_BAR0 + AHCI_BAR * 4u, 0xFFFFFFFFu);
    {
        uint32_t probe = micro_pci_read32(MICRO_PCI_DEV_AHCI, MICRO_PCI_BAR0 + AHCI_BAR * 4u);
        micro_pci_write32(MICRO_PCI_DEV_AHCI, MICRO_PCI_BAR0 + AHCI_BAR * 4u, bar_mask);
        expect_u32("dev2 BAR5 size mask", probe & ~0xFu, 0xFFFFF000u);
        micro_puts("micro/" NAME ": that is a ");
        micro_put_uint(micro_pci_bar_size(MICRO_PCI_DEV_AHCI, AHCI_BAR));
        micro_puts("-byte window\n");
    }

    /* Program it and read it back from CONFIG space. Whether MMIO to it then works is #548's
     * question; conflating the two would make a failure ambiguous. */
    (void)micro_pci_place_bar(MICRO_PCI_DEV_AHCI, AHCI_BAR, MICRO_BAR_WINDOW);
    bar_back = micro_pci_read32(MICRO_PCI_DEV_AHCI, MICRO_PCI_BAR0 + AHCI_BAR * 4u);
    expect_u32("dev2 BAR5 reads back what was programmed", bar_back & ~0xFu,
               (uint32_t)MICRO_BAR_WINDOW);

    /* Memory decoding and bus mastering must have stuck -- a BAR programmed with the command
     * register still closed is a device the guest cannot reach. */
    expect_u32("dev2 command has MEM|BUSMASTER",
               micro_pci_read32(MICRO_PCI_DEV_AHCI, MICRO_PCI_COMMAND) &
                   (MICRO_PCI_CMD_MEM_SPACE | MICRO_PCI_CMD_BUS_MASTER),
               MICRO_PCI_CMD_MEM_SPACE | MICRO_PCI_CMD_BUS_MASTER);

    /*
     * The capability list. Nothing tested this before, and #512's MSI delivery depends on a guest
     * finding it -- a guest that cannot falls back to INTx silently, which is a performance and
     * correctness difference nobody would notice.
     */
    {
        uint32_t cap = micro_pci_read32(MICRO_PCI_DEV_AHCI, PCI_CAP_PTR) & 0xFFu;
        int found_msi = 0;
        unsigned guard = 0u;

        micro_puts("micro/" NAME ": dev2 capability chain:");
        while (cap >= 0x40u && cap < 0x100u && guard++ < 16u) {
            uint32_t hdr = micro_pci_read32(MICRO_PCI_DEV_AHCI, cap);
            uint32_t cid = hdr & 0xFFu;
            micro_puts(" ");
            micro_put_hex(cid);
            if (cid == PCI_CAP_ID_MSI) {
                found_msi = 1;
            }
            cap = (hdr >> 8) & 0xFFu;
        }
        micro_puts("\n");
        if (!found_msi) {
            micro_puts("micro/" NAME ": no MSI capability found -- EXPECTED one (#512's delivery "
                       "path depends on it)\n");
            fail_count++;
        }
    }

    /* The interrupt line register is writable: OVMF reprograms it via Q35 routing and hype delivers
     * on whatever it holds, so a read-only register here would pin every guest to line 11. */
    {
        uint32_t saved = micro_pci_read32(MICRO_PCI_DEV_AHCI, MICRO_PCI_INTERRUPT_LINE);
        micro_pci_write32(MICRO_PCI_DEV_AHCI, MICRO_PCI_INTERRUPT_LINE,
                          (saved & 0xFFFFFF00u) | 0x0Au);
        expect_u32("dev2 interrupt line is writable",
                   micro_pci_read32(MICRO_PCI_DEV_AHCI, MICRO_PCI_INTERRUPT_LINE) & 0xFFu, 0x0Au);
        micro_pci_write32(MICRO_PCI_DEV_AHCI, MICRO_PCI_INTERRUPT_LINE, saved);
    }

    if (fail_count != 0) {
        micro_fail(NAME, "one or more config-space reads returned the wrong value -- see the lines "
                         "above, each of which names what it expected");
        micro_halt();
    }

    micro_pass(NAME);
    micro_halt();
}
