/*
 * #602: host-side libFuzzer harness over the NVMe front-end -- controller-register
 * writes and submission-queue-entry processing (devices/nvme.c's hype_nvme_process_sq(),
 * which internally decodes SQEs, walks PRP lists, and executes I/O against a block
 * backend), the same real code core/tests/test_nvme.c exercises directedly.
 *
 * hype_nvme_process_sq() is fully pure (no vcpu/CPU dependency, injected guest-memory
 * callbacks) so unlike the AHCI command processor it needs no coverage-exemption
 * workaround -- it is real, counted coverage.
 *
 * Register writes go through hype_nvme_mmio_write32() at fuzzed offsets across the
 * whole BAR0 window, which is where CC.EN, ASQ/ACQ and AQA legitimately get set --
 * and also where a random offset/size mismatch could reproduce the #305/#306 class of
 * decode panic the ticket calls out for MMIO/PIO models generally.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "fuzz_common.h"
#include "../../devices/nvme.h"
#include "../../core/blk_backend.h"
#include "../../core/fatal.h" /* hype_debug_set_level -- see fuzz_ahci.c's note */

/* Guest RAM window the SQ/CQ, PRP lists and I/O data all live in. */
#define FUZZ_GRAM_BYTES (64u * 1024u)
static uint8_t g_gram[FUZZ_GRAM_BYTES];
#define GRAM_BASE 0x30000000ull

#define FUZZ_DISK_SECTORS 128u
static uint8_t g_disk[FUZZ_DISK_SECTORS * 512u];

static int disk_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    if (lba + count > FUZZ_DISK_SECTORS || lba + count < lba) return -1;
    memcpy(buf, g_disk + lba * 512u, (size_t)count * 512u);
    return 0;
}

static int disk_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    (void)ctx;
    if (lba + count > FUZZ_DISK_SECTORS || lba + count < lba) return -1;
    memcpy(g_disk + lba * 512u, buf, (size_t)count * 512u);
    return 0;
}

static int gram_read(void *ctx, uint64_t gpa, uint32_t len, void *dst) {
    (void)ctx;
    if (gpa < GRAM_BASE || len > FUZZ_GRAM_BYTES || gpa - GRAM_BASE > FUZZ_GRAM_BYTES - len) {
        return -1;
    }
    memcpy(dst, g_gram + (gpa - GRAM_BASE), len);
    return 0;
}

static int gram_write(void *ctx, uint64_t gpa, uint32_t len, const void *src) {
    (void)ctx;
    if (gpa < GRAM_BASE || len > FUZZ_GRAM_BYTES || gpa - GRAM_BASE > FUZZ_GRAM_BYTES - len) {
        return -1;
    }
    memcpy(g_gram + (gpa - GRAM_BASE), src, len);
    return 0;
}

#define MAX_REG_WRITES 48u
#define MAX_ROUNDS 6u

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    hype_fuzz_cursor_t c;
    hype_nvme_t dev;
    hype_blk_backend_t be;
    hype_nvme_ctx_t ctx;
    static uint8_t bounce[8192];
    unsigned round;

    hype_debug_set_level(HYPE_LOG_ERROR);
    hype_fuzz_cursor_init(&c, data, size);

    hype_nvme_reset(&dev);
    hype_nvme_set_bus_master(&dev, 1);

    memset(&be, 0, sizeof(be));
    be.read = disk_read;
    be.write = disk_write;
    be.ctx = 0;
    be.total_sectors = FUZZ_DISK_SECTORS;

    memset(&ctx, 0, sizeof(ctx));
    ctx.be = &be;
    ctx.total_sectors = FUZZ_DISK_SECTORS;
    ctx.page_size = 4096u;
    ctx.gread = gram_read;
    ctx.gwrite = gram_write;
    ctx.gctx = 0;
    ctx.bounce = bounce;
    ctx.bounce_len = (uint32_t)sizeof(bounce);
    ctx.serial = "FUZZ0000000000000001";

    for (round = 0; round < MAX_ROUNDS && hype_fuzz_cursor_remaining(&c) > 0; round++) {
        unsigned n_regs = (unsigned)hype_fuzz_u8(&c) % (MAX_REG_WRITES + 1u);
        unsigned i;
        unsigned int qid;

        /* Controller register writes at fuzzed offsets -- CC.EN, ASQ/ACQ, AQA and
         * doorbells are all reachable this way, same as a real driver's MMIO BAR0
         * accesses. */
        for (i = 0; i < n_regs && hype_fuzz_cursor_remaining(&c) >= 8u; i++) {
            uint32_t off = hype_fuzz_u32(&c);
            uint32_t val = hype_fuzz_u32(&c);
            hype_nvme_mmio_write32(&dev, off, val);
            (void)hype_nvme_mmio_read32(&dev, off);
        }

        if (hype_fuzz_cursor_remaining(&c) > 0) {
            hype_nvme_set_bus_master(&dev, hype_fuzz_u8(&c) & 1);
        }

        /* Everything the queue processor reads -- SQEs, PRP lists, write payloads --
         * lives in this guest-RAM window, filled with whatever bytes remain. */
        hype_fuzz_fill(&c, g_gram, FUZZ_GRAM_BYTES);

        qid = (unsigned int)hype_fuzz_u8(&c) % HYPE_NVME_MAX_QUEUES;
        (void)hype_nvme_process_sq(&dev, qid, &ctx);
        (void)hype_nvme_irq_pending(&dev);
    }

    return 0;
}
