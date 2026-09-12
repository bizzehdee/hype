#ifndef HYPE_CORE_STACK_WATERMARK_H
#define HYPE_CORE_STACK_WATERMARK_H

#include <stdint.h>

/*
 * #817: per-core stack high-water marks.
 *
 * The AP stacks are plain 16 KiB slots packed next to each other, with no guard page between them
 * (the host map is 2 MiB pages). fw_1_resolve_on_any_fs() alone had a 19,160-byte frame, so a VM's
 * core resolving its disk ran 15 KB past the bottom of its slot: in QEMU it returned through
 * zeroed memory to address 0; where the memory below happens to be live, the same overflow
 * corrupts it silently.
 *
 * Paint a stack before its core first runs, then measure how far down the paint was disturbed.
 * Stacks grow down, so the used depth is the distance from the top to the lowest disturbed byte.
 * A frame byte that happens to equal the paint value can hide up to that one byte's worth of
 * depth at the low edge -- a measurement bound, not a correctness one.
 */

#define HYPE_STACK_PAINT_BYTE 0xA5u

/* The bottom this many bytes of a slot are its guard: any use there means the core reached, and
 * very probably ran past, the end of its stack. */
#define HYPE_STACK_GUARD_BYTES 256u

void hype_stack_paint(uint8_t *base, uint64_t len);

/* Bytes used, measured from the top (base + len). 0 for a stack never touched. */
uint64_t hype_stack_used(const uint8_t *base, uint64_t len);

/* 1 when `used` reaches into the guard at the bottom of a `len`-byte stack. */
int hype_stack_guard_hit(uint64_t used, uint64_t len);

#endif /* HYPE_CORE_STACK_WATERMARK_H */
