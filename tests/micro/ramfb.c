/*
 * #549: VIDEO-2 (ramfb), ported out of boot/main.c.
 *
 * The in-binary test drove a 40-byte hand-assembled machine-code payload whose only job was to
 * write two I/O ports and poll a word. Everything that made the test a test -- building the
 * 28-byte RAMFB_CONFIG, choosing the framebuffer address, checking the decode -- was done by the
 * HOST, against a buffer the host had also filled. It proved hype's fw_cfg DMA-write plumbing
 * moved bytes; it could not prove a guest can drive ramfb, because no guest was driving it.
 *
 * Here the guest does the whole thing a real OVMF QemuRamfbDxe does:
 *
 *   walk the fw_cfg file directory and find "etc/ramfb" by name
 *   allocate a framebuffer in its own RAM and build RAMFB_CONFIG pointing at it, big-endian
 *   hand the 28 bytes to the fw_cfg DMA engine with FW_CFG_DMA_CTL_WRITE
 *   write a known pattern into the framebuffer and read it back
 *
 * WHAT THIS CAN AND CANNOT SEE, which is the whole design of the test.
 *
 * The guest can prove the mapping: a pattern written to its framebuffer reads back, so the memory
 * it told hype about is memory it really owns. It CANNOT see whether hype then blitted those
 * pixels to the host's real GOP -- that observable belongs to the host, the same split the
 * PAUSE-filter test settled (#540). So hype prints a per-VM RAMFB line and the harness requires
 * both halves. A guest-only pass here would be a test that cannot fail for the reason that
 * matters.
 *
 * One thing the ticket assumed that turned out to be false: hype did NOT already count ramfb
 * blits per VM (only a last-blit timestamp existed). It does now. And a blit only happens for the
 * VM whose view is ON SCREEN, so the blit count is focus-dependent by nature; the REQUIRED host
 * observable is therefore the config DECODE, which is unconditional. Blits are reported next to
 * it, and are the stronger evidence when the test VM is the visible one.
 *
 * cmdline (#546): `w=`, `h=` override the surface size. Default is small on purpose -- a 64x8
 * surface is 2 KB, which fits any guest and still exercises stride arithmetic.
 */
#include "micro_fwcfg.h"

#define NAME "ramfb"

#define RAMFB_CONFIG_SIZE 28u
#define RAMFB_FORMAT_XRGB8888 0x34325258u

/* Guest-physical layout. Clear of the payload at 16 MB and of the DMA access struct at 3 MB. */
#define CONFIG_GPA 0x330000ull     /* the 28-byte RAMFB_CONFIG the guest hands to fw_cfg */
#define FRAMEBUFFER_GPA 0x400000ull /* 4 MB -- the surface itself */

static unsigned long long parse_uint(const char *s) {
    unsigned long long v = 0ull;
    if (s == 0) {
        return 0ull;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10ull + (unsigned long long)(*s - '0');
        s++;
    }
    return v;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    const char *cl = micro_cmdline(zero_page_gpa);
    uint32_t width = 64u, height = 8u, stride;
    const fw_file_t *ramfb;
    uint8_t *cfg = (uint8_t *)(uintptr_t)CONFIG_GPA;
    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)FRAMEBUFFER_GPA;
    uint32_t total, i;
    unsigned nfiles;

    micro_puts("\n");

    if (cl != 0) {
        const char *v = micro_cmdline_value(cl, "w");
        if (v != 0) {
            unsigned long long n = parse_uint(v);
            if (n == 0ull || n > 4096ull) {
                micro_fail(NAME, "cmdline w= must be 1..4096");
                micro_halt();
            }
            width = (uint32_t)n;
        }
        v = micro_cmdline_value(cl, "h");
        if (v != 0) {
            unsigned long long n = parse_uint(v);
            if (n == 0ull || n > 4096ull) {
                micro_fail(NAME, "cmdline h= must be 1..4096");
                micro_halt();
            }
            height = (uint32_t)n;
        }
    }
    stride = width * 4u; /* XRGB8888 is the only format this device speaks */

    nfiles = fw_read_dir(&total);
    micro_puts("micro/" NAME ": fw_cfg directory has ");
    micro_put_uint(total);
    micro_puts(" file(s)\n");
    if (total > nfiles) {
        /* Said out loud rather than truncating silently -- a walk that stops early and reports a
         * clean miss is indistinguishable from the file being absent. */
        micro_puts("micro/" NAME ": only the first ");
        micro_put_uint(nfiles);
        micro_puts(" recorded; raise FW_CFG_MAX_FILES if etc/ramfb is not among them\n");
    }

    ramfb = fw_find("etc/ramfb");
    if (ramfb == 0) {
        micro_fail(NAME, "no 'etc/ramfb' in the fw_cfg file directory -- hype registers it per VM");
        micro_halt();
    }
    micro_puts("micro/" NAME ": etc/ramfb select=");
    micro_put_hex(ramfb->select);
    micro_puts(" size=");
    micro_put_uint(ramfb->size);
    micro_puts("\n");
    if (ramfb->size != RAMFB_CONFIG_SIZE) {
        /* A real QemuRamfbDxe rejects any other size, so this is the guest behaving as the
         * firmware would rather than tolerating a mismatch hype's own device would not. */
        micro_fail(NAME, "etc/ramfb is not 28 bytes; a real ramfb driver refuses that");
        micro_halt();
    }

    /*
     * RAMFB_CONFIG, every field big-endian:
     *   u64 address, u32 fourcc, u32 flags, u32 width, u32 height, u32 stride
     */
    put_be64(cfg + 0, FRAMEBUFFER_GPA);
    put_be32(cfg + 8, RAMFB_FORMAT_XRGB8888);
    put_be32(cfg + 12, 0u); /* flags */
    put_be32(cfg + 16, width);
    put_be32(cfg + 20, height);
    put_be32(cfg + 24, stride);

    /*
     * Fill the surface BEFORE announcing it. hype may blit as soon as it has a valid config, and
     * a surface announced while still holding whatever was in RAM would put unintended bytes on
     * the operator's screen.
     */
    for (i = 0; i < width * height; i++) {
        fb[i] = 0x00FF00FFu | (i & 0xFFu); /* magenta-ish, with the index in the low byte */
    }

    if (fw_write_dma(NAME, ramfb->select, CONFIG_GPA, RAMFB_CONFIG_SIZE) != 0) {
        micro_fail(NAME, "the fw_cfg DMA write of RAMFB_CONFIG did not complete");
        micro_halt();
    }
    micro_puts("micro/" NAME ": announced ");
    micro_put_uint(width);
    micro_puts("x");
    micro_put_uint(height);
    micro_puts(" stride ");
    micro_put_uint(stride);
    micro_puts(" at gpa ");
    micro_put_hex(FRAMEBUFFER_GPA);
    micro_puts("\n");

    /*
     * Read the pattern back. This is the guest's half of the verdict: the framebuffer it handed
     * to hype is memory it still owns and still reads coherently. A device that mapped it away,
     * or a nested-paging mistake, shows up here.
     */
    for (i = 0; i < width * height; i++) {
        uint32_t want = 0x00FF00FFu | (i & 0xFFu);
        if (fb[i] != want) {
            micro_puts("micro/" NAME ": pixel ");
            micro_put_uint(i);
            micro_puts(" reads ");
            micro_put_hex(fb[i]);
            micro_puts(" expected ");
            micro_put_hex(want);
            micro_puts("\n");
            micro_fail(NAME, "the announced framebuffer does not read back what was written");
            micro_halt();
        }
    }
    micro_puts("micro/" NAME ": framebuffer reads back correctly (");
    micro_put_uint(width * height);
    micro_puts(" pixels)\n");

    /*
     * The guest is done and its half passed. The host's half -- did hype decode this config, and
     * did it blit -- is on hype's own RAMFB line, and the harness requires it. Reported here so a
     * reader of the guest log knows the verdict is only half the story.
     */
    micro_puts("micro/" NAME ": guest side complete; hype's RAMFB line carries the decode and "
               "blit counts\n");
    micro_pass(NAME);
    micro_halt();
}
