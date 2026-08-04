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
#define HYPE_USB_HID_MAX_SCANCODES 32u

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

#endif /* HYPE_CORE_USB_HID_H */
