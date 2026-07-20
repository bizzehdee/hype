#include "fw_cfg.h"

static void write_be32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static void write_be16(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)(value >> 8);
    dst[1] = (uint8_t)value;
}

static uint32_t byteswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}

static void rebuild_dir(hype_fw_cfg_t *fw) {
    uint32_t i, off, j;

    write_be32(fw->dir_blob, fw->file_count);
    off = 4;
    for (i = 0; i < fw->file_count; i++) {
        const char *name = fw->files[i].name;
        int k = 0;

        write_be32(fw->dir_blob + off, fw->files[i].size);
        write_be16(fw->dir_blob + off + 4, (uint16_t)(HYPE_FW_CFG_KEY_FILE_FIRST + i));
        write_be16(fw->dir_blob + off + 6, 0); /* reserved */
        for (j = 0; j < HYPE_FW_CFG_MAX_FILE_PATH; j++) {
            fw->dir_blob[off + 8 + j] = 0;
        }
        while (name[k] != '\0' && k < HYPE_FW_CFG_MAX_FILE_PATH - 1) {
            fw->dir_blob[off + 8 + (uint32_t)k] = (uint8_t)name[k];
            k++;
        }
        off += 8 + HYPE_FW_CFG_MAX_FILE_PATH;
    }
    fw->dir_blob_len = off;
}

void hype_fw_cfg_reset(hype_fw_cfg_t *fw) {
    uint32_t i;

    fw->file_count = 0;
    fw->selected_key = 0;
    fw->offset = 0;
    fw->dma_addr_high = 0;
    for (i = 0; i < sizeof(fw->dir_blob); i++) {
        fw->dir_blob[i] = 0;
    }
    rebuild_dir(fw);
}

int hype_fw_cfg_add_file(hype_fw_cfg_t *fw, const char *name, const uint8_t *data, uint32_t size) {
    int len = 0;

    if (fw->file_count >= HYPE_FW_CFG_MAX_FILES) {
        return -1;
    }
    while (name[len] != '\0') {
        if (len >= HYPE_FW_CFG_MAX_FILE_PATH - 1) {
            return -1;
        }
        len++;
    }

    fw->files[fw->file_count].name = name;
    fw->files[fw->file_count].data = data;
    fw->files[fw->file_count].size = size;
    fw->files[fw->file_count].write_data = 0;
    fw->file_count++;
    rebuild_dir(fw);

    return (int)(HYPE_FW_CFG_KEY_FILE_FIRST + fw->file_count - 1);
}

int hype_fw_cfg_add_writable_file(hype_fw_cfg_t *fw, const char *name, uint8_t *buf, uint32_t size) {
    int key = hype_fw_cfg_add_file(fw, name, buf, size);
    if (key < 0) {
        return key;
    }
    fw->files[fw->file_count - 1].write_data = buf;
    return key;
}

static int lookup_item(const hype_fw_cfg_t *fw, uint16_t key, const uint8_t **out_data, uint32_t *out_size) {
    static const uint8_t signature[4] = {'Q', 'E', 'M', 'U'};
    /* FW_CFG_VERSION (bit0) | FW_CFG_VERSION_DMA (bit1) -- native byte
     * order, matching real QEMU's own fw_cfg_add_i32() (cpu_to_le32),
     * confirmed against OVMF's own driver, which reads this value with
     * no byte-swap before testing feature bits (unlike the
     * FW_CFG_FILE_DIR entries, which the same driver DOES swap --
     * see this file's own top comment). */
    static const uint8_t id[4] = {0x03, 0x00, 0x00, 0x00};
    uint32_t i;

    if (key == HYPE_FW_CFG_KEY_SIGNATURE) {
        *out_data = signature;
        *out_size = 4;
        return 0;
    }
    if (key == HYPE_FW_CFG_KEY_ID) {
        *out_data = id;
        *out_size = 4;
        return 0;
    }
    if (key == HYPE_FW_CFG_KEY_FILE_DIR) {
        *out_data = fw->dir_blob;
        *out_size = fw->dir_blob_len;
        return 0;
    }
    for (i = 0; i < fw->file_count; i++) {
        if ((uint16_t)(HYPE_FW_CFG_KEY_FILE_FIRST + i) == key) {
            *out_data = fw->files[i].data;
            *out_size = fw->files[i].size;
            return 0;
        }
    }
    return -1;
}

/* Same lookup as lookup_item(), but only succeeds for a file registered
 * via hype_fw_cfg_add_writable_file() -- every other file (including
 * the synthetic SIGNATURE/ID/FILE_DIR items, which aren't even entries
 * in `files[]`) is not writable. */
static int lookup_writable_item(const hype_fw_cfg_t *fw, uint16_t key, uint8_t **out_data, uint32_t *out_size) {
    uint32_t i;

    for (i = 0; i < fw->file_count; i++) {
        if ((uint16_t)(HYPE_FW_CFG_KEY_FILE_FIRST + i) == key && fw->files[i].write_data != 0) {
            *out_data = fw->files[i].write_data;
            *out_size = fw->files[i].size;
            return 0;
        }
    }
    return -1;
}

void hype_fw_cfg_select(hype_fw_cfg_t *fw, uint16_t key) {
    fw->selected_key = key;
    fw->offset = 0;
}

uint8_t hype_fw_cfg_read_byte(hype_fw_cfg_t *fw) {
    const uint8_t *data;
    uint32_t size;
    uint8_t value;

    if (lookup_item(fw, fw->selected_key, &data, &size) != 0) {
        fw->offset++;
        return 0;
    }
    value = (fw->offset < size) ? data[fw->offset] : 0;
    fw->offset++;
    return value;
}

void hype_fw_cfg_dma_addr_high(hype_fw_cfg_t *fw, uint32_t wire_value) {
    fw->dma_addr_high = byteswap32(wire_value);
}

uint64_t hype_fw_cfg_dma_addr_low(hype_fw_cfg_t *fw, uint32_t wire_value) {
    uint32_t low = byteswap32(wire_value);
    return ((uint64_t)fw->dma_addr_high << 32) | (uint64_t)low;
}

void hype_fw_cfg_dma_decode(const uint8_t raw[16], hype_fw_cfg_dma_op_t *out) {
    uint32_t control_be =
        ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | (uint32_t)raw[3];
    uint32_t length_be =
        ((uint32_t)raw[4] << 24) | ((uint32_t)raw[5] << 16) | ((uint32_t)raw[6] << 8) | (uint32_t)raw[7];
    uint64_t address = 0;
    int i;

    for (i = 0; i < 8; i++) {
        address = (address << 8) | (uint64_t)raw[8 + i];
    }

    out->control = control_be;
    out->select_key = (uint16_t)(control_be >> 16);
    out->length = length_be;
    out->address = address;
}

uint32_t hype_fw_cfg_dma_execute(hype_fw_cfg_t *fw, const hype_fw_cfg_dma_op_t *op, uint8_t *guest_data_ptr) {
    const uint8_t *data;
    uint32_t size;
    uint32_t i;

    if (op->control & HYPE_FW_CFG_DMA_CTL_SELECT) {
        hype_fw_cfg_select(fw, op->select_key);
    }

    if (op->control & HYPE_FW_CFG_DMA_CTL_SKIP) {
        fw->offset += op->length;
        return 0;
    }

    if (op->control & HYPE_FW_CFG_DMA_CTL_WRITE) {
        uint8_t *dst;

        if (lookup_writable_item(fw, fw->selected_key, &dst, &size) != 0) {
            return HYPE_FW_CFG_DMA_CTL_ERROR;
        }
        for (i = 0; i < op->length; i++) {
            uint32_t dst_off = fw->offset + i;
            if (dst_off < size) {
                dst[dst_off] = guest_data_ptr[i];
            }
        }
        fw->offset += op->length;
        return 0;
    }

    if (op->control & HYPE_FW_CFG_DMA_CTL_READ) {
        /* An unregistered/absent item is NOT a DMA error: real QEMU fw_cfg
         * returns zeroes for a read of any absent selector (and this device's
         * own classic-port hype_fw_cfg_read_byte() already does exactly that).
         * Returning HYPE_FW_CFG_DMA_CTL_ERROR here instead made OVMF's DXE spin
         * -- it reads standard items hype doesn't register (e.g. 0x0e
         * FW_CFG_BOOT_MENU) via DMA and a spurious error stalls its boot-config
         * pass. So on a missing item, zero-fill and report success, matching
         * both QEMU and the classic-port path. */
        if (lookup_item(fw, fw->selected_key, &data, &size) != 0) {
            data = 0;
            size = 0;
        }
        for (i = 0; i < op->length; i++) {
            uint32_t src_off = fw->offset + i;
            guest_data_ptr[i] = (data != 0 && src_off < size) ? data[src_off] : 0;
        }
        fw->offset += op->length;
        return 0;
    }

    /* SELECT-only (or an empty control), neither an error nor further
     * data movement. */
    return 0;
}
