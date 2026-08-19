/*
 * #548: M4-5 (AHCI), ported out of boot/main.c as a guest-side driver.
 *
 * The in-binary test handed itself a device at a fixed address and did the register work from the
 * HOST, so the sequence a real guest's libata performs had never once been driven from inside a
 * guest here. #436's entire Windows investigation turned on AHCI command behaviour under a real
 * guest; a test driven from the guest side is the one that could have shown it.
 *
 * This guest does the whole thing:
 *
 *   find the HBA over ECAM, size and place ABAR itself, enable memory decode + bus mastering
 *   set GHC.AE before reading any port register
 *   walk PI for an implemented port and read its signature
 *   build a command list, a received-FIS area and a command table in its OWN RAM
 *   issue IDENTIFY DEVICE and check the completion, the task file and the returned data
 *
 * What makes this more than "the command returned": IDENTIFY's payload is checked for structure,
 * not just for being non-zero. A device that DMA'd nothing leaves the buffer as the guest
 * poisoned it; a device that DMA'd the wrong thing fails the field checks. Both are distinguished
 * from success, and from each other, in the output.
 *
 * The device this VM gets -- SATA disk or ATAPI drive -- comes from its config, so one artifact
 * covers both. IDENTIFY DEVICE is a disk command, so an ATAPI-only VM is reported as SKIPPED
 * rather than failed: a test that fails because it was pointed at the wrong device is reporting
 * on the config, not on hype.
 */
#include "micro_ahci.h"

#define NAME "ahci"

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    volatile uint16_t *id = (volatile uint16_t *)(uintptr_t)AHCI_DATA_GPA;
    uint32_t cap, ghc, vs;
    int port;
    unsigned i;
    uint64_t sectors;

    (void)zero_page_gpa;
    micro_puts("\n");

    if (ahci_attach(NAME) != 0) {
        micro_fail(NAME, "no AHCI HBA on the bus, or its ABAR is too small");
        micro_halt();
    }

    cap = ahci_r32(AHCI_REG_CAP);
    ghc = ahci_r32(AHCI_REG_GHC);
    vs = ahci_r32(AHCI_REG_VS);
    micro_puts("micro/" NAME ": CAP ");
    micro_put_hex(cap);
    micro_puts(" GHC ");
    micro_put_hex(ghc);
    micro_puts(" VS ");
    micro_put_hex(vs);
    micro_puts("\n");
    if ((ghc & AHCI_GHC_AE) == 0u) {
        micro_fail(NAME, "GHC.AE did not stick after the guest set it -- the HBA is refusing to "
                         "leave legacy mode, and every port register below would be undefined");
        micro_halt();
    }
    /* CAP's low 5 bits are "number of ports minus one". Zero ports is not a legal HBA. */
    micro_puts("micro/" NAME ": CAP reports ");
    micro_put_uint((cap & 0x1Fu) + 1u);
    micro_puts(" port(s), ");
    micro_put_uint(((cap >> 8) & 0x1Fu) + 1u);
    micro_puts(" command slot(s)\n");

    /*
     * Ask for a SATA DISK specifically. The rig attaches an ISO, so port 0 is an ATAPI drive and
     * a search for "any device" finds that first -- the first version of this test did exactly
     * that, skipped itself, and still reported a pass. A skip that reports success is the failure
     * mode this whole epic exists to remove, so this now demands the device it needs.
     */
    port = ahci_find_device(NAME, AHCI_SIG_ATA, 0);
    if (port < 0) {
        micro_fail(NAME, "no implemented port carries a SATA disk -- IDENTIFY DEVICE is a disk "
                         "command, so this VM needs a [disk.*] with bus = ahci-sata");
        micro_halt();
    }

    ahci_port_start((unsigned)port);

    /* Poison the buffer so "the device DMA'd nothing" and "the device returned zeros" are
     * different observable outcomes rather than the same one. */
    for (i = 0; i < 256u; i++) {
        id[i] = 0xDEADu;
    }

    if (ahci_issue(NAME, (unsigned)port, ATA_CMD_IDENTIFY_DEVICE, 0ull, 1u, 512u, 0) != 0) {
        micro_fail(NAME, "IDENTIFY DEVICE did not complete cleanly");
        micro_halt();
    }

    if (id[0] == 0xDEADu) {
        micro_fail(NAME, "the command completed but the buffer is untouched -- the HBA reported "
                         "success without transferring the IDENTIFY data");
        micro_halt();
    }

    /*
     * Words 60/61 are the 28-bit LBA sector count, and words 100..103 the 48-bit one. A disk
     * reporting zero for both completed a transfer that says nothing, which is a different bug
     * from not transferring at all.
     */
    sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    micro_puts("micro/" NAME ": IDENTIFY word0=");
    micro_put_hex(id[0]);
    micro_puts(" 28-bit sectors=");
    micro_put_uint(sectors);
    micro_puts(" (");
    micro_put_uint(sectors / 2048ull);
    micro_puts(" MiB)\n");

    /* Words 27..46 are the model string, ASCII in byte-swapped pairs. Printing it proves the
     * bytes are structured rather than merely present. */
    {
        char model[41];
        unsigned n = 0;
        for (i = 27u; i < 47u; i++) {
            model[n++] = (char)((id[i] >> 8) & 0xFFu);
            model[n++] = (char)(id[i] & 0xFFu);
        }
        model[40] = '\0';
        while (n > 0u && (model[n - 1u] == ' ' || model[n - 1u] == '\0')) {
            model[--n] = '\0';
        }
        micro_puts("micro/" NAME ": model '");
        micro_puts(model);
        micro_puts("'\n");
        if (model[0] == '\0') {
            micro_fail(NAME, "the IDENTIFY model string is empty -- the payload is not structured "
                             "IDENTIFY data");
            micro_halt();
        }
    }

    if (sectors == 0ull) {
        micro_fail(NAME, "IDENTIFY reports zero addressable sectors");
        micro_halt();
    }

    micro_pass(NAME);
    micro_halt();
}
