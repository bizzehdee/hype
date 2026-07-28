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

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

static int g_handler_calls;
static uint64_t g_handler_last_vector;

/* #248: counts how many frame fields other than `vector` were non-zero, so the
 * frameless dispatch path can be shown not to hand a handler stack residue. */
static int g_captured_nonzero_fields;

static void capture_frame_handler(const hype_isr_frame_t *frame) {
    int n = 0;
    if (frame->r15) n++; if (frame->r14) n++; if (frame->r13) n++; if (frame->r12) n++;
    if (frame->r11) n++; if (frame->r10) n++; if (frame->r9) n++; if (frame->r8) n++;
    if (frame->rbp) n++; if (frame->rdi) n++; if (frame->rsi) n++; if (frame->rdx) n++;
    if (frame->rcx) n++; if (frame->rbx) n++; if (frame->rax) n++;
    if (frame->error_code) n++;
    if (frame->rip) n++; if (frame->cs) n++; if (frame->rflags) n++;
    if (frame->rsp) n++; if (frame->ss) n++;
    g_captured_nonzero_fields = n;
}

static void mock_handler(const hype_isr_frame_t *frame) {
    g_handler_calls++;
    g_handler_last_vector = frame->vector;
}

static void test_register_rejects_exception_vectors(void) {
    CHECK_INT("register rejects vector 0", 0, hype_isr_register(0, mock_handler));
    CHECK_INT("register rejects vector 31", 0, hype_isr_register(31, mock_handler));
}

static void test_register_accepts_irq_vectors(void) {
    CHECK_INT("register accepts vector 32", 1, hype_isr_register(32, mock_handler));
    CHECK_INT("register accepts vector 255", 1, hype_isr_register(255, mock_handler));
}

static void test_dispatch_calls_registered_handler_and_returns(void) {
    hype_isr_frame_t frame;

    memset(&frame, 0, sizeof(frame));
    frame.vector = 200;
    hype_isr_register(200, mock_handler);

    g_handler_calls = 0;
    g_handler_last_vector = 999;

    /* This call must actually return -- if hype_isr_dispatch() fell
     * through to the fatal path here, this test would hang instead of
     * failing cleanly, which is exactly why the no-handler branch is
     * never exercised in this file (see isr.h). */
    hype_isr_dispatch(&frame);

    CHECK_INT("dispatch calls the registered handler exactly once", 1, g_handler_calls);
    CHECK_INT("dispatch passes the same frame through", 200, g_handler_last_vector);
}

static void test_dispatch_last_registration_wins(void) {
    hype_isr_frame_t frame;
    static int second_calls = 0;

    memset(&frame, 0, sizeof(frame));
    frame.vector = 201;

    hype_isr_register(201, mock_handler);
    hype_isr_register(201, mock_handler); /* re-register same vector */

    g_handler_calls = 0;
    hype_isr_dispatch(&frame);
    CHECK_INT("re-registering the same vector still dispatches once", 1, g_handler_calls);
    (void)second_calls;
}

/*
 * #248: hype_isr_dispatch_vector() -- the frameless entry point VMX needs when
 * acknowledge-interrupt-on-exit means the CPU consumed the interrupt and the host
 * IDT never fired, so only the vector number is available.
 */
static void test_dispatch_vector_runs_handler(void) {
    hype_isr_register(202, mock_handler);
    g_handler_calls = 0;
    g_handler_last_vector = 999;

    CHECK_INT("dispatch_vector reports that a handler ran", 1, hype_isr_dispatch_vector(202));
    CHECK_INT("dispatch_vector calls the handler exactly once", 1, g_handler_calls);
    CHECK_INT("dispatch_vector passes the vector through", 202, g_handler_last_vector);
}

static void test_dispatch_vector_unregistered_is_not_fatal(void) {
    /* The whole point of this entry point over hype_isr_dispatch(): an interrupt
     * the hardware acknowledged but hype has no handler for must report "nothing
     * ran" rather than panic. If this ever regresses to hype_fatal() the test
     * binary hangs instead of failing, which is itself the signal. */
    g_handler_calls = 0;
    CHECK_INT("dispatch_vector reports no handler for an unregistered vector", 0,
              hype_isr_dispatch_vector(203));
    CHECK_INT("dispatch_vector runs nothing when unregistered", 0, g_handler_calls);
}

static void test_dispatch_vector_zeroes_the_frame(void) {
    /* Handlers must not see stack garbage in the registers hype cannot supply. */
    hype_isr_register(204, capture_frame_handler);
    g_captured_nonzero_fields = -1;
    (void)hype_isr_dispatch_vector(204);
    CHECK_INT("dispatch_vector zeroes every frame field except vector", 0,
              g_captured_nonzero_fields);
}

int main(void) {
    test_vector_name();
    test_format_message();
    test_format_message_truncates_safely();
    test_register_rejects_exception_vectors();
    test_register_accepts_irq_vectors();
    test_dispatch_calls_registered_handler_and_returns();
    test_dispatch_last_registration_wins();
    test_dispatch_vector_runs_handler();
    test_dispatch_vector_unregistered_is_not_fatal();
    test_dispatch_vector_zeroes_the_frame();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
