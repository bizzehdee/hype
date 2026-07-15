#include <stdio.h>
#include "../../arch/x86_64/cpu/ps2_host.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_reset_is_empty(void) {
    hype_host_kbd_buffer_t buf;
    uint8_t value = 0xAAu;

    hype_host_kbd_buffer_reset(&buf);
    CHECK_HEX("pop on an empty buffer returns 0", 0, hype_host_kbd_buffer_pop(&buf, &value));
    CHECK_HEX("out param left untouched", 0xAAu, value);
}

static void test_push_then_pop_roundtrip(void) {
    hype_host_kbd_buffer_t buf;
    uint8_t value = 0;

    hype_host_kbd_buffer_reset(&buf);
    hype_host_kbd_buffer_push(&buf, 0x1Eu);

    CHECK_HEX("pop reports success", 1, hype_host_kbd_buffer_pop(&buf, &value));
    CHECK_HEX("scancode read back", 0x1Eu, value);
    CHECK_HEX("empty again after the pop", 0, hype_host_kbd_buffer_pop(&buf, &value));
}

static void test_fifo_ordering(void) {
    hype_host_kbd_buffer_t buf;
    uint8_t value = 0;

    hype_host_kbd_buffer_reset(&buf);
    hype_host_kbd_buffer_push(&buf, 0x1Eu);
    hype_host_kbd_buffer_push(&buf, 0x9Eu);
    hype_host_kbd_buffer_push(&buf, 0x30u);

    hype_host_kbd_buffer_pop(&buf, &value);
    CHECK_HEX("first pushed is popped first", 0x1Eu, value);
    hype_host_kbd_buffer_pop(&buf, &value);
    CHECK_HEX("second pushed is popped second", 0x9Eu, value);
    hype_host_kbd_buffer_pop(&buf, &value);
    CHECK_HEX("third pushed is popped third", 0x30u, value);
}

static void test_overflow_drops_extra_bytes_safely(void) {
    hype_host_kbd_buffer_t buf;
    unsigned int i;
    uint8_t value = 0;

    hype_host_kbd_buffer_reset(&buf);
    for (i = 0; i < HYPE_HOST_KBD_BUFFER_SIZE + 4; i++) {
        hype_host_kbd_buffer_push(&buf, (uint8_t)i);
    }

    CHECK_HEX("buffer holds exactly its own capacity, not corrupted", 1,
              hype_host_kbd_buffer_pop(&buf, &value));
    CHECK_HEX("the oldest byte (not one of the dropped overflow bytes) is still first", 0, value);
}

static void test_wraparound_after_fill_and_drain(void) {
    hype_host_kbd_buffer_t buf;
    unsigned int i;
    uint8_t value = 0;

    hype_host_kbd_buffer_reset(&buf);
    for (i = 0; i < HYPE_HOST_KBD_BUFFER_SIZE; i++) {
        hype_host_kbd_buffer_push(&buf, (uint8_t)i);
    }
    for (i = 0; i < HYPE_HOST_KBD_BUFFER_SIZE / 2; i++) {
        hype_host_kbd_buffer_pop(&buf, &value);
    }
    /* head has now advanced partway through the ring -- pushing more
     * must wrap correctly rather than corrupt already-buffered data. */
    hype_host_kbd_buffer_push(&buf, 0xAAu);
    hype_host_kbd_buffer_push(&buf, 0xBBu);

    for (i = HYPE_HOST_KBD_BUFFER_SIZE / 2; i < HYPE_HOST_KBD_BUFFER_SIZE; i++) {
        hype_host_kbd_buffer_pop(&buf, &value);
        CHECK_HEX("remaining original bytes read back in order", (uint8_t)i, value);
    }
    hype_host_kbd_buffer_pop(&buf, &value);
    CHECK_HEX("first wrapped-in byte", 0xAAu, value);
    hype_host_kbd_buffer_pop(&buf, &value);
    CHECK_HEX("second wrapped-in byte", 0xBBu, value);
    CHECK_HEX("empty afterward", 0, hype_host_kbd_buffer_pop(&buf, &value));
}

int main(void) {
    test_reset_is_empty();
    test_push_then_pop_roundtrip();
    test_fifo_ordering();
    test_overflow_drops_extra_bytes_safely();
    test_wraparound_after_fill_and_drain();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
