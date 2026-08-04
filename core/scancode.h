#ifndef HYPE_CORE_SCANCODE_H
#define HYPE_CORE_SCANCODE_H

#include <stdint.h>

/*
 * INPUT-11 (#284): ASCII -> PS/2 Set-1 scancode sequences, for typing into a guest's
 * emulated keyboard.
 *
 * The scripted-input runner (#279) already drives guests over ttyS0 (#280). This is the
 * second transport, for guests and firmware that read a KEYBOARD rather than a serial
 * port -- OVMF's boot menu, GRUB, and eventually graphical installers.
 *
 * It also retires HYPE_FW_1_AUTO_KEY_INJECT, which existed only to press "any key" at a
 * firmware prompt and was compiled off by default because pressing a key at the wrong
 * moment actively broke Ubuntu's GRUB: it consumed the Enter, GRUB stopped polling, and
 * the guest never booted. A script that waits for a SPECIFIC prompt before typing is the
 * correct version of that, so the blunt version goes away rather than remaining as a
 * second, dumber input path.
 *
 * Pure: produces byte sequences, touches no device. The mapping is where the errors
 * live -- a wrong scancode types the wrong character into an installer -- so it is
 * unit-tested rather than judged by watching a guest.
 */

/* Worst case for one character: shift make + key make + key break + shift break. */
#define HYPE_SCANCODE_MAX_PER_CHAR 4u

/*
 * Append the Set-1 sequence that types `ch` into `out`, and return how many bytes were
 * written. 0 means the character has no mapping and nothing was written -- callers
 * should treat that as "cannot type this", not as success.
 *
 * Emits a complete press-and-release: make code, then break code (0x80 | make). A make
 * with no break leaves the guest believing the key is still held, which for a modifier
 * would corrupt every subsequent character.
 *
 * For characters that need shift ('A', '!', ':' ...) the sequence is shift-make,
 * key-make, key-break, shift-release, so the guest never sees a shifted key without
 * shift held, and shift is never left stuck down afterwards.
 */
unsigned int hype_ascii_to_set1(char ch, uint8_t *out, unsigned int out_cap);

/*
 * Convenience: the whole NUL-terminated string. Returns bytes written. Stops early if
 * `out_cap` would be exceeded rather than truncating mid-character, because half a
 * character (a make with no break) leaves a key stuck down.
 */
unsigned int hype_ascii_string_to_set1(const char *s, uint8_t *out, unsigned int out_cap);

#endif /* HYPE_CORE_SCANCODE_H */
