#include "blk_phys.h"
#include "ahci_host.h"

/*
 * Coverage-exempt runtime binding for the physical blk_backend: the injected
 * per-chunk callbacks that actually poke the HBA via core/ahci_host's MMIO
 * path. Kept out of the unit-test build (like ahci_host_hw.c) since they reach
 * real hardware; the chunking/vtable logic they are plugged into (blk_phys.c)
 * is pure and tested with fakes.
 */

static int ahci_read_adapter(void *hw, uint64_t lba, uint32_t count, void *buf) {
    hype_blk_phys_ahci_t *a = (hype_blk_phys_ahci_t *)hw;
    /* count is <= HYPE_BLK_PHYS_MAX_CHUNK (8192) so it fits a uint16_t. */
    return hype_ahci_host_read(a->abar_phys, a->port, lba, (uint16_t)count, buf);
}

static int ahci_write_adapter(void *hw, uint64_t lba, uint32_t count, const void *buf) {
    hype_blk_phys_ahci_t *a = (hype_blk_phys_ahci_t *)hw;
    return hype_ahci_host_write(a->abar_phys, a->port, lba, (uint16_t)count, buf);
}

void hype_blk_phys_ahci_init(hype_blk_phys_t *p, hype_blk_phys_ahci_t *hw, hype_blk_backend_t *be,
                             uint64_t abar_phys, unsigned port, uint64_t total_sectors) {
    hw->abar_phys = abar_phys;
    hw->port = port;
    hype_blk_phys_init(p, be, ahci_read_adapter, ahci_write_adapter, hw, total_sectors);
}
