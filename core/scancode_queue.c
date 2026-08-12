#include "scancode_queue.h"

void hype_scancode_queue_reset(hype_scancode_queue_t *q) {
    unsigned int i;
    for (i = 0; i < HYPE_SCANCODE_QUEUE_SIZE; i++) {
        q->data[i] = 0;
    }
    __atomic_store_n(&q->head, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&q->tail, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&q->queued, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&q->consumed, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&q->dropped, 0ull, __ATOMIC_RELAXED);
}

int hype_scancode_queue_enqueue(hype_scancode_queue_t *q, uint8_t scancode) {
    uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
    uint32_t next = (tail + 1u) % HYPE_SCANCODE_QUEUE_SIZE;
    uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);

    if (next == head) {
        __atomic_add_fetch(&q->dropped, 1ull, __ATOMIC_RELAXED);
        return 0;
    }
    q->data[tail] = scancode;
    __atomic_store_n(&q->tail, next, __ATOMIC_RELEASE);
    __atomic_add_fetch(&q->queued, 1ull, __ATOMIC_RELAXED);
    return 1;
}

int hype_scancode_queue_dequeue(hype_scancode_queue_t *q, uint8_t *out) {
    uint32_t head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);

    if (out == 0 || head == tail) {
        return 0;
    }
    *out = q->data[head];
    __atomic_store_n(&q->head, (head + 1u) % HYPE_SCANCODE_QUEUE_SIZE, __ATOMIC_RELEASE);
    __atomic_add_fetch(&q->consumed, 1ull, __ATOMIC_RELAXED);
    return 1;
}

unsigned int hype_scancode_queue_pending(const hype_scancode_queue_t *q) {
    uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    return (tail + HYPE_SCANCODE_QUEUE_SIZE - head) % HYPE_SCANCODE_QUEUE_SIZE;
}

void hype_scancode_queue_stats(const hype_scancode_queue_t *q,
                               unsigned long long *queued,
                               unsigned long long *consumed,
                               unsigned long long *dropped) {
    if (queued != 0) {
        *queued = __atomic_load_n(&q->queued, __ATOMIC_RELAXED);
    }
    if (consumed != 0) {
        *consumed = __atomic_load_n(&q->consumed, __ATOMIC_RELAXED);
    }
    if (dropped != 0) {
        *dropped = __atomic_load_n(&q->dropped, __ATOMIC_RELAXED);
    }
}
