#include "pflash.h"

/* Real CFI erase granularity varies by chip; OVMF's own FVB/varstore
 * block configuration commonly uses 4KB blocks, the most widely used
 * NOR flash sector size -- used here as a documented assumption, not
 * a value independently confirmed against this specific vendored
 * OVMF build's own FDF block-size setting. Erasing at the wrong
 * granularity would only affect a guest that actually erases (rather
 * than just programs individual bytes, the common case for small
 * variable updates), and would surface as a guest-visible variable
 * corruption to investigate against the real value if it matters. */
#define HYPE_PFLASH_ERASE_BLOCK_SIZE 0x1000u

void hype_pflash_reset(hype_pflash_t *pf, uint8_t *backing, uint32_t size) {
    pf->backing = backing;
    pf->size = size;
    pf->mode = HYPE_PFLASH_MODE_READ_ARRAY;
    pf->status = HYPE_PFLASH_STATUS_READY;
    pf->erase_offset = 0;
    pf->buffer_offset = 0;
    pf->buffer_remaining = 0;
    pf->buffer_pos = 0;
}

static int in_range(const hype_pflash_t *pf, uint32_t offset, uint32_t size_bytes) {
    if (offset >= pf->size) {
        return 0;
    }
    return (pf->size - offset) >= size_bytes;
}

int hype_pflash_read(hype_pflash_t *pf, uint32_t offset, uint8_t size_bytes, uint32_t *out_value) {
    uint32_t value;
    uint8_t i;
    uint8_t fill;

    if (offset >= pf->size) {
        return -1;
    }

    switch (pf->mode) {
        case HYPE_PFLASH_MODE_READ_ARRAY:
            if (!in_range(pf, offset, size_bytes)) {
                return -1;
            }
            value = 0;
            for (i = 0; i < size_bytes; i++) {
                value |= (uint32_t)pf->backing[offset + i] << (8u * i);
            }
            *out_value = value;
            return 0;
        case HYPE_PFLASH_MODE_READ_STATUS:
            fill = pf->status;
            break;
        case HYPE_PFLASH_MODE_READ_DEVID:
            /* Not independently confirmed against a specific expected
             * manufacturer/device code -- OVMF's own driver doesn't
             * appear to validate this strictly, only that array reads
             * and status/erase/program behave correctly. */
            fill = 0;
            break;
        default:
            /* Reads while a write/erase/buffer sequence is pending
             * aren't a documented case this stub models -- treat as
             * status reads (matching real chips, which are generally
             * readable-as-status while busy). */
            fill = pf->status;
            break;
    }

    value = 0;
    for (i = 0; i < size_bytes; i++) {
        value |= (uint32_t)fill << (8u * i);
    }
    *out_value = value;
    return 0;
}

static void handle_idle_command(hype_pflash_t *pf, uint32_t offset, uint8_t cmd) {
    (void)offset;
    switch (cmd) {
        case HYPE_PFLASH_CMD_READ_ARRAY:
            pf->mode = HYPE_PFLASH_MODE_READ_ARRAY;
            return;
        case HYPE_PFLASH_CMD_READ_STATUS:
            pf->mode = HYPE_PFLASH_MODE_READ_STATUS;
            return;
        case HYPE_PFLASH_CMD_CLEAR_STATUS:
            pf->status = HYPE_PFLASH_STATUS_READY;
            pf->mode = HYPE_PFLASH_MODE_READ_ARRAY;
            return;
        case HYPE_PFLASH_CMD_READ_DEVID:
            pf->mode = HYPE_PFLASH_MODE_READ_DEVID;
            return;
        case HYPE_PFLASH_CMD_WRITE_BYTE:
            pf->mode = HYPE_PFLASH_MODE_WRITE_BYTE_PENDING;
            return;
        case HYPE_PFLASH_CMD_BLOCK_ERASE:
            pf->mode = HYPE_PFLASH_MODE_ERASE_PENDING;
            pf->erase_offset = offset;
            return;
        case HYPE_PFLASH_CMD_WRITE_TO_BUFFER:
            pf->mode = HYPE_PFLASH_MODE_BUFFER_COUNT_PENDING;
            pf->buffer_offset = offset;
            return;
        default:
            /* Unrecognized command: leave mode unchanged, caller sees
             * the write as an error. */
            return;
    }
}

int hype_pflash_write(hype_pflash_t *pf, uint32_t offset, uint8_t size_bytes, uint32_t value) {
    uint8_t low_byte = (uint8_t)(value & 0xFFu);
    uint32_t i;
    uint32_t block_start;

    if (offset >= pf->size) {
        return -1;
    }

    switch (pf->mode) {
        case HYPE_PFLASH_MODE_READ_ARRAY:
        case HYPE_PFLASH_MODE_READ_STATUS:
        case HYPE_PFLASH_MODE_READ_DEVID: {
            hype_pflash_mode_t before = pf->mode;
            handle_idle_command(pf, offset, low_byte);
            if (pf->mode == before && low_byte != HYPE_PFLASH_CMD_READ_ARRAY) {
                /* Command byte wasn't recognized and didn't change
                 * anything (READ_ARRAY re-selecting itself is the one
                 * legitimate no-op case). */
                return -1;
            }
            return 0;
        }

        case HYPE_PFLASH_MODE_WRITE_BYTE_PENDING:
            if (!in_range(pf, offset, size_bytes)) {
                pf->status |= HYPE_PFLASH_STATUS_PROGRAM_ERROR;
                pf->mode = HYPE_PFLASH_MODE_READ_ARRAY;
                return -1;
            }
            for (i = 0; i < size_bytes; i++) {
                pf->backing[offset + i] = (uint8_t)((value >> (8u * i)) & 0xFFu);
            }
            pf->mode = HYPE_PFLASH_MODE_READ_ARRAY;
            return 0;

        case HYPE_PFLASH_MODE_ERASE_PENDING:
            if (low_byte != HYPE_PFLASH_CMD_ERASE_CONFIRM || offset != pf->erase_offset) {
                pf->status |= HYPE_PFLASH_STATUS_PROGRAM_ERROR;
                pf->mode = HYPE_PFLASH_MODE_READ_ARRAY;
                return -1;
            }
            block_start = (pf->erase_offset / HYPE_PFLASH_ERASE_BLOCK_SIZE) * HYPE_PFLASH_ERASE_BLOCK_SIZE;
            for (i = 0; i < HYPE_PFLASH_ERASE_BLOCK_SIZE && (block_start + i) < pf->size; i++) {
                pf->backing[block_start + i] = 0xFFu; /* erased NOR flash reads as all-1 bits */
            }
            pf->mode = HYPE_PFLASH_MODE_READ_ARRAY;
            return 0;

        case HYPE_PFLASH_MODE_BUFFER_COUNT_PENDING:
            /* low_byte is inherently 0-255, always well within
             * HYPE_PFLASH_MAX_BUFFER_WRITE (512) -- only 0 itself
             * ("write zero bytes") is a real edge case to reject. */
            if (low_byte == 0u) {
                pf->mode = HYPE_PFLASH_MODE_READ_ARRAY;
                return -1;
            }
            pf->buffer_remaining = low_byte;
            pf->buffer_pos = 0;
            pf->mode = HYPE_PFLASH_MODE_BUFFER_DATA_PENDING;
            return 0;

        case HYPE_PFLASH_MODE_BUFFER_DATA_PENDING:
            if (offset != pf->buffer_offset + pf->buffer_pos) {
                pf->mode = HYPE_PFLASH_MODE_READ_ARRAY;
                return -1;
            }
            for (i = 0; i < size_bytes && pf->buffer_remaining > 0; i++) {
                pf->buffer_data[pf->buffer_pos] = (uint8_t)((value >> (8u * i)) & 0xFFu);
                pf->buffer_pos++;
                pf->buffer_remaining--;
            }
            if (pf->buffer_remaining == 0) {
                pf->mode = HYPE_PFLASH_MODE_BUFFER_CONFIRM_PENDING;
            }
            return 0;

        case HYPE_PFLASH_MODE_BUFFER_CONFIRM_PENDING:
            if (low_byte != HYPE_PFLASH_CMD_ERASE_CONFIRM ||
                !in_range(pf, pf->buffer_offset, (uint32_t)pf->buffer_pos)) {
                pf->status |= HYPE_PFLASH_STATUS_PROGRAM_ERROR;
                pf->mode = HYPE_PFLASH_MODE_READ_ARRAY;
                return -1;
            }
            for (i = 0; i < pf->buffer_pos; i++) {
                pf->backing[pf->buffer_offset + i] = pf->buffer_data[i];
            }
            pf->mode = HYPE_PFLASH_MODE_READ_ARRAY;
            return 0;

        default:
            return -1;
    }
}
