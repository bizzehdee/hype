#include <stdio.h>
#include <string.h>

#include "../usb_hid.h"
#include "../xhci.h"

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

    /* Right-hand Ctrl folds onto the left code: hype's decoder has no side-specific
     * behaviour, so "was Ctrl held" stays a single question. */
    mk_report(r1, 0x00u, 0, 0, 0);
    mk_report(r2, 0x10u, 0, 0, 0); /* Right Ctrl */
    n = hype_usb_hid_report_to_scancodes(r1, r2, out, sizeof(out));
    CHECK_HEX("right ctrl make", 1, n);
    CHECK_HEX("folds to 0x1D", 0x1D, out[0]);
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

int main(void) {
    test_usage_mapping();
    test_press_and_release();
    test_held_key_emits_nothing();
    test_null_prev_is_all_released();
    test_shift_precedes_the_key_it_modifies();
    test_release_before_press();
    test_modifier_release();
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
