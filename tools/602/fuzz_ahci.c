/*
 * #602: host-side libFuzzer harness over the AHCI front-end's command processing --
 * Command List -> Command Table -> PRDT -> FIS, via process_ahci_command_slot()
 * (ATAPI device) and process_ahci_ata_command_slot() (plain SATA disk), both defined
 * in arch/x86_64/svm/svm_vcpu.c and, per plan.md, vendor-neutral.
 *
 * Unlike virtio-blk, NO host unit test drives this pair today (core/tests/test_ahci.c
 * covers only the pure MMIO register model and the pure Command Header/PRDT/H2D-FIS
 * decoders) -- this is the ticket's highest-value AHCI gap: the guest-controlled
 * command-list-walk arithmetic has never run under a sanitizer, or at all, on the host.
 *
 * MMIO port registers (CLB/CLBU/FB/FBU, PxCMD, ...) are set directly on the struct
 * rather than through hype_ahci_mmio_write() -- those two ARE the guest-writable
 * surface for this command processor (a driver programs the command-list base through
 * exactly those registers before ringing PxCI), so driving them via fuzzed values is
 * the faithful way to get "command lists ... with random fields" including bases that
 * land outside the mapped guest-RAM window.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "fuzz_common.h"
#include "../../devices/ahci.h"
#include "../../devices/ata_disk.h"
#include "../../devices/atapi.h"
#include "../../core/guest_mem.h"
#include "../../core/fatal.h" /* hype_debug_set_level */

#define FUZZ_RAM_BYTES (64u * 1024u)
static uint8_t g_ram[FUZZ_RAM_BYTES];
#define FUZZ_RAM_GPA_BASE 0x40000000ull

/* ATAPI media (read-only optical disc image) and a plain ATA disk's media, each a
 * whole number of their own sector size. */
static uint8_t g_iso[32u * HYPE_ATAPI_SECTOR_SIZE];
static uint8_t g_ata_media[128u * HYPE_ATA_SECTOR_SIZE];

#define MAX_ROUNDS 8u

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    hype_fuzz_cursor_t c;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    hype_ata_disk_t disk;
    hype_gpa_map_t map;
    unsigned round;
    int use_atapi;

    /*
     * process_ahci_command_slot()/process_ahci_ata_command_slot() call hype_debug_print()
     * unconditionally on several refusal paths, with no injectable sink (unlike
     * virtio-blk's reject sink). At the default HYPE_LOG_DEBUG level that reaches
     * hype_serial_putc()'s real `inb`/`outb` -- a genuine I/O-privileged instruction that
     * SIGSEGVs in a host user process. Dropping the level below DEBUG makes
     * hype_debug_print() a no-op (core/log_level.c's msg<=current filter), which is what
     * every existing test achieves implicitly by never reaching a code path that logs.
     * This is a harness-safety measure, not a change to the model under test.
     */
    hype_debug_set_level(HYPE_LOG_ERROR);

    hype_fuzz_cursor_init(&c, data, size);

    hype_gpa_map_reset(&map);
    hype_gpa_map_add(&map, FUZZ_RAM_GPA_BASE, (uint64_t)(uintptr_t)g_ram, FUZZ_RAM_BYTES);

    hype_ahci_reset(&ahci);
    hype_ahci_set_bus_master(&ahci, 1);

    use_atapi = hype_fuzz_u8(&c) & 1;
    if (use_atapi) {
        hype_atapi_reset(&atapi, g_iso, sizeof(g_iso));
        hype_ahci_set_signature(&ahci, HYPE_AHCI_SIG_ATAPI);
    } else {
        hype_ata_disk_reset(&disk, g_ata_media, sizeof(g_ata_media));
        hype_ahci_set_signature(&ahci, HYPE_AHCI_SIG_ATA);
    }

    for (round = 0; round < MAX_ROUNDS && hype_fuzz_cursor_remaining(&c) >= 16u; round++) {
        uint32_t slot;

        /* The guest-writable command-list/FIS-area base registers: whatever a fuzzed
         * write produced, including values that put the command list outside g_ram
         * entirely -- the case the dma_map translation must refuse. */
        ahci.p_clb = hype_fuzz_u32(&c);
        ahci.p_clbu = hype_fuzz_u32(&c);
        ahci.p_fb = hype_fuzz_u32(&c);
        ahci.p_fbu = hype_fuzz_u32(&c);

        if (hype_fuzz_cursor_remaining(&c) > 0) {
            /* PxCMD -- fuzz ST/FRE alongside everything else via the real register
             * write path, so a nonsensical PxCMD state is reachable exactly as a
             * guest driver could reach it. */
            (void)hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CMD, 4u,
                                       hype_fuzz_u32(&c));
        }

        hype_fuzz_fill(&c, g_ram, FUZZ_RAM_BYTES);

        slot = hype_fuzz_u32(&c) % 32u; /* AHCI's own 32-slot command-list ceiling */

        if (use_atapi) {
            (void)process_ahci_command_slot(&ahci, &atapi, &map, slot);
        } else {
            (void)process_ahci_ata_command_slot(&ahci, &disk, &map, slot);
        }
        (void)hype_ahci_irq_pending(&ahci);
    }

    return 0;
}
