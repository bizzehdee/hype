#ifndef HYPE_DEVICES_AHCI_H
#define HYPE_DEVICES_AHCI_H

#include <stdint.h>

/*
 * Minimal single-port AHCI HBA register model (M4-5), backing the
 * virtual optical drive (devices/atapi.h carries the actual ATAPI/SCSI
 * command semantics; this module is the SATA/AHCI transport around
 * it). Register offsets, bit layouts, and the Command Header/PRDT wire
 * formats are transcribed directly from the Linux kernel's own AHCI
 * driver (drivers/ata/ahci.h, drivers/ata/libata-sata.c's
 * ata_tf_to_fis()/ata_tf_from_fis()) -- fetched and read for this task,
 * not reconstructed from memory, same discipline as this project's
 * other wire-format structs (arch/x86_64/svm/vmcb.h, devices/
 * acpi_loader.h). Scoped to exactly one port with one ATAPI device
 * attached -- this milestone's own scope ("a virtual optical drive"),
 * not a general multi-port/multi-device AHCI controller.
 *
 * This module models the MMIO register set only (hype_ahci_mmio_read/
 * write, plus the pure Command Header/PRDT decoders) -- it never
 * touches guest memory itself. Walking a guest's Command List/Command
 * Table/PRDT and dispatching the extracted ATAPI CDB to devices/
 * atapi.h is the exempt caller's job (arch/x86_64/svm/svm_vcpu.c),
 * same layering as every other MMIO-trapped device here (M4-3's
 * pflash, this project's own established pattern).
 */

/* HBA generic register byte offsets (from the start of the BAR). */
#define HYPE_AHCI_REG_CAP 0x00u
#define HYPE_AHCI_REG_GHC 0x04u
#define HYPE_AHCI_REG_IS 0x08u
#define HYPE_AHCI_REG_PI 0x0Cu
#define HYPE_AHCI_REG_VS 0x10u
#define HYPE_AHCI_REG_CCC_CTL 0x14u
#define HYPE_AHCI_REG_CCC_PORTS 0x18u
#define HYPE_AHCI_REG_EM_LOC 0x1Cu
#define HYPE_AHCI_REG_EM_CTL 0x20u
#define HYPE_AHCI_REG_CAP2 0x24u
#define HYPE_AHCI_REG_BOHC 0x28u

/* Port register block base and per-port stride (single port here, so
 * this project only ever uses port 0's block at HYPE_AHCI_PORT_BASE). */
#define HYPE_AHCI_PORT_BASE 0x100u
#define HYPE_AHCI_PORT_STRIDE 0x80u

/* Port register byte offsets, relative to HYPE_AHCI_PORT_BASE. */
#define HYPE_AHCI_PREG_CLB 0x00u
#define HYPE_AHCI_PREG_CLBU 0x04u
#define HYPE_AHCI_PREG_FB 0x08u
#define HYPE_AHCI_PREG_FBU 0x0Cu
#define HYPE_AHCI_PREG_IS 0x10u
#define HYPE_AHCI_PREG_IE 0x14u
#define HYPE_AHCI_PREG_CMD 0x18u
#define HYPE_AHCI_PREG_TFD 0x20u
#define HYPE_AHCI_PREG_SIG 0x24u
#define HYPE_AHCI_PREG_SSTS 0x28u
#define HYPE_AHCI_PREG_SCTL 0x2Cu
#define HYPE_AHCI_PREG_SERR 0x30u
#define HYPE_AHCI_PREG_SACT 0x34u
#define HYPE_AHCI_PREG_CI 0x38u
#define HYPE_AHCI_PREG_SNTF 0x3Cu

#define HYPE_AHCI_MMIO_SIZE (HYPE_AHCI_PORT_BASE + HYPE_AHCI_PORT_STRIDE)

/* GHC (Global HBA Control) bits this project models. */
#define HYPE_AHCI_GHC_HR (1u << 0)   /* HBA Reset (write-1; self-clears when the reset completes) */
#define HYPE_AHCI_GHC_AE (1u << 31)  /* AHCI Enable */

/* PxIS (Port Interrupt Status) completion bits a real AHCI driver polls
 * to learn a command finished (EDK2 AhciCheckFisReceived): DHRS when a
 * Device-to-Host Register FIS arrived (D2H/DMA/ATAPI-PACKET commands),
 * PSS when a PIO Setup FIS arrived (PIO-in commands -- IDENTIFY [PACKET]
 * DEVICE). The M4-5 model originally set an unrelated bit here, which a
 * cooperating hand-written test guest ignored (it polled PxCI) but a
 * real driver waits on -- corrected for FW-1h. */
#define HYPE_AHCI_PIS_DHRS (1u << 0) /* Device to Host Register FIS Interrupt */
#define HYPE_AHCI_PIS_PSS (1u << 1)  /* PIO Setup FIS Interrupt */

/* ATA IDENTIFY PACKET DEVICE (command 0xA1): the ATA-level command a
 * real AHCI driver issues to an ATAPI device (EDK2 AhciIdentifyPacket)
 * -- delivered as a plain H2D Register FIS command byte, NOT a SCSI CDB
 * inside a PACKET, so the Command Header's ATAPI ('A') bit is 0. (EDK2's
 * confusingly-named ATA_CMD_IDENTIFY_DEVICE == 0xA1 is this command;
 * 0xEC / ATA_CMD_IDENTIFY_DRIVE is the plain-ATA-disk one.) */
#define HYPE_AHCI_ATA_CMD_IDENTIFY_PACKET_DEVICE 0xA1u

/* ATA SET FEATURES (command 0xEF): a no-data ATA command a real AHCI
 * driver issues right after IDENTIFY to select the transfer mode (EDK2
 * AhciModeInitialization -> AhciDeviceSetFeature). This project models
 * a single fixed transfer profile, so it just acknowledges the command
 * with a successful, data-less completion. */
#define HYPE_AHCI_ATA_CMD_SET_FEATURES 0xEFu

/* PxCMD bits this project models. */
#define HYPE_AHCI_PCMD_ST (1u << 0)  /* Start (guest-set: spin up the port's DMA engine) */
#define HYPE_AHCI_PCMD_FRE (1u << 4) /* FIS Receive Enable */
#define HYPE_AHCI_PCMD_FR (1u << 14) /* FIS receive DMA engine running (device-reported) */
#define HYPE_AHCI_PCMD_CR (1u << 15) /* Command list DMA engine running (device-reported) */

/* ATAPI device signature (PxSIG): LBA_HIGH=0xEB, LBA_MID=0x14,
 * SECTOR_COUNT=0x01, LBA_LOW=0x01 -- the standard, universally
 * recognized way an AHCI port announces "an ATAPI device is attached
 * here" (a plain SATA disk instead reports 0x00000101). */
#define HYPE_AHCI_SIG_ATAPI 0xEB140101u

typedef struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_ports;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;

    uint32_t p_clb;
    uint32_t p_clbu;
    uint32_t p_fb;
    uint32_t p_fbu;
    uint32_t p_is;
    uint32_t p_ie;
    uint32_t p_cmd;
    uint32_t p_tfd;
    uint32_t p_sig;
    uint32_t p_ssts;
    uint32_t p_sctl;
    uint32_t p_serr;
    uint32_t p_sact;
    uint32_t p_ci;
    uint32_t p_sntf;
} hype_ahci_t;

/*
 * Resets to a single-port (PI=0x1), single-ATAPI-device (PxSIG),
 * AHCI-only (CAP.SAM), 64-bit-addressing-capable (CAP.S64A)
 * controller, link already up (PxSSTS = DET=3/IPM=1/SPD=1) -- this
 * project never models a real link-training handshake, so the guest
 * driver sees an already-ready port from the start, matching
 * hype_pit_emu_reset()-style "start in the state a driver would find
 * after its own init sequence" conventions elsewhere in this project.
 */
void hype_ahci_reset(hype_ahci_t *ahci);

/*
 * Reads the 32-bit register at `offset` (must be 4-byte aligned;
 * `size_bytes` must be 4 -- AHCI registers are architecturally 32-bit
 * only, no real driver accesses them any other width). An offset
 * within HYPE_AHCI_MMIO_SIZE that isn't one of this project's modeled
 * registers reads as 0 (a legitimate "reserved field reads as 0"
 * convention, not an error) rather than being rejected. Returns 0 on
 * success, -1 for a misaligned offset or wrong width.
 */
int hype_ahci_mmio_read(const hype_ahci_t *ahci, uint32_t offset, uint8_t size_bytes, uint32_t *out_value);

/*
 * Writes `value` to the 32-bit register at `offset` (same alignment/
 * width requirement as hype_ahci_mmio_read()). Applies each register's
 * real semantics: IS/PxIS/PxSERR/PxSNTF are write-1-to-clear; PxCI is
 * OR'd in (the guest sets bits to issue commands; a completed command
 * is cleared by the caller once processed, not by this function --
 * command *processing* itself needs guest-memory access this pure
 * register model deliberately doesn't have, so it stays the exempt
 * caller's job); PxCMD's ST/FRE bits are synchronously mirrored into
 * CR/FR (this project's controller has no real asynchronous DMA-engine
 * startup delay to model, so a driver's poll-until-running loop
 * succeeds immediately, same simplification devices/pflash.h's own
 * "always traps, correctness over performance" stance already takes).
 * A write to a read-only or unimplemented-but-in-range register is
 * silently ignored rather than rejected. Returns 0 on success, -1 for
 * a misaligned offset or wrong width.
 */
int hype_ahci_mmio_write(hype_ahci_t *ahci, uint32_t offset, uint8_t size_bytes, uint32_t value);

/* Command Header (32 bytes, Command List entry). */
typedef struct {
    uint8_t cfl;      /* Command FIS Length, in DWORDs */
    int is_atapi;     /* opts bit 5 (A) */
    int is_write;     /* opts bit 6 (W) */
    uint16_t prdtl;   /* PRDT entry count, opts bits 31:16 */
    uint64_t cmd_table_phys; /* CTBA | (CTBAU << 32) */
} hype_ahci_cmd_header_t;

/* Decodes a 32-byte Command Header. Pure bit extraction, no CPU/guest-
 * memory access -- the caller has already read these bytes out of
 * guest memory. */
void hype_ahci_decode_cmd_header(const uint8_t raw[32], hype_ahci_cmd_header_t *out);

/* PRDT (Physical Region Descriptor Table) entry, 16 bytes. */
typedef struct {
    uint64_t data_phys; /* DBA | (DBAU << 32) */
    uint32_t byte_count; /* already +1'd from the raw DBC field (spec: "byte count - 1") */
} hype_ahci_prdt_entry_t;

/* Decodes a 16-byte PRDT entry. Pure bit extraction. */
void hype_ahci_decode_prdt_entry(const uint8_t raw[16], hype_ahci_prdt_entry_t *out);

/*
 * M5-2: the H2D (Host-to-Device) Register FIS's own ATA-specific
 * fields (byte offsets fetched and confirmed against QEMU's
 * hw/ide/ahci.c handle_reg_h2d_fis() and the Linux kernel's own
 * include/linux/ata.h, not reconstructed from memory) -- command
 * (byte 2), the 48-bit LBA split across bytes 4-6 (LBA 23:0) and
 * bytes 8-10 (LBA 47:24, the "HOB"/expanded bytes), device register
 * (byte 7), and the 16-bit Count field (bytes 12-13). This project's
 * existing ATAPI path (process_ahci_command_slot0(), arch/x86_64/svm/
 * svm_vcpu.c) only ever checks byte 2 == 0xA0 (PACKET) inline; this
 * decoder is for M5-2's own plain-ATA command path, which needs the
 * LBA/count fields PACKET never carries.
 */
typedef struct {
    uint8_t command;
    uint64_t lba; /* full 48-bit value, already combined */
    uint8_t device;
    uint16_t count; /* raw Count field -- NOT yet resolved via the "0 means 65536" convention */
} hype_ahci_h2d_fis_t;

/* Decodes a 20-byte H2D Register FIS. Pure bit extraction, no guest-
 * memory access -- the caller has already read these bytes out of
 * guest memory (same split as every other decode function here). Does
 * not validate FIS type (byte 0) or the C bit (byte 1, bit 7) -- the
 * caller checks those itself, matching hype_ahci_decode_cmd_header()'s
 * own "just extract bits, the caller validates context" convention. */
void hype_ahci_decode_h2d_fis(const uint8_t raw[20], hype_ahci_h2d_fis_t *out);

#endif /* HYPE_DEVICES_AHCI_H */
