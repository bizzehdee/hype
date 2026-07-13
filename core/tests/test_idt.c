#include <stdio.h>
#include "../../arch/x86_64/cpu/idt.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_encode_entry(void) {
    hype_idt_entry_t e;
    /* handler=0x1122334455667788, selector=0x08, ist=0, type_attr=0x8E --
     * exercises every byte split point (offset_low/mid/high). */
    hype_idt_encode_entry(&e, 0x1122334455667788ULL, 0x08, 0, HYPE_IDT_TYPE_INTERRUPT_GATE);

    CHECK_HEX("offset_low", 0x7788, e.offset_low);
    CHECK_HEX("selector", 0x08, e.selector);
    CHECK_HEX("ist", 0, e.ist);
    CHECK_HEX("type_attr", 0x8E, e.type_attr);
    CHECK_HEX("offset_mid", 0x5566, e.offset_mid);
    CHECK_HEX("offset_high", 0x11223344, e.offset_high);
    CHECK_HEX("reserved is zero", 0, e.reserved);
}

static void test_encode_entry_ist_masked(void) {
    hype_idt_entry_t e;
    /* IST is only a 3-bit field; garbage in the upper bits must not
     * leak into type_attr or anywhere else. */
    hype_idt_encode_entry(&e, 0, 0, 0xFF, HYPE_IDT_TYPE_INTERRUPT_GATE);
    CHECK_HEX("ist masked to 3 bits", 0x07, e.ist);
}

static void test_build(void) {
    hype_idt_entry_t table[HYPE_IDT_ENTRY_COUNT];
    uint64_t stubs[HYPE_IDT_ENTRY_COUNT];
    unsigned int i;

    for (i = 0; i < HYPE_IDT_ENTRY_COUNT; i++) {
        stubs[i] = 0x2000ULL + (uint64_t)i * 8ULL;
    }

    hype_idt_build(table, stubs, 0x08);

    for (i = 0; i < HYPE_IDT_ENTRY_COUNT; i++) {
        uint64_t reconstructed = (uint64_t)table[i].offset_low |
                                  ((uint64_t)table[i].offset_mid << 16) |
                                  ((uint64_t)table[i].offset_high << 32);
        if (reconstructed != stubs[i]) {
            printf("FAIL: build vector %u handler mismatch: expected 0x%llx, got 0x%llx\n",
                   i, (unsigned long long)stubs[i], (unsigned long long)reconstructed);
            failures++;
        }
        if (table[i].selector != 0x08) {
            printf("FAIL: build vector %u selector mismatch\n", i);
            failures++;
        }
        if (table[i].type_attr != HYPE_IDT_TYPE_INTERRUPT_GATE) {
            printf("FAIL: build vector %u type_attr mismatch\n", i);
            failures++;
        }
        if (table[i].ist != 0) {
            printf("FAIL: build vector %u ist should be 0 for M1\n", i);
            failures++;
        }
    }
}

int main(void) {
    test_encode_entry();
    test_encode_entry_ist_masked();
    test_build();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
