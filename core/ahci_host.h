#ifndef HYPE_CORE_AHCI_HOST_H
#define HYPE_CORE_AHCI_HOST_H

#include <stdint.h>

/*
 * GLADDER-10 (streaming ISO backend): a minimal HOST-side AHCI driver, so hype
 * can read raw sectors off the physical SATA disk the ESP lives on -- post-
 * ExitBootServices, where UEFI BlockIo is gone -- to stream an installer ISO
 * instead of holding it all in RAM. This is the ENCODE direction: it builds the
 * command list header, the command table's Host-to-Device Register FIS, and the
 * PRDT that a real AHCI HBA consumes. (devices/ahci.c is the mirror image: it
 * DECODEs those same structures to emulate an HBA for the guest.)
 *
 * This header declares only the pure, unit-testable encoders. Byte layouts are
 * the exact inverse of devices/ahci.c's hype_ahci_decode_* helpers; ATA command
 * values come from devices/ata_disk.h. The register-poking bring-up/read path
 * (mapping the ABAR, port reset/enable, issuing a slot, polling PxCI) lives in
 * the hardware shim and is layered on top of these.
 */

/* Command-table layout offsets a real HBA expects (ACHI 1.3.1 §4.2.2/§4.2.3):
 * the Command FIS starts at byte 0, the first PRDT entry at byte 0x80. */
#define HYPE_AHCI_HOST_CT_CFIS_OFF 0x00u
#define HYPE_AHCI_HOST_CT_PRDT_OFF 0x80u
#define HYPE_AHCI_HOST_PRDT_ENTRY_SIZE 16u
/* A single PRDT entry's Data Byte Count is a 22-bit "bytes - 1" field, so one
 * entry transfers at most 4 MiB. Sector reads here stay well under that. */
#define HYPE_AHCI_HOST_PRDT_MAX_BYTES (4u * 1024u * 1024u)
#define HYPE_AHCI_HOST_SECTOR_SIZE 512u

/*
 * Builds the 32-byte command-list slot header for a single-command transfer:
 * command-FIS length = 5 dwords (a 20-byte H2D Register FIS), the write flag,
 * `prdtl` PRDT entries, and the 64-bit physical base of the command table.
 * Zeroes the whole 32-byte slot first. Pure -- writes only into `slot`.
 */
void hype_ahci_host_build_cmd_header(uint8_t slot[32], int is_write, uint16_t prdtl,
                                     uint64_t cmd_table_phys);

/*
 * Fills a command table for READ DMA EXT (ATA 0x25): a H2D Register FIS at
 * offset 0 carrying the 48-bit `lba` and 16-bit `count` (sectors), plus a
 * single PRDT entry at offset 0x80 pointing at `dst_phys` for count*512 bytes.
 * Zeroes the FIS and the PRDT entry it writes. `count` must be 1..8192 (<=4 MiB,
 * one PRDT entry); returns 0 on success, -1 if count is 0 or too large. Pure.
 */
int hype_ahci_host_build_read_dma_ext(uint8_t *cmd_table, uint64_t lba, uint16_t count,
                                      uint64_t dst_phys);

/* --- Hardware bring-up (host_pci_hw-style shim; real MMIO, not unit-tested) --- */

/*
 * Scans the HBA at `abar_phys` (identity-mapped MMIO) for a port with a SATA
 * hard disk attached (PxSSTS.DET == 3 and the non-ATAPI signature 0x00000101).
 * Returns the port number, or -1 if none. This is the disk the ESP -- and thus
 * the installer ISO -- lives on in the QEMU harness.
 */
int hype_ahci_host_find_sata_port(uint64_t abar_phys);

/*
 * Prepares `port` of the HBA at `abar_phys` for hype-driven I/O: stops the port,
 * points PxCLB/PxFB at hype's own command list / received-FIS buffers, clears
 * sticky errors, and restarts the engines. Call ONCE before a run of reads (the
 * streaming source does this at setup) -- hype_ahci_host_read() then reuses the
 * already-programmed port per call rather than reprogramming it each time.
 * Returns 0 on success, -1 if the port's DMA engines won't quiesce.
 * Post-ExitBootServices only (it takes over the real controller).
 */
int hype_ahci_host_init(uint64_t abar_phys, unsigned port);

/*
 * Reads `count` sectors (1..8192, one PRDT entry, <=4 MiB) starting at LBA `lba`
 * from an already-initialised `port` (see hype_ahci_host_init) into `dst` -- a
 * 512*count-byte, identity-mapped, sector-aligned host buffer the HBA DMAs into.
 * Builds slot 0 (READ DMA EXT), issues it, and polls PxCI (bounded spin) to
 * completion. Returns 0 on success, -1 on timeout or an ATA error. x86 DMA is
 * cache-coherent, so no explicit flush is needed.
 */
int hype_ahci_host_read(uint64_t abar_phys, unsigned port, uint64_t lba, uint16_t count, void *dst);

#endif /* HYPE_CORE_AHCI_HOST_H */
