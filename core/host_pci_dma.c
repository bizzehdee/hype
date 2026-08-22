#include "host_pci_dma.h"

unsigned int hype_dma_ring_advance(unsigned int index, unsigned int capacity) {
    if (capacity == 0u) {
        return 0u;
    }
    return (index + 1u) % capacity;
}

int hype_dma_ring_full(unsigned int head, unsigned int tail, unsigned int capacity) {
    if (capacity == 0u) {
        return 1;
    }
    return hype_dma_ring_advance(tail, capacity) == (head % capacity);
}

unsigned int hype_dma_ring_used(unsigned int head, unsigned int tail, unsigned int capacity) {
    if (capacity == 0u) {
        return 0u;
    }
    return (tail + capacity - (head % capacity)) % capacity;
}

void hype_dma_cqueue_advance(unsigned int *index, unsigned int *phase, unsigned int capacity) {
    if (index == 0 || phase == 0 || capacity == 0u) {
        return;
    }
    *index = (*index + 1u) % capacity;
    if (*index == 0u) {
        *phase ^= 1u;
    }
}

void hype_dma_link_ring_advance(unsigned int *enqueue, unsigned int *cycle, unsigned int capacity) {
    if (enqueue == 0 || cycle == 0 || capacity < 2u) {
        return;
    }
    if (*enqueue + 1u >= capacity - 1u) {
        *enqueue = 0u;
        *cycle ^= 1u;
    } else {
        *enqueue = *enqueue + 1u;
    }
}

#define HYPE_DMA_POOL_MAX_SLOTS 64u

void hype_dma_pool_init(hype_dma_pool_t *pool, unsigned int slot_count) {
    if (pool == 0) {
        return;
    }
    pool->slot_count = (slot_count > HYPE_DMA_POOL_MAX_SLOTS) ? HYPE_DMA_POOL_MAX_SLOTS : slot_count;
    pool->used_bitmap = 0;
}

int hype_dma_pool_alloc(hype_dma_pool_t *pool) {
    unsigned int i;
    if (pool == 0) {
        return -1;
    }
    for (i = 0; i < pool->slot_count; i++) {
        uint64_t bit = ((uint64_t)1) << i;
        if ((pool->used_bitmap & bit) == 0u) {
            pool->used_bitmap |= bit;
            return (int)i;
        }
    }
    return -1;
}

void hype_dma_pool_free(hype_dma_pool_t *pool, int index) {
    if (pool == 0 || index < 0 || (unsigned int)index >= pool->slot_count) {
        return;
    }
    pool->used_bitmap &= ~(((uint64_t)1) << (unsigned int)index);
}

int hype_dma_pool_is_used(const hype_dma_pool_t *pool, int index) {
    if (pool == 0 || index < 0 || (unsigned int)index >= pool->slot_count) {
        return 0;
    }
    return (pool->used_bitmap & (((uint64_t)1) << (unsigned int)index)) != 0u;
}

unsigned int hype_dma_pool_used_count(const hype_dma_pool_t *pool) {
    unsigned int i, n = 0;
    if (pool == 0) {
        return 0u;
    }
    for (i = 0; i < pool->slot_count; i++) {
        if ((pool->used_bitmap & (((uint64_t)1) << i)) != 0u) {
            n++;
        }
    }
    return n;
}
