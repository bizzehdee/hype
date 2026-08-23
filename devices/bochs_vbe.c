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
        uint64_t total_bytes64;
        uint64_t units64;

        hype_bochs_vbe_get_mode(dev, &mode);
        virt_height = effective_virtual_dimension(dev->regs[HYPE_BOCHS_VBE_INDEX_VIRT_HEIGHT],
                                                   dev->regs[HYPE_BOCHS_VBE_INDEX_YRES]);
        /* #655: stride_bytes is guest-controlled and so is virt_height -- their product can
         * exceed UINT32_MAX (see hype_bochs_vbe_get_mode's own comment). Compute in uint64_t and
         * saturate to 0xFFFF (this register is a 16-bit count of 64 KiB units, so any real
         * hardware report is bounded the same way) rather than silently wrapping. */
        total_bytes64 = (uint64_t)mode.stride_bytes * (uint64_t)virt_height;
        units64 = (total_bytes64 + 0xFFFFu) >> 16;
        *out_value = (units64 > 0xFFFFu) ? (uint16_t)0xFFFFu : (uint16_t)units64;
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

    /*
     * #565: enabling the device LATCHES the effective virtual width into the register a guest
     * reads, the way real bochs-display hardware does.
     *
     * hype already computed it -- effective_virtual_dimension() in hype_bochs_vbe_get_mode() --
     * but only for hype's own view of the surface. The register itself still read back whatever
     * the guest had written, which is 0 for a driver that never set it. A driver reads
     * VIRT_WIDTH to compute its stride, so it got a stride of zero and would render nothing,
     * while hype's own rendering path was perfectly correct. Two views of one device that
     * disagreed, with only the guest's being wrong.
     *
     * Found by tests/micro/bochsvbe.c: the in-binary VIDEO-3 test wrote every register from the
     * host and read hype's computed mode, so it could not see the register a guest would read.
     */
    if (index == HYPE_BOCHS_VBE_INDEX_ENABLE && (value & HYPE_BOCHS_VBE_ENABLE_ENABLED) != 0u) {
        uint16_t xres = (uint16_t)dev->regs[HYPE_BOCHS_VBE_INDEX_XRES];
        uint16_t yres = (uint16_t)dev->regs[HYPE_BOCHS_VBE_INDEX_YRES];
        if (dev->regs[HYPE_BOCHS_VBE_INDEX_VIRT_WIDTH] < xres) {
            dev->regs[HYPE_BOCHS_VBE_INDEX_VIRT_WIDTH] = xres;
        }
        if (dev->regs[HYPE_BOCHS_VBE_INDEX_VIRT_HEIGHT] < yres) {
            dev->regs[HYPE_BOCHS_VBE_INDEX_VIRT_HEIGHT] = yres;
        }
    }
    return 0;
}

void hype_bochs_vbe_get_mode(const hype_bochs_vbe_t *dev, hype_bochs_vbe_mode_t *out_mode) {
    uint32_t bpp = dev->regs[HYPE_BOCHS_VBE_INDEX_BPP];
    uint32_t enable = dev->regs[HYPE_BOCHS_VBE_INDEX_ENABLE];
    uint32_t bytes_per_pixel = bytes_per_pixel_for_bpp(bpp);
    uint32_t virt_width;
    uint32_t x_offset;
    uint32_t y_offset;
    uint64_t stride_bytes64;
    uint64_t fb_offset_bytes64;

    out_mode->width = dev->regs[HYPE_BOCHS_VBE_INDEX_XRES];
    out_mode->height = dev->regs[HYPE_BOCHS_VBE_INDEX_YRES];
    out_mode->bytes_per_pixel = bytes_per_pixel;

    virt_width = effective_virtual_dimension(dev->regs[HYPE_BOCHS_VBE_INDEX_VIRT_WIDTH], out_mode->width);
    x_offset = dev->regs[HYPE_BOCHS_VBE_INDEX_X_OFFSET];
    y_offset = dev->regs[HYPE_BOCHS_VBE_INDEX_Y_OFFSET];

    /*
     * #655: every factor here is guest-controlled (DISPI registers, up to 0xFFFF each), and their
     * products can exceed UINT32_MAX -- e.g. a virt_width/virt_height near 0xFFFF at 32bpp
     * overflows a plain uint32_t multiply and silently wraps to a small, wrong value. Compute in
     * uint64_t (matching devices/ramfb.c's hype_ramfb_frame_size, the guarded sibling this file
     * did not match) and refuse the mode -- rather than publish a wrapped stride/offset -- if
     * either product does not fit the 32-bit fields real bochs-display hardware actually reports.
     */
    stride_bytes64 = (uint64_t)virt_width * (uint64_t)bytes_per_pixel;
    fb_offset_bytes64 =
        (uint64_t)x_offset * (uint64_t)bytes_per_pixel + (uint64_t)y_offset * stride_bytes64;

    out_mode->stride_bytes = (uint32_t)stride_bytes64;
    out_mode->fb_offset_bytes = (uint32_t)fb_offset_bytes64;

    out_mode->valid = 0;
    if (bytes_per_pixel != 0u &&
        (enable & HYPE_BOCHS_VBE_ENABLE_ENABLED) != 0u &&
        (enable & HYPE_BOCHS_VBE_ENABLE_LFB_ENABLED) != 0u &&
        out_mode->width != 0u && out_mode->height != 0u &&
        stride_bytes64 <= 0xFFFFFFFFu && fb_offset_bytes64 <= 0xFFFFFFFFu) {
        out_mode->valid = 1;
    }
}

int hype_bochs_vbe_vram_read(const uint8_t *vram, uint32_t vram_size, uint32_t offset,
                             uint32_t len, uint32_t *out_value) {
    uint32_t value = 0;
    uint32_t i;

    if (vram == 0 || out_value == 0 || (len != 1u && len != 2u && len != 4u)) {
        return -1;
    }
    if ((uint64_t)offset + (uint64_t)len > (uint64_t)vram_size) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        value |= (uint32_t)vram[offset + i] << (8u * i);
    }
    *out_value = value;
    return 0;
}

int hype_bochs_vbe_vram_write(uint8_t *vram, uint32_t vram_size, uint32_t offset, uint32_t len,
                              uint32_t value) {
    uint32_t i;

    if (vram == 0 || (len != 1u && len != 2u && len != 4u)) {
        return -1;
    }
    if ((uint64_t)offset + (uint64_t)len > (uint64_t)vram_size) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        vram[offset + i] = (uint8_t)(value >> (8u * i));
    }
    return 0;
}
