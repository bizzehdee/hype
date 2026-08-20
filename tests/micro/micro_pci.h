#ifndef HYPE_MICRO_PCI_H
#define HYPE_MICRO_PCI_H

#include "micro.h"

/*
 * #536: guest-side PCI/ECAM access for micro-kernels.
 *
 * A kernel-boot VM has NO firmware, so nothing has enumerated the bus or programmed a single BAR
 * before the guest's first instruction. Every microtest that talks to a memory-mapped device
 * therefore has to do what firmware would: find the device, size its BAR, place it, and enable it.
 *
 * That is a feature, not a chore. The in-binary versions of these tests were handed a device at a
 * hardcoded guest-physical address by their own launch code, so they never exercised the ECAM path,
 * the BAR sizing protocol, or the command register at all. Doing it here means each device test
 * also covers how a real guest reaches that device.
 *
 * The addresses below are hype's real device model (boot/main.c's HYPE_FW_1_* constants), not
 * test-only ones. A microtest that pointed itself at a private aperture would re-create exactly the
 * coupling #534 exists to remove.
 */

/* HYPE_FW_1_ECAM_GPA. Bus 0 only -- hype implements one bus and advertises one (#436). */
#define MICRO_ECAM_BASE 0xE0000000ull

/* Device numbers on bus 0, from boot/main.c. */
#define MICRO_PCI_DEV_MCH 0u        /* Q35 host bridge */
#define MICRO_PCI_DEV_AHCI 2u       /* HYPE_FW_1_PCI_DEV_AHCI */
#define MICRO_PCI_DEV_VIRTIO_BLK 3u /* HYPE_FW_1_PCI_DEV_VIRTIO_BLK */
#define MICRO_PCI_DEV_NVME 5u       /* HYPE_FW_1_PCI_DEV_NVME */
#define MICRO_PCI_DEV_LPC 31u       /* HYPE_FW_1_PCI_DEV_ICH9_LPC */

#define MICRO_PCI_VENDOR_ID 0x00u
#define MICRO_PCI_DEVICE_ID 0x02u
#define MICRO_PCI_COMMAND 0x04u
#define MICRO_PCI_CLASS_REV 0x08u
#define MICRO_PCI_BAR0 0x10u
#define MICRO_PCI_INTERRUPT_LINE 0x3Cu

#define MICRO_PCI_CMD_IO_SPACE 0x0001u
#define MICRO_PCI_CMD_MEM_SPACE 0x0002u
#define MICRO_PCI_CMD_BUS_MASTER 0x0004u

/*
 * A guest-physical window for BARs this test programs. 0xC0000000 (3 GB) is below the ECAM window
 * at 0xE0000000 and far above any plausible mem_mb, so it collides with neither RAM nor ECAM. The
 * identity map covers it (4 GB, core/kboot.h), and hype's nested tables leave it not-present -- so
 * an access to an unprogrammed or wrongly-placed BAR faults into hype's MMIO decode and is
 * reported, rather than quietly reaching RAM.
 */
#define MICRO_BAR_WINDOW 0xC0000000ull

static inline uint64_t micro_ecam_addr(unsigned dev, unsigned func, unsigned off) {
    return MICRO_ECAM_BASE + ((uint64_t)dev << 15) + ((uint64_t)func << 12) + (uint64_t)off;
}

/* Declared below micro_pci_fread32/fwrite32, which carry the explanation of why these use an
 * explicit MOV. Function 0 of a device is just func == 0, so these delegate rather than repeat the
 * encoding -- two spellings of one access is how the two would drift. */
static inline uint32_t micro_pci_fread32(unsigned dev, unsigned func, unsigned off);
static inline void micro_pci_fwrite32(unsigned dev, unsigned func, unsigned off, uint32_t v);

static inline uint32_t micro_pci_read32(unsigned dev, unsigned off) {
    return micro_pci_fread32(dev, 0u, off);
}

static inline void micro_pci_write32(unsigned dev, unsigned off, uint32_t v) {
    micro_pci_fwrite32(dev, 0u, off, v);
}

/*
 * Function-aware variants. A real bus walk enumerates FUNCTIONS as well as devices: hype puts the
 * SATA AHCI controller on device 31 as an ICH9 function, exactly where a real chipset does
 * (00:1f.2), while the optical HBA is a device of its own. A test that walks devices only finds
 * the wrong controller and concludes there is no disk (#548).
 */
/*
 * EXPLICIT MOV, not a volatile dereference, and this is a real finding rather than style.
 *
 * A `volatile uint32_t` load is a single memory access, so clang is free to lower
 * `if ((read32(dev, 4) >> 16) & BIT)` to one `testl $imm32, disp(%reg,%reg)` -- the access count is
 * preserved, so the volatile contract holds. hype's MMIO decoder does not handle the F7 /0
 * TEST-with-immediate form, so the guest faulted undecodably at its ECAM window and hype stopped
 * the VM. The test looked like a device bug; it was the compiler picking a legal encoding.
 *
 * virtioblk.c already carries this lesson for its BAR accesses ("at -O2 clang folded one of these
 * into a form hype's virtio path refused"). It belongs here too, because config-space reads are
 * shared by every test and the fold depends on what the caller does with the value -- so any test
 * that masks a config field could trip it, at any optimisation level, without changing this file.
 *
 * Filed separately as the decoder gap it also is: a real guest driver testing a bit in an MMIO
 * register can emit exactly this instruction.
 */
static inline uint32_t micro_pci_fread32(unsigned dev, unsigned func, unsigned off) {
    uint32_t v;
    __asm__ volatile("movl (%1), %0"
                     : "=r"(v)
                     : "r"((volatile uint32_t *)(uintptr_t)micro_ecam_addr(dev, func, off))
                     : "memory");
    return v;
}

static inline void micro_pci_fwrite32(unsigned dev, unsigned func, unsigned off, uint32_t v) {
    __asm__ volatile("movl %0, (%1)"
                     :
                     : "r"(v), "r"((volatile uint32_t *)(uintptr_t)micro_ecam_addr(dev, func, off))
                     : "memory");
}

static inline int micro_pci_fpresent(unsigned dev, unsigned func) {
    uint32_t id = micro_pci_fread32(dev, func, MICRO_PCI_VENDOR_ID);
    return (id != 0xFFFFFFFFu && (id & 0xFFFFu) != 0xFFFFu) ? 1 : 0;
}

/* class(31:24) | subclass(23:16) | prog-IF(15:8) from the class/revision dword. */
static inline uint32_t micro_pci_fclass(unsigned dev, unsigned func) {
    return micro_pci_fread32(dev, func, MICRO_PCI_CLASS_REV) >> 8;
}

static inline uint16_t micro_pci_vendor(unsigned dev) {
    return (uint16_t)(micro_pci_read32(dev, MICRO_PCI_VENDOR_ID) & 0xFFFFu);
}

static inline int micro_pci_present(unsigned dev) {
    /* The standard convention every real bus-walk relies on: an absent device reads as all-1s. */
    uint32_t id = micro_pci_read32(dev, MICRO_PCI_VENDOR_ID);
    return (id != 0xFFFFFFFFu && (id & 0xFFFFu) != 0xFFFFu) ? 1 : 0;
}

/*
 * The BAR sizing protocol, exactly as firmware does it: write all-1s, read back, and the lowest
 * set bit of the writable field is the size. Returns 0 if the BAR is unimplemented.
 *
 * Note the reads go through EAX rather than an immediate-to-memory store: hype's MMIO decoder
 * supports the MOV/MOVZX register forms (0x88/0x89/0x8A/0x8B/0F B6/0F B7), not 0xC7, and matching
 * what the decoder handles is the existing convention here rather than something to extend for one
 * test's convenience.
 */
static inline uint32_t micro_pci_fbar_size(unsigned dev, unsigned func, unsigned bar_index) {
    unsigned off = MICRO_PCI_BAR0 + bar_index * 4u;
    uint32_t saved = micro_pci_fread32(dev, func, off);
    uint32_t probe;

    micro_pci_fwrite32(dev, func, off, 0xFFFFFFFFu);
    probe = micro_pci_fread32(dev, func, off);
    micro_pci_fwrite32(dev, func, off, saved);

    if (probe == 0u || probe == 0xFFFFFFFFu) {
        return 0u;
    }
    probe &= ~0xFu; /* memory BAR: low 4 bits are type/prefetch flags, not address */
    return (~probe) + 1u;
}

static inline uint32_t micro_pci_bar_size(unsigned dev, unsigned bar_index) {
    return micro_pci_fbar_size(dev, 0u, bar_index);
}

/* Place a memory BAR at `gpa` and enable memory decoding + bus mastering. Returns `gpa`. */
static inline uint64_t micro_pci_fplace_bar(unsigned dev, unsigned func, unsigned bar_index,
                                            uint64_t gpa) {
    unsigned off = MICRO_PCI_BAR0 + bar_index * 4u;

    micro_pci_fwrite32(dev, func, off, (uint32_t)gpa);
    micro_pci_fwrite32(dev, func, MICRO_PCI_COMMAND,
                       (micro_pci_fread32(dev, func, MICRO_PCI_COMMAND) & 0xFFFF0000u) |
                           MICRO_PCI_CMD_MEM_SPACE | MICRO_PCI_CMD_BUS_MASTER);
    return gpa;
}

static inline uint64_t micro_pci_place_bar(unsigned dev, unsigned bar_index, uint64_t gpa) {
    return micro_pci_fplace_bar(dev, 0u, bar_index, gpa);
}

#endif /* HYPE_MICRO_PCI_H */
