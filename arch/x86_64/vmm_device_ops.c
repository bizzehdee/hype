/*
 * #693/#694 (AUDIT-5, split from #659): vendor-neutral device-emulation
 * functions extracted out of arch/x86_64/svm/svm_vcpu.c, where they used to
 * live despite touching no privileged instruction and taking no vCPU
 * context -- only already-decoded device-model structs and the VM's guest
 * DMA map. Both backends (arch/x86_64/svm/svm_vcpu.c, arch/x86_64/vmx/
 * vmcs_hw.c) call these symbols directly; process_virtio_blk_queue() and
 * process_ahci_command_slot()/process_ahci_ata_command_slot() were already
 * shared this way (declared in devices/virtio_blk.h / devices/ahci.h)
 * before this move, which only relocates the *definitions* out of a file
 * that core/tests/run.sh's is_exempt() blanket-excludes from the coverage
 * floor (#659's AUDIT-5 finding) and into one that does not.
 *
 * g_ahci_trace is the one piece of state this file shares with svm_vcpu.c:
 * both process_ahci_command_slot() here and hype_svm_ahci_atapi_npf_common()
 * there gate the same ABAR MMIO trace on it, so it stays a single
 * non-static flag defined in svm_vcpu.c (setter: hype_svm_set_ahci_trace())
 * rather than being duplicated.
 */

#include "../../core/fatal.h"
#include "../../core/guest_mem.h"
#include "../../core/iso_stream.h"
#include "../../core/blk_backend.h"
#include "../../devices/ahci.h"
#include "../../devices/atapi.h"
#include "../../devices/ata_disk.h"
#include "../../devices/virtio_blk.h"

/* Defined non-static in arch/x86_64/svm/svm_vcpu.c; setter is
 * hype_svm_set_ahci_trace() (declared in arch/x86_64/svm/svm.h). See the
 * file comment above for why this one flag is shared rather than moved. */
extern int g_ahci_trace;

/* ---- AHCI: complete_ahci_soft_reset / process_ahci_command_slot ---- */

/* Copy n bytes 8 at a time (byte tail last) into a PRDT-described guest buffer.
 * __builtin_memcpy with a constant size lowers to a single unaligned mov
 * (x86_64 allows unaligned access), so there is no libc/memcpy dependency (this
 * is a freestanding build) and no strict-aliasing UB. ~8x fewer store ops than
 * the old byte loop for the flat-media / IDENTIFY PRDT copies. The streamed-media
 * path (below) still fetches into the guest buffer directly, one read call
 * per PRD, since that traffic already goes through hype_iso_stream_read(). */
static void ahci_copy_fast(uint8_t *dst, const uint8_t *src, uint32_t n) {
    uint32_t k = 0;
    while (k + 8u <= n) {
        __builtin_memcpy(dst + k, src + k, 8);
        k += 8u;
    }
    while (k < n) {
        dst[k] = src[k];
        k++;
    }
}

/*
 * #309: complete an AHCI software reset. See hype_ahci_soft_reset() for the protocol; this
 * is the part that needs the guest's Received-FIS area, so it lives with the other
 * DMA-touching code rather than in the device model.
 *
 * Returns 0 on success (the slot is completed either way), -1 only if the guest's FIS area
 * fails its VALID-3 bounds check.
 */
static int complete_ahci_soft_reset(hype_ahci_t *ahci, uint64_t rx_fis_phys,
                                    const hype_gpa_map_t *dma_map, unsigned slot,
                                    uint8_t control_byte) {
    uint8_t fis[20];
    uint8_t *rx_fis_host;
    unsigned i;

    if (!hype_ahci_soft_reset(ahci, control_byte, slot)) {
        return 0; /* SRST asserted, or a Control write announcing nothing: no FIS to post */
    }

    rx_fis_host = (uint8_t *)(uintptr_t)hype_guest_dma_xlate(dma_map, rx_fis_phys, 0x40u + 20u);
    if (rx_fis_host == 0) {
        hype_debug_print("ahci: slot %u reset -- received-FIS area gpa 0x%llx out of bounds\n",
                         slot, (unsigned long long)rx_fis_phys);
        return -1;
    }
    hype_ahci_build_signature_fis(fis, (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_DSC), 0,
                                  ahci->p_sig);
    for (i = 0; i < 20u; i++) {
        rx_fis_host[0x40 + i] = fis[i];
    }
    hype_ahci_set_pis(ahci, HYPE_AHCI_PIS_DHRS); /* #512: counted edge */
    if ((ahci->p_is & ahci->p_ie) != 0) {
        ahci->is |= HYPE_AHCI_IS_PORT0;
    }
    return 0;
}

/* #344: bounded completion trace, see its use below. */
static unsigned int g_atapi_completion_traced;

/*
 * #343: ATAPI transfer accounting. A SHORT transfer -- the PRDT list exhausted with bytes still
 * owed -- is reported to the guest as success, so nothing else in the system can notice it. Any
 * non-zero short count means a guest was handed a partly-filled buffer and told the read completed.
 *
 * Counters, not a trace: the ISO stream trace is capped at 24 records and the guest's kernel load
 * happens long after those, which is exactly why the first pass at this question had no evidence
 * either way. See #356 for the same lesson at greater cost.
 */
static unsigned long long g_atapi_xfers = 0;
static unsigned long long g_atapi_short_xfers = 0;
static unsigned long long g_atapi_req_bytes = 0;
static unsigned long long g_atapi_done_bytes = 0;
static unsigned long long g_atapi_owed_bytes = 0;
#if HYPE_343_VERIFY_READS
static unsigned long long g_343_verified = 0;
static unsigned long long g_343_mismatch = 0;
#endif

void hype_svm_vcpu_get_atapi_diag(unsigned long long *xfers, unsigned long long *short_xfers,
                                  unsigned long long *req_bytes, unsigned long long *done_bytes,
                                  unsigned long long *owed_bytes) {
    if (xfers != 0) { *xfers = g_atapi_xfers; }
    if (short_xfers != 0) { *short_xfers = g_atapi_short_xfers; }
    if (req_bytes != 0) { *req_bytes = g_atapi_req_bytes; }
    if (done_bytes != 0) { *done_bytes = g_atapi_done_bytes; }
    if (owed_bytes != 0) { *owed_bytes = g_atapi_owed_bytes; }
}

#if HYPE_343_VERIFY_READS
void hype_svm_vcpu_get_read_verify(unsigned long long *checked, unsigned long long *mismatched) {
    if (checked != 0) { *checked = g_343_verified; }
    if (mismatched != 0) { *mismatched = g_343_mismatch; }
}
#endif

/*
 * #372: refuse a command when the guest has not enabled PCI Bus Master.
 *
 * Every structure this function touches -- the command list, the command table, each PRD's data
 * pointer, the receive FIS -- is reached by the controller MASTERING THE BUS. With BME clear the
 * hardware cannot issue any of those cycles, so the command sits in PxCI and never retires, and a
 * driver polling for completion spins forever. That is the failure a guest driver which forgot to
 * set the bit must be allowed to see here.
 *
 * Leaving PxCI set is the whole behaviour: returning "done" or clearing the slot would hide it.
 * Said once on the diagnostic channel, because an operator debugging their own guest driver
 * deserves the reason rather than hype's silence -- which would only move the confusion one layer
 * down, and is the same mistake as a counter that cannot observe its own subject.
 */
static int ahci_bus_master_refused(const hype_ahci_t *ahci, const char *what) {
    static int reported;
    if (ahci->bus_master != 0) {
        return 0;
    }
    if (!reported) {
        reported = 1;
        hype_debug_print("ahci: %s IGNORED -- the guest has not set PCI Bus Master Enable "
                         "(Command bit 2), so the controller cannot reach the command list or any "
                         "PRD. PxCI stays set and this command will never complete, exactly as on "
                         "real hardware. [#372]\n",
                         what);
    }
    return 1;
}

/* Walks the guest's Command List (slot 0 only, this project's own
 * single-outstanding-command scope) -> Command Table -> ATAPI CDB,
 * dispatches it, copies the response into the PRDT-described guest
 * buffer(s), and updates the port's completion-observable state.
 * Every guest-memory access here is a plain pointer dereference, same
 * flat-identity-map reasoning as hype_svm_vcpu_handle_npf()'s own
 * instruction-byte fetch. Returns 0 if the command was a recognized
 * ATAPI PACKET command, -1 otherwise (a raw ATA command, or a Command
 * FIS that isn't even a Register H2D FIS) -- this project's own scope
 * is "one ATAPI CD-ROM," never a raw ATA disk on this port, so
 * anything else is fail-closed rather than guessed at, matching every
 * other MMIO/NPF handler's convention here. */
int process_ahci_command_slot(hype_ahci_t *ahci, hype_atapi_t *atapi,
                              const hype_gpa_map_t *dma_map, unsigned slot) {
    uint64_t cmd_list_phys =
        ((uint64_t)ahci->p_clb | ((uint64_t)ahci->p_clbu << 32)) + (uint64_t)slot * 32u;
    uint64_t rx_fis_phys = (uint64_t)ahci->p_fb | ((uint64_t)ahci->p_fbu << 32);
    uint8_t *cmd_hdr_bytes;
    hype_ahci_cmd_header_t hdr;
    const uint8_t *cmd_table_bytes;
    const uint8_t *prdt_bytes;
    uint8_t *rx_fis_host;
    hype_atapi_result_t result;
    uint8_t identify[HYPE_ATAPI_IDENTIFY_SIZE];
    int media_read_failed = 0; /* #287: backing-store read failed -> complete with ERR */
    const uint8_t *src;
    /* Default 0: the ATA paths (IDENTIFY PACKET / SET FEATURES) and the synth
     * ATAPI responses copy from a flat `src`; only a media-data ATAPI read on a
     * streamed backing sets this to 1 (below). Must be initialised or those paths
     * would take the streamed read with a stale media_offset -> spurious failure. */
    int stream_media = 0; /* GLADDER-10: media served on demand from a raw disk partition */
    uint64_t media_byte_off = 0; /* GLADDER-10(b): 64-bit byte offset = media_lba * sector size */
    uint32_t remaining;
    uint32_t transferred;
    uint32_t prd_idx;
    uint8_t status_reg;
    uint8_t error_reg;
    uint32_t pis_bit;
    int packet_pio_in = 0;
    uint8_t *d2h_fis;
    unsigned i;

    /* #372: before any of it -- can this controller master the bus at all? Returning 0 (not -1)
     * because nothing is WRONG: the guest asked for something the hardware would silently not do,
     * and the caller must not treat that as a decode failure and panic. PxCI stays set. */
    if (ahci_bus_master_refused(ahci, "PxCI write")) {
        return 0;
    }

    /* VALID-3: every guest-physical address the AHCI command structures
     * carry is guest-controlled, so each is translated through the VM's
     * bounds-checked gpa map (VALID-1) -- with its access length -- and
     * a rejected (0) translation fails the command rather than
     * dereferencing an out-of-range host pointer. The command header is
     * the 32-byte slot-0 entry. */
    cmd_hdr_bytes = (uint8_t *)(uintptr_t)hype_guest_dma_xlate(dma_map, cmd_list_phys, 32u);
    if (cmd_hdr_bytes == 0) {
        /* #309: every refusal here is reported unconditionally, not behind g_ahci_trace.
         * The only caller treats -1 as fatal and panics with "unhandled AHCI ABAR MMIO",
         * which names the PxCI register rather than the command that was actually refused
         * -- one message covering a decoder gap, an unmodelled register and a rejected
         * command. Whatever the reason, it is worth a line when the guest is about to die. */
        hype_debug_print("ahci: slot %u refused -- command list at gpa 0x%llx out of bounds\n",
                         slot, (unsigned long long)cmd_list_phys);
        return -1;
    }

    hype_ahci_decode_cmd_header(cmd_hdr_bytes, &hdr);
    /* Command Table = 0x80-byte CFIS/ACMD/reserved block + prdtl 16-byte
     * PRDT entries. A malicious prdtl that would run the table off the
     * region is caught here (the length is computed in 64-bit so it
     * cannot wrap before the check). */
    cmd_table_bytes = (const uint8_t *)(uintptr_t)hype_guest_dma_xlate(
        dma_map, hdr.cmd_table_phys, (uint64_t)0x80u + (uint64_t)hdr.prdtl * 16u);
    if (cmd_table_bytes == 0) {
        hype_debug_print("ahci: slot %u refused -- command table at gpa 0x%llx (prdtl=%u) out of "
                         "bounds\n",
                         slot, (unsigned long long)hdr.cmd_table_phys, (unsigned int)hdr.prdtl);
        return -1;
    }

    if (!hdr.is_atapi) {
        /* A plain H2D Register FIS command (Command Header's ATAPI bit
         * clear). A real AHCI driver issues two of these to an ATAPI
         * device during setup (EDK2 AhciModeInitialization):
         *   - IDENTIFY PACKET DEVICE (0xA1): PIO data-in of the fixed
         *     512-byte identify block. The driver waits for a PIO Setup
         *     FIS (PxIS.PSS) and requires PRDBC == 512.
         *   - SET FEATURES (0xEF): a no-data command selecting the
         *     transfer mode -- acknowledged with a data-less success
         *     (D2H FIS, PxIS.DHRS). */
        uint8_t ata_cmd = cmd_table_bytes[2];
        if (cmd_table_bytes[0] != 0x27u) {
            hype_debug_print("ahci: slot %u refused -- not a Register H2D FIS (type=0x%x cmd=0x%x)\n",
                             slot, (unsigned int)cmd_table_bytes[0], (unsigned int)ata_cmd);
            return -1;
        }
        /* #309: a Control-register write (C bit clear), not a command -- the software-reset
         * protocol FreeBSD runs before it will probe the port at all. */
        if (hype_ahci_h2d_is_control_write(cmd_table_bytes)) {
            return complete_ahci_soft_reset(ahci, rx_fis_phys, dma_map, slot, cmd_table_bytes[15]);
        }
        if (ata_cmd == HYPE_AHCI_ATA_CMD_IDENTIFY_PACKET_DEVICE) {
            hype_atapi_build_identify(atapi, identify);
            src = identify;
            remaining = HYPE_ATAPI_IDENTIFY_SIZE;
            status_reg = 0x50u; /* DRDY|DSC */
            error_reg = 0;
            /* #358: both bits, for the same reason as IDENTIFY DEVICE on the disk port -- this is
             * also a PIO data-in. It has worked with PSS alone because EDK2's ATAPI probe is
             * satisfied by the PIO Setup FIS, so this half is a correctness fix rather than a fix
             * for an observed failure; validated in the same run as the disk change, and the CD
             * still booting is the check that matters. */
            pis_bit = HYPE_AHCI_PIS_DHRS | HYPE_AHCI_PIS_PSS;
            if (g_ahci_trace) {
                hype_debug_print("ahci-trace: IDENTIFY PACKET DEVICE (0xA1) -> 512-byte PIO-in\n");
            }
        } else if (ata_cmd == HYPE_AHCI_ATA_CMD_SET_FEATURES) {
            src = identify; /* unused: no data transferred (remaining == 0) */
            remaining = 0;
            status_reg = 0x50u; /* DRDY|DSC, no error */
            error_reg = 0;
            pis_bit = HYPE_AHCI_PIS_DHRS;
            if (g_ahci_trace) {
                hype_debug_print("ahci-trace: SET FEATURES (0xEF) -> no-data ack\n");
            }
        } else {
            hype_debug_print("ahci: slot %u refused -- unmodelled ATA command 0x%x on the ATAPI "
                             "port (FIS type=0x%x)\n",
                             slot, (unsigned int)ata_cmd, (unsigned int)cmd_table_bytes[0]);
            return -1;
        }
    } else {
        uint8_t cdb[HYPE_ATAPI_CDB_MAX];
        if (cmd_table_bytes[0] != 0x27u || cmd_table_bytes[2] != 0xA0u) {
            /* not a Register H2D FIS carrying ATA_CMD_PACKET (0xA0) */
            hype_debug_print("ahci: slot %u refused -- ATAPI header but FIS type=0x%x cmd=0x%x, "
                             "expected 0x27/0xa0\n",
                             slot, (unsigned int)cmd_table_bytes[0], (unsigned int)cmd_table_bytes[2]);
            return -1;
        }
        for (i = 0; i < HYPE_ATAPI_CDB_MAX; i++) {
            cdb[i] = cmd_table_bytes[0x40 + i];
        }

        hype_atapi_execute_cdb(atapi, cdb, &result);

        if (g_ahci_trace) {
            /* #318: print the DECODED 32-bit LBA and block count for the read commands, not two
             * raw CDB bytes -- the whole point of this trace is comparing the requested LBAs
             * against the ISO's real directory extents, which bytes 2 and 5 alone cannot do. */
            uint32_t t_lba = ((uint32_t)cdb[2] << 24) | ((uint32_t)cdb[3] << 16) |
                             ((uint32_t)cdb[4] << 8) | (uint32_t)cdb[5];
            uint32_t t_cnt = (cdb[0] == 0xA8u)
                                 ? (((uint32_t)cdb[6] << 24) | ((uint32_t)cdb[7] << 16) |
                                    ((uint32_t)cdb[8] << 8) | (uint32_t)cdb[9])
                                 : (((uint32_t)cdb[7] << 8) | (uint32_t)cdb[8]);
            hype_debug_print(
                "ahci-trace: ATAPI CDB=0x%x lba=%u count=%u status=%s uses_media=%u len=%u\n",
                (unsigned int)cdb[0], (unsigned int)t_lba, (unsigned int)t_cnt,
                result.status == HYPE_ATAPI_STATUS_GOOD ? "GOOD" : "CHECK",
                (unsigned int)result.uses_media_data,
                (unsigned int)(result.uses_media_data ? result.media_length : result.synth_length));
        }

        /* GLADDER-10(a): media may be backed by a CHUNKED (non-contiguous) ISO
         * rather than a flat buffer. For flat media/synth, `src` is a plain
         * pointer advanced per PRD; for streamed media, `src` is unused and each
         * PRD reads from the chunk list at logical offset media_byte_off+transferred.
         * GLADDER-10(b): the byte offset is derived here from the 32-bit start
         * sector (media_lba) with a 64-bit multiply, so a >=4GB ISO (byte offset
         * past UINT32_MAX) addresses the right bytes -- the result struct only
         * needs to carry a 32-bit sector index (good to 8TB). */
        media_byte_off = (uint64_t)result.media_lba * (uint64_t)HYPE_ATAPI_SECTOR_SIZE;
        stream_media = result.uses_media_data && atapi->media_stream != 0;
        src = (result.uses_media_data && !stream_media)
                  ? (atapi->media_data + media_byte_off)
                  : (result.uses_media_data ? 0 : result.synth_data);
        remaining = result.uses_media_data ? result.media_length : result.synth_length;
        /* ATA STATUS register: DRDY|DSC always, +ERR on CHECK_CONDITION.
         * ATAPI convention: a failed PACKET command's ERROR register
         * carries the SCSI sense key in its upper nibble. */
        status_reg = (result.status == HYPE_ATAPI_STATUS_GOOD) ? 0x50u : 0x51u;
        error_reg = (result.status == HYPE_ATAPI_STATUS_GOOD) ? 0u : (uint8_t)(atapi->sense_key << 4);
        /* ATAPI PACKET data/no-data commands complete with a Device-to-
         * Host Register FIS (EDK2's AhciPioTransfer/AhciNonDataTransfer
         * wait on PxIS.DHRS for them). */
        pis_bit = HYPE_AHCI_PIS_DHRS;
        /* #318: ...but a PACKET command that moves data in PIO mode must ALSO be given a PIO
         * Setup FIS carrying the byte count, which is how a driver that reads the receive area
         * (rather than just the PxIS bit, as EDK2 does) learns how much arrived. Without it
         * OpenBSD's atapiscsi treats every READ(10) as suspect and re-interrogates the device
         * with TEST UNIT READY + REQUEST SENSE, tripling the command count per sector.
         * H2D Features bit 0 is the ATAPI DMA bit: set means a DMA transfer, which ends with
         * the D2H FIS alone and no PIO Setup. */
        packet_pio_in = (cmd_table_bytes[3] & 0x01u) == 0;
    }

    prdt_bytes = cmd_table_bytes + 0x80;
    prd_idx = 0;
    transferred = 0;
    g_atapi_xfers++;
    {
        /* #343: what the command ASKED for, before the PRDT list can cut it short. */
        g_atapi_req_bytes += (uint64_t)remaining;
    }
    while (remaining > 0 && prd_idx < hdr.prdtl) {
        hype_ahci_prdt_entry_t prd;
        uint32_t chunk;
        uint8_t *dst;

        hype_ahci_decode_prdt_entry(prdt_bytes + (uint32_t)prd_idx * 16u, &prd);
        chunk = (prd.byte_count < remaining) ? prd.byte_count : remaining;
        /* VALID-3: the PRD data buffer is guest-supplied -- bounds-check
         * [data_phys, data_phys+chunk) before writing the response into
         * it, so a guest-programmed PRD can never steer the copy at
         * hypervisor or another VM's memory. */
        dst = (uint8_t *)(uintptr_t)hype_guest_dma_xlate(dma_map, prd.data_phys, chunk);
        if (dst == 0) {
            hype_debug_print("ahci: slot %u refused -- PRD %u buffer gpa 0x%llx len %u out of "
                             "bounds\n",
                             slot, (unsigned int)prd_idx, (unsigned long long)prd.data_phys,
                             (unsigned int)chunk);
            return -1;
        }
        if (stream_media) {
            /* GLADDER-10: fetch these bytes on demand from the raw ISO partition
             * (disk read via hype_ahci_host_read) instead of a RAM copy. */
            static unsigned g_stream_dbg = 0;
            int srr = hype_iso_stream_read(atapi->media_stream, media_byte_off + transferred, dst,
                                           chunk);
#if HYPE_343_VERIFY_READS
            /*
             * #343: read the SAME range again and compare it against what was just written into
             * guest memory.
             *
             * The guest faults on a page of its own kernel image that is absent, which is a hole in
             * a loaded file rather than a truncated one -- so the question is whether hype ever
             * hands the guest something other than the ISO's bytes. Aggregate counters have
             * answered what they can (stream failures zero, ATAPI short transfers identical in
             * clean runs); this compares content, per read, which is the only thing left that can
             * distinguish "delivered wrong bytes" from "delivered fine and the guest lost the page".
             *
             * DIAGNOSTIC ONLY, compile-time gated: it doubles the reads on this path. A mismatch is
             * reported with the offset and the first diverging byte so the failing range can be
             * matched against the kernel image's own layout.
             */
            if (srr == 0) {
                static uint8_t v343[4096];
                static unsigned v343_reported = 0;
                uint32_t vlen = (chunk <= sizeof(v343)) ? chunk : (uint32_t)sizeof(v343);
                if (hype_iso_stream_read(atapi->media_stream, media_byte_off + transferred, v343,
                                         vlen) == 0) {
                    uint32_t vi;
                    g_343_verified++;
                    for (vi = 0; vi < vlen; vi++) {
                        if (v343[vi] != dst[vi]) {
                            g_343_mismatch++;
                            if (v343_reported < 8u) {
                                v343_reported++;
                                hype_debug_print("fw-1 #343 MISMATCH: iso_off=%llu +%u delivered=%02x "
                                                 "reread=%02x (chunk=%u)\n",
                                                 (unsigned long long)(media_byte_off + transferred),
                                                 (unsigned)vi, (unsigned)dst[vi], (unsigned)v343[vi],
                                                 (unsigned)chunk);
                            }
                            break;
                        }
                    }
                }
            }
#endif
            /* #346: the loader stops after reading root-dir LBA 51 and never fetches /etc
             * (LBA 56), so dump the exact bytes delivered for the DIRECTORY sectors -- those
             * decide what it looks for next. QEMU reads 56; real hardware does not, with every
             * layer below byte-perfect, so what it PARSED from 51 is the open question. */
            if (result.media_lba == 51u || result.media_lba == 56u) {
                hype_debug_print("dirsec lba=%llu off=%llu: %02x %02x %02x %02x %02x %02x %02x %02x "
                                 "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                                 (unsigned long long)result.media_lba,
                                 (unsigned long long)(media_byte_off + transferred),
                                 dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], dst[6], dst[7],
                                 dst[8], dst[9], dst[10], dst[11], dst[12], dst[13], dst[14],
                                 dst[15]);
            }
            if (g_stream_dbg < 24u || srr != 0) {
                g_stream_dbg++;
                /* #346: include the first 8 bytes AS DELIVERED TO GUEST RAM. On real hardware the
                 * guests retry LBA 0/16 forever with every layer below proven byte-perfect -- this
                 * shows whether the LAST hop (this very copy) is where the bytes go wrong. */
                hype_debug_print("stream-rd #%u: off=%llu chunk=%u lba0=%llu isosz=%llu ret=%d "
                                 "dst=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                                 g_stream_dbg, (unsigned long long)(media_byte_off + transferred),
                                 (unsigned)chunk,
                                 (unsigned long long)atapi->media_stream->part_start_lba,
                                 (unsigned long long)atapi->media_stream->iso_size, srr,
                                 dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], dst[6], dst[7]);
            }
            if (srr != 0) {
                /*
                 * #287: a BACKING-STORE failure is not "this is not my command".
                 *
                 * Returning -1 here meant the caller fell through to its
                 * unhandled-MMIO path and PANICKED on the guest's next perfectly
                 * ordinary ABAR write -- blaming a register that is in fact modelled,
                 * eleven log lines away from the read that actually failed. Any
                 * transient host-disk error took down the hypervisor and every guest.
                 *
                 * Report what a real drive reports instead: MEDIUM ERROR / unrecovered
                 * read error. Guests and firmware both know how to handle that, and
                 * hype stays up. Same spirit as GLADDER-1 absorbing unhandled MMIO
                 * rather than dying.
                 */
                hype_atapi_set_media_error(atapi, HYPE_ATAPI_SENSE_KEY_MEDIUM_ERROR,
                                           HYPE_ATAPI_ASC_UNRECOVERED_READ_ERROR);
                media_read_failed = 1;
                break;
            }
        } else {
            ahci_copy_fast(dst, src, chunk);
            src += chunk;
        }
        remaining -= chunk;
        transferred += chunk;
        prd_idx++;
    }

    /*
     * #287: a backing-store read failed part-way. Complete the command with ERR set
     * and the sense already stashed, rather than returning early -- an early return
     * leaves PxCI set and the guest waits on a command that will never finish, which
     * is a hang instead of an error. PRDBC below reports the partial count, which is
     * what a real drive does on a short/failed transfer.
     */
    if (media_read_failed) {
        status_reg = (uint8_t)(0x50u | 0x01u); /* DRDY|DSC|ERR */
        error_reg = (uint8_t)(atapi->sense_key << 4);
    }

    /*
     * #343: the loop above exits when the PRDT list runs out, NOT only when the request is
     * satisfied -- so a guest whose PRDT does not cover its own block count gets a SHORT transfer
     * reported as success, with PRDBC honestly reporting the short count. A driver that checks
     * PRDBC notices; one that does not believes it read the whole thing and carries on with a
     * partially-filled buffer. Count it, because that is a silent wrong-data path and the reason
     * this counter exists is a FreeBSD guest that page-faulted on a page of its own kernel image.
     *
     * A counter rather than a trace: the stream trace is capped at 24 records and the kernel load
     * happens long after those, which is precisely why the first attempt at this had no evidence
     * either way (see #356 for the same lesson).
     */
    if (remaining > 0u) {
        g_atapi_short_xfers++;
        g_atapi_owed_bytes += (uint64_t)remaining;
    }
    g_atapi_done_bytes += (uint64_t)transferred;

    /* PRDBC (Command Header dword 1, byte offset 4): the count of bytes
     * actually transferred. EDK2's PIO-in path (AhciPioTransfer, used by
     * IDENTIFY PACKET DEVICE) checks PRDBC == the requested DataCount and
     * fails the command otherwise, so it must be written back into the
     * guest's command header. Harmless for the other paths that ignore
     * it. */
    cmd_hdr_bytes[4] = (uint8_t)(transferred & 0xFFu);
    cmd_hdr_bytes[5] = (uint8_t)((transferred >> 8) & 0xFFu);
    cmd_hdr_bytes[6] = (uint8_t)((transferred >> 16) & 0xFFu);
    cmd_hdr_bytes[7] = (uint8_t)((transferred >> 24) & 0xFFu);

    ahci->p_tfd = (uint32_t)status_reg | ((uint32_t)error_reg << 8);

    /* VALID-3: the Received FIS area is guest-supplied. Validate
     * [rx_fis, rx_fis+0x54) (the D2H Register FIS sits at offset 0x40,
     * 20 bytes) as one range -- computing the +0x40 on the host pointer
     * after translation, so a near-top guest address cannot overflow
     * before the check. */
    rx_fis_host = (uint8_t *)(uintptr_t)hype_guest_dma_xlate(dma_map, rx_fis_phys, 0x40u + 20u);
    if (rx_fis_host == 0) {
        hype_debug_print("ahci: slot %u refused -- received-FIS area gpa 0x%llx out of bounds\n",
                         slot, (unsigned long long)rx_fis_phys);
        return -1;
    }
    /*
     * #314: a PIO data-in command must ALSO deliver a PIO Setup FIS at receive-area offset
     * 0x20, not merely latch PxIS.PSS.
     *
     * #262 slice 4 added this to the plain-ATA path and its comment claims the ATAPI path
     * "already does" it for IDENTIFY PACKET -- it does not; it sets the bit and nothing else,
     * so this receive area stayed whatever the guest left there. EDK2 waits on the PxIS.PSS
     * BIT, so the CD has always worked; FreeBSD reads the FIS itself, and its
     * ATAPI_IDENTIFY timed out on a completion hype had already finished (cs 00000000,
     * tfd 50, is 00000002).
     */
    if (packet_pio_in && transferred > 0) {
        pis_bit |= HYPE_AHCI_PIS_PSS;
    }
    if ((pis_bit & HYPE_AHCI_PIS_PSS) != 0) {
        hype_ahci_build_pio_setup_fis(rx_fis_host + 0x20, status_reg, error_reg, transferred);
    }

    d2h_fis = rx_fis_host + 0x40;
    hype_ahci_build_d2h_fis(d2h_fis, 0, status_reg, error_reg);

    /*
     * #344: what hype actually PUBLISHED for this command, not merely that it completed.
     *
     * The wedge profile is 100% CPU with ZERO VM exits and no output -- a guest spinning on
     * memory it already owns, not on MMIO. Two explanations for that are now eliminated by
     * reading the code rather than by measurement: the FIS is posted (process_ahci_command_slot
     * is shared by both backends, so the "VMX never posts it" idea was wrong), and it is posted
     * BEFORE PxCI is cleared, on the guest's own vCPU inside a VM exit, so there is no window in
     * which the guest could see stale bytes.
     *
     * That leaves CONTENT. EDK2's AhciPioTransfer checks PRDBC against the count it asked for and
     * fails the command otherwise; a guest that then polls the FIS area waits forever on a
     * transfer it believes incomplete. So record the three things it reads -- the byte count
     * written back into the command header, the PIO Setup FIS, and the D2H Register FIS -- for
     * the first commands of a run, which is where the wedge happens.
     *
     * Bounded, because the stream trace being capped at 24 records is exactly why an earlier
     * attempt at this had no evidence either way (#356, and the #343 counter above).
     */
    if (g_atapi_completion_traced < 24u) {
        g_atapi_completion_traced++;
        hype_debug_print(
            "ahci-cpl #%u slot=%u xfer=%u short=%u tfd=0x%04x st=0x%02x err=0x%02x pis=0x%08x | "
            "pio[0..3]=%02x%02x%02x%02x cnt=%02x%02x | d2h[0..3]=%02x%02x%02x%02x [#344]\n",
            g_atapi_completion_traced, slot, (unsigned)transferred, (unsigned)remaining,
            (unsigned)ahci->p_tfd, (unsigned)status_reg, (unsigned)error_reg, (unsigned)pis_bit,
            rx_fis_host[0x20], rx_fis_host[0x21], rx_fis_host[0x22], rx_fis_host[0x23],
            rx_fis_host[0x2C], rx_fis_host[0x2D], d2h_fis[0], d2h_fis[1], d2h_fis[2], d2h_fis[3]);
    }

    ahci->p_ci &= (uint32_t)~(1u << slot); /* this slot complete */
    /* Completion interrupt-status bit (PxIS.DHRS for D2H completions,
     * PxIS.PSS for PIO-in). A guest that polls waits on this directly;
     * one that took the interrupt-driven path (M4-6d2) enabled PxIE, so
     * also latch the port's bit in the global IS register -- its ISR
     * (Linux ahci_interrupt) reads IS first to learn which port fired.
     * The vCPU loop turns (GHC.IE && PxIS&PxIE) into a raised PIC IRQ
     * via hype_ahci_irq_pending(). */
    hype_ahci_set_pis(ahci, pis_bit); /* #512: counted edge */
    if ((ahci->p_is & ahci->p_ie) != 0) {
        ahci->is |= HYPE_AHCI_IS_PORT0;
    }
    return 0;
}

/* Fills the D2H (Device to Host) completion FIS and clears PxCI's slot
 * 0 -- shared tail shape between the ATAPI and plain-ATA command
 * paths, byte-for-byte the same fields process_ahci_command_slot()
 * already builds for ATAPI. */
static int complete_ahci_command_slot(hype_ahci_t *ahci, uint64_t rx_fis_phys, uint8_t status_reg,
                                      uint8_t error_reg, const hype_gpa_map_t *dma_map,
                                      unsigned slot, uint32_t pis_bit, uint32_t xfer_bytes,
                                      uint8_t *cmd_hdr_bytes) {
    /* #262 slice 3: rx_fis_phys is GUEST-physical. Identity holds for M5-2's
     * microtest (dma_map == 0) but not for the FW-1 guest, which remaps its RAM. */
    uint64_t rx_fis_host = hype_guest_dma_xlate(dma_map, rx_fis_phys, 0x40u + 20u);
    uint8_t *d2h_fis;
    /*
     * #677: a rejected translation (guest PxFB/PxFBU pointing outside its own mapped
     * range) was being used unchecked below -- every other Received-FIS-area
     * translation in this file already refuses a 0 result (see
     * process_ahci_command_slot()'s own rx_fis_host check); this completion path,
     * shared by every plain-ATA command, did not. Found by the #602 fuzz harness.
     */
    if (rx_fis_host == 0) {
        hype_debug_print("ahci: slot %u refused -- received-FIS area gpa 0x%llx out of bounds\n",
                         slot, (unsigned long long)rx_fis_phys);
        return -1;
    }
    d2h_fis = (uint8_t *)(uintptr_t)(rx_fis_host + 0x40);

    /*
     * #262 slice 4: a PIO data-in command must also deliver a PIO Setup FIS at
     * receive-area offset 0x20. EDK2 drives the two device classes down different
     * paths -- ATAPI through AhciPacketCommandExecute, which waits on the D2H FIS
     * at 0x40, but plain-ATA PIO (IDENTIFY DEVICE) through AhciPioTransfer, which
     * waits at 0x20. Writing only the D2H FIS is enough for the CD and for Linux
     * (it polls PxCI), and is why the optical drive has always booted while the
     * disk did not: the guest firmware issued exactly one IDENTIFY, waited at 0x20
     * for a FIS that never arrived, and dropped the device.
     */
    if ((pis_bit & HYPE_AHCI_PIS_PSS) != 0) {
        hype_ahci_build_pio_setup_fis((uint8_t *)(uintptr_t)(rx_fis_host + 0x20), status_reg,
                                      error_reg, xfer_bytes);
    }

    /*
     * #358: PRDBC (Command Header dword 1, byte offset 4) -- the count of bytes actually
     * transferred, which the HBA is required to write back.
     *
     * This is what made the guest firmware refuse every guest DISK while the CD on an
     * identically-presented AHCI function worked. EDK2's AhciPioTransfer branches on the command
     * type, and only the plain-ATA branch checks it:
     *
     *   if (Read && (AtapiCommand == 0)) {
     *     AhciWaitUntilFisReceived (..., SataFisPioSetup);
     *     PrdCount = ...AhciCmdList[Slot].AhciCmdPrdbc;
     *     if (PrdCount == DataCount) Status = EFI_SUCCESS; else Status = EFI_DEVICE_ERROR;
     *   } else {
     *     AhciWaitUntilFisReceived (..., SataFisD2H);          // ATAPI: PRDBC never read
     *   }
     *
     * So IDENTIFY DEVICE read back 0 against an expected 512 and failed with
     * "PIO command failed at retry 0" -- one IDENTIFY, then the device dropped. The ATAPI path
     * has written PRDBC since #287 and takes the branch that ignores it anyway; the disk path
     * never wrote it and takes the branch that requires it.
     *
     * Linux and OpenBSD never noticed, which is why the disk works under both: they poll PxCI
     * and read the transfer length from the FIS rather than from the command header.
     */
    if (cmd_hdr_bytes != 0) {
        cmd_hdr_bytes[4] = (uint8_t)(xfer_bytes & 0xFFu);
        cmd_hdr_bytes[5] = (uint8_t)((xfer_bytes >> 8) & 0xFFu);
        cmd_hdr_bytes[6] = (uint8_t)((xfer_bytes >> 16) & 0xFFu);
        cmd_hdr_bytes[7] = (uint8_t)((xfer_bytes >> 24) & 0xFFu);
    }

    ahci->p_tfd = (uint32_t)status_reg | ((uint32_t)error_reg << 8);

    hype_ahci_build_d2h_fis(d2h_fis, 0, status_reg, error_reg);

    ahci->p_ci &= ~(1u << slot);
    /* PxIS.DHRS -- the D2H Register FIS interrupt bit a real driver
     * polls for a plain-ATA command's completion (same correction as
     * the ATAPI path; the M4-5/M5-2 cooperating test guests polled PxCI
     * and never depended on this bit). Latch the global IS port bit for
     * an interrupt-driven guest, same as the ATAPI path (M4-6d2). */
    hype_ahci_set_pis(ahci, pis_bit); /* #512: counted edge */
    if ((ahci->p_is & ahci->p_ie) != 0) {
        ahci->is |= HYPE_AHCI_IS_PORT0;
    }
    return 0;
}

/*
 * #94: move a backend-disk transfer through a guest PRDT whose entry
 * boundaries need not fall on sector boundaries. Windows' storahci builds
 * PRDs from whatever physical fragments the MDL has -- 1536-byte and
 * 512+1024-byte splits are routine -- and real AHCI hardware does not care.
 * The old per-PRD path refused any entry that split a sector (ABRT), which
 * failed every NTFS/FAT format and Setup's CreateSystemVolume with
 * "wrote 0 bytes" (measured: ATA-SHORT cmd=0xc8/0xca did=0 with prdtl=2..33).
 *
 * Full-sector spans inside one PRD go straight between guest RAM and the
 * backend; only a sector that straddles a PRD boundary is staged through a
 * 512-byte buffer. Returns 0 with *out_done = bytes moved (short if the PRDT
 * ran out -- the caller reports that via PRDBC), or -1 on a refused DMA
 * translation, or -2 on a backend I/O error.
 */
static int ahci_backend_rw_prdt(hype_ata_disk_t *disk, const hype_gpa_map_t *dma_map,
                                const uint8_t *prdt_bytes, uint16_t prdtl, uint64_t lba_base,
                                uint32_t total_bytes, int is_write, uint32_t *out_done) {
    unsigned idx = 0;
    uint32_t prd_off = 0;
    uint32_t done = 0;
    hype_ahci_prdt_entry_t prd;
    int prd_valid = 0;

    while (done < total_bytes) {
        uint32_t prd_rem;
        if (!prd_valid) {
            if (idx >= prdtl) {
                break; /* PRDT exhausted: genuine short transfer */
            }
            hype_ahci_decode_prdt_entry(prdt_bytes + (uint32_t)idx * 16u, &prd);
            prd_valid = 1;
        }
        prd_rem = prd.byte_count - prd_off;
        if (prd_rem == 0) {
            idx++;
            prd_off = 0;
            prd_valid = 0;
            continue;
        }
        if ((done % HYPE_ATA_SECTOR_SIZE) == 0u && prd_rem >= HYPE_ATA_SECTOR_SIZE) {
            /* Aligned full sectors within this PRD: one backend call. */
            uint32_t span = prd_rem;
            uint8_t *ptr;
            if (span > total_bytes - done) {
                span = total_bytes - done;
            }
            span -= span % HYPE_ATA_SECTOR_SIZE;
            ptr = (uint8_t *)(uintptr_t)hype_guest_dma_xlate(dma_map, prd.data_phys + prd_off, span);
            if (ptr == 0) {
                return -1;
            }
            if (is_write ? hype_blk_backend_write(disk->be, lba_base + done / HYPE_ATA_SECTOR_SIZE,
                                                  span / HYPE_ATA_SECTOR_SIZE, ptr)
                         : hype_blk_backend_read(disk->be, lba_base + done / HYPE_ATA_SECTOR_SIZE,
                                                 span / HYPE_ATA_SECTOR_SIZE, ptr)) {
                return -2;
            }
            done += span;
            prd_off += span;
            continue;
        }
        {
            /* A sector that straddles PRD boundaries (or an unaligned PRD
             * tail): stage it. Reads fetch the sector first and scatter;
             * writes gather and store once the sector is complete. */
            uint8_t stage[HYPE_ATA_SECTOR_SIZE];
            uint32_t sec_off = 0;
            uint64_t lba = lba_base + done / HYPE_ATA_SECTOR_SIZE;
            if (!is_write && hype_blk_backend_read(disk->be, lba, 1u, stage)) {
                return -2;
            }
            while (sec_off < HYPE_ATA_SECTOR_SIZE) {
                uint32_t chunk;
                uint8_t *ptr;
                uint32_t i;
                if (!prd_valid) {
                    if (idx >= prdtl) {
                        *out_done = done + sec_off; /* short inside a sector */
                        return 0;
                    }
                    hype_ahci_decode_prdt_entry(prdt_bytes + (uint32_t)idx * 16u, &prd);
                    prd_valid = 1;
                }
                prd_rem = prd.byte_count - prd_off;
                if (prd_rem == 0) {
                    idx++;
                    prd_off = 0;
                    prd_valid = 0;
                    continue;
                }
                chunk = (prd_rem < HYPE_ATA_SECTOR_SIZE - sec_off) ? prd_rem
                                                                   : HYPE_ATA_SECTOR_SIZE - sec_off;
                ptr = (uint8_t *)(uintptr_t)hype_guest_dma_xlate(dma_map, prd.data_phys + prd_off, chunk);
                if (ptr == 0) {
                    return -1;
                }
                if (is_write) {
                    for (i = 0; i < chunk; i++) {
                        stage[sec_off + i] = ptr[i];
                    }
                } else {
                    for (i = 0; i < chunk; i++) {
                        ptr[i] = stage[sec_off + i];
                    }
                }
                sec_off += chunk;
                prd_off += chunk;
            }
            if (is_write && hype_blk_backend_write(disk->be, lba, 1u, stage)) {
                return -2;
            }
            done += HYPE_ATA_SECTOR_SIZE;
        }
    }
    *out_done = done;
    return 0;
}

/* M5-2's plain-ATA command dispatch, the H2D-FIS-command-byte-driven
 * counterpart to process_ahci_command_slot()'s own ATAPI-only path.
 * Returns -1 for anything that isn't this handler's command (the
 * Command Header carries an ATAPI PACKET, or the H2D FIS isn't a valid
 * command FIS at all, or the command byte isn't one this project
 * models) so the caller can fall through to whichever other handler
 * actually owns it. */
int process_ahci_ata_command_slot(hype_ahci_t *ahci, hype_ata_disk_t *disk,
                                  const hype_gpa_map_t *dma_map, unsigned slot) {
    uint64_t cmd_list_phys =
        ((uint64_t)ahci->p_clb | ((uint64_t)ahci->p_clbu << 32)) + (uint64_t)slot * 32u;
    uint64_t rx_fis_phys = (uint64_t)ahci->p_fb | ((uint64_t)ahci->p_fbu << 32);
    /* #262 slice 3: every address the guest hands us here is GUEST-physical, so it
     * goes through hype_guest_dma_xlate. A NULL map means the trusted identity-mapped
     * microtest, matching the ATAPI path's convention exactly. */
    /* #358: writable -- PRDBC is written back into this header on completion, as a real HBA does. */
    uint8_t *cmd_hdr_bytes = (uint8_t *)(uintptr_t)hype_guest_dma_xlate(dma_map, cmd_list_phys, 32u);
    hype_ahci_cmd_header_t hdr;
    const uint8_t *cmd_table_bytes;
    const uint8_t *prdt_bytes;
    hype_ahci_h2d_fis_t fis;
    uint8_t identify[HYPE_ATA_IDENTIFY_SIZE];
    const uint8_t *src = 0;
    uint8_t *dst_media = 0;
    uint32_t remaining;
    uint32_t prd_idx;
    uint64_t transferred = 0; /* #262: byte offset within this command, for backend LBAs */
    uint64_t lba_base = 0;    /* decoded per address size -- NOT fis.lba, which is the raw 48-bit field */
    uint8_t status_reg;
    uint8_t error_reg;
    /*
     * #262 slice 4: which PxIS bit signals completion depends on the command's
     * PROTOCOL, not just on success. IDENTIFY DEVICE is PIO data-in, and EDK2's
     * AhciPioTransfer waits on PxIS.PSS for it -- exactly as the ATAPI path
     * already does for IDENTIFY PACKET. Everything else here is DMA or no-data,
     * which completes with a D2H Register FIS (PxIS.DHRS).
     *
     * Signalling DHRS for IDENTIFY is invisible to Linux, which polls PxCI, but
     * the guest FIRMWARE times out waiting for PSS and drops the device: OVMF
     * issued one IDENTIFY, never read a sector, and reported "No bootable option
     * or device was found" -- with a perfectly good installed disk attached.
     */
    uint32_t pis_bit = HYPE_AHCI_PIS_DHRS;
    int is_write_direction = 0;

    /*
     * #672: the ATAPI sibling (process_ahci_command_slot) has always refused a rejected
     * translation here; this disk path never did, so a guest could point PxCLB/PxCLBU
     * (fully guest-controlled) outside its own mapped range and this function would
     * dereference the resulting NULL in hype_ahci_decode_cmd_header() below -- a
     * guest-triggerable host crash, not a guest-side fault, since hype has no process
     * boundary to contain it. Same refusal shape and message as the ATAPI path.
     */
    if (cmd_hdr_bytes == 0) {
        hype_debug_print("ahci: slot %u refused -- command list at gpa 0x%llx out of bounds\n",
                         slot, (unsigned long long)cmd_list_phys);
        return -1;
    }

    /* #372: the disk path masters the bus for exactly the same structures as the ATAPI one, so it
     * gets the same gate. 0, not -1: the caller panics on -1, and a guest that has not enabled bus
     * mastering is doing something the hardware ignores, not something undecodable. */
    if (ahci_bus_master_refused(ahci, "PxCI write (disk)")) {
        return 0;
    }

    hype_ahci_decode_cmd_header(cmd_hdr_bytes, &hdr);
    if (hdr.is_atapi) {
        return -1; /* not this handler's command -- the ATAPI path owns it */
    }

    cmd_table_bytes = (const uint8_t *)(uintptr_t)hype_guest_dma_xlate(
        dma_map, hdr.cmd_table_phys, (uint64_t)0x80u + (uint64_t)hdr.prdtl * 16u);
    if (cmd_table_bytes == 0) {
        /* VALID-3: a rejected translation was being dereferenced immediately below -- the
         * ATAPI path has always checked this, the disk path never did. */
        hype_debug_print("ahci-disk: slot %u refused -- command table at gpa 0x%llx (prdtl=%u) out "
                         "of bounds\n",
                         slot, (unsigned long long)hdr.cmd_table_phys, (unsigned int)hdr.prdtl);
        return -1;
    }
    if (cmd_table_bytes[0] != 0x27u) {
        return -1; /* not a Register H2D FIS at all */
    }
    /* #309: the C bit distinguishes a command from a Control-register write. The disk HBA gets
     * reset the same way the optical one does -- FreeBSD attaches a channel on both -- so
     * handling this only on the path that happened to panic would just move the failure. */
    if (hype_ahci_h2d_is_control_write(cmd_table_bytes)) {
        return complete_ahci_soft_reset(ahci, rx_fis_phys, dma_map, slot, cmd_table_bytes[15]);
    }
    hype_ahci_decode_h2d_fis(cmd_table_bytes, &fis);

    /*
     * #262: trace the first commands this disk is ever asked for.
     *
     * The whole difficulty on this ticket has been not knowing WHICH half is
     * failing: "the guest firmware never issued a command to the disk" and "it
     * issued commands and rejected what came back" both present identically as
     * `BdsDxe: No bootable option or device was found.`, and they have nothing in
     * common as fixes. Three hypotheses were already spent guessing at the second
     * without evidence for it. Bounded to the first few so a live guest's steady
     * read traffic cannot flood the log.
     */
    {
        static unsigned trace_n = 0;
        if (trace_n < 12u) {
            trace_n++;
            hype_debug_print("fw-1 #262 ATACMD#%02u: cmd=0x%02x lba=0x%llx count=%u prdtl=%u\n",
                             trace_n, (unsigned)fis.command, (unsigned long long)fis.lba,
                             (unsigned)fis.count, (unsigned)hdr.prdtl);
        }
    }

    status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_DSC);
    error_reg = 0;
    remaining = 0;

    if (fis.command == HYPE_ATA_CMD_IDENTIFY_DEVICE) {
        hype_ata_disk_build_identify(disk, identify);
        src = identify;
        remaining = HYPE_ATA_IDENTIFY_SIZE;
        /*
         * #358: BOTH bits. A PIO data-in command raises PxIS.PSS when the PIO Setup FIS arrives
         * and PxIS.DHRS when the closing D2H Register FIS does; real hardware sets both, and the
         * D2H FIS is already written at receive-area offset 0x40 a few lines below regardless.
         *
         * This used to ASSIGN PSS, dropping DHRS. EDK2 waits at 0x20 for the PIO Setup FIS -- which
         * is why assigning PSS fixed the earlier symptom (#262) -- and then waits for the command to
         * COMPLETE, which is DHRS. So it got half of what it needed: it started the port, issued one
         * IDENTIFY, saw PSS, never saw DHRS, timed out, and stopped the port again. The evidence is
         * the port register pair, disk versus the CD on an identically-presented function:
         *   HBA[cd-works]:   p_is=0x00000003 (DHRS|PSS)  p_cmd=0x03000000
         *   HBA[sata-fails]: p_is=0x00000002 (PSS only)  p_cmd=0x00000000  <- port stopped again
         * Linux and OpenBSD never noticed because they poll PxCI.
         */
        pis_bit = HYPE_AHCI_PIS_DHRS | HYPE_AHCI_PIS_PSS;
    } else if (fis.command == HYPE_ATA_CMD_READ_DMA_EXT || fis.command == HYPE_ATA_CMD_READ_DMA ||
               fis.command == HYPE_ATA_CMD_WRITE_DMA_EXT || fis.command == HYPE_ATA_CMD_WRITE_DMA) {
        int lba48 = hype_ata_cmd_is_lba48(fis.command);
        uint32_t sector_count = lba48 ? hype_ata_disk_resolve_sector_count(fis.count)
                                      : hype_ata_resolve_sector_count28(fis.count);
        lba_base = lba48 ? fis.lba : hype_ata_lba28_from_fis(fis.lba, fis.device);
        is_write_direction = (fis.command == HYPE_ATA_CMD_WRITE_DMA_EXT ||
                              fis.command == HYPE_ATA_CMD_WRITE_DMA)
                                 ? 1
                                 : 0;
        if (is_write_direction) {
            /* #94: the first writes the guest ever issues, and their fate --
             * "format wrote 0 bytes" names the symptom but not which layer
             * refused. Bounded like the ATACMD trace above. */
            static unsigned wtrace_n = 0;
            if (wtrace_n < 8u) {
                wtrace_n++;
                hype_debug_print("fw-1 #94 ATAWRITE#%u: cmd=0x%02x lba=0x%llx count=%u prdtl=%u "
                                 "in_bounds=%d be_total=%llu\n",
                                 wtrace_n, (unsigned)fis.command, (unsigned long long)lba_base,
                                 (unsigned)sector_count, (unsigned)hdr.prdtl,
                                 disk->be != 0
                                     ? (lba_base + sector_count <= disk->be->total_sectors)
                                     : hype_ata_disk_range_in_bounds(disk, lba_base, sector_count),
                                 (unsigned long long)(disk->be != 0 ? disk->be->total_sectors : 0));
            }
        }
        if (disk->be != 0 ? (lba_base + sector_count <= disk->be->total_sectors)
                          : hype_ata_disk_range_in_bounds(disk, lba_base, sector_count)) {
            uint8_t *media_at = (disk->be != 0) ? 0 : disk->media + lba_base * HYPE_ATA_SECTOR_SIZE;
            if (is_write_direction) {
                dst_media = media_at;
            } else {
                src = media_at;
            }
            remaining = sector_count * HYPE_ATA_SECTOR_SIZE;
        } else {
            status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
            error_reg = 0x10u; /* IDNF -- ID Not Found, the real ATA convention for an out-of-range LBA */
        }
    } else if (fis.command == HYPE_ATA_CMD_FLUSH_CACHE_EXT ||
               fis.command == HYPE_ATA_CMD_FLUSH_CACHE ||
               fis.command == HYPE_ATA_CMD_STANDBY_IMMEDIATE ||
               fis.command == HYPE_ATA_CMD_SET_FEATURES) {
        /*
         * Nothing to stream -- an immediate, no-data completion. SET FEATURES is
         * not optional: libata issues it to select the UDMA mode that IDENTIFY
         * advertises, and an unrecognized command here returns -1, which never
         * completes the slot and shows up in the guest as a qc timeout rather
         * than as an unsupported command.
         */
    } else {
        /*
         * An unmodelled command byte must still COMPLETE, with ABRT, the way real
         * hardware retires a command it does not support. Returning -1 here leaves
         * the slot's PxCI bit set and the MMIO write unhandled, so the guest retries
         * the same instruction forever and the whole vCPU wedges -- not just its
         * disk I/O. (Returning -1 stays correct for the ATAPI-header case above:
         * that genuinely belongs to another handler, which will clear the slot.)
         */
        {   /* #94: name the opcode being retired with ABRT -- an OS that needed
             * it sees only a failed I/O. Bounded. */
            static unsigned abrt_n = 0;
            if (abrt_n < 8u) {
                abrt_n++;
                hype_debug_print("fw-1 #94 ATA-ABRT#%u: unmodelled cmd=0x%02x count=%u prdtl=%u\n",
                                 abrt_n, (unsigned)fis.command, (unsigned)fis.count,
                                 (unsigned)hdr.prdtl);
            }
        }
        status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
        error_reg = 0x04u; /* ABRT */
    }

    prdt_bytes = cmd_table_bytes + 0x80;
    prd_idx = 0;
    /* #94: backend R/W goes through the PRD-cursor engine above, which
     * tolerates sector-splitting PRD boundaries. The per-PRD loop below still
     * serves the synthesised transfers (IDENTIFY) and the RAM-media path. */
    if (disk->be != 0 && remaining > 0 && error_reg == 0 &&
        (is_write_direction || fis.command == HYPE_ATA_CMD_READ_DMA ||
         fis.command == HYPE_ATA_CMD_READ_DMA_EXT)) {
        uint32_t done = 0;
        uint32_t requested = remaining;
        int erc = ahci_backend_rw_prdt(disk, dma_map, prdt_bytes, hdr.prdtl, lba_base, requested,
                                       is_write_direction, &done);
        if (erc == -1) {
            return -1; /* refused DMA translation: same contract as the loop below */
        }
        if (erc == -2) {
            static unsigned befail_n = 0;
            if (befail_n < 8u) {
                befail_n++;
                hype_debug_print("fw-1 #94 ATA-BE-FAIL#%u: cmd=0x%02x lba=0x%llx done=%u\n",
                                 befail_n, (unsigned)fis.command, (unsigned long long)lba_base,
                                 (unsigned)done);
            }
            status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
            error_reg = 0x10u;
        }
        transferred = done;
        remaining = requested - done;
        prd_idx = hdr.prdtl; /* the loop below must not re-run this transfer */
    }
    while (remaining > 0 && prd_idx < hdr.prdtl) {
        hype_ahci_prdt_entry_t prd;
        uint32_t chunk;

        hype_ahci_decode_prdt_entry(prdt_bytes + (uint32_t)prd_idx * 16u, &prd);
        chunk = (prd.byte_count < remaining) ? prd.byte_count : remaining;

        if (disk->be != 0 && fis.command != HYPE_ATA_CMD_IDENTIFY_DEVICE) {
            /*
             * #262 slice 1: storage lives behind a blk_backend, so DMA straight
             * between guest RAM and the backend instead of a RAM `media` array.
             * IDENTIFY is excluded: it is a synthesised response, not disk content.
             */
            uint64_t lba_off;
            uint32_t nsec;
            if (hype_ata_prd_sector_range(transferred, chunk, &lba_off, &nsec) != 0) {
                status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
                error_reg = 0x04u; /* ABRT: a PRD that splits a sector is not a transfer
                                    * we model, and guessing would hide the mismatch */
                break;
            }
            if (is_write_direction) {
                if (hype_blk_backend_write(
                        disk->be, lba_base + lba_off, nsec,
                        (const void *)(uintptr_t)hype_guest_dma_xlate(dma_map, prd.data_phys, chunk)) !=
                    0) {
                    static unsigned wfail_n = 0;
                    if (wfail_n < 8u) {
                        wfail_n++;
                        hype_debug_print("fw-1 #94 ATAWRITE-FAIL#%u: backend write lba=%llu "
                                         "nsec=%u chunk=%u\n",
                                         wfail_n, (unsigned long long)(lba_base + lba_off),
                                         (unsigned)nsec, (unsigned)chunk);
                    }
                    status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
                    error_reg = 0x10u;
                    break;
                }
            } else {
                if (hype_blk_backend_read(
                        disk->be, lba_base + lba_off, nsec,
                        (void *)(uintptr_t)hype_guest_dma_xlate(dma_map, prd.data_phys, chunk)) != 0) {
                    status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
                    error_reg = 0x10u;
                    break;
                }
            }
        } else if (is_write_direction) {
            const uint8_t *guest_src =
                (const uint8_t *)(uintptr_t)hype_guest_dma_xlate(dma_map, prd.data_phys, chunk);
            /*
             * #675: a rejected translation (guest PRD pointing outside its own mapped range)
             * was being dereferenced unchecked -- found by the #602 fuzz harness. Every other
             * hype_guest_dma_xlate() call site in this function already refuses a 0 translation;
             * this pair of flat-media branches did not.
             *
             * An ATA error completion (DRDY|ERR), not `return -1`: this loop's own
             * backend-read/write-failure branches a few lines above treat an unreachable DMA
             * target the same way -- the command header and command table already decoded
             * fine, so this is a per-command transfer error a real controller reports on the
             * completion, not a reason to escalate to the caller's fatal "unhandled AHCI ABAR
             * MMIO" panic (that path is for #672's command-list check, where nothing about the
             * command is coherent yet).
             */
            if (guest_src == 0) {
                status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
                error_reg = 0x10u;
                break;
            }
            ahci_copy_fast(dst_media, guest_src, chunk);
            dst_media += chunk;
        } else {
            uint8_t *guest_dst =
                (uint8_t *)(uintptr_t)hype_guest_dma_xlate(dma_map, prd.data_phys, chunk);
            /* #675: same as the write-direction check just above, other direction. */
            if (guest_dst == 0) {
                status_reg = (uint8_t)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_ERR);
                error_reg = 0x10u;
                break;
            }
            ahci_copy_fast(guest_dst, src, chunk);
            src += chunk;
        }
        transferred += chunk;
        remaining -= chunk;
        prd_idx++;
    }

    if (remaining > 0) {
        /* #94: the PRDT ran out before the command's byte count was satisfied --
         * a silent short transfer. Windows' format writes died exactly here. */
        static unsigned short_n = 0;
        if (short_n < 12u) {
            short_n++;
            hype_debug_print("fw-1 #94 ATA-SHORT#%u: cmd=0x%02x lba=0x%llx wanted=%u did=%u "
                             "prdtl=%u write=%d\n",
                             short_n, (unsigned)fis.command, (unsigned long long)lba_base,
                             (unsigned)(remaining + (uint32_t)transferred), (unsigned)transferred,
                             (unsigned)hdr.prdtl, is_write_direction);
        }
    }
    return complete_ahci_command_slot(ahci, rx_fis_phys, status_reg, error_reg, dma_map, slot,
                                      pis_bit, (uint32_t)transferred, cmd_hdr_bytes);
}

/* ---- virtio-blk: virtq_validate_chain / process_virtio_blk_queue ---- */

/*
 * Rate-limited to the first few, and deliberately so: this sits on the I/O path,
 * and a guest that produces one bad chain usually produces thousands. An
 * unbounded print would bury the rest of the log -- the exact failure mode #238
 * was about -- and the first occurrence is the informative one anyway. The
 * counter is unsynchronised across VMs for the same reason the write stats are:
 * a lost increment on a diagnostic beats a lock on the I/O path.
 */
#define HYPE_VIRTIO_BLK_REJECT_LOG_MAX 8u

static uint32_t g_virtio_blk_rejects;
static void (*g_virtio_blk_reject_sink)(const char *why);

void hype_virtio_blk_set_reject_sink(void (*sink)(const char *why)) {
    g_virtio_blk_reject_sink = sink;
    g_virtio_blk_rejects = 0;
}

/* Variant that names the offending value. A reject reason without the number is
 * half a diagnostic: "unsupported request type" sent the reader back to the spec
 * to guess which, on the first real-hardware run this logging ever did. */
static void virtio_blk_reject_val(const char *why, uint32_t value) {
    g_virtio_blk_rejects++;
    if (g_virtio_blk_rejects > HYPE_VIRTIO_BLK_REJECT_LOG_MAX) {
        return;
    }
    if (g_virtio_blk_reject_sink != 0) {
        g_virtio_blk_reject_sink(why);
        return;
    }
    hype_debug_print("virtio-blk: request REJECTED (#%u): %s (0x%x)\n",
                     (unsigned)g_virtio_blk_rejects, why, (unsigned)value);
    if (g_virtio_blk_rejects == HYPE_VIRTIO_BLK_REJECT_LOG_MAX) {
        hype_debug_print("virtio-blk: further rejections will not be logged\n");
    }
}

static void virtio_blk_reject(const char *why) {
    g_virtio_blk_rejects++;
    if (g_virtio_blk_rejects > HYPE_VIRTIO_BLK_REJECT_LOG_MAX) {
        return;
    }
    if (g_virtio_blk_reject_sink != 0) {
        g_virtio_blk_reject_sink(why);
        return;
    }
    hype_debug_print("virtio-blk: request REJECTED (#%u): %s\n", (unsigned)g_virtio_blk_rejects,
                     why);
    if (g_virtio_blk_rejects == HYPE_VIRTIO_BLK_REJECT_LOG_MAX) {
        hype_debug_print("virtio-blk: further rejections will not be logged\n");
    }
}

/*
 * Fetch descriptor `index` from this device's descriptor table, bounds-checking
 * the index against queue_size and translating the 16-byte entry through the
 * VALID-3 gpa map. Returns -1 if either check fails.
 */
static int virtq_fetch_desc(const hype_virtio_blk_t *dev, const hype_gpa_map_t *dma_map,
                            uint16_t index, hype_virtq_desc_t *out) {
    const uint8_t *dp;

    if (index >= dev->queue_size) {
        return -1;
    }
    dp = (const uint8_t *)(uintptr_t)hype_guest_dma_xlate(
        dma_map, dev->queue_desc + (uint64_t)index * 16u, 16u);
    if (dp == 0) {
        return -1;
    }
    hype_virtq_decode_desc(dp, out);
    return 0;
}

/*
 * #268: validate a whole request chain and locate its status descriptor WITHOUT
 * performing any I/O, then let the caller re-walk it to transfer data.
 *
 * The two passes are the point. Validating as we transferred would leave a
 * half-applied write behind whenever a chain turned out to be malformed
 * partway through -- so a malformed chain is rejected having changed nothing,
 * which is the property the old fixed-3 walk had for free and which the
 * variable-length walk has to earn.
 *
 * This covers the chain's SHAPE, which is the part that is guest-controlled
 * bookkeeping rather than data. It is deliberately not a promise of atomicity
 * for the transfer itself: a segment whose length is unusable, or a backend that
 * errors on the third of four segments, still completes the request with IOERR
 * after earlier segments have already landed. That matches a real disk, where an
 * error partway through a transfer does not un-write what preceded it -- IOERR
 * means "distrust this whole request", not "nothing happened".
 *
 * Chain shape per the virtio spec: header -> zero or more data segments ->
 * status. Every descriptor except the last carries NEXT, so the status
 * descriptor is exactly the one without it, and the data segments are exactly
 * the descriptors between. ZERO data segments is legal, not an error: a FLUSH
 * request carries no data at all, so its chain is just header -> status.
 */
static int virtq_validate_chain(const hype_virtio_blk_t *dev, const hype_gpa_map_t *dma_map,
                                uint16_t head, hype_virtq_desc_t *out_header,
                                hype_virtq_desc_t *out_status) {
    hype_virtq_desc_t d;
    uint32_t steps = 0;
    uint16_t cur;

    if (virtq_fetch_desc(dev, dma_map, head, out_header) != 0) {
        return -1;
    }
    /* A chain with no NEXT on its header has nowhere to put the status byte. */
    if ((out_header->flags & HYPE_VIRTQ_DESC_F_NEXT) == 0) {
        return -1;
    }
    cur = out_header->next;
    for (;;) {
        if (virtq_fetch_desc(dev, dma_map, cur, &d) != 0) {
            return -1;
        }
        /*
         * A legal chain visits each descriptor at most once, so more than
         * queue_size hops proves the guest built a cycle (A -> B -> A). The
         * bound is mandatory rather than defensive: the descriptor indices are
         * guest-controlled, and an unbounded follow-the-NEXT loop would spin
         * this core inside hype forever, taking that VM's dispatch loop with
         * it. The old fixed-3 walk could not loop at all, so the bound has to
         * arrive together with the loop that needs it.
         */
        steps++;
        if (steps > (uint32_t)dev->queue_size) {
            return -1;
        }
        if ((d.flags & HYPE_VIRTQ_DESC_F_NEXT) == 0) {
            *out_status = d;
            return 0;
        }
        cur = d.next;
    }
}

/*
 * Walks every newly-submitted chain in the (single) virtqueue since this
 * device's own last_avail_idx bookkeeping, processing each as a virtio_blk_req:
 * a header descriptor, any number of data segments, and a status descriptor.
 * Returns -1 if a chain is malformed (bad index, untranslatable address, cyclic
 * NEXT list, or no status descriptor); 0 otherwise.
 *
 * #268: this used to require EXACTLY three descriptors and reject anything
 * else. Two consequences, both fixed here:
 *   - A header + N-data + status chain was refused outright, capping every
 *     request at one contiguous segment. Linux produces multi-segment chains
 *     whenever a request spans non-contiguous physical pages, which is the
 *     normal case above a page once memory is fragmented -- and since hype
 *     advertises no VIRTIO_BLK_F_SEG_MAX, a conforming driver has no way to
 *     learn of a limit and is entitled to send them.
 *   - A 2-descriptor FLUSH chain (header -> status, no data) hit the same
 *     rejection. That path did not merely refuse the request: returning -1
 *     aborts the notify WITHOUT advancing last_avail_idx or writing a status
 *     byte, so the request is never completed and the guest waits on it
 *     forever. The FLUSH branch below was therefore unreachable in practice.
 *
 * No segment-count limit is imposed, so there is nothing to advertise via
 * VIRTIO_BLK_F_SEG_MAX: the walk streams one segment at a time and needs no
 * array to hold them.
 */
int process_virtio_blk_queue(hype_virtio_blk_t *dev, const hype_blk_backend_t *be,
                             const hype_gpa_map_t *dma_map) {
    /* VALID-3: the virtqueue base addresses (desc/avail/used), every descriptor
     * index, and every buffer pointer are guest-supplied. Each guest-physical
     * address is translated through this VM's bounds-checked gpa map
     * (hype_guest_dma_xlate -> 0 when out of range) BEFORE it is dereferenced, so a
     * malicious/garbled queue can never steer a read or write at hype's own or
     * another VM's memory. For an identity-mapped caller (M5-1) dma_map is NULL
     * and the translation returns the address unchanged. Descriptor indices are
     * additionally bounded by queue_size. */
    uint16_t qsz = dev->queue_size;
    const uint8_t *avail_base;
    uint8_t *used_base;
    uint16_t avail_idx;
    uint32_t drained = 0; /* #265: chains this kick found already pending */

    if (qsz == 0u) {
        return -1;
    }
    /*
     * #372: a device that cannot master the bus cannot reach the virtqueue either.
     *
     * Same gate as the AHCI paths, same return convention: 0, because the guest asked for
     * something the hardware would silently not do. Nothing is consumed from the avail ring and
     * nothing is placed in the used ring, so the driver waits forever -- which is the point.
     */
    if (dev->bus_master == 0) {
        static int reported;
        if (!reported) {
            reported = 1;
            hype_debug_print("virtio-blk: queue notify IGNORED -- the guest has not set PCI Bus "
                             "Master Enable (Command bit 2), so the device cannot reach the "
                             "virtqueue. No request will ever complete, exactly as on real "
                             "hardware. [#372]\n");
        }
        return 0;
    }
    /* avail ring: flags(2) + idx(2) + ring(2*qsz) + used_event(2). */
    avail_base = (const uint8_t *)(uintptr_t)hype_guest_dma_xlate(dma_map, dev->queue_driver,
                                                             4u + 2u * (uint64_t)qsz + 2u);
    /* used ring: flags(2) + idx(2) + elems(8*qsz) + avail_event(2). */
    used_base = (uint8_t *)(uintptr_t)hype_guest_dma_xlate(dma_map, dev->queue_device,
                                                      4u + 8u * (uint64_t)qsz + 2u);
    if (avail_base == 0 || used_base == 0) {
        return -1;
    }
    avail_idx = (uint16_t)(avail_base[2] | (avail_base[3] << 8));

    while (dev->last_avail_idx != avail_idx) {
        uint16_t ring_index = (uint16_t)(dev->last_avail_idx % qsz);
        uint16_t head_desc =
            (uint16_t)(avail_base[4 + 2 * ring_index] | (avail_base[4 + 2 * ring_index + 1] << 8));
        hype_virtq_desc_t header_desc, status_desc;
        const uint8_t *hdr;
        uint32_t req_type;
        uint64_t sector;
        uint8_t status_value;
        uint32_t used_len;
        uint16_t used_idx;
        uint16_t used_ring_index;
        uint32_t elem_off;

        /* Validate the whole chain first (bounds, translatability, no cycle) so a
         * malformed request is rejected before any data moves. */
        if (virtq_validate_chain(dev, dma_map, head_desc, &header_desc, &status_desc) != 0) {
            virtio_blk_reject("malformed descriptor chain");
            return -1;
        }

        /* virtio_blk_req header: type(4) + reserved(4) + sector(8) = 16 bytes. */
        hdr = (const uint8_t *)(uintptr_t)hype_guest_dma_xlate(dma_map, header_desc.addr, 16u);
        if (hdr == 0) {
            return -1;
        }
        req_type = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) |
                   ((uint32_t)hdr[3] << 24);
        sector = (uint64_t)hdr[8] | ((uint64_t)hdr[9] << 8) | ((uint64_t)hdr[10] << 16) |
                 ((uint64_t)hdr[11] << 24) | ((uint64_t)hdr[12] << 32) | ((uint64_t)hdr[13] << 40) |
                 ((uint64_t)hdr[14] << 48) | ((uint64_t)hdr[15] << 56);

        /* GLADDER/M5-7a: dispatch through the bounds-gated hype_blk_backend vtable
         * (#89) instead of a raw host buffer, so the frontend is backend-agnostic
         * (file / physical / qcow2). virtio-blk data is always a whole-sector
         * multiple; the LBA+count bounds check lives inside hype_blk_backend_*.
         * The guest data buffer is itself translated+bounded before the copy. */
        if (req_type == HYPE_VIRTIO_BLK_T_OUT || req_type == HYPE_VIRTIO_BLK_T_IN) {
            /*
             * #268: transfer every data segment in the chain, not just the first.
             * The LBA advances by each segment's sector count, so a scattered
             * buffer lands as one contiguous run on the backend -- which is
             * exactly what the guest asked for and what a real device does.
             */
            uint16_t cur = header_desc.next;
            uint64_t seg_lba = sector;
            uint64_t xfer_bytes = 0;
            uint32_t nsegs = 0;
            const char *err = 0;
            /*
             * #295: a WRITE chain's segments are gathered and issued as ONE vectored backend call
             * per batch instead of one call per segment. Within a request the segments are
             * contiguous on disk BY CONSTRUCTION (one virtio_blk_req has one start sector and its
             * data runs from there), so the batch needs no adjacency decision -- only a size cap.
             * The cap matches the seg_max hype advertises: a driver that honours it always fits
             * one batch; one that never negotiated SEG_MAX may exceed it, and then each full batch
             * flushes as its own (still contiguous) vectored call.
             *
             * Reads stay per-segment: the measured cost was the write path's one-command-per-4KiB
             * round trip (#265/#295), and the read path's throughput has never been the complaint.
             */
            hype_blk_seg_t wsegs[HYPE_VIRTIO_BLK_SEG_MAX];
            uint32_t nw = 0;
            uint64_t wbatch_lba = sector;

            for (;;) {
                hype_virtq_desc_t seg;
                uint32_t nsec;
                void *gbuf;

                /* Cannot fail: virtq_validate_chain() already walked this exact
                 * list. Checked anyway rather than assuming, since a failure here
                 * would otherwise be a dereference of an untranslated address. */
                if (virtq_fetch_desc(dev, dma_map, cur, &seg) != 0) {
                    err = "descriptor vanished mid-chain";
                    break;
                }
                if ((seg.flags & HYPE_VIRTQ_DESC_F_NEXT) == 0) {
                    break; /* this is the status descriptor -- chain done */
                }
                nsec = seg.len / HYPE_VIRTIO_BLK_SECTOR_SIZE;
                /*
                 * Each segment must itself be a whole number of sectors. The spec
                 * only constrains the total, but the backend is addressed in
                 * sectors, so a segment that splits one would need a bounce
                 * buffer this freestanding build has nowhere to allocate. Such a
                 * request is COMPLETED with IOERR rather than left dangling: the
                 * guest gets an error it can report, instead of an I/O that never
                 * returns. Linux's block layer aligns every segment to the
                 * logical block size, so this is not a case it can produce.
                 */
                if ((seg.len % HYPE_VIRTIO_BLK_SECTOR_SIZE) != 0u || nsec == 0u) {
                    err = "data segment is not a whole number of sectors";
                    break;
                }
                gbuf = (void *)(uintptr_t)hype_guest_dma_xlate(dma_map, seg.addr, seg.len);
                if (gbuf == 0) {
                    err = "data segment failed bounds check";
                    break;
                }
                if (req_type == HYPE_VIRTIO_BLK_T_OUT) {
                    if (nw == HYPE_VIRTIO_BLK_SEG_MAX) {
                        if (hype_blk_backend_writev(be, wbatch_lba, wsegs, nw) != 0) {
                            err = "backend rejected the transfer";
                            break;
                        }
                        wbatch_lba = seg_lba;
                        nw = 0;
                    }
                    wsegs[nw].buf = gbuf;
                    wsegs[nw].count = nsec;
                    nw++;
                } else if (hype_blk_backend_read(be, seg_lba, nsec, gbuf) != 0) {
                    err = "backend rejected the transfer";
                    break;
                }
                seg_lba += nsec;
                xfer_bytes += seg.len;
                nsegs++;
                cur = seg.next;
            }
            if (err == 0 && nw != 0u &&
                hype_blk_backend_writev(be, wbatch_lba, wsegs, nw) != 0) {
                err = "backend rejected the transfer";
            }

            if (err == 0 && nsegs == 0u) {
                /* A read/write with no data buffer at all. Not a chain-shape
                 * error (the shape is legal, it is what FLUSH uses) -- it is a
                 * meaningless request, so complete it with IOERR. */
                err = "read/write request carries no data segment";
            }
            if (err != 0) {
                virtio_blk_reject(err);
                status_value = HYPE_VIRTIO_BLK_S_IOERR;
                used_len = 1;
            } else {
                status_value = HYPE_VIRTIO_BLK_S_OK;
                /* used_len counts what the DEVICE wrote into guest memory: the
                 * data for a read, and the status byte in both directions. */
                used_len = (req_type == HYPE_VIRTIO_BLK_T_IN) ? (uint32_t)(xfer_bytes + 1u) : 1u;
            }
        } else if (req_type == HYPE_VIRTIO_BLK_T_GET_ID) {
            /*
             * #310: hand back the device's serial string. FreeBSD's vtblk issues this during
             * attach and reports "error getting device identifier: 45" when it is refused.
             *
             * Deliberately NOT folded into the T_IN path above: that path requires every
             * segment to be a whole-sector multiple, and this one carries a single 20-byte
             * device-writable segment, so reusing it would IOERR the request instead.
             */
            hype_virtq_desc_t seg;

            if (virtq_fetch_desc(dev, dma_map, header_desc.next, &seg) != 0) {
                virtio_blk_reject("GET_ID: data descriptor vanished mid-chain");
                status_value = HYPE_VIRTIO_BLK_S_IOERR;
                used_len = 1;
            } else if ((seg.flags & HYPE_VIRTQ_DESC_F_NEXT) == 0) {
                /* No data descriptor at all -- the next link is already the status byte. */
                virtio_blk_reject("GET_ID: chain carries no data descriptor");
                status_value = HYPE_VIRTIO_BLK_S_IOERR;
                used_len = 1;
            } else {
                /*
                 * Write at most what the guest offered AND at most the field width, then
                 * translate for exactly that many bytes. A short buffer is the guest's
                 * business; overrunning it would be hype's.
                 */
                uint32_t n = (seg.len < HYPE_VIRTIO_BLK_ID_BYTES) ? seg.len
                                                                  : HYPE_VIRTIO_BLK_ID_BYTES;
                uint8_t *gbuf = (uint8_t *)(uintptr_t)hype_guest_dma_xlate(dma_map, seg.addr, n);
                if (gbuf == 0) {
                    virtio_blk_reject("GET_ID: data segment failed bounds check");
                    status_value = HYPE_VIRTIO_BLK_S_IOERR;
                    used_len = 1;
                } else {
                    uint32_t i;
                    for (i = 0; i < n; i++) {
                        gbuf[i] = dev->serial[i];
                    }
                    status_value = HYPE_VIRTIO_BLK_S_OK;
                    /* used_len counts what the device wrote, plus the status byte. */
                    used_len = n + 1u;
                }
            }
        } else if (req_type == HYPE_VIRTIO_BLK_T_FLUSH) {
            /* Synchronous backend: writes already durable, so FLUSH is a no-op ACK
             * (a real guest issues FLUSH; returning UNSUPP would stall its I/O).
             * #268: this branch is only now REACHABLE. A FLUSH chain carries no
             * data descriptor, so the old exactly-3-descriptor requirement failed
             * it before this switch was ever consulted -- and failed it by
             * aborting the notify, which never completed the request at all. */
            status_value = HYPE_VIRTIO_BLK_S_OK;
            used_len = 1;
        } else {
            virtio_blk_reject_val("unsupported request type", req_type);
            status_value = HYPE_VIRTIO_BLK_S_UNSUPP;
            used_len = 1;
        }

        {
            uint8_t *st = (uint8_t *)(uintptr_t)hype_guest_dma_xlate(dma_map, status_desc.addr, 1u);
            if (st != 0) {
                *st = status_value;
            }
        }

        used_idx = (uint16_t)(used_base[2] | (used_base[3] << 8));
        used_ring_index = (uint16_t)(used_idx % dev->queue_size);
        elem_off = 4u + 8u * used_ring_index;
        used_base[elem_off + 0] = (uint8_t)(head_desc & 0xFFu);
        used_base[elem_off + 1] = (uint8_t)((head_desc >> 8) & 0xFFu);
        used_base[elem_off + 2] = 0;
        used_base[elem_off + 3] = 0;
        used_base[elem_off + 4] = (uint8_t)(used_len & 0xFFu);
        used_base[elem_off + 5] = (uint8_t)((used_len >> 8) & 0xFFu);
        used_base[elem_off + 6] = (uint8_t)((used_len >> 16) & 0xFFu);
        used_base[elem_off + 7] = (uint8_t)((used_len >> 24) & 0xFFu);
        used_idx = (uint16_t)(used_idx + 1u);
        used_base[2] = (uint8_t)(used_idx & 0xFFu);
        used_base[3] = (uint8_t)((used_idx >> 8) & 0xFFu);

        dev->isr_status |= 0x01u;
        dev->last_avail_idx = (uint16_t)(dev->last_avail_idx + 1u);
        drained++;
    }

    /* #265: record the queue depth this kick saw. Counted here rather than from
     * avail_idx arithmetic so a kick that found nothing new contributes nothing
     * -- an empty notify says nothing about how deeply the guest queues. */
    hype_virtio_blk_depth_record(hype_virtio_blk_depth(), drained);
    return 0;
}
