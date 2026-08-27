#include <stdio.h>
#include <string.h>

#include "../usb_hid.h"
#include "../xhci.h"
#include "../../arch/x86_64/cpu/leader_chord.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* Build a boot-protocol report: modifier byte + up to six usage ids. */
static void mk_report(uint8_t out[8], uint8_t mods, uint8_t k0, uint8_t k1, uint8_t k2) {
    memset(out, 0, 8);
    out[0] = mods;
    out[2] = k0;
    out[3] = k1;
    out[4] = k2;
}

static void test_usage_mapping(void) {
    /* Set-1 is the physical keyboard matrix, not alphabetical -- so these are the
     * cases where a "computed" mapping would be wrong. */
    CHECK_HEX("a -> 0x1E", 0x1E, hype_usb_hid_usage_to_scancode(0x04));
    CHECK_HEX("q -> 0x10", 0x10, hype_usb_hid_usage_to_scancode(0x14));
    CHECK_HEX("z -> 0x2C", 0x2C, hype_usb_hid_usage_to_scancode(0x1D));
    CHECK_HEX("1 -> 0x02", 0x02, hype_usb_hid_usage_to_scancode(0x1E));
    CHECK_HEX("0 -> 0x0B", 0x0B, hype_usb_hid_usage_to_scancode(0x27));
    CHECK_HEX("Enter -> 0x1C", 0x1C, hype_usb_hid_usage_to_scancode(0x28));
    CHECK_HEX("Backspace -> 0x0E", 0x0E, hype_usb_hid_usage_to_scancode(0x2A));
    CHECK_HEX("Space -> 0x39", 0x39, hype_usb_hid_usage_to_scancode(0x2C));
    /* Unmapped usages must yield 0 -- a wrong scancode in a destructive-write
     * confirmation prompt is worse than a missing one. */
    CHECK_HEX("usage 0 unmapped", 0, hype_usb_hid_usage_to_scancode(0x00));
    CHECK_HEX("keypad unmapped", 0, hype_usb_hid_usage_to_scancode(0x62));
    CHECK_HEX("top of range unmapped", 0, hype_usb_hid_usage_to_scancode(0xFF));
}

static void test_press_and_release(void) {
    uint8_t r0[8], r1[8], r2[8];
    uint8_t out[HYPE_USB_HID_MAX_SCANCODES];
    unsigned n;

    mk_report(r0, 0, 0, 0, 0);
    mk_report(r1, 0, 0x04, 0, 0); /* 'a' down */
    mk_report(r2, 0, 0, 0, 0);    /* released */

    n = hype_usb_hid_report_to_scancodes(r0, r1, out, sizeof(out));
    CHECK_HEX("one make code", 1, n);
    CHECK_HEX("make is 0x1E", 0x1E, out[0]);

    n = hype_usb_hid_report_to_scancodes(r1, r2, out, sizeof(out));
    CHECK_HEX("one break code", 1, n);
    CHECK_HEX("break is 0x9E", 0x9E, out[0]);
}

static void test_held_key_emits_nothing(void) {
    uint8_t r[8];
    uint8_t out[HYPE_USB_HID_MAX_SCANCODES];

    mk_report(r, 0, 0x04, 0, 0);
    /*
     * A boot keyboard RE-SENDS the same report while a key is held. Emitting a make
     * code each time would type the character continuously -- so an identical report
     * must produce nothing at all. This is the case that makes diffing necessary
     * rather than just translating the current state.
     */
    CHECK_HEX("identical report -> no codes", 0,
              hype_usb_hid_report_to_scancodes(r, r, out, sizeof(out)));
}

static void test_null_prev_is_all_released(void) {
    uint8_t r[8];
    uint8_t out[HYPE_USB_HID_MAX_SCANCODES];
    unsigned n;

    mk_report(r, 0, 0x05, 0, 0); /* 'b' */
    n = hype_usb_hid_report_to_scancodes(0, r, out, sizeof(out));
    CHECK_HEX("first report treated as all-released", 1, n);
    CHECK_HEX("b -> 0x30", 0x30, out[0]);
}

static void test_shift_precedes_the_key_it_modifies(void) {
    uint8_t r0[8], r1[8];
    uint8_t out[HYPE_USB_HID_MAX_SCANCODES];
    unsigned n;

    mk_report(r0, 0, 0, 0, 0);
    mk_report(r1, 0x02u, 0x04, 0, 0); /* Left Shift + 'a', in ONE report */

    n = hype_usb_hid_report_to_scancodes(r0, r1, out, sizeof(out));
    CHECK_HEX("shift + key", 2, n);
    /*
     * ORDER IS THE ASSERTION. A real keyboard sends shift and the key in one report,
     * and the decoder is a state machine: if 'a' arrives before the shift make code it
     * decodes unshifted. For a typed drive serial that silently produces the wrong
     * string, which the confirm would then reject with no clue why.
     */
    CHECK_HEX("shift make FIRST", 0x2A, out[0]);
    CHECK_HEX("then the key", 0x1E, out[1]);
}

static void test_release_before_press(void) {
    uint8_t r1[8], r2[8];
    uint8_t out[HYPE_USB_HID_MAX_SCANCODES];
    unsigned n;

    mk_report(r1, 0, 0x04, 0, 0); /* 'a' held */
    mk_report(r2, 0, 0x05, 0, 0); /* now 'b' instead */

    n = hype_usb_hid_report_to_scancodes(r1, r2, out, sizeof(out));
    CHECK_HEX("one release + one press", 2, n);
    /* Release first, so the transition never momentarily looks like both held. */
    CHECK_HEX("release 'a' first", 0x9E, out[0]);
    CHECK_HEX("then press 'b'", 0x30, out[1]);
}

static void test_modifier_release(void) {
    uint8_t r1[8], r2[8];
    uint8_t out[HYPE_USB_HID_MAX_SCANCODES];
    unsigned n;

    mk_report(r1, 0x01u, 0, 0, 0); /* Left Ctrl held */
    mk_report(r2, 0x00u, 0, 0, 0);
    n = hype_usb_hid_report_to_scancodes(r1, r2, out, sizeof(out));
    CHECK_HEX("ctrl release", 1, n);
    CHECK_HEX("ctrl break 0x9D", 0x9D, out[0]);

    /*
     * #734: the RIGHT-hand Ctrl is `E0 1D`, NOT a bare 0x1D. It used to be folded onto
     * the left-hand code, and this test asserted the folding -- which is how a broken
     * leader chord passed the suite for as long as it did. hype_chord_feed_scancode()
     * distinguishes the sides and every chord needs the right-hand pair.
     */
    mk_report(r1, 0x00u, 0, 0, 0);
    mk_report(r2, 0x10u, 0, 0, 0); /* Right Ctrl */
    n = hype_usb_hid_report_to_scancodes(r1, r2, out, sizeof(out));
    CHECK_HEX("right ctrl make is two bytes", 2, n);
    CHECK_HEX("E0 prefix", 0xE0, out[0]);
    CHECK_HEX("then 0x1D", 0x1D, out[1]);

    mk_report(r1, 0x10u, 0, 0, 0);
    mk_report(r2, 0x00u, 0, 0, 0);
    n = hype_usb_hid_report_to_scancodes(r1, r2, out, sizeof(out));
    CHECK_HEX("right ctrl break is two bytes", 2, n);
    CHECK_HEX("E0 prefix on break too", 0xE0, out[0]);
    CHECK_HEX("then 0x9D", 0x9D, out[1]);

    /* The LEFT-hand pair stays single-byte -- that part was always right. */
    mk_report(r1, 0x00u, 0, 0, 0);
    mk_report(r2, 0x01u, 0, 0, 0); /* Left Ctrl */
    n = hype_usb_hid_report_to_scancodes(r1, r2, out, sizeof(out));
    CHECK_HEX("left ctrl stays one byte", 1, n);
    CHECK_HEX("bare 0x1D", 0x1D, out[0]);

    /* Right SHIFT is genuinely single-byte on Set-1; it must NOT gain a prefix. */
    mk_report(r1, 0x00u, 0, 0, 0);
    mk_report(r2, 0x20u, 0, 0, 0);
    n = hype_usb_hid_report_to_scancodes(r1, r2, out, sizeof(out));
    CHECK_HEX("right shift is one byte", 1, n);
    CHECK_HEX("0x36", 0x36, out[0]);
}

static void test_arrows_are_extended(void) {
    uint8_t r0[8], r1[8];
    uint8_t out[HYPE_USB_HID_MAX_SCANCODES];
    unsigned n;

    /*
     * #734: arrows are `E0 4B` and friends. The un-prefixed forms are keypad codes, and
     * core/kbd_decode.c only maps the arrows behind SC_EXT_PREFIX -- so the old
     * single-byte output meant an arrow key did nothing in hype's own line editor and
     * could not complete Right-Ctrl+Right-Alt+Left either.
     */
    mk_report(r0, 0, 0, 0, 0);
    mk_report(r1, 0, 0x50, 0, 0); /* Left arrow */
    n = hype_usb_hid_report_to_scancodes(r0, r1, out, sizeof(out));
    CHECK_HEX("left arrow is two bytes", 2, n);
    CHECK_HEX("E0", 0xE0, out[0]);
    CHECK_HEX("0x4B", 0x4B, out[1]);

    n = hype_usb_hid_report_to_scancodes(r1, r0, out, sizeof(out));
    CHECK_HEX("release is two bytes", 2, n);
    CHECK_HEX("E0", 0xE0, out[0]);
    CHECK_HEX("0xCB", 0xCB, out[1]);
}

static void test_a_lone_e0_is_never_emitted_at_the_cap(void) {
    uint8_t r0[8], r1[8];
    uint8_t out[1];
    unsigned n;

    /*
     * A one-byte buffer cannot hold `E0 4B`. Emitting just the 0xE0 would re-prefix
     * whatever byte the NEXT call produced, turning an unrelated key into an extended
     * one -- so the pair is dropped whole.
     */
    mk_report(r0, 0, 0, 0, 0);
    mk_report(r1, 0, 0x50, 0, 0);
    n = hype_usb_hid_report_to_scancodes(r0, r1, out, sizeof(out));
    CHECK_HEX("no room for the pair, so nothing", 0, n);
}

static void test_usb_keyboard_can_actually_fire_a_leader_chord(void) {
    uint8_t r0[8], r1[8], r2[8];
    uint8_t out[HYPE_USB_HID_MAX_SCANCODES];
    hype_chord_state_t st;
    hype_chord_result_t res;
    unsigned n, i;

    /*
     * #734, and the reason any of the above matters: drive the REAL path end to end --
     * boot report -> scancodes -> hype_chord_feed_scancode -> action. On the 5950X this
     * read `scancodes=147 chords=0`, and the operator could get into a guest with the
     * `focus` command and had no way back out, because Right-Ctrl+Right-Alt+Esc could
     * not be assembled from what the USB path emitted.
     */
    hype_chord_state_reset(&st);
    res.action = HYPE_CHORD_ACTION_NONE;

    mk_report(r0, 0, 0, 0, 0);
    mk_report(r1, 0x50u, 0, 0, 0);    /* Right Ctrl + Right Alt held */
    mk_report(r2, 0x50u, 0x29, 0, 0); /* ... and Escape */

    n = hype_usb_hid_report_to_scancodes(r0, r1, out, sizeof(out));
    for (i = 0; i < n; i++) {
        res = hype_chord_feed_scancode(&st, out[i]);
    }
    CHECK_HEX("modifiers alone are not an action", HYPE_CHORD_ACTION_NONE, res.action);

    n = hype_usb_hid_report_to_scancodes(r1, r2, out, sizeof(out));
    for (i = 0; i < n; i++) {
        res = hype_chord_feed_scancode(&st, out[i]);
    }
    CHECK_HEX("Right-Ctrl+Right-Alt+Esc returns to the dashboard",
              HYPE_CHORD_ACTION_RETURN_TO_DASHBOARD, res.action);
}

static void test_usb_keyboard_chord_jump_and_cycle(void) {
    uint8_t r0[8], r1[8], r2[8];
    uint8_t out[HYPE_USB_HID_MAX_SCANCODES];
    hype_chord_state_t st;
    hype_chord_result_t res;
    unsigned n, i;

    hype_chord_state_reset(&st);
    res.action = HYPE_CHORD_ACTION_NONE;
    mk_report(r0, 0, 0, 0, 0);
    mk_report(r1, 0x50u, 0, 0, 0);
    mk_report(r2, 0x50u, 0x1E, 0, 0); /* '1' */

    n = hype_usb_hid_report_to_scancodes(r0, r1, out, sizeof(out));
    for (i = 0; i < n; i++) (void)hype_chord_feed_scancode(&st, out[i]);
    n = hype_usb_hid_report_to_scancodes(r1, r2, out, sizeof(out));
    for (i = 0; i < n; i++) res = hype_chord_feed_scancode(&st, out[i]);
    CHECK_HEX("jump to VM", HYPE_CHORD_ACTION_JUMP_TO_VM, res.action);
    CHECK_HEX("vm 1", 1, res.vm_index);

    /* And the cycle chord, which needs BOTH halves extended -- the modifiers and the
     * arrow. It is the one that fails if only one of the two fixes is applied. */
    hype_chord_state_reset(&st);
    mk_report(r2, 0x50u, 0x50, 0, 0); /* Right Ctrl + Right Alt + Left arrow */
    n = hype_usb_hid_report_to_scancodes(r0, r1, out, sizeof(out));
    for (i = 0; i < n; i++) (void)hype_chord_feed_scancode(&st, out[i]);
    n = hype_usb_hid_report_to_scancodes(r1, r2, out, sizeof(out));
    for (i = 0; i < n; i++) res = hype_chord_feed_scancode(&st, out[i]);
    CHECK_HEX("cycle to the previous VM", HYPE_CHORD_ACTION_CYCLE_PREV, res.action);
}

static void test_unmapped_keys_are_skipped_not_zero_emitted(void) {
    uint8_t r0[8], r1[8];
    uint8_t out[HYPE_USB_HID_MAX_SCANCODES];
    unsigned n;

    mk_report(r0, 0, 0, 0, 0);
    mk_report(r1, 0, 0x62, 0x04, 0); /* keypad (unmapped) + 'a' */
    n = hype_usb_hid_report_to_scancodes(r0, r1, out, sizeof(out));
    /* Emitting a 0 byte for the unmapped key would look like a real scancode to the
     * decoder; it must be dropped entirely, leaving just 'a'. */
    CHECK_HEX("only the mapped key", 1, n);
    CHECK_HEX("and it is 'a'", 0x1E, out[0]);
}

static void test_output_capacity_is_respected(void) {
    uint8_t r0[8], r1[8];
    uint8_t out[2];
    unsigned n;

    memset(r1, 0, sizeof(r1));
    mk_report(r0, 0, 0, 0, 0);
    r1[2] = 0x04; r1[3] = 0x05; r1[4] = 0x06; r1[5] = 0x07; r1[6] = 0x08; r1[7] = 0x09;
    n = hype_usb_hid_report_to_scancodes(r0, r1, out, sizeof(out));
    CHECK_HEX("never writes past out_cap", 2, n);
}

static void test_null_safe(void) {
    uint8_t r[8];
    uint8_t out[4];
    mk_report(r, 0, 0x04, 0, 0);
    CHECK_HEX("NULL cur", 0, hype_usb_hid_report_to_scancodes(r, 0, out, sizeof(out)));
    CHECK_HEX("NULL out", 0, hype_usb_hid_report_to_scancodes(r, r, 0, 4));
}

static void test_find_keyboard(void) {
    /* config(9) + HID boot-keyboard interface(9) + HID descriptor(9) + int-IN ep(7) */
    static const uint8_t cfg[] = {
        9, HYPE_USB_DESC_CONFIG, 34, 0, 1, 1, 0, 0xA0, 50,
        9, HYPE_USB_DESC_INTERFACE, 0, 0, 1, HYPE_USB_CLASS_HID, HYPE_USB_SUBCLASS_BOOT,
           HYPE_USB_PROTO_KEYBOARD, 0,
        9, 0x21, 0x11, 0x01, 0, 1, 0x22, 65, 0,       /* HID class descriptor */
        7, HYPE_USB_DESC_ENDPOINT, 0x81, 0x03, 8, 0, 10
    };
    hype_usb_hid_kbd_t k;

    CHECK_HEX("keyboard found", 0, hype_usb_hid_find_keyboard(cfg, (unsigned)sizeof(cfg), &k));
    CHECK_HEX("found flag", 1, k.found);
    CHECK_HEX("interrupt IN ep 0x81", 0x81, k.int_in_ep);
    CHECK_HEX("mps 8", 8, k.mps);
    CHECK_HEX("interval 10", 10, k.interval);
    CHECK_HEX("config value 1", 1, k.config_value);
}

static void test_find_keyboard_ignores_interrupt_out(void) {
    /* A boot keyboard may expose an interrupt OUT for its LEDs FIRST. Binding to that
     * would leave hype waiting forever for a report on an endpoint that never sends
     * one -- indistinguishable from a dead keyboard. */
    static const uint8_t cfg[] = {
        9, HYPE_USB_DESC_CONFIG, 41, 0, 1, 1, 0, 0xA0, 50,
        9, HYPE_USB_DESC_INTERFACE, 0, 0, 2, HYPE_USB_CLASS_HID, HYPE_USB_SUBCLASS_BOOT,
           HYPE_USB_PROTO_KEYBOARD, 0,
        7, HYPE_USB_DESC_ENDPOINT, 0x02, 0x03, 8, 0, 10,   /* interrupt OUT */
        7, HYPE_USB_DESC_ENDPOINT, 0x83, 0x03, 8, 0, 10    /* interrupt IN  */
    };
    hype_usb_hid_kbd_t k;

    CHECK_HEX("found", 0, hype_usb_hid_find_keyboard(cfg, (unsigned)sizeof(cfg), &k));
    CHECK_HEX("picked the IN endpoint", 0x83, k.int_in_ep);
}

static void test_find_keyboard_rejects_non_boot_and_malformed(void) {
    hype_usb_hid_kbd_t k;
    /* HID but NOT boot protocol: its report layout is defined by a report descriptor
     * hype does not parse, so guessing the byte meanings would produce phantom keys. */
    {
        static const uint8_t cfg[] = {
            9, HYPE_USB_DESC_CONFIG, 25, 0, 1, 1, 0, 0xA0, 50,
            9, HYPE_USB_DESC_INTERFACE, 0, 0, 1, HYPE_USB_CLASS_HID, 0x00, 0x00, 0,
            7, HYPE_USB_DESC_ENDPOINT, 0x81, 0x03, 8, 0, 10
        };
        CHECK_HEX("non-boot HID refused", (unsigned long long)(-1),
                  (unsigned long long)hype_usb_hid_find_keyboard(cfg, (unsigned)sizeof(cfg), &k));
        CHECK_HEX("found stays 0", 0, k.found);
    }
    /* A mouse must not be taken as a keyboard. */
    {
        static const uint8_t cfg[] = {
            9, HYPE_USB_DESC_CONFIG, 25, 0, 1, 1, 0, 0xA0, 50,
            9, HYPE_USB_DESC_INTERFACE, 0, 0, 1, HYPE_USB_CLASS_HID, HYPE_USB_SUBCLASS_BOOT,
               HYPE_USB_PROTO_MOUSE, 0,
            7, HYPE_USB_DESC_ENDPOINT, 0x81, 0x03, 4, 0, 10
        };
        CHECK_HEX("mouse refused", (unsigned long long)(-1),
                  (unsigned long long)hype_usb_hid_find_keyboard(cfg, (unsigned)sizeof(cfg), &k));
    }
    /* Zero-length descriptor: device-supplied, must terminate rather than spin. */
    {
        static const uint8_t cfg[] = {0, HYPE_USB_DESC_INTERFACE, 0, 0, 1, 3, 1, 1, 0};
        CHECK_HEX("zero-length terminates", (unsigned long long)(-1),
                  (unsigned long long)hype_usb_hid_find_keyboard(cfg, (unsigned)sizeof(cfg), &k));
    }
    CHECK_HEX("NULL cfg", (unsigned long long)(-1),
              (unsigned long long)hype_usb_hid_find_keyboard(0, 9u, &k));
    CHECK_HEX("NULL out", (unsigned long long)(-1),
              (unsigned long long)hype_usb_hid_find_keyboard((const uint8_t *)"x", 1u, 0));
}


static void test_unmapped_modifiers_and_modifier_capacity(void) {
    uint8_t r0[8], r1[8];
    uint8_t out[HYPE_USB_HID_MAX_SCANCODES];
    uint8_t tiny[1];

    /* GUI (Windows) keys are deliberately unmapped -- nothing consumes them, and
     * inventing a scancode would inject a keypress the operator did not make. */
    mk_report(r0, 0x00u, 0, 0, 0);
    mk_report(r1, 0x88u, 0, 0, 0); /* Left GUI | Right GUI */
    CHECK_HEX("GUI modifiers produce nothing", 0,
              hype_usb_hid_report_to_scancodes(r0, r1, out, sizeof(out)));

    /* Capacity must bound the MODIFIER loop too, not just the key loops: two
     * transitions into a one-byte buffer. */
    mk_report(r1, 0x03u, 0, 0, 0); /* Ctrl + Shift together */
    CHECK_HEX("modifier loop respects out_cap", 1,
              hype_usb_hid_report_to_scancodes(r0, r1, tiny, sizeof(tiny)));
}

static void test_find_keyboard_rejects_non_interrupt_endpoint(void) {
    /* A HID boot-keyboard interface whose only endpoint is BULK. Accepting it would
     * configure a bulk endpoint and then wait for interrupt reports that never come. */
    static const uint8_t cfg[] = {
        9, HYPE_USB_DESC_CONFIG, 25, 0, 1, 1, 0, 0xA0, 50,
        9, HYPE_USB_DESC_INTERFACE, 0, 0, 1, HYPE_USB_CLASS_HID, HYPE_USB_SUBCLASS_BOOT,
           HYPE_USB_PROTO_KEYBOARD, 0,
        7, HYPE_USB_DESC_ENDPOINT, 0x81, 0x02, 8, 0, 10   /* bulk IN, not interrupt */
    };
    hype_usb_hid_kbd_t k;
    CHECK_HEX("bulk endpoint refused", (unsigned long long)(-1),
              (unsigned long long)hype_usb_hid_find_keyboard(cfg, (unsigned)sizeof(cfg), &k));
}

static void test_find_keyboard_short_descriptors(void) {
    hype_usb_hid_kbd_t k;
    /* Descriptors long enough to walk but too short to hold the fields read from them.
     * These must be skipped rather than read past -- the buffer is device-supplied. */
    {
        static const uint8_t cfg[] = {
            4, HYPE_USB_DESC_CONFIG, 0, 0,                 /* short config */
            5, HYPE_USB_DESC_INTERFACE, 0, 0, 1,           /* short interface */
            4, HYPE_USB_DESC_ENDPOINT, 0x81, 0x03          /* short endpoint */
        };
        CHECK_HEX("short descriptors -> no keyboard", (unsigned long long)(-1),
                  (unsigned long long)hype_usb_hid_find_keyboard(cfg, (unsigned)sizeof(cfg), &k));
    }
}

/* --- USB-6 (#219): boot-protocol mouse --- */

static void test_find_mouse_picks_the_mouse_interface(void) {
    /* A composite device with BOTH a keyboard and a mouse interface -- which is what a
     * wireless receiver presents. The finder must pick the one it was asked for, or a
     * mouse claim binds to the keyboard's endpoint and reports never make sense. */
    static const uint8_t cfg[] = {
        9, 0x02, 0, 0, 2, 1, 0, 0xA0, 50,
        9, 0x04, 0, 0, 1, 0x03, 0x01, 0x01, 0,   /* iface 0: HID boot KEYBOARD */
        9, 0x21, 0x11, 0x01, 0, 1, 0x22, 65, 0,  /* HID descriptor */
        7, 0x05, 0x81, 0x03, 0x08, 0x00, 10,     /* int IN, keyboard */
        9, 0x04, 1, 0, 1, 0x03, 0x01, 0x02, 0,   /* iface 1: HID boot MOUSE */
        9, 0x21, 0x11, 0x01, 0, 1, 0x22, 52, 0,
        7, 0x05, 0x82, 0x03, 0x04, 0x00, 7,      /* int IN, mouse */
    };
    hype_usb_hid_kbd_t m, k;

    CHECK_HEX("mouse found", 0, hype_usb_hid_find_mouse(cfg, (unsigned)sizeof(cfg), &m));
    CHECK_HEX("mouse interface number", 1, (int)m.interface_num);
    CHECK_HEX("mouse endpoint", 0x82u, m.int_in_ep);
    CHECK_HEX("mouse mps", 4, (int)m.mps);
    CHECK_HEX("mouse interval", 7, (int)m.interval);

    CHECK_HEX("keyboard still found", 0, hype_usb_hid_find_keyboard(cfg, (unsigned)sizeof(cfg), &k));
    CHECK_HEX("keyboard endpoint is the other one", 0x81u, k.int_in_ep);
}

static void test_find_mouse_refuses_a_non_boot_mouse(void) {
    /* Report-protocol HID: its bytes need the report descriptor parsed to interpret, so
     * it is refused rather than guessed at. */
    static const uint8_t cfg[] = {
        9, 0x02, 0, 0, 1, 1, 0, 0xA0, 50,
        9, 0x04, 0, 0, 1, 0x03, 0x00, 0x02, 0,   /* subclass 0 = NOT boot */
        7, 0x05, 0x81, 0x03, 0x04, 0x00, 7,
    };
    hype_usb_hid_kbd_t m;

    CHECK_HEX("non-boot mouse refused", -1, hype_usb_hid_find_mouse(cfg, (unsigned)sizeof(cfg), &m));
    CHECK_HEX("found flag clear", 0, m.found);
}

static void test_mouse_report_inverts_y(void) {
    /* HID +Y is DOWN, PS/2 +Y is UP. Getting this wrong gives a pointer that moves
     * vertically backwards, which reads as a broken mouse rather than a sign error. */
    const uint8_t rep[4] = {0x00, 5, 7, 0}; /* no buttons, +5 right, +7 down */
    uint8_t ps2[HYPE_USB_HID_PS2_PACKET_LEN];

    CHECK_HEX("packet written", 3, (int)hype_usb_hid_mouse_report_to_ps2(rep, 4u, ps2, 3u));
    CHECK_HEX("dx passes through", 5u, ps2[1]);
    CHECK_HEX("dy is negated", (uint8_t)(-7), ps2[2]);
    /* dx positive -> X sign clear; dy now negative -> Y sign SET. Bit 3 always set. */
    CHECK_HEX("status: bit3 set, Y sign set, X sign clear", 0x28u, ps2[0]);
}

static void test_mouse_report_buttons_and_signs_agree(void) {
    const uint8_t rep[3] = {0x05, (uint8_t)(-3), 0x00}; /* left+middle, -3 left, no Y */
    uint8_t ps2[3];

    CHECK_HEX("packet written", 3, (int)hype_usb_hid_mouse_report_to_ps2(rep, 3u, ps2, 3u));
    CHECK_HEX("left+middle carried through", 0x05u, ps2[0] & 0x07u);
    CHECK_HEX("X sign set for a negative dx", 0x10u, ps2[0] & 0x10u);
    CHECK_HEX("Y sign clear for zero dy", 0u, ps2[0] & 0x20u);
    CHECK_HEX("dx byte", (uint8_t)(-3), ps2[1]);
    CHECK_HEX("dy byte", 0u, ps2[2]);
}

static void test_mouse_report_clamps_rather_than_wraps(void) {
    /* -128 has no positive counterpart in 8-bit two's complement, so negating it would
     * wrap to -128 again -- a large upward movement becoming a large DOWNWARD one. */
    const uint8_t rep[3] = {0x00, 0x00, 0x80}; /* dy = -128 */
    uint8_t ps2[3];

    CHECK_HEX("packet written", 3, (int)hype_usb_hid_mouse_report_to_ps2(rep, 3u, ps2, 3u));
    CHECK_HEX("clamped to +127, not wrapped to -128", 127u, ps2[2]);
    CHECK_HEX("Y sign clear -- the movement is positive", 0u, ps2[0] & 0x20u);
}

static void test_mouse_report_rejects_bad_input(void) {
    const uint8_t rep[3] = {0, 1, 1};
    uint8_t ps2[3];

    CHECK_HEX("NULL report", 0, (int)hype_usb_hid_mouse_report_to_ps2(0, 3u, ps2, 3u));
    CHECK_HEX("NULL out", 0, (int)hype_usb_hid_mouse_report_to_ps2(rep, 3u, 0, 3u));
    CHECK_HEX("report too short", 0, (int)hype_usb_hid_mouse_report_to_ps2(rep, 2u, ps2, 3u));
    CHECK_HEX("output too small", 0, (int)hype_usb_hid_mouse_report_to_ps2(rep, 3u, ps2, 2u));
}

int main(void) {
    test_find_mouse_picks_the_mouse_interface();
    test_find_mouse_refuses_a_non_boot_mouse();
    test_mouse_report_inverts_y();
    test_mouse_report_buttons_and_signs_agree();
    test_mouse_report_clamps_rather_than_wraps();
    test_mouse_report_rejects_bad_input();
    test_usage_mapping();
    test_press_and_release();
    test_held_key_emits_nothing();
    test_null_prev_is_all_released();
    test_shift_precedes_the_key_it_modifies();
    test_release_before_press();
    test_modifier_release();
    test_arrows_are_extended();
    test_a_lone_e0_is_never_emitted_at_the_cap();
    test_usb_keyboard_can_actually_fire_a_leader_chord();
    test_usb_keyboard_chord_jump_and_cycle();
    test_unmapped_keys_are_skipped_not_zero_emitted();
    test_output_capacity_is_respected();
    test_null_safe();
    test_unmapped_modifiers_and_modifier_capacity();
    test_find_keyboard_rejects_non_interrupt_endpoint();
    test_find_keyboard_short_descriptors();
    test_find_keyboard();
    test_find_keyboard_ignores_interrupt_out();
    test_find_keyboard_rejects_non_boot_and_malformed();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
