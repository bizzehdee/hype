#ifndef HYPE_CORE_USB_MSC_H
#define HYPE_CORE_USB_MSC_H

#include <stdint.h>

/*
 * USB-3 (#215): pure USB Mass Storage Bulk-Only Transport (BOT) + the handful
 * of SCSI commands hype needs -- Command/Status Wrapper build/validate and CDB
 * encoders. Freestanding + unit-testable; the actual bulk transfers live in the
 * xhci_hw shim. (BOT: CBW out -> optional data -> CSW in, xHCI-independent.)
 */

#define HYPE_USB_CBW_SIGNATURE 0x43425355u /* 'USBC' */
#define HYPE_USB_CSW_SIGNATURE 0x53425355u /* 'USBS' */
#define HYPE_USB_CBW_LEN 31u
#define HYPE_USB_CSW_LEN 13u

/*
 * Builds a 31-byte Command Block Wrapper: tag, expected data length, direction
 * (dir_in=1 => Data-In from device), LUN, and the SCSI CDB (<= 16 bytes). The
 * unused CDB tail is zeroed.
 */
void hype_usb_bot_cbw(uint8_t cbw[31], uint32_t tag, uint32_t data_len, int dir_in,
                      unsigned int lun, const uint8_t *cdb, unsigned int cdb_len);

/* 1 if the 13-byte CSW has the right signature, matches expect_tag, and reports
 * command-passed (bCSWStatus == 0). */
int hype_usb_bot_csw_ok(const uint8_t csw[13], uint32_t expect_tag);

/* SCSI CDB encoders (big-endian on-wire fields). */
void hype_scsi_cdb_read_capacity10(uint8_t cdb[10]);
void hype_scsi_cdb_read10(uint8_t cdb[10], uint32_t lba, uint16_t blocks);
void hype_scsi_cdb_write10(uint8_t cdb[10], uint32_t lba, uint16_t blocks);
void hype_scsi_cdb_inquiry(uint8_t cdb[6], uint8_t alloc_len);

/* Parses an 8-byte READ CAPACITY(10) response (big-endian): the LAST LBA and
 * the logical block size in bytes. */
void hype_scsi_parse_read_capacity10(const uint8_t rc[8], uint32_t *last_lba,
                                     uint32_t *block_size);

#endif /* HYPE_CORE_USB_MSC_H */
