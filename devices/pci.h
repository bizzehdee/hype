#ifndef HYPE_DEVICES_PCI_H
#define HYPE_DEVICES_PCI_H

#include <stdint.h>

/*
 * Minimal PCI configuration-space model (PCI-1), accessed via ECAM
 * (Enhanced Configuration Access Mechanism) -- the flat MMIO region
 * this project's own synthesized ACPI MCFG table (devices/acpi.h,
 * M4-4) already describes to guest firmware, previously unbacked by
 * any actual device model (devices/acpi.h's own comment flagged
 * exactly this gap: "not yet backed by a real MMIO/PCI config-space
 * device model -- a future PCI milestone's job"). Without this, a real
 * guest's PCI bus driver (UEFI's PciBusDxe, or Linux's own PCI core)
 * has no way to discover any device at all, including M4-5's AHCI
 * controller -- that wiring is PCI-2's job, once this generic model
 * exists.
 *
 * Real PCI configuration-space register layout (Vendor/Device ID,
 * Command/Status, Class Code, Header Type, BARs, ...) is stable,
 * decades-old, unchanging spec knowledge -- offsets transcribed
 * directly from the PCI Local Bus Specification's Type 0 (normal,
 * single-function device) configuration header, the same layout every
 * real PCI device and OS/firmware driver already assumes. This
 * project's own scope: bus 0 only (no PCI-to-PCI bridges modeled, so
 * no further bus is ever reachable -- every device presents a Type 0,
 * not Type 1, header, meaning compliant firmware has no reason to look
 * for one). A device can expose up to eight Type 0 functions. Function 0
 * owns the PCI multifunction bit, and functions 1--7 are absent unless
 * explicitly registered.
 *
 * ECAM address decoding (PCI Express spec): a config-space MMIO
 * access's offset within the whole MCFG-described region encodes which
 * bus/device/function/register it targets:
 *   offset = (bus << 20) | (device << 15) | (function << 12) | register
 * Bus 0's own config space is the first 1MB (32 devices * 8 functions *
 * 4KB) of that region -- comfortably within a single 2MB NPT entry, the
 * same not-present-then-NPF-trap mechanism M4-3/M4-5's own MMIO devices
 * already use (hype_npt_mark_not_present(), arch/x86_64/svm/npt.h).
 *
 * A config-space access architecturally never faults/errors the way a
 * real memory access can -- accessing an absent device/function/bus
 * simply reads back all-1s (0xFFFFFFFF, truncated to the access
 * width), the standard convention every PCI bus-walk implementation
 * relies on to know where the device list ends; a write to an absent
 * device is silently dropped. hype_pci_config_read()/_write() reflect
 * this: they always succeed, there is no "unrecognized access" failure
 * mode the way every other device model in this project has.
 */

#define HYPE_PCI_ECAM_BUS_SHIFT 20
#define HYPE_PCI_ECAM_DEVICE_SHIFT 15
#define HYPE_PCI_ECAM_FUNCTION_SHIFT 12
#define HYPE_PCI_ECAM_REGISTER_MASK 0xFFFu
#define HYPE_PCI_ECAM_DEVICE_MASK 0x1Fu
#define HYPE_PCI_ECAM_FUNCTION_MASK 0x7u
#define HYPE_PCI_ECAM_BUS_MASK 0xFFu

/* Bus 0's own share of the whole (nominally much larger) MCFG-
 * described ECAM region: 32 devices * 8 functions * 4KB = 1MB. This
 * project only ever backs bus 0 (PCI-1), so this is also the exact
 * size the exempt NPF glue (arch/x86_64/svm/svm_vcpu.c's
 * hype_svm_vcpu_handle_pci_ecam_npf()) should treat as "mine, not some
 * other device's" -- needed once PCI-2 introduces a second NPT-trapped
 * region (a device's own dynamically-BAR-programmed MMIO window) that
 * could otherwise be mistaken for an ECAM access if only a lower bound
 * were checked. */
#define HYPE_PCI_ECAM_BUS0_SIZE 0x100000u

/* This project's own placeholder vendor ID -- not a real PCI-SIG
 * assignment, and deliberately not pretending to be any real vendor's
 * hardware (same "honest, not pretending compatibility" choice as
 * CPUMSR-1's "HypeHypeHype" CPUID hypervisor signature). Real AHCI/
 * class-code-based drivers bind on class code, not vendor/device ID,
 * so this has no effect on real-world driver compatibility. */
#define HYPE_PCI_VENDOR_ID_HYPE 0xFFFEu

#define HYPE_PCI_MAX_DEVICES 32
#define HYPE_PCI_MAX_FUNCTIONS 8
#define HYPE_PCI_CONFIG_SIZE 256

/* PCI MSI is a conventional capability, linked from config 0x34.  Keep it
 * out of the Type 0 header and at a fixed, naturally aligned location so the
 * simple 256-byte configuration model remains sufficient.  This model
 * implements one 32-bit message only: that is enough for an AHCI HBA's single
 * completion source, and avoids advertising MSI-X or multiple vectors that
 * the interrupt delivery side does not implement. */
#define HYPE_PCI_STATUS_CAPABILITIES_LIST 0x0010u
#define HYPE_PCI_CAP_PTR_OFFSET 0x34u
#define HYPE_PCI_MSI_CAP_OFFSET 0x50u
#define HYPE_PCI_CAP_ID_MSI 0x05u
#define HYPE_PCI_MSI_CONTROL_ENABLE 0x0001u
#define HYPE_PCI_MSI_CONTROL_OFFSET (HYPE_PCI_MSI_CAP_OFFSET + 0x02u)
#define HYPE_PCI_MSI_ADDR_OFFSET (HYPE_PCI_MSI_CAP_OFFSET + 0x04u)
#define HYPE_PCI_MSI_DATA_OFFSET (HYPE_PCI_MSI_CAP_OFFSET + 0x08u)

typedef struct {
    uint8_t config[HYPE_PCI_CONFIG_SIZE];
    /* 0 = this BAR index isn't implemented (always reads 0, never
     * sized) -- only 32-bit, non-prefetchable memory BARs are modeled;
     * this project's own scope has no need for I/O-space or 64-bit
     * BARs yet. Must be a power of two when nonzero. */
    uint32_t bar_size[6];
    /* BAR type bit (config bit 0): I/O-space when set, memory-space when
     * clear.  Both forms use the same power-of-two sizing protocol. */
    uint8_t bar_is_io[6];
    int in_use;
} hype_pci_device_t;

typedef struct {
    /* Function 0 stays in this array so existing single-function callers do
     * not need a different data path. functions[d][f - 1] stores f=1..7. */
    hype_pci_device_t devices[HYPE_PCI_MAX_DEVICES];
    hype_pci_device_t functions[HYPE_PCI_MAX_DEVICES][HYPE_PCI_MAX_FUNCTIONS - 1];
    /* Last value written to the legacy 0xCF8 config-address port (FW-1)
     * -- genuinely part of the host bridge's own state, the same way
     * devices/fw_cfg.h's hype_fw_cfg_t keeps its own selected_key/offset
     * internally rather than pushing that state out to the exempt glue
     * layer. */
    uint32_t cf8_selected;
} hype_pci_t;

typedef struct {
    unsigned int bus;
    unsigned int device;
    unsigned int function;
    unsigned int register_offset;
} hype_pci_ecam_addr_t;

/* Resets to no devices registered. Call on every (re)start, same
 * convention as every other device model here. */
void hype_pci_reset(hype_pci_t *pci);

/*
 * Registers a single-function device at bus 0, the given device
 * number (0-31), function 0. Fills the Type 0 header's Vendor/Device ID,
 * Class Code (base/sub/interface), and Revision ID (always 0) fields.
 * Returns 0 on success, -1 if `device_number` is out of range. Pure
 * struct-filling.
 */
int hype_pci_add_device(hype_pci_t *pci, uint8_t device_number, uint16_t vendor_id, uint16_t device_id,
                         uint8_t class_base, uint8_t class_sub, uint8_t class_interface);

/* Registers a non-zero Type 0 function below an already registered function
 * 0. This sets function 0's multifunction bit. Returns -1 for an invalid
 * device/function or if function 0 is absent. */
int hype_pci_add_function(hype_pci_t *pci, uint8_t device_number, uint8_t function_number,
                          uint16_t vendor_id, uint16_t device_id, uint8_t class_base,
                          uint8_t class_sub, uint8_t class_interface);

/*
 * Declares BAR `bar_index` (0-5) of the device at `device_number` as a
 * 32-bit, non-prefetchable memory BAR of `size` bytes (must be a power
 * of two). Must be called after hype_pci_add_device() for that device.
 * Pure struct mutation.
 */
void hype_pci_set_bar_size(hype_pci_t *pci, uint8_t device_number, unsigned int bar_index, uint32_t size);
void hype_pci_set_function_bar_size(hype_pci_t *pci, uint8_t device_number, uint8_t function_number,
                                    unsigned int bar_index, uint32_t size);

/* Declares a 32-bit I/O-space BAR.  This is configuration-space fidelity;
 * callers that expose a live PIO block still route it separately. */
void hype_pci_set_io_bar_size(hype_pci_t *pci, uint8_t device_number, unsigned int bar_index,
                              uint32_t size);
void hype_pci_set_function_io_bar_size(hype_pci_t *pci, uint8_t device_number,
                                       uint8_t function_number, unsigned int bar_index,
                                       uint32_t size);

/* Registers the ICH9 AHCI SATA function at `device_number`. Function 0 must
 * already be the ICH9 LPC bridge; the helper adds function 2 and its complete
 * PCI-visible BAR/capability identity. MMIO/PIO service remains the caller's
 * device-model responsibility. */
int hype_pci_add_ich9_ahci_function(hype_pci_t *pci, uint8_t device_number);

/*
 * Sets a device's Interrupt Pin (config 0x3D) and Interrupt Line
 * (config 0x3C). Interrupt Pin is read-only, firmware-fixed hardware
 * wiring (1=INTA .. 4=INTD; 0 = the function uses no interrupt);
 * Interrupt Line is the ISA/PIC IRQ the platform routed that pin to,
 * which firmware programs and a legacy (no-ACPI-_PRT) guest reads back
 * verbatim to decide which IRQ to request. This project has no ACPI
 * interrupt-routing tables the guest honours, so it must present a
 * usable line here directly. Must be called after hype_pci_add_device()
 * for that device. Pure struct mutation.
 */
void hype_pci_set_interrupt(hype_pci_t *pci, uint8_t device_number, uint8_t int_pin, uint8_t int_line);
void hype_pci_set_function_interrupt(hype_pci_t *pci, uint8_t device_number, uint8_t function_number,
                                     uint8_t int_pin, uint8_t int_line);

/* Adds one 32-bit-message MSI capability to a registered device.  The guest
 * owns the MSI enable bit and message address/data registers through normal
 * config writes; the fixed capability headers and Status/Capabilities Pointer
 * remain hardware-owned. */
void hype_pci_set_msi_capability(hype_pci_t *pci, uint8_t device_number);
void hype_pci_set_function_msi_capability(hype_pci_t *pci, uint8_t device_number, uint8_t function_number);

/* Returns whether the guest has enabled this device's MSI capability and, if
 * so, the x86 interrupt vector in its message data.  Delivery intentionally
 * remains the VMM's responsibility: the guest-programmed APIC message address
 * chooses an in-guest LAPIC destination, while Hype has one active guest vCPU
 * context in which to inject the vector. */
int hype_pci_msi_enabled(const hype_pci_t *pci, uint8_t device_number);
uint8_t hype_pci_msi_vector(const hype_pci_t *pci, uint8_t device_number);
int hype_pci_function_msi_enabled(const hype_pci_t *pci, uint8_t device_number, uint8_t function_number);
uint8_t hype_pci_function_msi_vector(const hype_pci_t *pci, uint8_t device_number, uint8_t function_number);
/* #512: destination ID from the guest-programmed MSI address (bits 19:12); *logical is set
 * iff the message is logical-mode lowest-priority (RH && DM). Returns 0 when the function or
 * its MSI capability is absent. */
uint32_t hype_pci_msi_dest(const hype_pci_t *pci, uint8_t device_number, int *logical);
uint32_t hype_pci_function_msi_dest(const hype_pci_t *pci, uint8_t device_number,
                                    uint8_t function_number, int *logical);

/* Reads back a device's current Interrupt Line (config 0x3C) -- whatever
 * value firmware/the guest last programmed there (a guest reads it to
 * pick which 8259 IRQ to request). Returns 0 for an absent device. Used
 * by the vCPU loop to deliver a device's completion IRQ on the exact
 * line the guest is listening on. Pure struct read. */
uint8_t hype_pci_get_interrupt_line(const hype_pci_t *pci, uint8_t device_number);
uint8_t hype_pci_get_function_interrupt_line(const hype_pci_t *pci, uint8_t device_number,
                                             uint8_t function_number);

/*
 * Reads back a device's current BAR value (whatever a prior
 * hype_pci_config_write() stored there -- either a size-sizing mask
 * response or the guest's own programmed address, aligned to the
 * BAR's own size). 0 if the BAR isn't implemented or the device
 * doesn't exist. Pure struct read -- used by PCI-2 to resolve where
 * the guest actually placed a device's MMIO region, once Memory Space
 * Enable (Command register bit 1, offset 0x04) is set.
 */
uint32_t hype_pci_get_bar_value(const hype_pci_t *pci, uint8_t device_number, unsigned int bar_index);
uint32_t hype_pci_get_function_bar_value(const hype_pci_t *pci, uint8_t device_number,
                                         uint8_t function_number, unsigned int bar_index);

/*
 * True if the device's Command register (offset 0x04) has Memory
 * Space Enable (bit 1) set -- real hardware only actually decodes a
 * BAR's address once this bit is set; PCI-2 uses this to decide
 * whether a device's BAR-programmed MMIO region should currently be
 * NPT-trapped at all.
 */
int hype_pci_memory_space_enabled(const hype_pci_t *pci, uint8_t device_number);
int hype_pci_function_memory_space_enabled(const hype_pci_t *pci, uint8_t device_number,
                                           uint8_t function_number);

/*
 * #372: True if the device's Command register has Bus Master Enable (bit 2) set.
 *
 * On real hardware this gates the device's ability to initiate bus transactions AT ALL. With it
 * clear, a command written to the controller is accepted and simply never completes, and a driver
 * polling for completion spins forever -- which is the single most common cause of "my AHCI
 * command never finishes" and the first thing an experienced driver author checks.
 *
 * hype's emulated devices used to transfer regardless, so a guest driver with that bug worked here
 * and hung on real hardware. This is a FIDELITY predicate, not a safety one: hype's "DMA" is a copy
 * within guest RAM it already owns, so there is nothing to contain -- the point is to refuse work
 * the hardware would refuse, rather than give a passing result for code that is wrong.
 */
int hype_pci_bus_master_enabled(const hype_pci_t *pci, uint8_t device_number);
int hype_pci_function_bus_master_enabled(const hype_pci_t *pci, uint8_t device_number,
                                         uint8_t function_number);

/* Returns a registered function's raw 256-byte config space, or NULL when
 * absent. Intended for diagnostics that report the guest-visible state. */
const uint8_t *hype_pci_function_config(const hype_pci_t *pci, uint8_t device_number,
                                        uint8_t function_number);

/*
 * Decodes an ECAM byte offset (relative to the whole MCFG-described
 * region's own base) into bus/device/function/register. Pure bit
 * extraction, no CPU/guest-memory access of its own.
 */
void hype_pci_decode_ecam_offset(uint64_t offset, hype_pci_ecam_addr_t *out);

/*
 * Config-space read: always succeeds (see this header's own top
 * comment for why config-space accesses never fault). `size_bytes` is
 * 1, 2, or 4, matching hype_mmio_decode()'s own supported access
 * widths. An access against bus != 0, an out-of-range device/function,
 * or an unregistered function reads back as all-1s (masked to
 * size_bytes) -- the standard "no device here" PCI convention. A BAR
 * register (offset 0x10-0x24, 4-byte-aligned
 * access only -- the only form real firmware/OS ever uses to probe/
 * program a BAR) reads back whatever hype_pci_config_write() last
 * stored there; any other register reads directly from the device's
 * own config-space byte buffer.
 */
void hype_pci_config_read(const hype_pci_t *pci, const hype_pci_ecam_addr_t *addr, uint8_t size_bytes,
                           uint32_t *out_value);

/*
 * Config-space write: always succeeds. A write to an absent device is
 * silently dropped. A 4-byte-aligned write to a BAR register (0x10-
 * 0x24) implements the standard PCI BAR sizing/programming protocol:
 * writing all-1s (0xFFFFFFFF) stores back the BAR's own size mask
 * (~(size-1)) for the next read -- how firmware/OS discovers a BAR's
 * size before deciding where to place it; any other value is masked
 * to the BAR's own alignment requirement and stored as the guest's
 * chosen address. An unimplemented BAR (size 0) always stores/reads
 * back 0 regardless of what's written, matching real hardware. Any
 * other register is a plain byte-buffer write, narrowed to
 * `size_bytes` -- Vendor/Device ID, Class Code, Revision ID, and
 * Header Type are all read-only on real hardware, but since nothing in
 * this project's scope ever deliberately writes to them, no extra
 * read-only enforcement is added here (revisit if a real guest driver
 * is ever observed doing so).
 */
void hype_pci_config_write(hype_pci_t *pci, const hype_pci_ecam_addr_t *addr, uint8_t size_bytes,
                            uint32_t value);

/*
 * Legacy CF8/CFC port-based config-space access (PCI Local Bus
 * Specification, "Configuration Address Register"/"Configuration Data
 * Register") -- an older mechanism many guests still probe before, or
 * instead of, ECAM (confirmed: this project's own vendored OVMF reads
 * the host bridge's device ID this way during PlatformPei, well before
 * ACPI's MCFG table -- and by extension ECAM -- would even be parsed).
 * Register layout of the 32-bit address value written to 0xCF8: bit 31
 * = enable (ignored here -- this project has no need to model the
 * "disabled" behavior some real chipsets exhibit when it's clear, the
 * same "config-space access never fails" reasoning as every other
 * access in this header), bits 23:16 = bus, 15:11 = device, 10:8 =
 * function, 7:2 = register (dword-aligned; bits 1:0 are always zero).
 */
#define HYPE_PCI_CF8_PORT 0xCF8u
#define HYPE_PCI_CFC_PORT 0xCFCu
#define HYPE_PCI_CF8_BUS_SHIFT 16
#define HYPE_PCI_CF8_DEVICE_SHIFT 11
#define HYPE_PCI_CF8_FUNCTION_SHIFT 8
#define HYPE_PCI_CF8_REGISTER_MASK 0xFCu

/* Stores the address a later CFC-family access will operate on (a
 * plain OUT to 0xCF8 always succeeds). Pure struct mutation. */
void hype_pci_cf8_write(hype_pci_t *pci, uint32_t value);

/* Reads back the last value written to 0xCF8 (real hardware supports
 * this readback too; this project's OVMF guest doesn't rely on it, but
 * modeling it costs nothing and keeps the port fully bidirectional).
 * Pure struct read. */
uint32_t hype_pci_cf8_read(const hype_pci_t *pci);

/*
 * #518: CONFIG_ADDRESS is a 32-bit register occupying 0xCF8-0xCFB, and software does reach its
 * upper bytes individually -- Linux's pci_check_type1() writes a byte to 0xCFB before testing the
 * register. Decoding only the dword at 0xCF8 left those accesses to the absorb-unknown-port path,
 * where a read returns all-ones and a write is dropped.
 *
 * These splice a byte/word/dword access at `byte_offset` (0..3) into the stored register, so a
 * partial write leaves the other bytes intact and a partial read returns just its own.
 *
 * NOT for 0xCF9: within this register's span that address is the chipset reset register (#94), and
 * the caller must route it there instead. Real chipsets make the same choice.
 */
uint32_t hype_pci_cf8_read_bytes(const hype_pci_t *pci, unsigned int byte_offset,
                                 unsigned int size);
void hype_pci_cf8_write_bytes(hype_pci_t *pci, unsigned int byte_offset, unsigned int size,
                              uint32_t value);

/*
 * Decodes a raw 0xCF8 address value into bus/device/function/register
 * -- pure bit extraction, no CPU/guest-memory access of its own, same
 * shape as hype_pci_decode_ecam_offset().
 */
void hype_pci_decode_cf8_address(uint32_t cf8_value, hype_pci_ecam_addr_t *out);

/*
 * Config-data access through the legacy CFC/CFD/CFE/CFF ports: reads/
 * writes the register the currently-selected 0xCF8 address decodes to,
 * plus `byte_offset` (0-3, from which of CFC/CFD/CFE/CFF the guest
 * used) -- the standard convention for sub-4-byte accesses through
 * this mechanism. Composes hype_pci_decode_cf8_address() with
 * hype_pci_config_read()/_write(); always succeeds, same reasoning as
 * every other config-space access here.
 */
void hype_pci_cf8_config_read(const hype_pci_t *pci, unsigned int byte_offset, uint8_t size_bytes,
                               uint32_t *out_value);
void hype_pci_cf8_config_write(hype_pci_t *pci, unsigned int byte_offset, uint8_t size_bytes, uint32_t value);

#endif /* HYPE_DEVICES_PCI_H */
