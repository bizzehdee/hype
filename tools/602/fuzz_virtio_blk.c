/*
 * #602: host-side libFuzzer harness over the virtio-blk guest-facing trust boundary --
 * descriptor chains and common-cfg register writes -- driving the REAL device model
 * (devices/virtio_blk.c's config-space accessors and process_virtio_blk_queue(),
 * arch/x86_64/svm/svm_vcpu.c) exactly as core/tests/test_virtio_blk.c does, just with
 * adversarial rather than directed input.
 *
 * Design: everything the harness feeds the device is something a guest driver could
 * itself have written -- config-space registers via hype_virtio_blk_common_cfg_write()
 * (offset/size/value straight from the fuzz input, "config-space writes in random
 * order" per the ticket), and guest RAM contents via a fixed hype_gpa_map_t region a
 * guest could DMA into. queue_desc/driver/device are therefore whatever value a fuzzed
 * QUEUE_DESC_LO/HI etc. write produced -- some in-map, most not, which is exactly the
 * "GPAs outside the map" case the ticket asks for: process_virtio_blk_queue() must
 * refuse those through guest_dma_xlate(), never dereference them.
 *
 * No test in core/tests/test_virtio_blk.c drives MULTIPLE kicks with registers mutated
 * between them from one fuzzer-controlled byte stream, so this also gets a slice of
 * state-machine depth (reset mid-stream, device_status transitions, queue resize
 * between kicks) that the directed suite does not attempt.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "fuzz_common.h"
#include "../../devices/virtio_blk.h"
#include "../../core/guest_mem.h"
#include "../../core/blk_backend.h"
#include "../../core/fatal.h" /* hype_debug_set_level */

/* Guest RAM this device's descriptor/avail/used/data pointers may land in. Large
 * enough to hold a real virtqueue (256 entries: 4096B desc table + ~2.5KB rings)
 * plus scattered data buffers, small enough that refilling it every round does not
 * dominate the fuzzer's exec/sec. */
#define FUZZ_RAM_BYTES (32u * 1024u)
static uint8_t g_ram[FUZZ_RAM_BYTES];
#define FUZZ_RAM_GPA_BASE 0x10000000ull

/* Backing disk for the block backend: fixed size, read/write into a static buffer --
 * same shape as core/tests/test_virtio_blk.c's own backend, just smaller. */
#define FUZZ_DISK_SECTORS 128u
static uint8_t g_disk[FUZZ_DISK_SECTORS * HYPE_VIRTIO_BLK_SECTOR_SIZE];

static int disk_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    if (lba + count > FUZZ_DISK_SECTORS || lba + count < lba) return -1;
    memcpy(buf, g_disk + lba * HYPE_VIRTIO_BLK_SECTOR_SIZE,
           (size_t)count * HYPE_VIRTIO_BLK_SECTOR_SIZE);
    return 0;
}

static int disk_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    (void)ctx;
    if (lba + count > FUZZ_DISK_SECTORS || lba + count < lba) return -1;
    memcpy(g_disk + lba * HYPE_VIRTIO_BLK_SECTOR_SIZE, buf,
           (size_t)count * HYPE_VIRTIO_BLK_SECTOR_SIZE);
    return 0;
}

static int disk_writev(void *ctx, uint64_t lba, const hype_blk_seg_t *segs, uint32_t nsegs) {
    uint64_t cur = lba;
    uint32_t i;
    (void)ctx;
    for (i = 0; i < nsegs; i++) {
        if (disk_write(ctx, cur, segs[i].count, segs[i].buf) != 0) return -1;
        cur += segs[i].count;
    }
    return 0;
}

/* Silence: hype_debug_print reaches a real UART through port I/O, which faults in a
 * user process (same reasoning as core/tests/test_virtio_blk.c's own reject sink). */
static void reject_sink(const char *why) {
    (void)why;
}

/* Bounded rather than unbounded: a malformed input must not turn one fuzz iteration
 * into an unbounded loop (libFuzzer treats a hang the same as a crash, but there is no
 * reason to manufacture one -- the interesting bugs here are memory-safety bugs, not
 * "the harness itself spins"). */
#define MAX_CFG_WRITES 64u
#define MAX_ROUNDS 6u

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    hype_fuzz_cursor_t c;
    hype_virtio_blk_t dev;
    hype_gpa_map_t map;
    hype_blk_backend_t be;
    unsigned round;
    static int inited;

    if (!inited) {
        hype_virtio_blk_set_reject_sink(reject_sink);
        /* Belt-and-braces alongside the reject sink: the bus-master-disabled path logs via
         * hype_debug_print() directly (no sink), which at the default level reaches a real
         * `inb`/`outb` and SIGSEGVs in a host process (see fuzz_ahci.c's fuller note). */
        hype_debug_set_level(HYPE_LOG_ERROR);
        inited = 1;
    }

    hype_fuzz_cursor_init(&c, data, size);

    hype_gpa_map_reset(&map);
    hype_gpa_map_add(&map, FUZZ_RAM_GPA_BASE, (uint64_t)(uintptr_t)g_ram, FUZZ_RAM_BYTES);

    memset(&be, 0, sizeof(be));
    be.read = disk_read;
    be.write = disk_write;
    be.writev = disk_writev;
    be.ctx = 0;
    be.total_sectors = FUZZ_DISK_SECTORS;

    hype_virtio_blk_reset(&dev, FUZZ_DISK_SECTORS);
    hype_virtio_blk_set_bus_master(&dev, 1);

    for (round = 0; round < MAX_ROUNDS && hype_fuzz_cursor_remaining(&c) > 0; round++) {
        unsigned n_cfg = (unsigned)hype_fuzz_u8(&c) % (MAX_CFG_WRITES + 1u);
        unsigned i;
        uint32_t out_value;

        /* Config-space register writes in random order (ticket item 1), covering
         * device_status transitions, queue_select, queue_size, and the desc/driver/
         * device GPA halves -- every write a virtio-blk driver could issue. */
        for (i = 0; i < n_cfg && hype_fuzz_cursor_remaining(&c) >= 9u; i++) {
            uint32_t offset = hype_fuzz_u32(&c);
            uint8_t size_bytes = hype_fuzz_u8(&c);
            uint32_t value = hype_fuzz_u32(&c);
            (void)hype_virtio_blk_common_cfg_write(&dev, offset, size_bytes, value);
            /* Reads share the same offset/width validation and must be equally
             * crash-proof against a register a write never touched. */
            (void)hype_virtio_blk_common_cfg_read(&dev, offset, size_bytes, &out_value);
            (void)hype_virtio_blk_device_cfg_read(&dev, offset, size_bytes, &out_value);
        }

        /* Occasionally flip bus mastering, mirroring a guest toggling PCI Command. */
        if (hype_fuzz_cursor_remaining(&c) > 0) {
            hype_virtio_blk_set_bus_master(&dev, hype_fuzz_u8(&c) & 1);
        }

        /* Everything downstream of here -- the descriptor table, avail ring, used
         * ring, header, data segments, status byte -- lives in guest RAM the guest
         * itself controls, so it is exactly where adversarial byte shapes belong:
         * random flags/next cycles, ragged segment lengths, cross-region overlaps. */
        hype_fuzz_fill(&c, g_ram, FUZZ_RAM_BYTES);

        (void)process_virtio_blk_queue(&dev, &be, &map);
        (void)hype_virtio_blk_isr_read(&dev);
        (void)hype_virtio_blk_is_queue_ready(&dev);
    }

    return 0;
}
