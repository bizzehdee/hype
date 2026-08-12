#ifndef HYPE_CORE_SCANCODE_QUEUE_H
#define HYPE_CORE_SCANCODE_QUEUE_H

#include <stdint.h>

/*
 * #375: BSP-to-vCPU Set-1 scancode transport.
 *
 * The BSP owns the physical keyboard. Each guest device model is owned by that
 * guest's vCPU core. This SPSC ring crosses that boundary without allowing the
 * BSP to mutate a live guest device model concurrently with its owner.
 */
#define HYPE_SCANCODE_QUEUE_SIZE 64u

typedef struct {
    uint8_t data[HYPE_SCANCODE_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
    unsigned long long queued;
    unsigned long long consumed;
    unsigned long long dropped;
} hype_scancode_queue_t;

void hype_scancode_queue_reset(hype_scancode_queue_t *q);
int hype_scancode_queue_enqueue(hype_scancode_queue_t *q, uint8_t scancode);
int hype_scancode_queue_dequeue(hype_scancode_queue_t *q, uint8_t *out);
unsigned int hype_scancode_queue_pending(const hype_scancode_queue_t *q);
void hype_scancode_queue_stats(const hype_scancode_queue_t *q,
                               unsigned long long *queued,
                               unsigned long long *consumed,
                               unsigned long long *dropped);

#endif /* HYPE_CORE_SCANCODE_QUEUE_H */
