#include "pci.h"

static void write_le16(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static uint16_t read_le16(const uint8_t *src) {
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static unsigned int msi_cap_offset(const hype_pci_device_t *dev) {
    unsigned int offset = dev->config[HYPE_PCI_CAP_PTR_OFFSET];
    unsigned int hops;

    for (hops = 0; offset >= 0x40u && offset + 2u <= HYPE_PCI_CONFIG_SIZE && hops < 48u; hops++) {
        if (dev->config[offset] == HYPE_PCI_CAP_ID_MSI) return offset;
        offset = dev->config[offset + 1u];
    }
    return 0;
}

static void clear_device(hype_pci_device_t *dev) {
    unsigned int i;

    dev->in_use = 0;
    for (i = 0; i < HYPE_PCI_CONFIG_SIZE; i++) {
        dev->config[i] = 0;
    }
    for (i = 0; i < 6; i++) {
        dev->bar_size[i] = 0;
        dev->bar_is_io[i] = 0;
    }
}

static hype_pci_device_t *device_mut(hype_pci_t *pci, uint8_t device_number, uint8_t function_number) {
    if (device_number >= HYPE_PCI_MAX_DEVICES || function_number >= HYPE_PCI_MAX_FUNCTIONS) {
        return 0;
    }
    return function_number == 0 ? &pci->devices[device_number]
                                : &pci->functions[device_number][function_number - 1u];
}

static const hype_pci_device_t *device_const(const hype_pci_t *pci, uint8_t device_number,
                                              uint8_t function_number) {
    if (device_number >= HYPE_PCI_MAX_DEVICES || function_number >= HYPE_PCI_MAX_FUNCTIONS) {
        return 0;
    }
    return function_number == 0 ? &pci->devices[device_number]
                                : &pci->functions[device_number][function_number - 1u];
}

void hype_pci_reset(hype_pci_t *pci) {
    unsigned int i, f;

    for (i = 0; i < HYPE_PCI_MAX_DEVICES; i++) {
        clear_device(&pci->devices[i]);
        for (f = 0; f < HYPE_PCI_MAX_FUNCTIONS - 1u; f++) {
            clear_device(&pci->functions[i][f]);
        }
    }
    pci->cf8_selected = 0;
}

int hype_pci_add_device(hype_pci_t *pci, uint8_t device_number, uint16_t vendor_id, uint16_t device_id,
                         uint8_t class_base, uint8_t class_sub, uint8_t class_interface) {
    hype_pci_device_t *dev;

    if (device_number >= HYPE_PCI_MAX_DEVICES) {
        return -1;
    }

    dev = &pci->devices[device_number];
    clear_device(dev);
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

int hype_pci_add_function(hype_pci_t *pci, uint8_t device_number, uint8_t function_number,
                          uint16_t vendor_id, uint16_t device_id, uint8_t class_base,
                          uint8_t class_sub, uint8_t class_interface) {
    hype_pci_device_t *dev;

    if (device_number >= HYPE_PCI_MAX_DEVICES || function_number == 0 ||
        function_number >= HYPE_PCI_MAX_FUNCTIONS || !pci->devices[device_number].in_use) {
        return -1;
    }

    dev = &pci->functions[device_number][function_number - 1u];
    clear_device(dev);
    dev->in_use = 1;
    write_le16(dev->config + 0x00, vendor_id);
    write_le16(dev->config + 0x02, device_id);
    dev->config[0x08] = 0;
    dev->config[0x09] = class_interface;
    dev->config[0x0A] = class_sub;
    dev->config[0x0B] = class_base;
    dev->config[0x0E] = 0x00;
    pci->devices[device_number].config[0x0E] |= 0x80u;
    return 0;
}

void hype_pci_set_bar_size(hype_pci_t *pci, uint8_t device_number, unsigned int bar_index, uint32_t size) {
    hype_pci_set_function_bar_size(pci, device_number, 0, bar_index, size);
}

void hype_pci_set_function_bar_size(hype_pci_t *pci, uint8_t device_number, uint8_t function_number,
                                    unsigned int bar_index, uint32_t size) {
    hype_pci_device_t *dev = device_mut(pci, device_number, function_number);

    if (!dev || !dev->in_use || bar_index >= 6) {
        return;
    }
    dev->bar_size[bar_index] = size;
    dev->bar_is_io[bar_index] = 0;
}

void hype_pci_set_io_bar_size(hype_pci_t *pci, uint8_t device_number, unsigned int bar_index,
                              uint32_t size) {
    hype_pci_set_function_io_bar_size(pci, device_number, 0, bar_index, size);
}

void hype_pci_set_function_io_bar_size(hype_pci_t *pci, uint8_t device_number, uint8_t function_number,
                                       unsigned int bar_index, uint32_t size) {
    hype_pci_device_t *dev = device_mut(pci, device_number, function_number);

    if (!dev || !dev->in_use || bar_index >= 6) {
        return;
    }
    dev->bar_size[bar_index] = size;
    dev->bar_is_io[bar_index] = 1;
}

int hype_pci_add_ich9_ahci_function(hype_pci_t *pci, uint8_t device_number) {
    hype_pci_device_t *dev;

    if (hype_pci_add_function(pci, device_number, 2u, 0x8086u, 0x2922u,
                              0x01u, 0x06u, 0x01u) != 0) {
        return -1;
    }
    dev = device_mut(pci, device_number, 2u);
    dev->config[0x08] = 0x02u; /* ICH9 AHCI revision */
    dev->config[0x0C] = 0x08u; /* cache line size */
    dev->config[0x34] = 0x80u; /* MSI -> SATA capability chain */
    dev->config[0x06] |= 0x10u;
    dev->config[0x80] = 0x05u; /* MSI */
    dev->config[0x81] = 0xA8u;
    dev->config[0x82] = 0x80u; /* one 64-bit-message capable vector */
    dev->config[0x90] = 0x40u; /* AHCI mode */
    dev->config[0xA8] = 0x12u; /* SATA capability */
    dev->config[0xAA] = 0x10u; /* SATA capability revision */
    dev->config[0xAC] = 0x48u; /* index-data port: BAR4 + log2(16) */
    hype_pci_set_function_io_bar_size(pci, device_number, 2u, 0u, 8u);
    hype_pci_set_function_io_bar_size(pci, device_number, 2u, 1u, 4u);
    hype_pci_set_function_io_bar_size(pci, device_number, 2u, 2u, 8u);
    hype_pci_set_function_io_bar_size(pci, device_number, 2u, 3u, 4u);
    hype_pci_set_function_io_bar_size(pci, device_number, 2u, 4u, 32u);
    hype_pci_set_function_bar_size(pci, device_number, 2u, 5u, 0x800u);
    hype_pci_set_function_interrupt(pci, device_number, 2u, 1u, 0u);
    return 0;
}

void hype_pci_set_interrupt(hype_pci_t *pci, uint8_t device_number, uint8_t int_pin, uint8_t int_line) {
    hype_pci_set_function_interrupt(pci, device_number, 0, int_pin, int_line);
}

void hype_pci_set_function_interrupt(hype_pci_t *pci, uint8_t device_number, uint8_t function_number,
                                     uint8_t int_pin, uint8_t int_line) {
    hype_pci_device_t *dev = device_mut(pci, device_number, function_number);

    if (!dev || !dev->in_use) {
        return;
    }
    dev->config[0x3C] = int_line;
    dev->config[0x3D] = int_pin;
}

void hype_pci_set_msi_capability(hype_pci_t *pci, uint8_t device_number) {
    hype_pci_set_function_msi_capability(pci, device_number, 0);
}

void hype_pci_set_function_msi_capability(hype_pci_t *pci, uint8_t device_number, uint8_t function_number) {
    hype_pci_device_t *dev = device_mut(pci, device_number, function_number);

    if (!dev || !dev->in_use) {
        return;
    }
    write_le16(dev->config + 0x06,
               read_le16(dev->config + 0x06) | HYPE_PCI_STATUS_CAPABILITIES_LIST);
    dev->config[HYPE_PCI_CAP_PTR_OFFSET] = HYPE_PCI_MSI_CAP_OFFSET;
    dev->config[HYPE_PCI_MSI_CAP_OFFSET] = HYPE_PCI_CAP_ID_MSI;
    dev->config[HYPE_PCI_MSI_CAP_OFFSET + 1u] = 0; /* end of capability list */
    write_le16(dev->config + HYPE_PCI_MSI_CONTROL_OFFSET, 0); /* one 32-bit message */
}

int hype_pci_msi_enabled(const hype_pci_t *pci, uint8_t device_number) {
    return hype_pci_function_msi_enabled(pci, device_number, 0);
}

int hype_pci_function_msi_enabled(const hype_pci_t *pci, uint8_t device_number, uint8_t function_number) {
    const hype_pci_device_t *dev = device_const(pci, device_number, function_number);
    unsigned int offset;

    if (!dev) return 0;
    offset = dev->in_use ? msi_cap_offset(dev) : 0;
    return offset != 0 && (read_le16(dev->config + offset + 2u) & HYPE_PCI_MSI_CONTROL_ENABLE) != 0;
}

uint8_t hype_pci_msi_vector(const hype_pci_t *pci, uint8_t device_number) {
    return hype_pci_function_msi_vector(pci, device_number, 0);
}

uint8_t hype_pci_function_msi_vector(const hype_pci_t *pci, uint8_t device_number,
                                     uint8_t function_number) {
    const hype_pci_device_t *dev = device_const(pci, device_number, function_number);
    unsigned int offset, data_offset;

    if (!dev) return 0;
    if (!dev->in_use) {
        return 0;
    }
    offset = msi_cap_offset(dev);
    if (offset == 0) return 0;
    data_offset = offset + ((read_le16(dev->config + offset + 2u) & 0x0080u) ? 12u : 8u);
    return data_offset < HYPE_PCI_CONFIG_SIZE ? dev->config[data_offset] : 0;
}

uint32_t hype_pci_msi_dest(const hype_pci_t *pci, uint8_t device_number, int *logical) {
    return hype_pci_function_msi_dest(pci, device_number, 0, logical);
}

uint32_t hype_pci_function_msi_dest(const hype_pci_t *pci, uint8_t device_number,
                                    uint8_t function_number, int *logical) {
    const hype_pci_device_t *dev = device_const(pci, device_number, function_number);
    unsigned int offset;
    uint32_t addr;

    if (logical) *logical = 0;
    if (!dev || !dev->in_use) return 0;
    offset = msi_cap_offset(dev);
    if (offset == 0 || offset + 8u > HYPE_PCI_CONFIG_SIZE) return 0;
    addr = (uint32_t)dev->config[offset + 4u] | ((uint32_t)dev->config[offset + 5u] << 8) |
           ((uint32_t)dev->config[offset + 6u] << 16) | ((uint32_t)dev->config[offset + 7u] << 24);
    /* MSI address word (SDM 3A 11.11.1): destination ID in bits 19:12; the message is
     * logical-destination lowest-priority only when BOTH the Redirection Hint (bit 3) and
     * Destination Mode (bit 2) are set -- with RH clear, DM is ignored and delivery is
     * physical/fixed to the destination ID. */
    if (logical) *logical = ((addr & 0x8u) != 0u) && ((addr & 0x4u) != 0u);
    return (addr >> 12) & 0xFFu;
}

uint8_t hype_pci_get_interrupt_line(const hype_pci_t *pci, uint8_t device_number) {
    return hype_pci_get_function_interrupt_line(pci, device_number, 0);
}

uint8_t hype_pci_get_function_interrupt_line(const hype_pci_t *pci, uint8_t device_number,
                                             uint8_t function_number) {
    const hype_pci_device_t *dev = device_const(pci, device_number, function_number);

    return dev && dev->in_use ? dev->config[0x3C] : 0;
}

uint32_t hype_pci_get_bar_value(const hype_pci_t *pci, uint8_t device_number, unsigned int bar_index) {
    return hype_pci_get_function_bar_value(pci, device_number, 0, bar_index);
}

uint32_t hype_pci_get_function_bar_value(const hype_pci_t *pci, uint8_t device_number,
                                         uint8_t function_number, unsigned int bar_index) {
    const hype_pci_device_t *dev = device_const(pci, device_number, function_number);
    const uint8_t *p;

    if (!dev || bar_index >= 6) {
        return 0;
    }
    if (!dev->in_use || dev->bar_size[bar_index] == 0) {
        return 0;
    }
    p = dev->config + 0x10 + bar_index * 4;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int hype_pci_memory_space_enabled(const hype_pci_t *pci, uint8_t device_number) {
    return hype_pci_function_memory_space_enabled(pci, device_number, 0);
}

int hype_pci_function_memory_space_enabled(const hype_pci_t *pci, uint8_t device_number,
                                           uint8_t function_number) {
    const hype_pci_device_t *dev = device_const(pci, device_number, function_number);
    uint16_t command;

    if (!dev || !dev->in_use) {
        return 0;
    }
    command = (uint16_t)dev->config[0x04] | ((uint16_t)dev->config[0x05] << 8);
    return (command & 0x0002u) != 0;
}

int hype_pci_bus_master_enabled(const hype_pci_t *pci, uint8_t device_number) {
    return hype_pci_function_bus_master_enabled(pci, device_number, 0);
}

int hype_pci_function_bus_master_enabled(const hype_pci_t *pci, uint8_t device_number,
                                         uint8_t function_number) {
    const hype_pci_device_t *dev = device_const(pci, device_number, function_number);
    uint16_t command;

    if (!dev || !dev->in_use) {
        return 0;
    }
    command = (uint16_t)dev->config[0x04] | ((uint16_t)dev->config[0x05] << 8);
    return (command & 0x0004u) != 0;
}

const uint8_t *hype_pci_function_config(const hype_pci_t *pci, uint8_t device_number,
                                        uint8_t function_number) {
    const hype_pci_device_t *dev = device_const(pci, device_number, function_number);

    return dev && dev->in_use ? dev->config : 0;
}

void hype_pci_decode_ecam_offset(uint64_t offset, hype_pci_ecam_addr_t *out) {
    out->bus = (unsigned int)((offset >> HYPE_PCI_ECAM_BUS_SHIFT) & HYPE_PCI_ECAM_BUS_MASK);
    out->device = (unsigned int)((offset >> HYPE_PCI_ECAM_DEVICE_SHIFT) & HYPE_PCI_ECAM_DEVICE_MASK);
    out->function = (unsigned int)((offset >> HYPE_PCI_ECAM_FUNCTION_SHIFT) & HYPE_PCI_ECAM_FUNCTION_MASK);
    out->register_offset = (unsigned int)(offset & HYPE_PCI_ECAM_REGISTER_MASK);
}

static int is_present_function(const hype_pci_t *pci, const hype_pci_ecam_addr_t *addr) {
    const hype_pci_device_t *dev;

    if (addr->bus != 0) {
        return 0;
    }
    dev = device_const(pci, (uint8_t)addr->device, (uint8_t)addr->function);
    return dev && dev->in_use;
}

static int is_bar_register(unsigned int register_offset, uint8_t size_bytes) {
    return size_bytes == 4 && register_offset >= 0x10 && register_offset <= 0x24 &&
           (register_offset % 4) == 0;
}

static int is_msi_hardware_owned_byte(const hype_pci_device_t *dev, unsigned int offset) {
    unsigned int msi_offset = msi_cap_offset(dev);
    return offset == 0x06u || offset == 0x07u || offset == HYPE_PCI_CAP_PTR_OFFSET ||
           (msi_offset != 0 && (offset == msi_offset || offset == msi_offset + 1u));
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

    dev = device_const(pci, (uint8_t)addr->device, (uint8_t)addr->function);
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
    dev = device_mut(pci, (uint8_t)addr->device, (uint8_t)addr->function);

    if (is_bar_register(addr->register_offset, size_bytes)) {
        unsigned int bar_index = (addr->register_offset - 0x10) / 4;
        uint32_t stored;

        if (dev->bar_size[bar_index] == 0) {
            stored = 0;
        } else if (value == 0xFFFFFFFFu) {
            stored = ~(dev->bar_size[bar_index] - 1u);
            if (dev->bar_is_io[bar_index]) stored |= 1u;
        } else {
            stored = value & ~(dev->bar_size[bar_index] - 1u);
            if (dev->bar_is_io[bar_index]) stored |= 1u;
        }
        write_le16(dev->config + addr->register_offset, (uint16_t)stored);
        write_le16(dev->config + addr->register_offset + 2, (uint16_t)(stored >> 16));
        return;
    }

    for (i = 0; i < size_bytes && (addr->register_offset + i) < HYPE_PCI_CONFIG_SIZE; i++) {
        unsigned int offset = addr->register_offset + i;
        if (!is_msi_hardware_owned_byte(dev, offset)) {
            dev->config[offset] = (uint8_t)(value >> (8 * i));
        }
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
