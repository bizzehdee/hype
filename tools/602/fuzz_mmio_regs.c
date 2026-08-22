/*
 * #602: host-side libFuzzer harness over five guest-facing register models --
 * IO-APIC, LAPIC MMIO, PIT, PS/2 keyboard, PS/2 mouse -- fed with random
 * offset/port + size + value sequences. This is the class of bug #305/#306
 * exemplify: an input shape nobody wrote a directed case for panicking hype's
 * own decode path, not a missing bounds check inside these particular models
 * (all five already self-validate offset/size before indexing any array --
 * confirmed by reading every one of them for this ticket). The fuzzer's job
 * here is proving that invariant holds under sequences no human enumerated,
 * not hunting a known-missing check.
 *
 * One binary, selector byte per run, because all five targets are small, pure,
 * dependency-free register models and splitting them into five near-identical
 * translation units would not buy distinct coverage libFuzzer cares about --
 * it already differentiates by code path taken, not by binary.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "fuzz_common.h"
#include "../../devices/ioapic.h"
#include "../../devices/guest_lapic.h"
#include "../../devices/pit.h"
#include "../../devices/ps2_keyboard.h"
#include "../../devices/ps2_mouse.h"
#include "../../core/fatal.h" /* hype_debug_set_level -- see fuzz_ahci.c's note */

#define MAX_OPS 256u

static void fuzz_ioapic(hype_fuzz_cursor_t *c) {
    hype_ioapic_t dev;
    unsigned i;
    hype_ioapic_reset(&dev);
    for (i = 0; i < MAX_OPS && hype_fuzz_cursor_remaining(c) >= 7u; i++) {
        uint32_t offset = hype_fuzz_u16(c); /* keep offsets near the real 0x20-byte window
                                              * most of the time, but u16 still reaches
                                              * plenty of out-of-range values */
        uint32_t value = hype_fuzz_u32(c);
        uint8_t op = hype_fuzz_u8(c);
        if (op & 1) {
            uint32_t out = 0;
            (void)hype_ioapic_mmio_read(&dev, offset, &out);
        } else {
            (void)hype_ioapic_mmio_write(&dev, offset, value);
        }
    }
}

static void fuzz_lapic(hype_fuzz_cursor_t *c) {
    hype_guest_lapic_t dev;
    unsigned i;
    hype_guest_lapic_reset(&dev);
    for (i = 0; i < MAX_OPS && hype_fuzz_cursor_remaining(c) >= 7u; i++) {
        uint32_t offset = hype_fuzz_u16(c);
        uint32_t value = hype_fuzz_u32(c);
        uint8_t sizesel = hype_fuzz_u8(c);
        /* Real widths are 1/2/4; also try a handful of other values (the
         * "wrong width" case both virtio accessors explicitly reject). */
        static const unsigned int sizes[6] = {1u, 2u, 4u, 8u, 0u, 3u};
        unsigned int size = sizes[sizesel % 6u];
        if (sizesel & 0x80u) {
            uint32_t out = 0;
            (void)hype_guest_lapic_read(&dev, offset, size, &out);
        } else {
            (void)hype_guest_lapic_write(&dev, offset, size, value);
        }
    }
}

static void fuzz_pit(hype_fuzz_cursor_t *c) {
    hype_pit_emu_t dev;
    unsigned i;
    hype_pit_emu_reset(&dev);
    for (i = 0; i < MAX_OPS && hype_fuzz_cursor_remaining(c) >= 4u; i++) {
        uint16_t port = hype_fuzz_u16(c);
        uint8_t value = hype_fuzz_u8(c);
        uint8_t op = hype_fuzz_u8(c);
        if (op & 1) {
            uint8_t out = 0;
            (void)hype_pit_emu_io_read(&dev, port, &out);
        } else {
            (void)hype_pit_emu_io_write(&dev, port, value);
        }
    }
}

static void fuzz_ps2_kbd(hype_fuzz_cursor_t *c) {
    hype_ps2_kbd_t dev;
    unsigned i;
    hype_ps2_kbd_reset(&dev);
    for (i = 0; i < MAX_OPS && hype_fuzz_cursor_remaining(c) >= 4u; i++) {
        uint16_t port = hype_fuzz_u16(c);
        uint8_t value = hype_fuzz_u8(c);
        uint8_t op = hype_fuzz_u8(c);
        if (op & 1) {
            uint8_t out = 0;
            (void)hype_ps2_kbd_io_read(&dev, port, &out);
        } else {
            (void)hype_ps2_kbd_io_write(&dev, port, value);
        }
    }
}

static void fuzz_ps2_mouse(hype_fuzz_cursor_t *c) {
    hype_ps2_mouse_t dev;
    unsigned i;
    hype_ps2_mouse_reset(&dev);
    for (i = 0; i < MAX_OPS && hype_fuzz_cursor_remaining(c) >= 1u; i++) {
        uint8_t command = hype_fuzz_u8(c);
        hype_ps2_mouse_write_command(&dev, command);
        while (hype_ps2_mouse_has_pending_byte(&dev)) {
            (void)hype_ps2_mouse_read_byte(&dev);
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    hype_fuzz_cursor_t c;
    uint8_t which;

    if (size == 0) return 0;

    hype_debug_set_level(HYPE_LOG_ERROR);
    hype_fuzz_cursor_init(&c, data, size);
    which = hype_fuzz_u8(&c) % 5u;

    switch (which) {
        case 0: fuzz_ioapic(&c); break;
        case 1: fuzz_lapic(&c); break;
        case 2: fuzz_pit(&c); break;
        case 3: fuzz_ps2_kbd(&c); break;
        case 4: fuzz_ps2_mouse(&c); break;
        default: break;
    }
    return 0;
}
