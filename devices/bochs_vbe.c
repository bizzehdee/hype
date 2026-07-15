#include "bochs_vbe.h"

void hype_bochs_vbe_reset(hype_bochs_vbe_t *dev) {
    uint32_t i;

    for (i = 0; i < HYPE_BOCHS_VBE_NUM_REGS; i++) {
        dev->regs[i] = 0;
    }
}

static int decode_dispi_index(uint32_t offset, uint32_t *out_index) {
    if ((offset & 0x1u) != 0) {
        return -1;
    }
    if (offset >= HYPE_BOCHS_VBE_DISPI_SIZE) {
        return -1;
    }
    *out_index = offset >> 1;
    return 0;
}

static uint32_t bytes_per_pixel_for_bpp(uint32_t bpp) {
    if (bpp == 16u) {
        return 2u;
    }
    if (bpp == 32u) {
        return 4u;
    }
    return 0u;
}

/* Real Bochs VBE auto-raises virtual width/height to at least the
 * requested resolution rather than leaving a too-small (or zero, if
 * the guest never touched the virtual-size registers) value in
 * place -- see this header's own top-level doc comment. */
static uint32_t effective_virtual_dimension(uint32_t virt, uint32_t requested) {
    if (virt < requested) {
        return requested;
    }
    return virt;
}

int hype_bochs_vbe_mmio_read(const hype_bochs_vbe_t *dev, uint32_t offset, uint16_t *out_value) {
    uint32_t index;

    if (decode_dispi_index(offset, &index) != 0) {
        return -1;
    }

    if (index == HYPE_BOCHS_VBE_INDEX_ID) {
        *out_value = (uint16_t)HYPE_BOCHS_VBE_ID5;
        return 0;
    }
    if (index == HYPE_BOCHS_VBE_INDEX_VIDEO_MEMORY_64K) {
        hype_bochs_vbe_mode_t mode;
        uint32_t virt_height;
        uint32_t total_bytes;

        hype_bochs_vbe_get_mode(dev, &mode);
        virt_height = effective_virtual_dimension(dev->regs[HYPE_BOCHS_VBE_INDEX_VIRT_HEIGHT],
                                                   dev->regs[HYPE_BOCHS_VBE_INDEX_YRES]);
        total_bytes = mode.stride_bytes * virt_height;
        *out_value = (uint16_t)((total_bytes + 0xFFFFu) >> 16);
        return 0;
    }

    *out_value = dev->regs[index];
    return 0;
}

int hype_bochs_vbe_mmio_write(hype_bochs_vbe_t *dev, uint32_t offset, uint16_t value) {
    uint32_t index;

    if (decode_dispi_index(offset, &index) != 0) {
        return -1;
    }

    if (index == HYPE_BOCHS_VBE_INDEX_ID || index == HYPE_BOCHS_VBE_INDEX_VIDEO_MEMORY_64K) {
        return 0;
    }

    dev->regs[index] = value;
    return 0;
}

void hype_bochs_vbe_get_mode(const hype_bochs_vbe_t *dev, hype_bochs_vbe_mode_t *out_mode) {
    uint32_t bpp = dev->regs[HYPE_BOCHS_VBE_INDEX_BPP];
    uint32_t enable = dev->regs[HYPE_BOCHS_VBE_INDEX_ENABLE];
    uint32_t bytes_per_pixel = bytes_per_pixel_for_bpp(bpp);
    uint32_t virt_width;
    uint32_t x_offset;
    uint32_t y_offset;

    out_mode->width = dev->regs[HYPE_BOCHS_VBE_INDEX_XRES];
    out_mode->height = dev->regs[HYPE_BOCHS_VBE_INDEX_YRES];
    out_mode->bytes_per_pixel = bytes_per_pixel;

    virt_width = effective_virtual_dimension(dev->regs[HYPE_BOCHS_VBE_INDEX_VIRT_WIDTH], out_mode->width);
    x_offset = dev->regs[HYPE_BOCHS_VBE_INDEX_X_OFFSET];
    y_offset = dev->regs[HYPE_BOCHS_VBE_INDEX_Y_OFFSET];

    out_mode->stride_bytes = virt_width * bytes_per_pixel;
    out_mode->fb_offset_bytes = (x_offset * bytes_per_pixel) + (y_offset * out_mode->stride_bytes);

    out_mode->valid = 0;
    if (bytes_per_pixel != 0u &&
        (enable & HYPE_BOCHS_VBE_ENABLE_ENABLED) != 0u &&
        (enable & HYPE_BOCHS_VBE_ENABLE_LFB_ENABLED) != 0u &&
        out_mode->width != 0u && out_mode->height != 0u) {
        out_mode->valid = 1;
    }
}
