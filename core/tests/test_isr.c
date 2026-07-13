#include <stdio.h>
#include <string.h>
#include "../../arch/x86_64/cpu/isr.h"

static int failures = 0;

#define CHECK_STR(desc, expected, actual) \
    do { \
        if (strcmp((expected), (actual)) != 0) { \
            printf("FAIL: %s: expected \"%s\", got \"%s\"\n", (desc), (expected), (actual)); \
            failures++; \
        } \
    } while (0)

static void test_vector_name(void) {
    static const struct {
        uint64_t vector;
        const char *name;
    } cases[] = {
        {0, "Divide Error"},
        {1, "Debug"},
        {2, "NMI"},
        {3, "Breakpoint"},
        {4, "Overflow"},
        {5, "BOUND Range Exceeded"},
        {6, "Invalid Opcode"},
        {7, "Device Not Available"},
        {8, "Double Fault"},
        {9, "Coprocessor Segment Overrun"},
        {10, "Invalid TSS"},
        {11, "Segment Not Present"},
        {12, "Stack-Segment Fault"},
        {13, "General Protection Fault"},
        {14, "Page Fault"},
        {16, "x87 Floating-Point Exception"},
        {17, "Alignment Check"},
        {18, "Machine Check"},
        {19, "SIMD Floating-Point Exception"},
        {20, "Virtualization Exception"},
        {21, "Control Protection Exception"},
        {28, "Hypervisor Injection Exception"},
        {29, "VMM Communication Exception"},
        {30, "Security Exception"},
    };
    unsigned long long i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK_STR("named vector", cases[i].name, hype_isr_vector_name(cases[i].vector));
    }

    CHECK_STR("vector 15 is a reserved gap", "Reserved", hype_isr_vector_name(15));
    CHECK_STR("vector 22 is a reserved gap before Control Protection's neighbors",
              "Reserved", hype_isr_vector_name(22));
    CHECK_STR("vector 31 is the last reserved slot", "Reserved", hype_isr_vector_name(31));
    CHECK_STR("vector 32 is the first IRQ/user vector", "IRQ/User-Defined", hype_isr_vector_name(32));
    CHECK_STR("vector 255 (last possible)", "IRQ/User-Defined", hype_isr_vector_name(255));
}

static void test_format_message(void) {
    hype_isr_frame_t frame;
    char buf[256];

    memset(&frame, 0, sizeof(frame));
    frame.vector = 13;
    frame.error_code = 0x10;
    frame.rip = 0xffff800012345678ULL;
    frame.cs = 0x08;
    frame.rflags = 0x246;

    hype_isr_format_message(buf, sizeof(buf), &frame);

    if (strstr(buf, "vector=13") == 0) {
        printf("FAIL: format_message missing vector number: %s\n", buf);
        failures++;
    }
    if (strstr(buf, "General Protection Fault") == 0) {
        printf("FAIL: format_message missing vector name: %s\n", buf);
        failures++;
    }
    if (strstr(buf, "0x10") == 0) {
        printf("FAIL: format_message missing error code: %s\n", buf);
        failures++;
    }
    if (strstr(buf, "ffff800012345678") == 0) {
        printf("FAIL: format_message missing rip: %s\n", buf);
        failures++;
    }
}

static void test_format_message_truncates_safely(void) {
    hype_isr_frame_t frame;
    char buf[8];
    int found_nul = 0;
    unsigned long long i;

    memset(&frame, 0, sizeof(frame));
    frame.vector = 0;

    /* Must not crash/overflow with a tiny buffer -- confirms bufsz is
     * actually passed through to hype_snprintf rather than, say, a
     * hardcoded internal size. */
    hype_isr_format_message(buf, sizeof(buf), &frame);
    for (i = 0; i < sizeof(buf); i++) {
        if (buf[i] == '\0') {
            found_nul = 1;
            break;
        }
    }
    if (!found_nul) {
        printf("FAIL: format_message did not NUL-terminate within a tiny buffer\n");
        failures++;
    }
}

int main(void) {
    test_vector_name();
    test_format_message();
    test_format_message_truncates_safely();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
