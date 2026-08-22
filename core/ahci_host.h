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
 * #325: host-side ATAPI (a real optical drive).
 *
 * The ATAPI command block lives at offset 0x40 of the command table, between the CFIS and the PRDT --
 * that gap exists precisely for it.
 *
 * 2048-byte sectors, and the conversion is the most likely source of a subtle bug in this whole
 * ticket: every other LBA in hype's media path is in 512-byte units (core/iso_stream.h's contract,
 * HYPE_VIRTIO_BLK_SECTOR_SIZE), so a CD LBA is a QUARTER of the 512-byte LBA at the same byte offset.
 * The conversion is therefore done in exactly one place (hype_ahci_host_atapi_lba512_to_lba2k) and
 * tested, rather than open-coded at each call.
 */
#define HYPE_AHCI_HOST_CT_ACMD_OFF 0x40u
#define HYPE_AHCI_HOST_CD_SECTOR_SIZE 2048u
/* SATA signatures reported in PxSIG: a plain disk vs a packet device. */
#define HYPE_AHCI_HOST_SIG_ATA 0x00000101u
#define HYPE_AHCI_HOST_SIG_ATAPI 0xEB140101u

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

/*
 * As hype_ahci_host_build_read_dma_ext but for WRITE DMA EXT (ATA 0x35): the
 * PRDT points at `src_phys`, the guest data to be written out. The caller must
 * set the command header's W bit (hype_ahci_host_build_cmd_header is_write=1).
 * Same 1..8192-sector (<=4 MiB) limit; returns 0 on success, -1 otherwise. Pure.
 */
int hype_ahci_host_build_write_dma_ext(uint8_t *cmd_table, uint64_t lba, uint16_t count,
                                       uint64_t src_phys);

/*
 * Fills a command table for IDENTIFY DEVICE (ATA 0xEC): a H2D Register FIS at
 * offset 0 (no LBA/count -- IDENTIFY takes none) and a single PRDT entry at
 * offset 0x80 pointing at `dst_phys` for the 512-byte response. Zeroes the FIS
 * and the PRDT entry it writes. Pure -- writes only into `cmd_table`.
 */
void hype_ahci_host_build_identify(uint8_t *cmd_table, uint64_t dst_phys);

/*
 * M10-2: the physical disk's captured identity -- the fields a `physical:`
 * target-disk safety guard keys on (serial match) and a block backend needs
 * for its own real-capacity bounds check (total_sectors). Strings are NUL-
 * terminated and trailing-space-trimmed.
 */
typedef struct {
    char serial[21];        /* ATA serial number (IDENTIFY words 10-19, 20 chars) */
    char model[41];         /* ATA model number  (IDENTIFY words 27-46, 40 chars) */
    uint64_t total_sectors; /* 48-bit LBA capacity if supported, else 28-bit */
} hype_host_disk_info_t;

/*
 * Parses a 512-byte ATA IDENTIFY DEVICE response into *out: serial and model
 * (byte-swapped ASCII per the ATA convention -- the exact inverse of
 * devices/ata_disk.c's write_swapped_ascii), and the total sector count
 * (48-bit words 100-103 when word 83 bit 10 marks 48-bit addressing supported,
 * otherwise the 28-bit words 60-61 fallback). Pure.
 */
/*
 * #657: returns 0 on success, -1 if both the 48-bit and 28-bit capacity fields are zero (an
 * implausible/garbled response -- there is no sane "0-sector disk"), matching
 * hype_nvme_parse_identify_ns's contract (core/nvme_host.c). Callers must not inventory or attach
 * a disk whose IDENTIFY failed to parse.
 */
int hype_ahci_host_parse_identify(const uint8_t id[512], hype_host_disk_info_t *out);

/* --- Hardware bring-up (host_pci_hw-style shim; real MMIO, not unit-tested) --- */

/*
 * Scans the HBA at `abar_phys` (identity-mapped MMIO) for a port with a SATA
 * hard disk attached (PxSSTS.DET == 3 and the non-ATAPI signature 0x00000101).
 * Returns the port number, or -1 if none. This is the disk the ESP -- and thus
 * the installer ISO -- lives on in the QEMU harness.
 */
int hype_ahci_host_find_sata_port(uint64_t abar_phys);

/*
 * #258: the same scan, resumable. Returns the first matching port at or after
 * `start_port`, or -1 when there are no more.
 *
 * The single-shot version above returns only the LOWEST matching port, and the
 * whole host-disk path was built on that one value -- so hype could see exactly
 * one SATA disk per controller and a `physical:` target on any other port was
 * unreachable by construction. Callers that need every disk (the inventory in
 * boot/main.c) iterate with this instead. Same PHY-settle retry as the
 * single-shot scan, because it IS that scan -- not a second copy that would
 * drift from it.
 */
int hype_ahci_host_find_sata_port_from(uint64_t abar_phys, unsigned int start_port);

/*
 * #258: does `port` carry a device matching the current scan? One port only, so a caller can walk
 * 0..31 in a SINGLE pass and inventory what it finds. Re-entering the whole scan per disk made
 * enumeration quadratic -- each call re-polls the empty tail with the PHY-settle retry -- and that
 * alone stalled host discovery past a 110-second timeout.
 */
int hype_ahci_host_port_matches(uint64_t abar_phys, unsigned int port);

/*
 * #369: how long to keep waiting for a port whose PxSSTS.DET says nothing is there.
 *
 * The PHY-settle wait exists for a disk that is present but has not finished negotiating at cold
 * boot (the AMD laptop's SATA SSD, found only on some boots). AHCI 1.3.1 PxSSTS.DET separates that
 * state (1 = device present, no communication) from a genuinely empty port (0 = nothing detected,
 * PHY offline) -- and the wait used to treat them identically, so every empty port burned the
 * whole budget. A 6-port controller with one disk paid five full budgets and stalled ~60 s before
 * any guest started.
 *
 * This is the shorter budget for the "nothing is negotiating" case: still a real wait, so a port
 * that reports 0 for a moment before its PHY announces itself is not written off, but small enough
 * that an empty backplane costs milliseconds instead of seconds. A port that reads DET 1 or 2 gets
 * the full ceiling, unchanged.
 */
#define HYPE_AHCI_HOST_SETTLE_EMPTY_SPINS 20000u

/*
 * #369: the PHY-settle stop/continue decision, as pure logic so it is testable without an HBA.
 *
 * Called once per settle iteration for a WHOLE controller, with a census of its implemented ports:
 *   `pending`     -- ports not yet reporting DET==3 (established).
 *   `negotiating` -- the subset of those reporting DET==1 or 2, i.e. something is actually there
 *                    and still coming up. DET==0 (nothing) and DET==4 (PHY disabled) are excluded:
 *                    neither can progress to 3 on its own.
 *   `elapsed`     -- iterations already spent on this controller.
 *
 * Returns 1 to keep waiting, 0 to stop. Stops immediately when nothing is pending, waits without
 * limit (up to the caller's own ceiling) while anything is negotiating, and otherwise gives up
 * after HYPE_AHCI_HOST_SETTLE_EMPTY_SPINS.
 */
int hype_ahci_host_settle_continue(unsigned int pending, unsigned int negotiating,
                                   unsigned int elapsed);

/*
 * Diagnostic: log CAP + PI and, for each implemented port, PxSSTS (DET/SPD/IPM),
 * PxSIG, PxCMD, PxTFD. Read-only. Called when find_sata_port finds nothing so a
 * real-HW log shows WHY (e.g. DET != 3 -> no PHY / disk asleep, or a signature
 * find_sata_port doesn't match).
 */
void hype_ahci_host_dump_ports(uint64_t abar_phys);

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

/*
 * #658: records which core is the BSP, mirroring hype_blk_usb_set_bsp_apic() (core/blk_usb.h) --
 * so the per-port lock can give the BSP a bounded wait instead of the unbounded one guest AP
 * callers get. Call once, from the same place USB's BSP core is latched.
 */
void hype_ahci_host_set_bsp_apic(unsigned int apic_id);

/* Nonzero after a real-HW run means the BSP hit its bounded lock budget at least once -- the
 * AHCI counterpart of hype_blk_usb_bsp_lock_timeouts() (core/blk_usb.h). */
unsigned long long hype_ahci_host_bsp_lock_timeouts(void);

/*
 * Issues IDENTIFY DEVICE to an already-initialised `port` (see
 * hype_ahci_host_init) and copies the 512-byte response into `dst512` (a
 * 512-byte, identity-mapped, sector-aligned host buffer). Builds slot 0
 * (IDENTIFY), issues it, and polls PxCI to completion. Returns 0 on success,
 * -1 on timeout or an ATA error. Post-ExitBootServices only.
 */
int hype_ahci_host_identify(uint64_t abar_phys, unsigned port, void *dst512);

/*
 * Writes `count` sectors (1..8192, <=4 MiB) starting at LBA `lba` from `src` (a
 * 512*count-byte, identity-mapped, sector-aligned host buffer) to an already-
 * initialised `port`. Builds slot 0 (WRITE DMA EXT), issues it, polls PxCI.
 * Returns 0 on success, -1 on timeout or an ATA error. Post-ExitBootServices
 * only. DESTRUCTIVE: callers must have cleared the §6d safety guard (serial/GUID
 * match + interactive confirm + non-empty-partition guard) before using this.
 */
int hype_ahci_host_write(uint64_t abar_phys, unsigned port, uint64_t lba, uint16_t count,
                         const void *src);


/*
 * #325: build a PACKET (0xA0) command carrying a 12-byte READ(10) CDB for `count2k` 2048-byte sectors
 * at CD LBA `lba2k`, reading into `dst_phys`.
 *
 * Two things differ from the ATA builders and both are easy to miss:
 *   - the command-list header needs its A (ATAPI) bit set, so the HBA sends the ACMD block at all --
 *     see hype_ahci_host_build_cmd_header_atapi();
 *   - the FIS carries the transfer size in the byte-count (LBA mid/high) fields, not a sector count.
 *
 * Returns 0, or -1 if the request is empty or exceeds one PRDT entry.
 */
int hype_ahci_host_build_atapi_read10(uint8_t *cmd_table, uint32_t lba2k, uint16_t count2k,
                                      uint64_t dst_phys);

/* As hype_ahci_host_build_cmd_header(), but sets the A bit (ATAPI). Always a read here: hype never
 * writes to an optical drive. */
void hype_ahci_host_build_cmd_header_atapi(uint8_t slot[32], uint16_t prdtl, uint64_t cmd_table_phys);

/*
 * The 512-to-2048 conversion, in ONE place. Returns 0 and writes the CD LBA + sector count, or -1 if
 * the 512-byte range is not 2048-aligned -- which is a caller bug rather than something to round,
 * since silently reading the wrong 3 KiB is exactly the off-by-4 this is here to prevent.
 */
int hype_ahci_host_atapi_lba512_to_lba2k(uint64_t lba512, uint32_t count512, uint32_t *out_lba2k,
                                         uint16_t *out_count2k);

/*
 * #325: the most blocks one PACKET command may ask for.
 *
 * The byte count limit a PACKET command carries lives in two bytes of the command FIS, so it tops
 * out at 65535 -- one byte short of 32 sectors. Asking for 32 makes the limit 0x10000, which
 * truncates to 0 in those two fields, i.e. "you may return no data". The drive then transfers
 * nothing and the command fails, which is exactly how a 64 KiB read of an El Torito boot image
 * failed while every 2 KiB read succeeded. 31 blocks is 63488 bytes and fits.
 */
#define HYPE_AHCI_HOST_ATAPI_MAX_BLOCKS 31u

/* Scans for a port with an ATAPI device attached (PxSSTS.DET == 3 and the packet signature).
 * Returns the port number, or -1. Counterpart to the hard-disk scan above, which by requiring the
 * NON-ATAPI signature skips a real optical drive by construction (#325). */
int hype_ahci_host_find_atapi_port(uint64_t abar_phys);

/* Issues the above against a real drive. 2048-byte sectors. */
int hype_ahci_host_atapi_read(uint64_t abar_phys, unsigned port, uint32_t lba2k, uint16_t count2k,
                              void *dst);

/*
 * #295: gathering adjacent writes into ONE multi-PRDT command.
 *
 * The physical write path issues one AHCI command at a time and spins for completion, so
 * throughput is 1/latency no matter how big each transfer is. Measured on the QEMU rig with a
 * 48 MiB dd: 12,288 writes, every one exactly 4 KiB (max=8 sectors, nothing larger got through
 * despite bs=1M), against a virtqueue holding a mean of 27.7 chains per kick and 32+ on 84% of
 * kicks. The guest will not merge these itself; there is a great deal to merge, and hype is the
 * only place left to do it.
 *
 * AHCI already carries scatter-gather: one command, one PRDT entry per segment, each pointing at a
 * different host address -- so adjacent requests merge with NO bounce buffer and no copying.
 *
 * This is the DECISION half, kept pure and separate from command construction on purpose. It is
 * the part that gets a gather wrong, and a gather bug on the write path puts the right bytes at the
 * wrong LBA, which is worse than being slow.
 */
typedef struct {
    uint64_t lba;      /* 512-byte LBA this request starts at */
    uint32_t sectors;  /* length in 512-byte sectors */
    uint64_t buf_phys; /* host-physical address of this request's data */
    int is_write;      /* direction; a merge never crosses it */
} hype_ahci_host_gather_req_t;

/*
 * How many of the first `n` requests may be issued as ONE command, starting at reqs[0].
 *
 * Returns at least 1 whenever reqs[0] is individually valid, and 0 when it is not (zero-length, or
 * a single request too large for one PRDT entry) -- so a caller can always distinguish "merge these
 * k" from "this request cannot be issued at all".
 *
 * A request joins only when ALL of these hold, and each one is a way a look-ahead loop gets it
 * wrong:
 *   - same direction as the run (mixing a read into a write run would transfer the wrong way),
 *   - its LBA exactly continues the previous one (a gap would write the right bytes to the wrong
 *     place -- the failure this ticket calls out by name),
 *   - the run stays within `max_segs` PRDT entries and `max_sectors` total,
 *   - its own length fits one PRDT entry (HYPE_AHCI_HOST_PRDT_MAX_BYTES).
 *
 * Deliberately does NOT merge across a non-contiguous LBA even when the buffers happen to be
 * adjacent in host memory: the LBA is what the disk acts on.
 */
unsigned int hype_ahci_host_gather_span(const hype_ahci_host_gather_req_t *reqs, unsigned int n,
                                        unsigned int max_segs, uint32_t max_sectors);

/*
 * #295: WRITE DMA EXT with one PRDT entry PER SEGMENT -- the construction half.
 *
 * Same command as hype_ahci_host_build_write_dma_ext, but the data comes from `nsegs` separate host
 * buffers instead of one. That is what lets adjacent guest requests merge with no bounce buffer:
 * each request keeps its own buffer and contributes its own PRDT entry.
 *
 * The caller must set PRDTL to `nsegs` in the command header (hype_ahci_host_build_cmd_header) --
 * a header still saying 1 would transfer only the first segment and report success, which is a
 * short write with no error.
 *
 * `count` is the TOTAL sectors and must equal the sum of the segment lengths; the mismatch is
 * refused rather than trusted, because the FIS count is what the drive acts on and the PRDT is what
 * it moves. Returns 0, or -1 if nsegs is 0 or above `max_prdt`, if any segment is empty, not a
 * whole number of sectors, or larger than one PRDT entry allows, or if the lengths do not sum to
 * `count`.
 */
typedef struct {
    uint64_t phys;  /* host-physical base of this segment */
    uint32_t bytes; /* its length; must be a whole number of 512-byte sectors */
} hype_ahci_host_sg_t;

int hype_ahci_host_build_write_dma_ext_sg(uint8_t *cmd_table, uint64_t lba, uint16_t count,
                                          const hype_ahci_host_sg_t *segs, unsigned int nsegs,
                                          unsigned int max_prdt);

/*
 * #295: most PRDT entries one command table carries -- the vectored write path's per-command
 * segment cap. 32 matches HYPE_VIRTIO_BLK_SEG_MAX, so a whole guest request that honours the
 * advertised seg_max always fits one command. g_cmd_table in ahci_host_hw.c is sized from this.
 */
#define HYPE_AHCI_HOST_SG_MAX_PRDT 32u

/*
 * #295: issue ONE WRITE DMA EXT whose data comes from `nsegs` scattered host buffers (`count` =
 * total sectors, must equal the segment sum -- the builder refuses a mismatch). Serialised per
 * port like the scalar read/write. Hardware-touching shim; the batching decisions live in
 * blk_phys.c and the command construction in hype_ahci_host_build_write_dma_ext_sg, both tested.
 */
int hype_ahci_host_writev(uint64_t abar_phys, unsigned port, uint64_t lba,
                          const hype_ahci_host_sg_t *sg, unsigned int nsegs, uint16_t count);

#endif /* HYPE_CORE_AHCI_HOST_H */
