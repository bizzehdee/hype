#ifndef HYPE_DEVICES_FW_CFG_H
#define HYPE_DEVICES_FW_CFG_H

#include <stdint.h>

/*
 * QEMU-compatible fw_cfg device (M4-4) -- how guest firmware (this
 * project's own vendored, unmodified OVMF, M4-2) fetches synthesized
 * ACPI content (devices/acpi.h/acpi_loader.h) at boot. Port numbers,
 * well-known selector keys, the file-directory entry format, and the
 * DMA interface's control-bit layout and big-endian wire encoding are
 * all transcribed directly from QEMU's own
 * include/standard-headers/linux/qemu_fw_cfg.h and cross-checked
 * against this project's vendored OVMF driver source
 * (edk2/OvmfPkg/Library/QemuFwCfgLib) -- fetched and read for this
 * task, not reconstructed from memory. Confirmed empirically (real
 * QEMU/KVM run) that OVMF's own DMA-support probe reads the classic
 * FW_CFG_ID item and checks a feature bit -- it does NOT rely on
 * reading back the DMA address register's "QEMU CFG" signature, so
 * this device doesn't need to implement that particular probe path to
 * work with this project's actual vendored OVMF build.
 *
 * Host port layout (all x86 port I/O, dispatched the same way
 * devices/pic.h/pit.h already are -- via IOIO VM-exit, never MMIO):
 *   0x510 (16-bit write)  -- select an item by key ("classic" interface)
 *   0x511 (8-bit read)    -- next byte of the selected item, auto-advancing
 *   0x514 (32-bit write)  -- upper 32 bits of a DMA access-struct's
 *                            guest-physical address (big-endian on the wire)
 *   0x518 (32-bit write)  -- lower 32 bits (also big-endian); this write
 *                            triggers the DMA operation
 *
 * This module is split, same pattern as devices/pflash.h: everything
 * here is pure logic operating on already-resolved values (a "guest
 * data pointer" the caller has already turned a guest-physical address
 * into) -- the actual guest-memory reads/writes (fetching the 16-byte
 * access struct, and the data transfer itself) are the exempt caller's
 * job (arch/x86_64/svm/svm_vcpu.c), same layering M4-3's NPF/pflash
 * wiring already established.
 */

#define HYPE_FW_CFG_KEY_SIGNATURE 0x00u
#define HYPE_FW_CFG_KEY_ID 0x01u
#define HYPE_FW_CFG_KEY_FILE_DIR 0x19u
#define HYPE_FW_CFG_KEY_FILE_FIRST 0x20u

/* FW_CFG_DMA_CTL_* bits (fw_cfg_dma_access.control, low-order bits;
 * bits 16-31 carry the select-key operand when SELECT is set). */
#define HYPE_FW_CFG_DMA_CTL_ERROR (1u << 0)
#define HYPE_FW_CFG_DMA_CTL_READ (1u << 1)
#define HYPE_FW_CFG_DMA_CTL_SKIP (1u << 2)
#define HYPE_FW_CFG_DMA_CTL_SELECT (1u << 3)
#define HYPE_FW_CFG_DMA_CTL_WRITE (1u << 4)

/* Generous cap matching this project's other fixed-size device-model
 * buffers -- this milestone only ever registers 3 files (rsdp, tables,
 * table-loader). */
#define HYPE_FW_CFG_MAX_FILES 8

/* fw_cfg "file name" is up to 56 bytes including the NUL, per spec
 * (FW_CFG_MAX_FILE_PATH). */
#define HYPE_FW_CFG_MAX_FILE_PATH 56

typedef struct {
    const char *name; /* caller-owned, must outlive this fw_cfg instance */
    const uint8_t *data; /* caller-owned */
    uint32_t size;
} hype_fw_cfg_file_t;

typedef struct {
    hype_fw_cfg_file_t files[HYPE_FW_CFG_MAX_FILES];
    uint32_t file_count;
    /* Synthesized FW_CFG_FILE_DIR content: big-endian uint32 count
     * followed by `file_count` 64-byte fw_cfg_file entries -- rebuilt
     * whenever a file is added. */
    uint8_t dir_blob[4 + HYPE_FW_CFG_MAX_FILES * 64];
    uint32_t dir_blob_len;
    uint16_t selected_key;
    uint32_t offset;
    uint32_t dma_addr_high; /* staged between the 0x514 and 0x518 port writes */
} hype_fw_cfg_t;

typedef struct {
    uint32_t control; /* full 32-bit control field, native byte order */
    uint16_t select_key; /* meaningful only if control has the SELECT bit set */
    uint32_t length;
    uint64_t address; /* guest-physical address of the transfer buffer, native byte order */
} hype_fw_cfg_dma_op_t;

/* Resets to no files registered, item 0 selected, offset 0. Call on
 * every (re)start, same convention as every other device model here. */
void hype_fw_cfg_reset(hype_fw_cfg_t *fw);

/*
 * Registers a read-only file (`data`/`size` caller-owned, must outlive
 * `fw`). `name` must be a NUL-terminated ASCII path under 56 bytes.
 * Returns the assigned selector key (>= HYPE_FW_CFG_KEY_FILE_FIRST) on
 * success, -1 if the registry is full or `name` doesn't fit. Rebuilds
 * the FW_CFG_FILE_DIR content. Pure struct/buffer filling.
 */
int hype_fw_cfg_add_file(hype_fw_cfg_t *fw, const char *name, const uint8_t *data, uint32_t size);

/* Classic interface: selects `key` for reading, resetting the read
 * offset to 0 -- port 0x510's write handler. */
void hype_fw_cfg_select(hype_fw_cfg_t *fw, uint16_t key);

/* Classic interface: returns the next byte of the currently selected
 * item and advances the offset -- port 0x511's read handler.
 * Unrecognized key or a read past the item's own size both return 0
 * (matching real hardware's "reads past the end return 0"
 * convention) rather than being treated as an error -- there is no
 * error-reporting channel in the classic interface. */
uint8_t hype_fw_cfg_read_byte(hype_fw_cfg_t *fw);

/*
 * Port 0x514's write handler: stages the upper 32 bits of a DMA
 * access-struct's guest-physical address. `wire_value` is the raw
 * 32-bit value the guest's OUT instruction carried (big-endian on the
 * wire, per spec and confirmed against OVMF's own SwapBytes32 call
 * before writing it) -- the byte-swap back to native order happens
 * inside this function.
 */
void hype_fw_cfg_dma_addr_high(hype_fw_cfg_t *fw, uint32_t wire_value);

/*
 * Port 0x518's write handler: combines `wire_value` (the lower 32
 * bits, also big-endian on the wire) with the previously staged upper
 * half and returns the full, native-byte-order guest-physical address
 * of the access struct. This write is what triggers the DMA operation
 * on real hardware -- the caller's job from here is: read 16 bytes of
 * guest memory at the returned address, hype_fw_cfg_dma_decode() them,
 * resolve the decoded op's own `address` field into a guest pointer,
 * call hype_fw_cfg_dma_execute(), and write the (big-endian) result
 * back into the access struct's Control field in guest memory.
 */
uint64_t hype_fw_cfg_dma_addr_low(hype_fw_cfg_t *fw, uint32_t wire_value);

/*
 * Decodes the 16 raw bytes of a fw_cfg_dma_access structure (control,
 * length, address -- each 4/4/8 bytes, big-endian on the wire per
 * spec) into `*out`, native byte order. Pure bit manipulation.
 */
void hype_fw_cfg_dma_decode(const uint8_t raw[16], hype_fw_cfg_dma_op_t *out);

/*
 * Executes a decoded DMA op against `fw`'s currently selected item and
 * read/write offset. `guest_data_ptr` is a plain pointer the caller has
 * already resolved from `op->address` -- this function never touches
 * guest memory or performs any address translation itself, which is
 * what keeps it fully unit-testable with a plain stack/static buffer
 * standing in for guest memory. SELECT (if set) is applied first, so a
 * single op can select-then-read/write/skip in one call, matching real
 * hardware. WRITE is rejected (HYPE_FW_CFG_DMA_CTL_ERROR) -- every file
 * this project serves via fw_cfg is read-only guest-visible content
 * (ACPI tables/loader script), there is nothing to accept a write into.
 * A read past the selected item's own size fills the remainder with 0,
 * same convention as the classic interface. Returns the value the
 * caller should write back (big-endian) into the guest's Control
 * field: 0 on success, HYPE_FW_CFG_DMA_CTL_ERROR set otherwise.
 */
uint32_t hype_fw_cfg_dma_execute(hype_fw_cfg_t *fw, const hype_fw_cfg_dma_op_t *op, uint8_t *guest_data_ptr);

#endif /* HYPE_DEVICES_FW_CFG_H */
