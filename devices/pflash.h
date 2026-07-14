#ifndef HYPE_DEVICES_PFLASH_H
#define HYPE_DEVICES_PFLASH_H

#include <stdint.h>

/*
 * Guest-visible CFI (Common Flash Interface) NOR flash emulation
 * (M4-3) -- what a guest's OVMF-based firmware actually expects to
 * find at its "OVMF_VARS.fd" flash region (Intel/Sharp Extended CFI
 * command set; no AMD-style unlock sequence needed). Backs the guest's
 * UEFI variable store (boot order, Secure Boot state, ...), which
 * EDK2's own variable driver inside the guest firmware entirely
 * manages the on-disk format of -- this device only needs to emulate
 * the flash chip's command/status protocol, not understand the
 * variable-store byte format at all.
 *
 * Real hardware (and QEMU's own pflash_cfi01 model) keeps the region
 * directly memory-mapped for ordinary reads while idle ("ROMD" mode)
 * and only traps once a command byte is written, for as long as that
 * command sequence takes. This project's own minimal scope always
 * traps every access instead (see devices/pflash.h's caller for the
 * NPT wiring) -- simpler, and correctness-focused rather than
 * performance-focused, matching this project's own stated goals; this
 * struct's `mode` field is what a caller uses to decide what a
 * READ should currently return (backing array data, or a synthesized
 * status/device-ID/query value).
 *
 * Command set implemented (per OVMF's own
 * QemuFlashFvbServicesRuntimeDxe/QemuFlash.c driver, the only commands
 * it actually issues): READ_ARRAY (0xFF), READ_STATUS (0x70),
 * CLEAR_STATUS (0x50), READ_DEVID (0x90), WRITE_BYTE (0x10 + data),
 * BLOCK_ERASE (0x20 + confirm 0xD0), WRITE_TO_BUFFER (0xE8 + count +
 * data... + confirm 0xD0).
 */

#define HYPE_PFLASH_CMD_WRITE_BYTE 0x10u
#define HYPE_PFLASH_CMD_BLOCK_ERASE 0x20u
#define HYPE_PFLASH_CMD_ERASE_CONFIRM 0xD0u
#define HYPE_PFLASH_CMD_CLEAR_STATUS 0x50u
#define HYPE_PFLASH_CMD_READ_STATUS 0x70u
#define HYPE_PFLASH_CMD_READ_DEVID 0x90u
#define HYPE_PFLASH_CMD_WRITE_TO_BUFFER 0xE8u
#define HYPE_PFLASH_CMD_READ_ARRAY 0xFFu

/* Status register bits this project actually sets. */
#define HYPE_PFLASH_STATUS_READY 0x80u        /* Write State Machine ready (always, this stub never "busies") */
#define HYPE_PFLASH_STATUS_PROGRAM_ERROR 0x10u /* set if a write/erase targets an out-of-range offset */

typedef enum {
    HYPE_PFLASH_MODE_READ_ARRAY,
    HYPE_PFLASH_MODE_READ_STATUS,
    HYPE_PFLASH_MODE_READ_DEVID,
    HYPE_PFLASH_MODE_WRITE_BYTE_PENDING,   /* 0x10 issued, next write is the data byte */
    HYPE_PFLASH_MODE_ERASE_PENDING,        /* 0x20 issued, next write must be 0xD0 at the same offset */
    HYPE_PFLASH_MODE_BUFFER_COUNT_PENDING, /* 0xE8 issued, next write is the byte count */
    HYPE_PFLASH_MODE_BUFFER_DATA_PENDING,  /* collecting `buffer_remaining` data bytes */
    HYPE_PFLASH_MODE_BUFFER_CONFIRM_PENDING /* all data collected, waiting for 0xD0 */
} hype_pflash_mode_t;

#define HYPE_PFLASH_MAX_BUFFER_WRITE 512

typedef struct {
    uint8_t *backing;        /* caller-owned array of `size` bytes -- the actual variable-store content */
    uint32_t size;
    hype_pflash_mode_t mode;
    uint8_t status;
    uint32_t erase_offset;       /* offset BLOCK_ERASE was issued at, for the confirm's sanity check */
    uint32_t buffer_offset;      /* offset WRITE_TO_BUFFER was issued at */
    uint16_t buffer_remaining;   /* bytes still expected before the confirm */
    uint16_t buffer_pos;         /* write cursor into buffer_data/offset from buffer_offset */
    uint8_t buffer_data[HYPE_PFLASH_MAX_BUFFER_WRITE];
} hype_pflash_t;

/* Binds `backing`/`size` and resets to READ_ARRAY mode, status clear.
 * Call on every (re)start, same as every other guest-visible state in
 * this project -- but note this does NOT zero `backing` itself
 * (M2-6's guest-RAM-zeroing invariant applies to *guest RAM*; this is
 * the guest's *persistent variable store*, which by definition must
 * survive a guest restart -- the caller decides whether/when to
 * initialize backing's contents, e.g. from a fresh empty store on
 * first use or a previously persisted one). */
void hype_pflash_reset(hype_pflash_t *pf, uint8_t *backing, uint32_t size);

/*
 * Handles a guest read of `size_bytes` (1, 2, or 4) at `offset` (byte
 * offset within the flash region), filling *out_value. In
 * HYPE_PFLASH_MODE_READ_ARRAY, returns backing[offset..]; in
 * READ_STATUS/READ_DEVID, returns a synthesized value replicated
 * across the read width (matching real CFI chips, which repeat a
 * 8/16-bit status/ID value across the bus width). Returns 0 if
 * `offset` is in range, non-zero otherwise (caller's job to decide
 * whether an out-of-range access is fatal).
 */
int hype_pflash_read(hype_pflash_t *pf, uint32_t offset, uint8_t size_bytes, uint32_t *out_value);

/*
 * Handles a guest write of `value` (`size_bytes` wide) at `offset`.
 * Interprets `value`'s low byte as a CFI command when in READ_ARRAY/
 * READ_STATUS/READ_DEVID mode (idle), or as command-sequence data
 * otherwise (the data byte after WRITE_BYTE, the confirm after
 * BLOCK_ERASE/WRITE_TO_BUFFER, the count/data bytes of a buffered
 * write). Returns 0 if handled, non-zero if `offset` is out of range
 * or a confirm byte doesn't match what was expected (a malformed
 * command sequence -- real hardware would also reject this).
 */
int hype_pflash_write(hype_pflash_t *pf, uint32_t offset, uint8_t size_bytes, uint32_t value);

#endif /* HYPE_DEVICES_PFLASH_H */
