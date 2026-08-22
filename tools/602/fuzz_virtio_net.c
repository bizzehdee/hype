/*
 * #602: host-side libFuzzer harness over the virtio-net guest-facing trust boundary --
 * TX/RX descriptor chains (core/virtio_net_ring.c's hype_virtio_net_drain_tx() /
 * hype_virtio_net_deliver_rx()) and common-cfg register writes (devices/virtio_net.c),
 * the same real code core/tests/test_virtio_net.c and test_virtio_net_ring.c exercise
 * directedly.
 *
 * Same shape as the virtio-blk harness: every value the device sees is something a
 * config-space write or a guest-RAM DMA could have produced. queue_desc/driver/device
 * come from fuzzed QUEUE_DESC_LO/HI etc. writes, so most runs point the rings at GPAs
 * outside the mapped region -- exactly the case the ring walker's guest_dma_xlate()
 * calls must refuse rather than dereference.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "fuzz_common.h"
#include "../../devices/virtio_net.h"
#include "../../devices/virtio_blk.h" /* HYPE_VIRTIO_COMMON_CFG_* offsets, shared layout */
#include "../../core/guest_mem.h"
#include "../../core/virtio_net_ring.h"
#include "../../core/fatal.h" /* hype_debug_set_level -- see fuzz_ahci.c's note */

#define FUZZ_RAM_BYTES (32u * 1024u)
static uint8_t g_ram[FUZZ_RAM_BYTES];
#define FUZZ_RAM_GPA_BASE 0x20000000ull

#define MAX_CFG_WRITES 64u
#define MAX_ROUNDS 6u

static unsigned long long g_sink_bytes; /* touched so the sink is not optimized away */

static int tx_sink(void *user, const uint8_t *frame, unsigned int len) {
    unsigned int i;
    (void)user;
    for (i = 0; i < len; i++) {
        g_sink_bytes += frame[i];
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    hype_fuzz_cursor_t c;
    hype_virtio_net_t dev;
    hype_gpa_map_t map;
    hype_virtio_net_ring_stats_t stats;
    static uint8_t scratch[HYPE_VIRTIO_NET_MAX_FRAME_LEN];
    static uint8_t rx_frame[HYPE_VIRTIO_NET_MAX_FRAME_LEN];
    const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x60, 0x02, 0x00};
    unsigned round;

    hype_debug_set_level(HYPE_LOG_ERROR);
    hype_fuzz_cursor_init(&c, data, size);

    hype_gpa_map_reset(&map);
    hype_gpa_map_add(&map, FUZZ_RAM_GPA_BASE, (uint64_t)(uintptr_t)g_ram, FUZZ_RAM_BYTES);

    hype_virtio_net_reset(&dev, mac);
    hype_virtio_net_set_bus_master(&dev, 1);
    memset(&stats, 0, sizeof(stats));

    for (round = 0; round < MAX_ROUNDS && hype_fuzz_cursor_remaining(&c) > 0; round++) {
        unsigned n_cfg = (unsigned)hype_fuzz_u8(&c) % (MAX_CFG_WRITES + 1u);
        unsigned i;
        uint32_t out_value;
        unsigned rx_len;

        for (i = 0; i < n_cfg && hype_fuzz_cursor_remaining(&c) >= 9u; i++) {
            uint32_t offset = hype_fuzz_u32(&c);
            uint8_t size_bytes = hype_fuzz_u8(&c);
            uint32_t value = hype_fuzz_u32(&c);
            (void)hype_virtio_net_common_cfg_write(&dev, offset, size_bytes, value);
            (void)hype_virtio_net_common_cfg_read(&dev, offset, size_bytes, &out_value);
            (void)hype_virtio_net_device_cfg_read(&dev, offset, size_bytes, &out_value);
        }

        if (hype_fuzz_cursor_remaining(&c) > 0) {
            hype_virtio_net_set_bus_master(&dev, hype_fuzz_u8(&c) & 1);
        }

        hype_fuzz_fill(&c, g_ram, FUZZ_RAM_BYTES);

        (void)hype_virtio_net_drain_tx(&dev, &map, tx_sink, 0, scratch, sizeof(scratch), &stats);

        rx_len = (unsigned)hype_fuzz_u16(&c) % (HYPE_VIRTIO_NET_MAX_FRAME_LEN + 1u);
        hype_fuzz_fill(&c, rx_frame, sizeof(rx_frame));
        (void)hype_virtio_net_deliver_rx(&dev, &map, rx_frame, rx_len, &stats);

        (void)hype_virtio_net_isr_read(&dev);
        (void)hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_RX);
        (void)hype_virtio_net_is_queue_ready(&dev, HYPE_VIRTIO_NET_VQ_TX);
        (void)hype_virtio_net_hdr_len(&dev);
    }

    return 0;
}
