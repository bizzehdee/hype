#ifndef HYPE_DEVICES_BOCHS_VBE_H
#define HYPE_DEVICES_BOCHS_VBE_H

#include <stdint.h>

/*
 * VIDEO-3: a post-boot, PCI-discoverable Bochs-VBE-class display
 * adapter -- what Windows' inbox Basic Display Adapter driver and
 * Linux/BSD's vesafb/efifb/simpledrm expect to find and drive after
 * their own graphics driver loads (distinct from VIDEO-1/VIDEO-2's
 * firmware-time GOP/ramfb protocol, which only works pre-OS-driver).
 *
 * Modeled after QEMU's "bochs-display" device specifically (not the
 * combined legacy-VGA+VBE "std-vga"/"VGA" device) -- pure Bochs VBE,
 * no legacy VGA text-mode/CRTC registers or ISA I/O ports, matching
 * this project's own "primitive now, integration later" bias toward
 * the simplest device that satisfies real guest drivers. Register
 * indices, MMIO offset formula, ENABLE flag bits, and framebuffer/mode
 * semantics below were fetched and confirmed directly from QEMU's own
 * source (hw/display/bochs-display.c, include/hw/display/bochs-vbe.h)
 * at implementation time, not reconstructed from memory -- same
 * discipline as this project's other wire-format structs.
 *
 * PCI identity: vendor 0x1234 (PCI_VENDOR_ID_QEMU), device 0x1111
 * (PCI_DEVICE_ID_QEMU_VGA), class 0x03/0x80/0x00 (PCI_CLASS_DISPLAY_OTHER
 * -- bochs-display is deliberately *not* VGA-compatible-class 0x0300).
 * BAR0 = framebuffer/VRAM (prefetchable memory), BAR2 = 0x1000-byte
 * MMIO register window.
 *
 * This module models the MMIO register set + mode computation only --
 * it never touches guest memory itself (no framebuffer pixel access
 * here), same layering as devices/ahci.h's own register-model/
 * transport split.
 */

/* DISPI register indices, within the MMIO window at
 * HYPE_BOCHS_VBE_DISPI_OFFSET, register N at byte offset N*2
 * (16-bit, little-endian). */
#define HYPE_BOCHS_VBE_INDEX_ID 0x0u
#define HYPE_BOCHS_VBE_INDEX_XRES 0x1u
#define HYPE_BOCHS_VBE_INDEX_YRES 0x2u
#define HYPE_BOCHS_VBE_INDEX_BPP 0x3u
#define HYPE_BOCHS_VBE_INDEX_ENABLE 0x4u
#define HYPE_BOCHS_VBE_INDEX_BANK 0x5u
#define HYPE_BOCHS_VBE_INDEX_VIRT_WIDTH 0x6u
#define HYPE_BOCHS_VBE_INDEX_VIRT_HEIGHT 0x7u
#define HYPE_BOCHS_VBE_INDEX_X_OFFSET 0x8u
#define HYPE_BOCHS_VBE_INDEX_Y_OFFSET 0x9u
/* Read-only, computed from the other registers -- not stored. */
#define HYPE_BOCHS_VBE_INDEX_VIDEO_MEMORY_64K 0xAu
#define HYPE_BOCHS_VBE_NUM_REGS 0xAu /* stored registers: indices 0x0..0x9 */

/* ENABLE register flag bits. */
#define HYPE_BOCHS_VBE_ENABLE_DISABLED 0x00u
#define HYPE_BOCHS_VBE_ENABLE_ENABLED 0x01u
#define HYPE_BOCHS_VBE_ENABLE_GETCAPS 0x02u
#define HYPE_BOCHS_VBE_ENABLE_8BIT_DAC 0x20u
#define HYPE_BOCHS_VBE_ENABLE_LFB_ENABLED 0x40u
#define HYPE_BOCHS_VBE_ENABLE_NOCLEARMEM 0x80u

/* ID register values -- the ID register is read-only and always
 * reports the highest version this device implements (ID5), matching
 * bochs-display.c's own unconditional read behavior. */
#define HYPE_BOCHS_VBE_ID0 0xB0C0u
#define HYPE_BOCHS_VBE_ID5 0xB0C5u

/* MMIO window layout (BAR2). */
#define HYPE_BOCHS_VBE_MMIO_SIZE 0x1000u
#define HYPE_BOCHS_VBE_DISPI_OFFSET 0x500u
/* 11 registers (indices 0x0..0xA), 2 bytes each. */
#define HYPE_BOCHS_VBE_DISPI_SIZE 0x16u

/* PCI identity constants (see this header's own top comment). */
#define HYPE_BOCHS_VBE_PCI_VENDOR_ID 0x1234u
#define HYPE_BOCHS_VBE_PCI_DEVICE_ID 0x1111u
#define HYPE_BOCHS_VBE_PCI_CLASS_BASE 0x03u
#define HYPE_BOCHS_VBE_PCI_CLASS_SUB 0x80u
#define HYPE_BOCHS_VBE_PCI_CLASS_INTERFACE 0x00u

typedef struct {
    uint16_t regs[HYPE_BOCHS_VBE_NUM_REGS];
} hype_bochs_vbe_t;

/* Only bpp 16 (r5g6b5) and 32 (x8r8g8b8, little-endian byte order)
 * are real, matching bochs-display's own restricted mode set (the
 * combined std-vga device additionally supports 4/8/15/24bpp for
 * legacy VBE compatibility -- deliberately out of scope here). */
typedef struct {
    int valid;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_pixel;
    uint32_t stride_bytes;     /* virt_width * bytes_per_pixel */
    uint32_t fb_offset_bytes;  /* x_offset*bpp + y_offset*stride */
} hype_bochs_vbe_mode_t;

/* Resets every stored register to 0 (XRES/YRES/BPP/ENABLE/BANK/
 * VIRT_WIDTH/VIRT_HEIGHT/X_OFFSET/Y_OFFSET all start at 0, matching
 * real hardware/QEMU's own post-reset state -- a guest driver must
 * program every field itself before enabling the LFB). */
void hype_bochs_vbe_reset(hype_bochs_vbe_t *dev);

/*
 * Reads the 16-bit DISPI register at `offset` (byte offset relative to
 * HYPE_BOCHS_VBE_DISPI_OFFSET's own base within the MMIO window, i.e.
 * the caller has already subtracted HYPE_BOCHS_VBE_DISPI_OFFSET from
 * the BAR-relative address). Must be 2-byte aligned and within
 * HYPE_BOCHS_VBE_DISPI_SIZE. The ID register (index 0) always reads
 * back HYPE_BOCHS_VBE_ID5 regardless of what was last written, and
 * VIDEO_MEMORY_64K (index 0xA) is computed from XRES/YRES/BPP rather
 * than stored -- both match real hardware. Returns 0 on success, -1
 * for a misaligned or out-of-range offset.
 */
int hype_bochs_vbe_mmio_read(const hype_bochs_vbe_t *dev, uint32_t offset, uint16_t *out_value);

/*
 * Writes `value` to the DISPI register at `offset` (same alignment/
 * range rule as hype_bochs_vbe_mmio_read()). Writes to the read-only
 * ID and VIDEO_MEMORY_64K registers are silently ignored (real
 * hardware convention for a read-only register in an otherwise
 * writable block). Returns 0 on success, -1 for a misaligned or
 * out-of-range offset.
 */
int hype_bochs_vbe_mmio_write(hype_bochs_vbe_t *dev, uint32_t offset, uint16_t value);

/*
 * Computes the current display mode from the register state, mirroring
 * bochs_display_get_mode()'s own arithmetic: stride = virt_width * bpp,
 * fb_offset = x_offset*bpp + y_offset*stride. Reports out->valid = 0
 * (an unusable mode) unless ENABLE has both ENABLED and LFB_ENABLED
 * set and BPP is exactly 16 or 32 -- any other BPP is a real-hardware
 * restriction of the bochs-display device this project models, not an
 * arbitrary simplification.
 *
 * VIRT_WIDTH is clamped up to at least XRES if the guest left it
 * smaller (including the common case of never touching it at all,
 * leaving it 0) -- this matches the well-documented real Bochs VBE
 * convention that virtual resolution auto-raises to the requested
 * resolution rather than producing a nonsensical (too-small) stride.
 */
void hype_bochs_vbe_get_mode(const hype_bochs_vbe_t *dev, hype_bochs_vbe_mode_t *out_mode);

#endif /* HYPE_DEVICES_BOCHS_VBE_H */
