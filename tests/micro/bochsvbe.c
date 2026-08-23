/*
 * #565: VIDEO-3 (Bochs VBE), ported out of boot/main.c as a guest-side driver.
 *
 * The in-binary test built its OWN PCI bus and its own device, at an address it chose, and drove
 * the registers from the host. So it never answered the question that matters: can a guest find
 * this adapter on its bus, place its BAR and program a mode? That needed the device to exist for a
 * real VM, which it did not -- which is why this was #565 and not part of #549.
 *
 * This guest does the whole sequence:
 *
 *   walk PCI by class (0x03 display) for 1234:1111, rather than being told where it is
 *   size and place BAR0 and BAR2 itself, then enable memory decoding
 *   read the ID register and require 0xB0C5 -- read-only, so a guest CANNOT have set it
 *   program XRES/YRES/BPP and enable, then read the values back
 *   check VIDEO_MEMORY_64K, which is COMPUTED rather than stored
 *   write a pattern into BAR0 (the linear framebuffer) and read it back byte-exact (#690)
 *
 * The read-only and computed registers are the interesting half. The in-binary test wrote every
 * register from the host, so it could not tell a modelled register from a stored one -- if the
 * model simply remembered whatever was written, that test passed. A guest writing them and reading
 * back something different is the only way to know the model computes.
 *
 * #690: the in-binary test's original bar item 3 ("write a pattern into the linear framebuffer and
 * read it back") was never implemented at all -- devices/bochs_vbe.c modeled only the BAR2
 * register window, with no BAR0 memory window backing it. Found auditing #565's own closed
 * deliverable list (#615, AUDIT-4) and filed as #690. This guest is the actual bar for that item.
 *
 * `display = bochs` in this VM's config is what makes the device exist (§10 decision 49). Every
 * other VM has no VBE adapter, deliberately: a Linux guest with bochs-drm inbox would bind it and
 * move its console away from the surface hype renders.
 */
#include "micro_pci.h"

#define NAME "bochsvbe"

#define VBE_PCI_VENDOR 0x1234u
#define VBE_PCI_DEVICE 0x1111u
#define VBE_PCI_CLASS 0x038000u /* display / other / 0 */

/* DISPI registers live at BAR2 + 0x500, each 16 bits, register N at N*2. */
#define VBE_DISPI_OFFSET 0x500u
#define VBE_IDX_ID 0x0u
#define VBE_IDX_XRES 0x1u
#define VBE_IDX_YRES 0x2u
#define VBE_IDX_BPP 0x3u
#define VBE_IDX_ENABLE 0x4u
#define VBE_IDX_VIRT_WIDTH 0x6u
#define VBE_IDX_VIDEO_MEMORY_64K 0xAu

#define VBE_ID5 0xB0C5u
#define VBE_ENABLE_ENABLED 0x01u
#define VBE_ENABLE_LFB 0x40u

static volatile uint8_t *g_bar;
static volatile uint8_t *g_vram;

static uint16_t dispi_read(unsigned idx) {
    uint16_t v;
    __asm__ volatile("movw (%1), %0"
                     : "=r"(v)
                     : "r"(g_bar + VBE_DISPI_OFFSET + idx * 2u)
                     : "memory");
    return v;
}

static void dispi_write(unsigned idx, uint16_t v) {
    __asm__ volatile("movw %0, (%1)"
                     :
                     : "r"(v), "r"(g_bar + VBE_DISPI_OFFSET + idx * 2u)
                     : "memory");
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    unsigned dev, func, found_dev = 32u, found_func = 0u;
    uint32_t bar_size, vram_size;
    uint64_t bar_gpa, vram_gpa;
    uint16_t id, xres, yres, bpp, enable, vwidth, mem64k;

    (void)zero_page_gpa;
    micro_puts("\n");

    /* Walk the bus by CLASS. Being told the device number would test nothing about discovery, and
     * the slot is hype's choice rather than something a guest may assume. */
    for (dev = 0; dev < 32u && found_dev == 32u; dev++) {
        for (func = 0; func < 8u; func++) {
            if (!micro_pci_fpresent(dev, func)) {
                continue;
            }
            if (micro_pci_fclass(dev, func) != VBE_PCI_CLASS) {
                continue;
            }
            {
                uint32_t vd = micro_pci_fread32(dev, func, MICRO_PCI_VENDOR_ID);
                micro_puts("micro/" NAME ": display device at ");
                micro_put_uint(dev);
                micro_puts(".");
                micro_put_uint(func);
                micro_puts(" ");
                micro_put_hex(vd & 0xFFFFu);
                micro_puts(":");
                micro_put_hex((vd >> 16) & 0xFFFFu);
                micro_puts("\n");
                if ((vd & 0xFFFFu) == VBE_PCI_VENDOR && ((vd >> 16) & 0xFFFFu) == VBE_PCI_DEVICE) {
                    found_dev = dev;
                    found_func = func;
                    break;
                }
            }
        }
    }
    if (found_dev == 32u) {
        micro_fail(NAME, "no Bochs VBE adapter on the bus -- this VM needs display = bochs in its "
                         "config (decision 49; it is off by default on purpose)");
        micro_halt();
    }

    /*
     * #690: size BOTH bars before placing EITHER one. hype's own ECAM-write latch (fw_1's
     * shared-window dispatch) fires on every PCI config write once Memory Space Enable is set --
     * including the size-probe's own transient all-ones write, which is otherwise
     * indistinguishable from a real placement. Placing BAR0 first (which enables memory space)
     * and only THEN probing BAR2's size fed that transient 0xFFFFFFFF probe value into the
     * latch, permanently wedging it before the real BAR2 placement below ever got a chance --
     * found while wiring this test. Doing every probe first, while memory space is still
     * disabled, keeps the probes silent the way the original BAR2-only version of this test did.
     */
    vram_size = micro_pci_fbar_size(found_dev, found_func, 0u);
    bar_size = micro_pci_fbar_size(found_dev, found_func, 2u);
    if (vram_size == 0u) {
        micro_fail(NAME, "BAR0 (the linear framebuffer) is not present -- #565's own bar item 3 "
                         "was never wired (#690)");
        micro_halt();
    }
    if (bar_size < 0x1000u) {
        micro_fail(NAME, "BAR2 is too small to hold the DISPI register window");
        micro_halt();
    }

    vram_gpa = micro_pci_fplace_bar(found_dev, found_func, 0u, MICRO_BAR_WINDOW);
    g_vram = (volatile uint8_t *)(uintptr_t)vram_gpa;
    micro_puts("micro/" NAME ": BAR0 (VRAM) size ");
    micro_put_hex(vram_size);
    micro_puts(" placed at ");
    micro_put_hex(vram_gpa);
    micro_puts("\n");

    bar_gpa = micro_pci_fplace_bar(found_dev, found_func, 2u, MICRO_BAR_WINDOW + vram_size);
    g_bar = (volatile uint8_t *)(uintptr_t)bar_gpa;
    micro_puts("micro/" NAME ": BAR2 size ");
    micro_put_hex(bar_size);
    micro_puts(" placed at ");
    micro_put_hex(bar_gpa);
    micro_puts("\n");

    /*
     * The ID register is READ-ONLY and always reports the highest version the device implements.
     * A guest cannot have set it, so a correct value here is evidence the model answers rather
     * than echoes -- which the host-driven test could not establish.
     */
    id = dispi_read(VBE_IDX_ID);
    micro_puts("micro/" NAME ": DISPI ID ");
    micro_put_hex(id);
    micro_puts(" (expecting 0xb0c5)\n");
    if (id != VBE_ID5) {
        micro_fail(NAME, "the read-only ID register did not report ID5 -- either this is not the "
                         "VBE model or it is echoing writes rather than answering");
        micro_halt();
    }

    /* Try to WRITE the read-only ID. It must not change. */
    dispi_write(VBE_IDX_ID, 0x1234u);
    if (dispi_read(VBE_IDX_ID) != VBE_ID5) {
        micro_fail(NAME, "the ID register accepted a write -- it is read-only on real hardware, so "
                         "a guest could corrupt the version the driver binds against");
        micro_halt();
    }

    /* Program a mode, then read every field back. */
    dispi_write(VBE_IDX_ENABLE, 0u); /* disable while reprogramming, as a real driver does */
    dispi_write(VBE_IDX_XRES, 640u);
    dispi_write(VBE_IDX_YRES, 480u);
    dispi_write(VBE_IDX_BPP, 32u);
    dispi_write(VBE_IDX_ENABLE, VBE_ENABLE_ENABLED | VBE_ENABLE_LFB);

    xres = dispi_read(VBE_IDX_XRES);
    yres = dispi_read(VBE_IDX_YRES);
    bpp = dispi_read(VBE_IDX_BPP);
    enable = dispi_read(VBE_IDX_ENABLE);
    vwidth = dispi_read(VBE_IDX_VIRT_WIDTH);
    micro_puts("micro/" NAME ": mode readback ");
    micro_put_uint(xres);
    micro_puts("x");
    micro_put_uint(yres);
    micro_puts("x");
    micro_put_uint(bpp);
    micro_puts(", enable ");
    micro_put_hex(enable);
    micro_puts(", virt_width ");
    micro_put_uint(vwidth);
    micro_puts("\n");
    if (xres != 640u || yres != 480u || bpp != 32u) {
        micro_fail(NAME, "the programmed mode did not read back");
        micro_halt();
    }
    if ((enable & VBE_ENABLE_ENABLED) == 0u) {
        micro_fail(NAME, "the ENABLE bit did not stick");
        micro_halt();
    }
    /* VIRT_WIDTH defaults to XRES when the guest has not set it -- a value the model must derive,
     * not a register it merely stores. */
    if (vwidth != 640u) {
        micro_fail(NAME, "VIRT_WIDTH did not default to XRES -- the model is storing rather than "
                         "computing it");
        micro_halt();
    }

    /*
     * VIDEO_MEMORY_64K is READ-ONLY and COMPUTED from the mode. A model that stored it would
     * return whatever was last written; one that computes it returns a size consistent with
     * 640x480x32. This is the register that distinguishes the two, which is why it is checked.
     */
    mem64k = dispi_read(VBE_IDX_VIDEO_MEMORY_64K);
    micro_puts("micro/" NAME ": video memory ");
    micro_put_uint(mem64k);
    micro_puts(" x 64 KiB\n");
    if (mem64k == 0u) {
        micro_fail(NAME, "the device reports zero video memory, so no framebuffer could be mapped");
        micro_halt();
    }
    /* 640x480x4 bytes = 1200 KiB = 18.75 x 64 KiB, so a computed value must be at least 19. */
    if (mem64k < 19u) {
        micro_fail(NAME, "the reported video memory is smaller than the mode just programmed "
                         "needs -- it is not computed from the mode");
        micro_halt();
    }

    /*
     * #690: write a pattern into the linear framebuffer through BAR0 and read it back byte-exact
     * -- #565's own bar item 3, never implemented until now. Exercises 1/2/4-byte accesses (the
     * decode widths the shared VMX/SVM MMIO handler actually supports) at the start, middle and
     * last dword of the just-enabled mode's own byte size, not just offset 0 -- a model that
     * ignored the offset entirely would still pass an offset-0-only check.
     */
    {
        uint32_t fb_bytes = (uint32_t)xres * (uint32_t)yres * ((uint32_t)bpp / 8u);
        uint32_t mid = (fb_bytes / 2u) & ~3u;
        uint32_t last = (fb_bytes - 4u) & ~3u;
        uint8_t byte_val = 0xA5u;
        uint16_t word_val = 0x1234u;
        uint32_t dword_val = 0xDEADBEEFu;

        g_vram[0] = byte_val;
        if (g_vram[0] != byte_val) {
            micro_fail(NAME, "BAR0 byte write at offset 0 did not read back");
            micro_halt();
        }
        __asm__ volatile("movw %0, (%1)" : : "r"(word_val), "r"(g_vram + 4u) : "memory");
        {
            uint16_t back;
            __asm__ volatile("movw (%1), %0" : "=r"(back) : "r"(g_vram + 4u) : "memory");
            if (back != word_val) {
                micro_fail(NAME, "BAR0 word write/readback mismatch");
                micro_halt();
            }
        }
        __asm__ volatile("movl %0, (%1)" : : "r"(dword_val), "r"(g_vram + mid) : "memory");
        {
            uint32_t back;
            __asm__ volatile("movl (%1), %0" : "=r"(back) : "r"(g_vram + mid) : "memory");
            if (back != dword_val) {
                micro_fail(NAME, "BAR0 dword write/readback mismatch at the framebuffer's midpoint "
                                 "-- a model that ignored the offset would still pass an offset-0 "
                                 "check");
                micro_halt();
            }
        }
        __asm__ volatile("movl %0, (%1)" : : "r"(dword_val), "r"(g_vram + last) : "memory");
        {
            uint32_t back;
            __asm__ volatile("movl (%1), %0" : "=r"(back) : "r"(g_vram + last) : "memory");
            if (back != dword_val) {
                micro_fail(NAME, "BAR0 dword write/readback mismatch at the last dword of the "
                                 "programmed mode's own framebuffer size");
                micro_halt();
            }
        }
        micro_puts("micro/" NAME ": BAR0 pattern write/readback OK at offsets 0, ");
        micro_put_hex(mid);
        micro_puts(", ");
        micro_put_hex(last);
        micro_puts("\n");
    }

    micro_pass(NAME);
    micro_halt();
}
