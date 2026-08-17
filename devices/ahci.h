#ifndef HYPE_DEVICES_AHCI_H
#define HYPE_DEVICES_AHCI_H

#include <stdint.h>

#include "../core/guest_mem.h" /* hype_gpa_map_t (command-slot DMA) */
#include "ata_disk.h"          /* hype_ata_disk_t (SATA disk command semantics) */
#include "atapi.h"             /* hype_atapi_t (ATAPI command semantics) */

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

/* Port register block base and per-port stride. ICH9 exposes six ports; port
 * zero is the active medium in Hype's compact model while the remaining empty
 * ports retain their architectural, readable register aperture. */
#define HYPE_AHCI_PORT_BASE 0x100u
#define HYPE_AHCI_PORT_STRIDE 0x80u
#define HYPE_AHCI_PORT_COUNT 6u

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

#define HYPE_AHCI_MMIO_SIZE (HYPE_AHCI_PORT_BASE + HYPE_AHCI_PORT_COUNT * HYPE_AHCI_PORT_STRIDE)

/* GHC (Global HBA Control) bits this project models. */
#define HYPE_AHCI_GHC_HR (1u << 0)   /* HBA Reset (write-1; self-clears when the reset completes) */
#define HYPE_AHCI_GHC_IE (1u << 1)   /* Interrupt Enable (global: gates HBA interrupt assertion) */
#define HYPE_AHCI_GHC_AE (1u << 31)  /* AHCI Enable */

/* IS (global Interrupt Status): one bit per port. This project models a
 * single port (0), so only bit 0 is ever set. Hardware sets IS.IPS[p]
 * when port p has an interrupt condition (PxIS & PxIE != 0); software
 * clears it write-1 (RW1C). A real driver's ISR (Linux ahci_interrupt)
 * reads IS first to learn which port fired, so it must reflect port 0. */
#define HYPE_AHCI_IS_PORT0 (1u << 0)

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
/*
 * #262: plain (non-packet) SATA disk signature -- LBA_HIGH/LBA_MID zero, sector
 * count and LBA_LOW 1. hype_ahci_reset() defaults every instance to ATAPI because
 * the model was written for M4-5's optical drive; a second HBA carrying a real disk
 * must say so, or the guest issues IDENTIFY PACKET DEVICE (0xA1) instead of IDENTIFY
 * DEVICE (0xEC), which the ATA path does not implement -- observed as
 * "ata2.00: qc timeout after 5000 msecs (cmd 0xa1)" followed by failed IDENTIFY.
 */
#define HYPE_AHCI_SIG_ATA 0x00000101u

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

    /*
     * #512: count of 0->1 transitions of hype_ahci_irq_pending(), kept by whoever mutates the
     * state (completion posting, PxIE/GHC writes). The MSI edge sender used a sampled-level
     * latch; a second vCPU clearing PxIS and a new completion re-raising it between two of the
     * sender's polls swallowed the new edge (observed on real hardware as `ata7.00: qc timeout
     * (cmd 0xec)` -- an IDENTIFY whose completion MSI never fired). Count edges at the source.
     */
    unsigned long long irq_events;

    /*
     * #309: whether the guest has asserted SRST and not yet released it. A software reset is
     * TWO commands, and only the second one completes with a device signature, so the model
     * has to remember it saw the first. Cleared by hype_ahci_reset().
     */
    int srst_asserted;

    /*
     * #372: does the guest's PCI Command register have Bus Master Enable set?
     *
     * Mirrored in from PCI config rather than read from it, so this model stays free of any PCI
     * dependency (devices/ahci.c references PCI nowhere, deliberately). The live path pushes the
     * real value with hype_ahci_set_bus_master() at setup and on every config write.
     *
     * DEFAULTS TO ENABLED in hype_ahci_reset(), and that default is a compromise worth naming:
     * twenty-four call sites drive this model directly with no PCI at all (sixteen unit tests and
     * eight boot microtests), and a refusing default would break every one of them for no gain --
     * none of them is a guest, so none can demonstrate the behaviour this models. The default is
     * therefore permissive and the LIVE path is explicit. That is the weaker half of this fix: a
     * future device path that forgets to push its state gets the old permissive behaviour rather
     * than a loud failure.
     */
    int bus_master;
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
 * #372: mirror the guest's PCI Bus Master Enable state into the controller.
 *
 * Call it whenever the guest writes the PCI Command register, and once at setup so the initial
 * state is the real one rather than the permissive default. With it clear, a command written to
 * PxCI is accepted and never completes -- exactly what the hardware does, and what a guest driver
 * that forgot to enable bus mastering must be allowed to discover here rather than in the field.
 */
void hype_ahci_set_bus_master(hype_ahci_t *ahci, int enabled);

/* Override the port signature after reset, so one HBA can present a plain SATA disk
 * while another presents the optical drive (#262). */
void hype_ahci_set_signature(hype_ahci_t *ahci, uint32_t sig);

/*
 * #309: AHCI software reset (AHCI 1.3.1 SS10.4.1).
 *
 * A Register H2D FIS whose C bit is CLEAR is not a command at all -- it is a write to the
 * device's Control register. A driver resets a port with two of them: the first with SRST
 * set, the second with it clear. Both carry ATA command 0x00, which is why hype used to
 * refuse them as "unmodelled ATA command 0x0", and why FreeBSD never issued a single
 * IDENTIFY -- it resets a port before probing it, so refusing the reset meant the attached
 * device was never identified at all.
 */
#define HYPE_AHCI_FIS_H2D_FLAG_C 0x80u /* raw[1] bit 7: this FIS updates the Command register */
#define HYPE_AHCI_ATA_CONTROL_SRST 0x04u /* raw[15] bit 2: Device Control register's SRST */

/* 1 if `raw` is a Control-register write rather than a command (C bit clear). */
int hype_ahci_h2d_is_control_write(const uint8_t raw[20]);

/*
 * Advance the port's reset state for a Control-register write carrying `control_byte`, and
 * clear command slot `slot`. Returns 1 if the caller must now deliver a D2H Register FIS
 * carrying the port signature (the reset has completed), 0 if not.
 *
 * The reset is modelled as INSTANTANEOUS, exactly as GHC.HR already is: real hardware
 * asserts BSY and clears it when the reset finishes, and a guest polling for that clearing
 * must not be made to wait out a timeout.
 */
int hype_ahci_soft_reset(hype_ahci_t *ahci, uint8_t control_byte, unsigned slot);

/*
 * #314: the received-FIS builders.
 *
 * Every command hype completes has to leave bytes in the port's Received FIS area for the
 * guest to read, and there were three separate hand-rolled copies of "build a completion
 * FIS" -- one per completion path plus the signature one. They had already drifted: the
 * ATAPI path set PxIS.PSS for a PIO-in without ever writing the PIO Setup FIS that goes with
 * it, which is what made FreeBSD's ATAPI_IDENTIFY time out on a command hype had finished
 * correctly. These builders are the single definition, and are unit-tested directly.
 *
 * Offsets within the Received FIS area (AHCI 1.3.1 SS4.2.1) are the caller's business: the
 * D2H Register FIS goes at 0x40 and the PIO Setup FIS at 0x20.
 */
#define HYPE_AHCI_FIS_TYPE_D2H_REGISTER 0x34u /* fis[0] of a Register Device-to-Host FIS */
#define HYPE_AHCI_FIS_TYPE_PIO_SETUP 0x5Fu    /* fis[0] of a PIO Setup Device-to-Host FIS */
#define HYPE_AHCI_FIS_D2H_FLAG_I 0x40u        /* fis[1] bit 6: interrupt */
#define HYPE_AHCI_FIS_PIO_FLAG_D 0x20u        /* fis[1] bit 5: device-to-host direction */

/*
 * Build the 20-byte Register Device-to-Host FIS that reports a command's result registers.
 * `flags` is fis[1] verbatim -- callers that want the port interrupt asked for in the FIS
 * itself pass HYPE_AHCI_FIS_D2H_FLAG_I. hype sets PxIS explicitly either way, so the two
 * existing completion paths pass 0 and keep the exact bytes EDK2 already accepts; whether
 * they should assert I is a separate question from de-duplicating them.
 */
void hype_ahci_build_d2h_fis(uint8_t fis[20], uint8_t flags, uint8_t status_reg,
                             uint8_t error_reg);

/*
 * Build the 20-byte PIO Setup Device-to-Host FIS that a PIO data-in command must deliver in
 * ADDITION to latching PxIS.PSS. `xfer_bytes` is the byte count actually transferred.
 *
 * Latching the bit alone is not enough: EDK2's AhciPioTransfer waits on the PxIS.PSS bit, but
 * FreeBSD reads this FIS's E_Status and Transfer Count to end the transaction, so for FreeBSD
 * a completion signalled only through the bit never happens at all.
 */
void hype_ahci_build_pio_setup_fis(uint8_t fis[20], uint8_t status_reg, uint8_t error_reg,
                                   uint32_t xfer_bytes);

/*
 * Build the 20-byte D2H Register FIS that completes a software reset. The LBA and
 * sector-count fields carry `sig` (a PxSIG value), which is how the driver learns whether it
 * reset a packet device or a plain disk -- an all-zero FIS would leave it unable to tell.
 */
void hype_ahci_build_signature_fis(uint8_t fis[20], uint8_t status_reg, uint8_t error_reg,
                                   uint32_t sig);

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

/* Returns 1 if the HBA would be asserting its interrupt line right now,
 * 0 otherwise. Per AHCI 1.3.1 SS5.5.3 (interrupt generation): a port
 * raises an interrupt when (PxIS & PxIE) != 0, and the HBA asserts its
 * PCI interrupt only while GHC.IE is also set. This project models one
 * port, so the condition reduces to GHC.IE && (p_is & p_ie). Pure read;
 * the caller (the vCPU loop) turns a transition-to-pending into a raised
 * PIC IRQ line, and the guest deasserts by clearing PxIS/IS (RW1C). */
int hype_ahci_irq_pending(const hype_ahci_t *ahci);

/* #512: post completion status -- sets PxIS bits and counts the interrupt-condition edge.
 * Every completion path must use this instead of writing p_is directly, or an MSI edge can
 * be swallowed by the sampled-level race described at hype_ahci_t.irq_events. */
void hype_ahci_set_pis(hype_ahci_t *ahci, uint32_t bits);

/*
 * Processes one issued AHCI command slot: walks the guest's Command List ->
 * Command Table -> PRDT, executes the SATA/ATAPI command against the device
 * models, DMAs data to/from guest RAM (translated via dma_map; 0 = identity),
 * writes the receive FIS, and clears the slot's PxCI bit. Vendor-neutral (no
 * vcpu context) -- the SVM and VMX MMIO handlers both call it on a PxCI write.
 * Defined in arch/x86_64/svm/svm_vcpu.c. Returns 0 on success, -1 on error.
 */
int process_ahci_command_slot(hype_ahci_t *ahci, hype_atapi_t *atapi,
                              const hype_gpa_map_t *dma_map, unsigned slot);

/* Processes AHCI command slot 0 against a SATA (non-ATAPI) disk model -- the
 * disk counterpart of process_ahci_command_slot(). Vendor-neutral; the SVM and
 * VMX ahci_disk MMIO handlers both call it on a PxCI slot-0 write. Defined in
 * arch/x86_64/svm/svm_vcpu.c. Returns 0 on success, -1 on error. */
/*
 * #262 slice 3: `dma_map` translates the GUEST-physical addresses the command carries
 * (command list, command table, each PRD's data pointer, the RX FIS) into host
 * addresses. Pass 0 for a trusted identity-mapped guest -- M5-2's microtest -- exactly
 * as the ATAPI path's own convention. The FW-1 guest remaps its RAM, so passing 0
 * there dereferences guest addresses as host pointers and faults hype.
 */
int process_ahci_ata_command_slot(hype_ahci_t *ahci, hype_ata_disk_t *disk,
                                  const hype_gpa_map_t *dma_map, unsigned slot);

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
