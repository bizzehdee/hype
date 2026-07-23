#include "host_pci.h"

/*
 * Hardware shim for host_pci.c: the legacy 0xCF8/0xCFC PCI configuration
 * mechanism. Kept separate from the pure enumeration logic (and excluded from
 * the host unit-test build) exactly like core/serial_hw.c / ps2_host_hw.c,
 * since it issues real port I/O.
 */

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* 0xCF8 CONFIG_ADDRESS layout: bit31 enable, [23:16] bus, [15:11] device,
 * [10:8] function, [7:2] register (dword-aligned). */
static inline uint32_t cf8_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)(dev & 0x1Fu) << 11) |
           ((uint32_t)(func & 0x7u) << 8) | ((uint32_t)offset & 0xFCu);
}

uint32_t hype_host_pci_read32_hw(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    outl(0xCF8u, cf8_addr(bus, dev, func, offset));
    return inl(0xCFCu);
}

void hype_host_pci_write32_hw(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                              uint32_t value) {
    outl(0xCF8u, cf8_addr(bus, dev, func, offset));
    outl(0xCFCu, value);
}
