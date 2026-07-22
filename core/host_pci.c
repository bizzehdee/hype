#include "host_pci.h"

/* Config-space register offsets (PCI 3.0, type-0 header). */
#define CFG_VENDOR_DEVICE 0x00u /* [15:0] vendor, [31:16] device */
#define CFG_CLASS_REVISION 0x08u /* [31:24] class, [23:16] subclass, [15:8] prog-if, [7:0] rev */
#define CFG_HEADER_TYPE 0x0Cu /* byte 2 (bits [23:16]): bit7 = multi-function */
#define CFG_BAR0 0x10u
#define CFG_BAR5 0x24u

#define VENDOR_INVALID 0xFFFFu

/* Assemble a memory BAR's base address from `read32`, handling the 64-bit BAR
 * form (type bits [2:1] == 10b: the high dword lives in the next BAR). Returns
 * 0 for an I/O-space BAR (bit0 set) -- storage register windows are always
 * memory. `bar_off` must be the offset of a BAR register (0x10..0x24). */
static uint64_t read_mem_bar(hype_host_pci_read32_fn read32, uint8_t bus, uint8_t dev,
                             uint8_t func, uint8_t bar_off) {
    uint32_t lo = read32(bus, dev, func, bar_off);
    uint64_t base;

    if ((lo & 0x1u) != 0u) {
        return 0u; /* I/O-space BAR, not a memory window */
    }
    base = (uint64_t)(lo & ~0xFu); /* clear the low flag nibble */
    if (((lo >> 1) & 0x3u) == 0x2u) { /* 64-bit memory BAR: high half in next dword */
        uint32_t hi = read32(bus, dev, func, (uint8_t)(bar_off + 4u));
        base |= (uint64_t)hi << 32;
    }
    return base;
}

int hype_host_pci_find_storage(hype_host_pci_read32_fn read32, uint8_t max_bus,
                               hype_host_storage_t *out) {
    unsigned bus;
    unsigned dev;

    out->kind = HYPE_HOST_STORAGE_NONE;
    out->bar_phys = 0u;

    for (bus = 0; bus <= (unsigned)max_bus; bus++) {
        for (dev = 0; dev < 32u; dev++) {
            unsigned last_func = 0u; /* single-function until the MF bit proves otherwise */
            unsigned func;

            for (func = 0; func <= last_func; func++) {
                uint32_t vd = read32((uint8_t)bus, (uint8_t)dev, (uint8_t)func, CFG_VENDOR_DEVICE);
                uint32_t cls;
                uint8_t class_base;
                uint8_t subclass;

                if ((vd & 0xFFFFu) == VENDOR_INVALID) {
                    continue; /* no device/function here */
                }
                if (func == 0u) {
                    uint32_t hdr = read32((uint8_t)bus, (uint8_t)dev, 0u, CFG_HEADER_TYPE);
                    if (((hdr >> 16) & 0x80u) != 0u) {
                        last_func = 7u; /* multi-function device: probe all eight */
                    }
                }

                cls = read32((uint8_t)bus, (uint8_t)dev, (uint8_t)func, CFG_CLASS_REVISION);
                class_base = (uint8_t)((cls >> 24) & 0xFFu);
                subclass = (uint8_t)((cls >> 16) & 0xFFu);
                if (class_base != HYPE_HOST_PCI_CLASS_STORAGE) {
                    continue;
                }

                if (subclass == HYPE_HOST_PCI_SUBCLASS_NVME) {
                    out->kind = HYPE_HOST_STORAGE_NVME;
                } else if (subclass == HYPE_HOST_PCI_SUBCLASS_AHCI) {
                    out->kind = HYPE_HOST_STORAGE_AHCI;
                } else {
                    continue; /* some other storage class (IDE/SCSI/...) -- not driven here */
                }

                out->bus = (uint8_t)bus;
                out->dev = (uint8_t)dev;
                out->func = (uint8_t)func;
                out->vendor_id = (uint16_t)(vd & 0xFFFFu);
                out->device_id = (uint16_t)((vd >> 16) & 0xFFFFu);
                /* NVMe registers are at BAR0; AHCI's ABAR is BAR5. */
                out->bar_phys = read_mem_bar(read32, (uint8_t)bus, (uint8_t)dev, (uint8_t)func,
                                             (out->kind == HYPE_HOST_STORAGE_NVME) ? CFG_BAR0 : CFG_BAR5);
                return 1;
            }
        }
    }
    return 0;
}
