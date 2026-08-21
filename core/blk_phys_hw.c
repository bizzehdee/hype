#include "blk_phys.h"
#include "ahci_host.h"
#include "nvme_host.h"

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

/*
 * #295: the vectored adapter. Converts the bus-agnostic segment list into the AHCI SG shape
 * (host-virtual == host-physical in this identity-mapped build, same cast the scalar adapters
 * make) and issues ONE multi-PRDT command. blk_phys's batching loop guarantees nsegs and the total
 * fit the caps handed to hype_blk_phys_enable_writev below, so the stack array is bounded.
 */
static int ahci_writev_adapter(void *hw, uint64_t lba, const hype_blk_seg_t *segs, uint32_t nsegs) {
    hype_blk_phys_ahci_t *a = (hype_blk_phys_ahci_t *)hw;
    hype_ahci_host_sg_t sg[HYPE_AHCI_HOST_SG_MAX_PRDT];
    uint64_t total = 0;
    uint32_t i;

    if (nsegs > HYPE_AHCI_HOST_SG_MAX_PRDT) {
        return -1;
    }
    for (i = 0; i < nsegs; i++) {
        sg[i].phys = (uint64_t)(uintptr_t)segs[i].buf;
        sg[i].bytes = segs[i].count * (uint32_t)HYPE_BLK_SECTOR_SIZE;
        total += (uint64_t)segs[i].count;
    }
    return hype_ahci_host_writev(a->abar_phys, a->port, lba, sg, nsegs, (uint16_t)total);
}

void hype_blk_phys_ahci_init(hype_blk_phys_t *p, hype_blk_phys_ahci_t *hw, hype_blk_backend_t *be,
                             uint64_t abar_phys, unsigned port, uint64_t total_sectors) {
    hw->abar_phys = abar_phys;
    hw->port = port;
    hype_blk_phys_init(p, be, ahci_read_adapter, ahci_write_adapter, hw, total_sectors);
    /* #295: AHCI carries scatter-gather natively, so arm the vectored write path. Caps: the
     * command table's PRDT slots, and the 4 MiB-per-command ceiling the SG builder enforces
     * (HYPE_BLK_PHYS_MAX_CHUNK is exactly that in sectors -- and it keeps `count` in uint16_t). */
    hype_blk_phys_enable_writev(p, be, ahci_writev_adapter, HYPE_AHCI_HOST_SG_MAX_PRDT,
                                HYPE_BLK_PHYS_MAX_CHUNK);
}

/* M10-1c (#197): NVMe adapters -- same shape, driving core/nvme_host's MMIO. */
static int nvme_read_adapter(void *hw, uint64_t lba, uint32_t count, void *buf) {
    hype_blk_phys_nvme_t *a = (hype_blk_phys_nvme_t *)hw;
    return hype_nvme_host_read(a->abar_phys, lba, (uint16_t)count, buf);
}

static int nvme_write_adapter(void *hw, uint64_t lba, uint32_t count, const void *buf) {
    hype_blk_phys_nvme_t *a = (hype_blk_phys_nvme_t *)hw;
    return hype_nvme_host_write(a->abar_phys, lba, (uint16_t)count, buf);
}

void hype_blk_phys_nvme_init(hype_blk_phys_t *p, hype_blk_phys_nvme_t *hw, hype_blk_backend_t *be,
                             uint64_t abar_phys, uint64_t total_sectors) {
    hw->abar_phys = abar_phys;
    hype_blk_phys_init(p, be, nvme_read_adapter, nvme_write_adapter, hw, total_sectors);
}
