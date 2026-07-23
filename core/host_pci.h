#ifndef HYPE_CORE_HOST_PCI_H
#define HYPE_CORE_HOST_PCI_H

#include <stdint.h>

/*
 * GLADDER-10 (streaming ISO backend, foundation): host-side PCI enumeration.
 *
 * Everything in devices/pci.c is the *guest-facing* emulated config space.
 * This module is the opposite direction: it reads the REAL host PCI bus so
 * hype can find the physical storage controller it must later drive itself
 * (post-ExitBootServices, where UEFI's own BlockIo is gone) to stream an ISO
 * off disk instead of holding the whole thing in RAM. It also underpins the
 * eventual M5-3/M10-3 host-backed guest disks.
 *
 * The enumeration is pure logic driven by an injected config-space read32
 * callback (the gop.c/mp.c/file_io.c dependency-injection pattern), so it is
 * unit-tested against a fake config space. The real read32 (legacy 0xCF8/0xCFC
 * config mechanism) lives in the host_pci_hw.c shim -- 0xCF8/0xCFC port I/O
 * needs no Boot Services, so it works before or after ExitBootServices.
 *
 * Config-space register offsets, the 0xCF8 address layout, class-code field
 * positions, and memory-BAR encoding are stable, decades-old PCI spec
 * constants -- the same ones devices/pci.c transcribes for the guest side.
 */

/* Class code (config offset 0x0B) / subclass (0x0A) / prog-if (0x09). */
#define HYPE_HOST_PCI_CLASS_STORAGE 0x01u
#define HYPE_HOST_PCI_SUBCLASS_AHCI 0x06u /* SATA controller; prog-if 0x01 = AHCI 1.0 */
#define HYPE_HOST_PCI_SUBCLASS_NVME 0x08u /* Non-Volatile Memory; prog-if 0x02 = NVMe */

typedef enum {
    HYPE_HOST_STORAGE_NONE = 0,
    HYPE_HOST_STORAGE_AHCI,
    HYPE_HOST_STORAGE_NVME
} hype_host_storage_kind_t;

typedef struct {
    hype_host_storage_kind_t kind;
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    /* MMIO base of the controller's register window: AHCI ABAR (BAR5) or NVMe
     * BAR0. Low flag bits masked off; a 64-bit memory BAR is fully assembled
     * from its two dwords. */
    uint64_t bar_phys;
} hype_host_storage_t;

/*
 * Reads the 32-bit config dword at (bus,dev,func,offset); `offset` is
 * dword-aligned (low two bits ignored). A device-absent slot returns
 * 0xFFFFFFFF (as real hardware does for an unimplemented function). Injected
 * so the enumerator can be unit-tested against a synthetic config space.
 */
typedef uint32_t (*hype_host_pci_read32_fn)(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

/*
 * Writes the 32-bit config dword at (bus,dev,func,offset); `offset` is
 * dword-aligned (low two bits ignored). Injected alongside read32 so the
 * interrupt-disable orchestration below can be unit-tested against a mutable
 * synthetic config space.
 */
typedef void (*hype_host_pci_write32_fn)(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                                         uint32_t value);

/* PCI capability IDs (config offset `cap`, byte 0) walked by
 * hype_host_pci_find_cap / disabled by hype_host_pci_disable_interrupts. */
#define HYPE_HOST_PCI_CAP_MSI 0x05u
#define HYPE_HOST_PCI_CAP_MSIX 0x11u

/*
 * Walks the PCI capabilities linked list (Status[4] "Capabilities List" bit +
 * the capabilities pointer at config 0x34) looking for capability `cap_id`.
 * Returns its dword-aligned config offset (>=0x40), or 0 if the device has no
 * capability list or the id is absent. The walk is bounded (a malformed
 * self-referential `next` pointer cannot loop forever). Pure.
 */
uint8_t hype_host_pci_find_cap(hype_host_pci_read32_fn read32, uint8_t bus, uint8_t dev,
                               uint8_t func, uint8_t cap_id);

/*
 * GLADDER-10 reliability: hype drives the host storage controller as a strictly
 * POLLED driver, so it must never receive an interrupt from it. Masking the
 * AHCI-internal enables (PxIE + GHC.IE) proved insufficient on real hardware --
 * firmware often leaves MSI enabled, and an MSI is delivered independently of
 * the legacy INTx path, landing on a host IDT vector hype has no handler for
 * (-> hype_fatal / a silent fault on the AP running a guest). This authoritatively
 * silences the controller at the PCI level, which gates every delivery mode:
 *   - sets Command[10] "Interrupt Disable" (masks legacy INTx),
 *   - clears MSI Enable (Message Control[0]) in the MSI capability, if present,
 *   - clears MSI-X Enable (Message Control[15]) in the MSI-X capability, if present.
 * A read-modify-write on each affected dword via the injected callbacks; pure
 * given them, so unit-tested against a mutable fake config space. The real
 * 0xCF8/0xCFC read/write pair lives in the host_pci_hw.c shim.
 */
void hype_host_pci_disable_interrupts(hype_host_pci_read32_fn read32,
                                      hype_host_pci_write32_fn write32, uint8_t bus, uint8_t dev,
                                      uint8_t func);

/*
 * Scans buses 0..max_bus for the first AHCI or NVMe mass-storage controller,
 * filling *out. Returns 1 if one was found (out->kind != NONE), else 0.
 * Multi-function devices are probed only when the header-type MF bit is set,
 * matching real enumeration. Pure logic -- no port/MMIO access of its own.
 */
int hype_host_pci_find_storage(hype_host_pci_read32_fn read32, uint8_t max_bus,
                               hype_host_storage_t *out);

/*
 * Real config read via the legacy 0xCF8 (address) / 0xCFC (data) mechanism.
 * The hardware-touching shim (host_pci_hw.c); usable pre- or post-EBS.
 */
uint32_t hype_host_pci_read32_hw(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

/*
 * Real config write via the legacy 0xCF8 (address) / 0xCFC (data) mechanism.
 * The hardware-touching shim (host_pci_hw.c); usable pre- or post-EBS.
 */
void hype_host_pci_write32_hw(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                              uint32_t value);

#endif /* HYPE_CORE_HOST_PCI_H */
