#ifndef HYPE_CORE_USB_HID_H
#define HYPE_CORE_USB_HID_H

#include <stdint.h>

/*
 * USB-5 (#217): USB HID boot-protocol keyboard, as a HOST input device.
 *
 * Why this exists: on the real AMD laptop the operator types on a USB keyboard and
 * there is no serial port, so without this hype has no way to receive a keystroke on
 * that machine at all -- no leader chord, no dashboard command, and therefore no
 * interactive physical-write confirm (#233).
 *
 * The design decision that keeps this small: rather than add a second input path
 * beside the PS/2 one, this translates HID reports into **PS/2 Set-1 scancodes** and
 * feeds them to the SAME hype_kbd_decode_feed()/leader-chord/cmdline layer the PS/2
 * host keyboard already drives. So every consumer works unchanged, one decoder is
 * maintained instead of two, and a USB keyboard and a PS/2 keyboard cannot drift
 * apart in behaviour. The alternative -- HID straight to ASCII -- would have needed
 * its own modifier tracking and its own chord recognition.
 *
 * Everything here is pure: descriptor walking and report diffing, no xHCI registers
 * and no transfers, so the translation is unit-tested rather than only ever exercised
 * against a keyboard nobody can attach to a test rig.
 */

/* A boot-protocol keyboard report is exactly 8 bytes: modifiers, reserved, then up
 * to six concurrently-held key usage IDs (HID Usage Tables 1.12, §10). */
#define HYPE_USB_HID_REPORT_LEN 8u
#define HYPE_USB_HID_MAX_KEYS 6u

/* Worst case per report: 6 keys released + 6 pressed + 4 modifier transitions, and a
 * break code is two bytes for the extended set -- sized generously since overflowing
 * would silently drop keystrokes, which is the one failure an input path must not
 * have. */
/* #734: 48, not 32. Extended keys now cost TWO bytes (E0 + code), so the worst-case
 * transition -- eight modifier changes plus six releases and six presses, arrows and
 * right-hand modifiers among them -- no longer fits in 32 and would silently drop its
 * tail. */
#define HYPE_USB_HID_MAX_SCANCODES 48u

/* The interrupt-IN endpoint a boot keyboard delivers its reports on. */
typedef struct {
    int found;
    unsigned int interface_num;
    unsigned int config_value;  /* bConfigurationValue to SET_CONFIGURATION */
    unsigned int int_in_ep;     /* endpoint address, including the 0x80 IN bit */
    unsigned int mps;           /* wMaxPacketSize */
    unsigned int interval;      /* bInterval, raw from the descriptor */
} hype_usb_hid_kbd_t;

/*
 * Find a boot-protocol keyboard interface and its interrupt-IN endpoint in a
 * configuration descriptor. Returns 0 on success (and sets out->found), -1 otherwise.
 *
 * Requires class 0x03 / subclass 0x01 / protocol 0x01 specifically: the BOOT
 * protocol is the one whose report layout is fixed by the spec. A HID device that is
 * not boot-protocol needs its report DESCRIPTOR parsed to know what its bytes mean,
 * which is a different and much larger job -- so those are refused rather than
 * guessed at, since misreading a report layout produces phantom keystrokes.
 */
int hype_usb_hid_find_keyboard(const uint8_t *cfg, unsigned int len, hype_usb_hid_kbd_t *out);

/*
 * Translate the transition between two boot reports into PS/2 Set-1 scancodes.
 *
 * `prev` may be NULL for the first report (treated as all-released). Writes make
 * codes for newly-pressed keys and break codes (0x80 | make) for released ones, plus
 * modifier transitions, into `out`. Returns the number of bytes written.
 *
 * Diffing rather than sending the current state is what makes auto-repeat and
 * held-key behaviour correct: a boot keyboard re-sends the same report while a key is
 * held, and emitting a make code each time would type the character continuously.
 */
unsigned int hype_usb_hid_report_to_scancodes(const uint8_t *prev, const uint8_t *cur,
                                              uint8_t *out, unsigned int out_cap);

/* Set-1 make code for a HID keyboard usage id, or 0 if unmapped. Exposed for tests. */
uint8_t hype_usb_hid_usage_to_scancode(uint8_t usage);

/*
 * USB-6 (#219): USB HID boot-protocol MOUSE, as a HOST pointer device.
 *
 * Same shape as the keyboard above and for the same reason: rather than add a second
 * pointer path, a boot mouse report is translated into the standard PS/2 3-byte movement
 * packet and handed to the guest's PS/2 mouse model (INPUT-2), which every consumer
 * already drives. One translation to maintain, and a USB pointer and a PS/2 pointer
 * cannot drift apart in behaviour.
 *
 * Boot protocol only, for the reason the keyboard gives: a non-boot HID needs its report
 * DESCRIPTOR parsed to know what its bytes mean, and misreading a pointer report produces
 * phantom clicks.
 */

/* A boot-protocol mouse report is at least 3 bytes: buttons, then signed X and Y. Many
 * devices append a wheel byte; it is read when present and ignored otherwise, because the
 * guest PS/2 model here is the 3-byte (non-IntelliMouse) protocol. */
#define HYPE_USB_HID_MOUSE_REPORT_MIN 3u

/* PS/2 movement packet: status, dx, dy. */
#define HYPE_USB_HID_PS2_PACKET_LEN 3u

/*
 * Find a boot-protocol mouse interface and its interrupt-IN endpoint. Returns 0 on
 * success (and sets out->found), -1 otherwise. Shares hype_usb_hid_kbd_t because the
 * fields wanted are identical -- interface, config value, endpoint, mps, interval.
 */
int hype_usb_hid_find_mouse(const uint8_t *cfg, unsigned int len, hype_usb_hid_kbd_t *out);

/*
 * Translate one boot mouse report into a PS/2 movement packet. Returns the number of
 * bytes written (3), or 0 if the report is too short or the output too small.
 *
 * Two conversions that are easy to get wrong and are therefore unit-tested rather than
 * judged by watching a cursor:
 *
 *  - PS/2 Y is INVERTED relative to HID. HID reports +Y as "toward the user" (down the
 *    screen); PS/2 reports +Y as up. Getting this wrong gives a pointer that moves
 *    vertically backwards, which looks like a broken mouse rather than a sign error.
 *  - The status byte carries the SIGN of each delta in bits 4 and 5 and the deltas
 *    themselves as 8-bit two's complement, plus bit 3 always set. A guest that ignores
 *    the sign bits still works; one that honours them sees the wrong direction if they
 *    disagree with the byte, so they are derived from the same value.
 *
 * Deltas outside the 8-bit signed range are CLAMPED, not truncated, and the overflow bits
 * are left clear: truncation wraps a large movement into a movement the other way, which
 * is worse than a short one.
 */
unsigned int hype_usb_hid_mouse_report_to_ps2(const uint8_t *report, unsigned int len,
                                              uint8_t *out, unsigned int out_cap);

#endif /* HYPE_CORE_USB_HID_H */
