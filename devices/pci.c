#include "pci.h"

static void write_le16(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

void hype_pci_reset(hype_pci_t *pci) {
    unsigned int i, j;

    for (i = 0; i < HYPE_PCI_MAX_DEVICES; i++) {
        pci->devices[i].in_use = 0;
        for (j = 0; j < HYPE_PCI_CONFIG_SIZE; j++) {
            pci->devices[i].config[j] = 0;
        }
        for (j = 0; j < 6; j++) {
            pci->devices[i].bar_size[j] = 0;
        }
    }
    pci->cf8_selected = 0;
}

int hype_pci_add_device(hype_pci_t *pci, uint8_t device_number, uint16_t vendor_id, uint16_t device_id,
                         uint8_t class_base, uint8_t class_sub, uint8_t class_interface) {
    hype_pci_device_t *dev;
    unsigned int i;

    if (device_number >= HYPE_PCI_MAX_DEVICES) {
        return -1;
    }

    dev = &pci->devices[device_number];
    for (i = 0; i < HYPE_PCI_CONFIG_SIZE; i++) {
        dev->config[i] = 0;
    }
    for (i = 0; i < 6; i++) {
        dev->bar_size[i] = 0;
    }
    dev->in_use = 1;

    write_le16(dev->config + 0x00, vendor_id);
    write_le16(dev->config + 0x02, device_id);
    dev->config[0x08] = 0; /* Revision ID */
    dev->config[0x09] = class_interface;
    dev->config[0x0A] = class_sub;
    dev->config[0x0B] = class_base;
    dev->config[0x0E] = 0x00; /* Header Type: single-function, Type 0 */

    return 0;
}

void hype_pci_set_bar_size(hype_pci_t *pci, uint8_t device_number, unsigned int bar_index, uint32_t size) {
    if (device_number >= HYPE_PCI_MAX_DEVICES || bar_index >= 6) {
        return;
    }
    pci->devices[device_number].bar_size[bar_index] = size;
}

void hype_pci_set_interrupt(hype_pci_t *pci, uint8_t device_number, uint8_t int_pin, uint8_t int_line) {
    if (device_number >= HYPE_PCI_MAX_DEVICES) {
        return;
    }
    pci->devices[device_number].config[0x3C] = int_line; /* Interrupt Line */
    pci->devices[device_number].config[0x3D] = int_pin;  /* Interrupt Pin  */
}

uint8_t hype_pci_get_interrupt_line(const hype_pci_t *pci, uint8_t device_number) {
    if (device_number >= HYPE_PCI_MAX_DEVICES || !pci->devices[device_number].in_use) {
        return 0;
    }
    return pci->devices[device_number].config[0x3C];
}

uint32_t hype_pci_get_bar_value(const hype_pci_t *pci, uint8_t device_number, unsigned int bar_index) {
    const hype_pci_device_t *dev;
    const uint8_t *p;

    if (device_number >= HYPE_PCI_MAX_DEVICES || bar_index >= 6) {
        return 0;
    }
    dev = &pci->devices[device_number];
    if (!dev->in_use || dev->bar_size[bar_index] == 0) {
        return 0;
    }
    p = dev->config + 0x10 + bar_index * 4;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int hype_pci_memory_space_enabled(const hype_pci_t *pci, uint8_t device_number) {
    const hype_pci_device_t *dev;
    uint16_t command;

    if (device_number >= HYPE_PCI_MAX_DEVICES) {
        return 0;
    }
    dev = &pci->devices[device_number];
    if (!dev->in_use) {
        return 0;
    }
    command = (uint16_t)dev->config[0x04] | ((uint16_t)dev->config[0x05] << 8);
    return (command & 0x0002u) != 0;
}

void hype_pci_decode_ecam_offset(uint64_t offset, hype_pci_ecam_addr_t *out) {
    out->bus = (unsigned int)((offset >> HYPE_PCI_ECAM_BUS_SHIFT) & HYPE_PCI_ECAM_BUS_MASK);
    out->device = (unsigned int)((offset >> HYPE_PCI_ECAM_DEVICE_SHIFT) & HYPE_PCI_ECAM_DEVICE_MASK);
    out->function = (unsigned int)((offset >> HYPE_PCI_ECAM_FUNCTION_SHIFT) & HYPE_PCI_ECAM_FUNCTION_MASK);
    out->register_offset = (unsigned int)(offset & HYPE_PCI_ECAM_REGISTER_MASK);
}

static int is_present_function(const hype_pci_t *pci, const hype_pci_ecam_addr_t *addr) {
    if (addr->bus != 0 || addr->function != 0 || addr->device >= HYPE_PCI_MAX_DEVICES) {
        return 0;
    }
    return pci->devices[addr->device].in_use;
}

static int is_bar_register(unsigned int register_offset, uint8_t size_bytes) {
    return size_bytes == 4 && register_offset >= 0x10 && register_offset <= 0x24 &&
           (register_offset % 4) == 0;
}

void hype_pci_config_read(const hype_pci_t *pci, const hype_pci_ecam_addr_t *addr, uint8_t size_bytes,
                           uint32_t *out_value) {
    const hype_pci_device_t *dev;
    uint32_t value;
    unsigned int i;

    if (!is_present_function(pci, addr)) {
        *out_value = (size_bytes == 4) ? 0xFFFFFFFFu : (size_bytes == 2) ? 0xFFFFu : 0xFFu;
        return;
    }

    dev = &pci->devices[addr->device];
    value = 0;
    for (i = 0; i < size_bytes && (addr->register_offset + i) < HYPE_PCI_CONFIG_SIZE; i++) {
        value |= (uint32_t)dev->config[addr->register_offset + i] << (8 * i);
    }
    *out_value = value;
}

void hype_pci_config_write(hype_pci_t *pci, const hype_pci_ecam_addr_t *addr, uint8_t size_bytes,
                            uint32_t value) {
    hype_pci_device_t *dev;
    unsigned int i;

    if (!is_present_function(pci, addr)) {
        return;
    }
    dev = &pci->devices[addr->device];

    if (is_bar_register(addr->register_offset, size_bytes)) {
        unsigned int bar_index = (addr->register_offset - 0x10) / 4;
        uint32_t stored;

        if (dev->bar_size[bar_index] == 0) {
            stored = 0;
        } else if (value == 0xFFFFFFFFu) {
            stored = ~(dev->bar_size[bar_index] - 1);
        } else {
            stored = value & ~(dev->bar_size[bar_index] - 1);
        }
        write_le16(dev->config + addr->register_offset, (uint16_t)stored);
        write_le16(dev->config + addr->register_offset + 2, (uint16_t)(stored >> 16));
        return;
    }

    for (i = 0; i < size_bytes && (addr->register_offset + i) < HYPE_PCI_CONFIG_SIZE; i++) {
        dev->config[addr->register_offset + i] = (uint8_t)(value >> (8 * i));
    }
}

void hype_pci_cf8_write(hype_pci_t *pci, uint32_t value) {
    pci->cf8_selected = value;
}

uint32_t hype_pci_cf8_read(const hype_pci_t *pci) {
    return pci->cf8_selected;
}

void hype_pci_decode_cf8_address(uint32_t cf8_value, hype_pci_ecam_addr_t *out) {
    out->bus = (unsigned int)((cf8_value >> HYPE_PCI_CF8_BUS_SHIFT) & HYPE_PCI_ECAM_BUS_MASK);
    out->device = (unsigned int)((cf8_value >> HYPE_PCI_CF8_DEVICE_SHIFT) & HYPE_PCI_ECAM_DEVICE_MASK);
    out->function = (unsigned int)((cf8_value >> HYPE_PCI_CF8_FUNCTION_SHIFT) & HYPE_PCI_ECAM_FUNCTION_MASK);
    out->register_offset = (unsigned int)(cf8_value & HYPE_PCI_CF8_REGISTER_MASK);
}

void hype_pci_cf8_config_read(const hype_pci_t *pci, unsigned int byte_offset, uint8_t size_bytes,
                               uint32_t *out_value) {
    hype_pci_ecam_addr_t addr;

    hype_pci_decode_cf8_address(pci->cf8_selected, &addr);
    addr.register_offset += byte_offset;
    hype_pci_config_read(pci, &addr, size_bytes, out_value);
}

void hype_pci_cf8_config_write(hype_pci_t *pci, unsigned int byte_offset, uint8_t size_bytes, uint32_t value) {
    hype_pci_ecam_addr_t addr;

    hype_pci_decode_cf8_address(pci->cf8_selected, &addr);
    addr.register_offset += byte_offset;
    hype_pci_config_write(pci, &addr, size_bytes, value);
}
