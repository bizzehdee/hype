#include <stdio.h>
#include "../scancode_queue.h"

static int failures;

#define CHECK(desc, cond) do { if (!(cond)) { printf("FAIL: %s\n", desc); failures++; } } while (0)

int main(void) {
    hype_scancode_queue_t q;
    unsigned long long queued = 0, consumed = 0, dropped = 0;
    uint8_t b = 0;
    unsigned int i;

    hype_scancode_queue_reset(&q);
    CHECK("reset empty", hype_scancode_queue_pending(&q) == 0u);
    CHECK("empty dequeue", !hype_scancode_queue_dequeue(&q, &b));
    CHECK("null output rejected", !hype_scancode_queue_dequeue(&q, 0));

    CHECK("first enqueue", hype_scancode_queue_enqueue(&q, 0x1eu));
    CHECK("second enqueue", hype_scancode_queue_enqueue(&q, 0x9eu));
    CHECK("two pending", hype_scancode_queue_pending(&q) == 2u);
    CHECK("first dequeue", hype_scancode_queue_dequeue(&q, &b) && b == 0x1eu);
    CHECK("second dequeue", hype_scancode_queue_dequeue(&q, &b) && b == 0x9eu);

    for (i = 0; i < HYPE_SCANCODE_QUEUE_SIZE - 1u; i++) {
        CHECK("fill accepted", hype_scancode_queue_enqueue(&q, (uint8_t)i));
    }
    CHECK("full count", hype_scancode_queue_pending(&q) == HYPE_SCANCODE_QUEUE_SIZE - 1u);
    CHECK("full drops newest", !hype_scancode_queue_enqueue(&q, 0xffu));
    for (i = 0; i < HYPE_SCANCODE_QUEUE_SIZE - 1u; i++) {
        CHECK("wrapped order", hype_scancode_queue_dequeue(&q, &b) && b == (uint8_t)i);
    }
    CHECK("drained", hype_scancode_queue_pending(&q) == 0u);

    hype_scancode_queue_stats(&q, &queued, &consumed, &dropped);
    CHECK("queued stats", queued == HYPE_SCANCODE_QUEUE_SIZE + 1u);
    CHECK("consumed stats", consumed == HYPE_SCANCODE_QUEUE_SIZE + 1u);
    CHECK("dropped stats", dropped == 1u);
    hype_scancode_queue_stats(&q, 0, 0, 0);

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
