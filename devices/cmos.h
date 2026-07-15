#ifndef HYPE_DEVICES_CMOS_H
#define HYPE_DEVICES_CMOS_H

#include <stdint.h>

/*
 * Minimal CMOS/RTC device model (FW-1) -- confirmed necessary via
 * source-level investigation of this project's own vendored OVMF
 * (edk2/OvmfPkg/Library/PlatformInitLib/MemDetect.c,
 * PlatformGetSystemMemorySizeBelow4gb()): if fw_cfg's "etc/e820" file
 * doesn't describe a nonzero low-memory size, OVMF falls back to
 * reading CMOS registers 0x34/0x35 ("system memory above 16MB, in
 * 64KB chunks, high/low byte") via the classic index/data port pair.
 * Without this, an unhandled port 0x70/0x71 access reads back all-1s
 * (this project's generic "unhandled IOIO" default), giving OVMF a
 * wildly wrong memory size to work with.
 *
 * Standard PC chipset convention: port 0x70 (write-only in this
 * project's scope) selects a register by index (bit 7 of the written
 * byte is conventionally the NMI-disable bit, not part of the index --
 * masked off here since this project has no NMI model to disable);
 * port 0x71 reads/writes the selected register's own byte in a
 * 128-byte register file. This project only ever gives registers
 * 0x34/0x35 a meaningful value (the real memory-size fallback path);
 * every other register defaults to 0 (RTC time/date fields read as
 * midnight/day zero -- irrelevant to boot correctness, not modeled).
 */

#define HYPE_CMOS_SIZE 128u
#define HYPE_CMOS_INDEX_MASK 0x7Fu
#define HYPE_CMOS_REG_EXTMEM_LOW 0x34u
#define HYPE_CMOS_REG_EXTMEM_HIGH 0x35u

typedef struct {
    uint8_t index;
    uint8_t registers[HYPE_CMOS_SIZE];
} hype_cmos_t;

/* Resets to index 0, every register 0. Call on every (re)start, same
 * convention as every other device model here. */
void hype_cmos_reset(hype_cmos_t *cmos);

/*
 * Populates registers 0x34/0x35 with `size_64kb_units` (the standard
 * "memory above 16MB, in 64KB chunks" encoding
 * PlatformGetSystemMemorySizeBelow4gb() itself decodes) -- the
 * caller's job to compute this from the actual amount of memory this
 * guest should believe it has. Pure struct mutation.
 */
void hype_cmos_set_extended_memory_above_16mb(hype_cmos_t *cmos, uint16_t size_64kb_units);

/* Port 0x70 write: selects a register (bits 0-6; bit 7, conventionally
 * NMI-disable, is masked off -- not modeled). Pure struct mutation. */
void hype_cmos_index_write(hype_cmos_t *cmos, uint8_t value);

/* Port 0x71 read: the currently-selected register's byte. Pure struct
 * read. */
uint8_t hype_cmos_data_read(const hype_cmos_t *cmos);

/* Port 0x71 write: stores into the currently-selected register. Pure
 * struct mutation. */
void hype_cmos_data_write(hype_cmos_t *cmos, uint8_t value);

#endif /* HYPE_DEVICES_CMOS_H */
