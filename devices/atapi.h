#ifndef HYPE_DEVICES_ATAPI_H
#define HYPE_DEVICES_ATAPI_H

#include <stdint.h>
#include "../core/chunked_iso.h"

/*
 * ATAPI/SCSI command layer for the virtual optical drive (M4-5). An
 * ATAPI device is addressed through a SCSI Command Descriptor Block
 * (CDB) carried inside an ATA PACKET command (arch/x86_64/svm's AHCI
 * glue extracts this CDB from the command table and hands it here) --
 * this module only ever sees the CDB bytes and the backing media, no
 * AHCI/FIS/PRDT structure at all, matching devices/pflash.c's own
 * "pure protocol logic, hardware glue elsewhere" split. Scoped to
 * exactly the commands a real ATAPI driver (UEFI's own AhciBusDxe,
 * Linux's ata_piix/ahci+sr_mod) actually issues to enumerate and read
 * a read-only optical disc: TEST UNIT READY, INQUIRY, READ CAPACITY
 * (10), READ (10), and REQUEST SENSE (issued after a CHECK CONDITION
 * status to learn why). Backed by an in-memory ISO 9660 image buffer
 * for now -- real persistence/host-file reading needs a disk driver,
 * M5's job, the same circular-dependency situation M4-3's flash
 * emulation already had (build the primitive now, in-memory-backed;
 * wire to a real host file once M5 exists).
 */

#define HYPE_ATAPI_CDB_MAX 16
#define HYPE_ATAPI_SECTOR_SIZE 2048u

/* IDENTIFY PACKET DEVICE (ATA command 0xA1) response size: 256 words =
 * 512 bytes, the fixed ATA identify block length. A real UEFI AHCI
 * driver (EDK2 AhciIdentifyPacket) issues this ATA command -- NOT a
 * SCSI CDB -- to an ATAPI device right after it reads the ATAPI port
 * signature, and requires a successful 512-byte PIO-in completion
 * before it will enumerate the drive. The AHCI glue calls the builder
 * below for it; the device-type detail the driver actually acts on
 * comes from the port signature + later INQUIRY, so this block only
 * needs to be a valid ATAPI identify (word 0 marks a packet device). */
#define HYPE_ATAPI_IDENTIFY_SIZE 512u

#define HYPE_ATAPI_CMD_TEST_UNIT_READY 0x00u
#define HYPE_ATAPI_CMD_REQUEST_SENSE 0x03u
#define HYPE_ATAPI_CMD_INQUIRY 0x12u
#define HYPE_ATAPI_CMD_READ_CAPACITY 0x25u
#define HYPE_ATAPI_CMD_READ10 0x28u
#define HYPE_ATAPI_CMD_READ12 0xA8u

/* SCSI status codes this project actually returns. */
#define HYPE_ATAPI_STATUS_GOOD 0x00u
#define HYPE_ATAPI_STATUS_CHECK_CONDITION 0x02u

/* Sense keys/ASC values this project actually sets. */
#define HYPE_ATAPI_SENSE_KEY_NO_SENSE 0x00u
#define HYPE_ATAPI_SENSE_KEY_NOT_READY 0x02u
#define HYPE_ATAPI_SENSE_KEY_ILLEGAL_REQUEST 0x05u
#define HYPE_ATAPI_ASC_MEDIUM_NOT_PRESENT 0x3Au
#define HYPE_ATAPI_ASC_INVALID_COMMAND_OPCODE 0x20u
#define HYPE_ATAPI_ASC_LBA_OUT_OF_RANGE 0x21u

/* Max size of a synthesized (non-media-streamed) response this
 * project's supported command set ever produces in one shot --
 * INQUIRY's 36-byte standard response is the largest. */
#define HYPE_ATAPI_MAX_SYNTH_RESPONSE 36

typedef struct {
    const uint8_t *media_data; /* caller-owned backing ISO image (flat); NULL if chunked */
    /* GLADDER-10(a): alternative CHUNKED backing for multi-GB ISOs that can't
     * be one contiguous allocation. Exactly one of media_data / media_chunks is
     * non-NULL. READ(10) still reports a logical byte offset (media_offset); the
     * caller copies from whichever backing is set (see svm_vcpu.c AHCI glue). */
    const hype_chunked_iso_t *media_chunks;
    uint32_t media_size;       /* logical bytes; must be a multiple of HYPE_ATAPI_SECTOR_SIZE */
    /* Sense state left behind by the most recently failed command, for
     * a driver that follows a CHECK CONDITION status with REQUEST
     * SENSE -- cleared to NO_SENSE by a subsequent successful command,
     * matching real device behavior closely enough for this project's
     * scope (a real device also clears on certain resets/other
     * conditions this project doesn't model). */
    uint8_t sense_key;
    uint8_t asc;
    /* Diagnostic counters (M4-6 real-HW debugging): total CDBs executed
     * and the most-recent opcode + count of READ(10)s. Let a caller
     * report AHCI/CD progress compactly (e.g. on a screen-only real-
     * hardware box where per-command tracing would flood/stall the GOP
     * console) instead of tracing every command. Reset by
     * hype_atapi_reset. */
    uint32_t command_count;
    uint32_t read10_count;
    uint32_t read12_count; /* READ(12) opcode 0xA8, issued by a DMA-capable libata */
    uint8_t last_cdb;
    /* ATAPI-DMA (task #105) measure-first instrumentation: the READ(10)
     * transfer-size profile, so a single boot reveals whether CD reads are
     * bottlenecked by many small commands (each = one VM exit) or are already
     * large. read10_sectors_total sums every READ(10)'s block count;
     * read10_max_count is the largest single request; read10_size_hist buckets
     * requests by block count -- [0]=1, [1]=2..8, [2]=9..16, [3]=17..64,
     * [4]=65..256, [5]=>256 (2048-byte blocks, so bucket 4's ceiling is the
     * 512 KiB / 256-block transfer a DMA-capable drive typically reaches).
     * Purely observational; reset by hype_atapi_reset[_chunked]. */
    uint64_t read10_sectors_total;
    uint32_t read10_max_count;
    uint32_t read10_size_hist[6];
} hype_atapi_t;

/* Number of READ(10) transfer-size buckets in hype_atapi_t::read10_size_hist. */
#define HYPE_ATAPI_READ10_HIST_BUCKETS 6

/* Maps a READ(10) block count to its read10_size_hist bucket index (0..5).
 * A count of 0 (the legal no-op READ) maps to bucket 0. Pure classifier --
 * exposed so the AHCI glue and the unit tests agree on the boundaries. */
uint32_t hype_atapi_read10_size_bucket(uint32_t block_count);

typedef struct {
    uint8_t status; /* HYPE_ATAPI_STATUS_* */
    /* Exactly one of these two describes the response payload (empty
     * for e.g. TEST UNIT READY, which returns no data at all):
     * uses_media_data selects streaming directly from `media_data`
     * (READ(10), avoiding a copy through this struct for what can be a
     * large transfer) versus a small synthesized buffer built here
     * (INQUIRY/READ CAPACITY/REQUEST SENSE). The caller
     * (arch/x86_64/svm/svm_vcpu.c's AHCI glue) is the one that actually
     * copies bytes into the guest's PRDT-described buffers either
     * way -- this struct only describes where those bytes come from. */
    int uses_media_data;
    uint32_t media_offset;
    uint32_t media_length;
    uint8_t synth_data[HYPE_ATAPI_MAX_SYNTH_RESPONSE];
    uint32_t synth_length;
} hype_atapi_result_t;

/* Resets sense state to NO_SENSE and binds the backing media. Call on
 * every (re)start, same convention as every other device model here. */
void hype_atapi_reset(hype_atapi_t *dev, const uint8_t *media_data, uint32_t media_size);

/* GLADDER-10(a): reset with a CHUNKED backing (multi-GB ISO split across
 * non-contiguous buffers). media_size is taken from iso->total_bytes; the flat
 * media_data pointer is cleared. */
void hype_atapi_reset_chunked(hype_atapi_t *dev, const hype_chunked_iso_t *iso);

/*
 * Executes a 16-byte CDB (shorter real CDBs, e.g. TEST UNIT READY's 6
 * bytes, are still passed as a full 16-byte buffer with the unused
 * tail zeroed -- the caller's job) against `dev`, filling `*out`.
 * Unrecognized opcodes return CHECK_CONDITION with ILLEGAL_REQUEST/
 * INVALID_COMMAND_OPCODE sense, matching real ATAPI device behavior
 * for a command it doesn't implement -- never silently guessed at.
 * Pure command decode/dispatch -- no CPU/guest-memory access.
 */
void hype_atapi_execute_cdb(hype_atapi_t *dev, const uint8_t cdb[HYPE_ATAPI_CDB_MAX],
                            hype_atapi_result_t *out);

/*
 * Builds the 512-byte ATA IDENTIFY PACKET DEVICE response for this
 * ATAPI drive into `out`. Word 0 (general configuration) marks a
 * removable ATAPI CD-ROM using a 12-byte command packet (bits 15:14=10b
 * ATAPI, bits 12:8=00101b CD-ROM, bits 1:0=00 12-byte packet) -- the
 * standard value real CD-ROM firmware and QEMU both report (0x85C0).
 * Model/firmware/serial ATA strings are filled with byte-swapped ASCII
 * per the ATA string convention; every other word stays 0 (this
 * project's minimal, read-only-optical scope). Pure data synthesis --
 * no CPU/guest-memory access.
 */
void hype_atapi_build_identify(const hype_atapi_t *dev, uint8_t out[HYPE_ATAPI_IDENTIFY_SIZE]);

#endif /* HYPE_DEVICES_ATAPI_H */
