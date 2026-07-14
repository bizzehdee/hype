#ifndef HYPE_DEVICES_ATAPI_H
#define HYPE_DEVICES_ATAPI_H

#include <stdint.h>

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

#define HYPE_ATAPI_CMD_TEST_UNIT_READY 0x00u
#define HYPE_ATAPI_CMD_REQUEST_SENSE 0x03u
#define HYPE_ATAPI_CMD_INQUIRY 0x12u
#define HYPE_ATAPI_CMD_READ_CAPACITY 0x25u
#define HYPE_ATAPI_CMD_READ10 0x28u

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
    const uint8_t *media_data; /* caller-owned backing ISO image */
    uint32_t media_size;       /* bytes; must be a multiple of HYPE_ATAPI_SECTOR_SIZE */
    /* Sense state left behind by the most recently failed command, for
     * a driver that follows a CHECK CONDITION status with REQUEST
     * SENSE -- cleared to NO_SENSE by a subsequent successful command,
     * matching real device behavior closely enough for this project's
     * scope (a real device also clears on certain resets/other
     * conditions this project doesn't model). */
    uint8_t sense_key;
    uint8_t asc;
} hype_atapi_t;

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

#endif /* HYPE_DEVICES_ATAPI_H */
