#ifndef HYPE_ARCH_PIT_H
#define HYPE_ARCH_PIT_H

#include <stdint.h>

/*
 * 8253/8254 PIT (M1-8): the host's own timer tick source. Same split
 * as serial.c/serial_hw.c -- divisor arithmetic is pure and tested
 * here; the actual outb programming sequence is the thin, exempt
 * hardware shim in pit_hw.c.
 */

#define HYPE_PIT_CHANNEL0_DATA 0x40
#define HYPE_PIT_COMMAND 0x43
#define HYPE_PIT_BASE_FREQUENCY_HZ 1193182u /* fixed PIT input clock */

/* HYPE_PIT_BASE_FREQUENCY_HZ / hz, e.g. 1000 -> 1193, 100 -> 11931.
 * Returns 0 (invalid) for hz == 0, or a divisor that doesn't fit in
 * the PIT's 16-bit counter (hz too slow), or that would round to 0
 * (hz faster than the PIT can go). */
uint16_t hype_pit_divisor_for_frequency(uint32_t hz);

/* Programs channel 0 for the given frequency in mode 2 (rate
 * generator), firing on IRQ0. Exempt from unit testing -- see
 * pit_hw.c. */
void hype_pit_init(uint32_t hz);

#endif /* HYPE_ARCH_PIT_H */
