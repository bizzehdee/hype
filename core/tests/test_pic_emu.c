#include <stdio.h>
#include "../../devices/pic.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_reset_is_fully_masked(void) {
    hype_pic_emu_t pic;
    hype_pic_emu_reset(&pic);
    CHECK_HEX("master IMR fully masked after reset", 0xFF, pic.master.imr);
    CHECK_HEX("slave IMR fully masked after reset", 0xFF, pic.slave.imr);
}

static void test_unrecognized_port_rejected(void) {
    hype_pic_emu_t pic;
    uint8_t value = 0;
    hype_pic_emu_reset(&pic);
    CHECK_HEX("write to an unrelated port is rejected", 1, hype_pic_emu_io_write(&pic, 0x60, 0) != 0);
    CHECK_HEX("read from an unrelated port is rejected", 1,
              hype_pic_emu_io_read(&pic, 0x60, &value) != 0);
}

static void test_master_init_sequence_cascade_with_icw4(void) {
    hype_pic_emu_t pic;
    uint8_t value;

    hype_pic_emu_reset(&pic);

    /* ICW1: IC4=1 (0x01), SNGL=0 (cascade mode), init bit (0x10) set. */
    hype_pic_emu_io_write(&pic, 0x20, 0x11);
    /* ICW2: vector offset 0x20 (standard remap target). */
    hype_pic_emu_io_write(&pic, 0x21, 0x20);
    /* ICW3: cascade wiring bitmap (master) -- content unused by the stub. */
    hype_pic_emu_io_write(&pic, 0x21, 0x04);
    /* ICW4: 8086 mode. */
    hype_pic_emu_io_write(&pic, 0x21, 0x01);

    CHECK_HEX("master vector offset programmed via ICW2", 0x20, pic.master.irq_offset);
    CHECK_HEX("master init sequence complete (state back to normal)", 0, pic.master.init_state);

    /* Now in normal operation: data port writes are OCW1 (mask). */
    hype_pic_emu_io_write(&pic, 0x21, 0xFB); /* unmask IRQ2 (cascade line), mask everything else */
    hype_pic_emu_io_read(&pic, 0x21, &value);
    CHECK_HEX("OCW1 mask readable back", 0xFB, value);
}

static void test_init_without_icw4(void) {
    hype_pic_emu_t pic;
    hype_pic_emu_reset(&pic);

    /* ICW1: IC4=0 (no ICW4), SNGL=1 (single PIC, no ICW3). */
    hype_pic_emu_io_write(&pic, 0x20, 0x10 | 0x02);
    hype_pic_emu_io_write(&pic, 0x21, 0x08); /* ICW2 */

    CHECK_HEX("no ICW3/ICW4 expected -- init completes after just ICW2", 0, pic.master.init_state);
}

static void test_init_single_mode_with_icw4(void) {
    hype_pic_emu_t pic;
    hype_pic_emu_reset(&pic);

    /* ICW1: IC4=1 (0x01), SNGL=1 (0x02, single PIC -- no ICW3 expected). */
    hype_pic_emu_io_write(&pic, 0x20, 0x10 | 0x02 | 0x01);
    CHECK_HEX("single mode still expects ICW2 first", 1, pic.master.init_state);

    hype_pic_emu_io_write(&pic, 0x21, 0x08); /* ICW2 */
    CHECK_HEX("single mode with IC4 skips ICW3, goes straight to expecting ICW4", 3,
              pic.master.init_state);

    hype_pic_emu_io_write(&pic, 0x21, 0x01); /* ICW4 */
    CHECK_HEX("init sequence complete after ICW4", 0, pic.master.init_state);
}

static void test_read_slave_command_port(void) {
    hype_pic_emu_t pic;
    uint8_t value;
    hype_pic_emu_reset(&pic);

    pic.slave.irr = 0x03;
    hype_pic_emu_io_read(&pic, 0xA0, &value);
    CHECK_HEX("slave command-port read returns its own IRR (default select)", 0x03, value);
}

static void test_ocw2_non_specific_eoi_clears_lowest_isr_bit(void) {
    hype_pic_emu_t pic;
    hype_pic_emu_reset(&pic);

    pic.master.isr = 0x06; /* IRQ1 and IRQ2 both "in service" */
    hype_pic_emu_io_write(&pic, 0x20, 0x20); /* non-specific EOI */

    CHECK_HEX("non-specific EOI clears the lowest-numbered set ISR bit (IRQ1)", 0x04, pic.master.isr);
}

static void test_ocw2_specific_eoi_clears_named_bit(void) {
    hype_pic_emu_t pic;
    hype_pic_emu_reset(&pic);

    pic.master.isr = 0x06;
    hype_pic_emu_io_write(&pic, 0x20, 0x60 | 0x02); /* specific EOI for IRQ2 */

    CHECK_HEX("specific EOI clears exactly the named bit (IRQ2)", 0x02, pic.master.isr);
}

static void test_ocw3_selects_irr_vs_isr_read(void) {
    hype_pic_emu_t pic;
    uint8_t value;
    hype_pic_emu_reset(&pic);

    pic.master.irr = 0x01;
    pic.master.isr = 0x02;

    hype_pic_emu_io_write(&pic, 0x20, 0x08 | 0x02 | 0x00); /* OCW3, RR=1, RIS=0 -> select IRR */
    hype_pic_emu_io_read(&pic, 0x20, &value);
    CHECK_HEX("OCW3 RIS=0 selects IRR on next command-port read", 0x01, value);

    hype_pic_emu_io_write(&pic, 0x20, 0x08 | 0x02 | 0x01); /* OCW3, RR=1, RIS=1 -> select ISR */
    hype_pic_emu_io_read(&pic, 0x20, &value);
    CHECK_HEX("OCW3 RIS=1 selects ISR on next command-port read", 0x02, value);
}

static void test_slave_is_independent_of_master(void) {
    hype_pic_emu_t pic;
    uint8_t value;
    hype_pic_emu_reset(&pic);

    hype_pic_emu_io_write(&pic, 0xA0, 0x11);
    hype_pic_emu_io_write(&pic, 0xA1, 0x28);
    hype_pic_emu_io_write(&pic, 0xA1, 0x02);
    hype_pic_emu_io_write(&pic, 0xA1, 0x01);
    hype_pic_emu_io_write(&pic, 0xA1, 0xFF);

    hype_pic_emu_io_read(&pic, 0xA1, &value);
    CHECK_HEX("slave IMR independent of master's", 0xFF, value);
    CHECK_HEX("master untouched by slave programming", 0xFF, pic.master.imr);
    CHECK_HEX("slave vector offset programmed", 0x28, pic.slave.irq_offset);
}

static void test_raise_irq_sets_irr_bit_independent_of_mask(void) {
    hype_pic_emu_t pic;
    hype_pic_emu_reset(&pic); /* IMR = 0xFF, everything masked */

    hype_pic_emu_raise_irq(&pic.master, 3);

    CHECK_HEX("IRR reflects a pending request regardless of masking", 0x08, pic.master.irr);
}

static void test_raise_irq_ignores_out_of_range(void) {
    hype_pic_emu_t pic;
    hype_pic_emu_reset(&pic);

    hype_pic_emu_raise_irq(&pic.master, 8);

    CHECK_HEX("out-of-range irq number is a no-op", 0, pic.master.irr);
}

static void test_acknowledge_computes_vector_from_irq_offset(void) {
    hype_pic_emu_t pic;
    uint8_t vector = 0;

    hype_pic_emu_reset(&pic);
    pic.master.irq_offset = 0x20u; /* matches the real-mode BIOS default remap */
    pic.master.imr = 0x00u;        /* nothing masked */
    hype_pic_emu_raise_irq(&pic.master, 1);

    CHECK_HEX("acknowledge reports a pending IRQ", 1,
              hype_pic_emu_acknowledge_highest_priority(&pic.master, &vector));
    CHECK_HEX("vector = irq_offset + irq number", 0x21u, vector);
    CHECK_HEX("IRR bit moved out", 0, pic.master.irr & (1u << 1));
    CHECK_HEX("ISR bit now set (in service)", 1, (pic.master.isr & (1u << 1)) != 0);
}

static void test_acknowledge_returns_zero_when_masked(void) {
    hype_pic_emu_t pic;
    uint8_t vector = 0xAAu;

    hype_pic_emu_reset(&pic);
    pic.master.irq_offset = 0x20u;
    pic.master.imr = 0xFFu; /* everything masked */
    hype_pic_emu_raise_irq(&pic.master, 1);

    CHECK_HEX("nothing to acknowledge -- masked", 0,
              hype_pic_emu_acknowledge_highest_priority(&pic.master, &vector));
    CHECK_HEX("out_vector left untouched", 0xAAu, vector);
    CHECK_HEX("IRR bit still pending", 1, (pic.master.irr & (1u << 1)) != 0);
}

static void test_acknowledge_returns_zero_when_nothing_pending(void) {
    hype_pic_emu_t pic;
    uint8_t vector = 0xAAu;

    hype_pic_emu_reset(&pic);
    pic.master.irq_offset = 0x20u;
    pic.master.imr = 0x00u;

    CHECK_HEX("nothing to acknowledge -- no IRQ raised", 0,
              hype_pic_emu_acknowledge_highest_priority(&pic.master, &vector));
    CHECK_HEX("out_vector left untouched", 0xAAu, vector);
}

static void test_acknowledge_picks_lowest_numbered_irq_first(void) {
    hype_pic_emu_t pic;
    uint8_t vector = 0;

    hype_pic_emu_reset(&pic);
    pic.master.irq_offset = 0x20u;
    pic.master.imr = 0x00u;
    hype_pic_emu_raise_irq(&pic.master, 3);
    hype_pic_emu_raise_irq(&pic.master, 1);

    CHECK_HEX("acknowledge reports a pending IRQ", 1,
              hype_pic_emu_acknowledge_highest_priority(&pic.master, &vector));
    CHECK_HEX("lower IRQ number (higher priority) wins", 0x21u, vector);
    CHECK_HEX("the other IRQ is still pending", 1, (pic.master.irr & (1u << 3)) != 0);
}

int main(void) {
    test_reset_is_fully_masked();
    test_unrecognized_port_rejected();
    test_master_init_sequence_cascade_with_icw4();
    test_init_without_icw4();
    test_init_single_mode_with_icw4();
    test_read_slave_command_port();
    test_ocw2_non_specific_eoi_clears_lowest_isr_bit();
    test_ocw2_specific_eoi_clears_named_bit();
    test_ocw3_selects_irr_vs_isr_read();
    test_slave_is_independent_of_master();
    test_raise_irq_sets_irr_bit_independent_of_mask();
    test_raise_irq_ignores_out_of_range();
    test_acknowledge_computes_vector_from_irq_offset();
    test_acknowledge_returns_zero_when_masked();
    test_acknowledge_returns_zero_when_nothing_pending();
    test_acknowledge_picks_lowest_numbered_irq_first();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
