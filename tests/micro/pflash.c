/*
 * #544: M4-3's pflash MMIO test, ported out of boot/main.c and aimed at the REAL flash window.
 *
 * The in-binary test put a private pflash at a private 3 GB address, with its own backing array and
 * its own NPT entry marked not-present. The real device model puts the OVMF flash window at
 * [4 GB - combined_size, 4 GB) and, since #457, maps it READ-ONLY in the nested tables so guest
 * writes fault into the CFI model instead of reaching RAM -- with a VARS region at the bottom and
 * the CODE image above it.
 *
 * So this port tests something the in-binary version could not: that #457's read-only mapping and
 * the CFI model agree, on the window a real guest actually sees.
 *
 * IT DISCOVERS THE WINDOW RATHER THAN BEING TOLD. The guest cannot know combined_size, and passing
 * it on the command line would need updating whenever the vendored firmware changes size -- a wrong
 * value would fault instead of failing cleanly. Instead the guest does what firmware does: the
 * window always ENDS at 4 GB, and the VARS region begins with an EFI firmware volume whose header
 * carries the "_FVH" signature at offset 40, so probing 2 MB-aligned addresses downward from 4 GB
 * finds the base. Reads need no write first -- the window is directly mapped read-only until a write
 * engages trap mode -- so the probe itself costs nothing.
 *
 * WHY WRITING HERE IS SAFE. This test programs bytes into its own VM's VARS region, i.e. its
 * varstore. That is disposable by construction: a `boot = kernel` VM runs no firmware, so nothing
 * ever reads that varstore back, and section 6i's hard invariant (enforced since #531) guarantees no
 * other VM shares it. It writes well inside the region rather than over the volume header, so a
 * human inspecting the file afterwards still sees a recognisable FV.
 */
/*
 * STATUS: THIS TEST FAILS TODAY, and the failure is the point -- see #556.
 *
 * A guest CFI read after a command returns array data rather than the model's synthesized response:
 * READ_STATUS gives 0x00 where devices/pflash.h makes 0x80 unconditional. hype's own counters show
 * the writes arriving (vars_writes=4, trap_flips=5), so only the read side is wrong, and #457's trap
 * appears to disengage before the guest's read executes.
 *
 * It is excluded from run-micro.sh's default list (MICRO_EXCLUDE_DEFAULT) so a default run stays a
 * signal, and it is committed rather than held back because the test IS the reproduction. Run it
 * with `tools/micro/run-micro.sh pflash`.
 */
#include "micro.h"

#define NAME "pflash"

/* devices/pflash.h's command set. */
#define PFLASH_CMD_WRITE_BYTE 0x10u
#define PFLASH_CMD_READ_STATUS 0x70u
#define PFLASH_CMD_READ_DEVID 0x90u
#define PFLASH_CMD_READ_ARRAY 0xFFu
#define PFLASH_STATUS_READY 0x80u

#define FOUR_GB 0x100000000ull
#define STEP (2ull * 1024ull * 1024ull)
#define MAX_PROBE (64ull * 1024ull * 1024ull) /* generous: the vendored pair is 4 MB */
#define FVH_OFFSET 40u
/* Where to program. Inside VARS (which is 528 KB for the vendored VARS.fd) and clear of the volume
 * header, so the file stays recognisable to a human afterwards. */
#define TEST_OFFSET 0x40000ull

static inline void mmio_write8(uint64_t gpa, uint8_t v) {
    /* Register-form MOV only: hype_mmio_decode() supports 0x88/0x89/0x8A/0x8B/0F B6/0F B7 and not
     * the immediate-to-memory forms, so an `imm8 -> [mem]` store would fault as undecodable. Every
     * hand-written payload in this family follows the same rule. */
    __asm__ volatile("movb %0, (%1)" : : "q"(v), "r"((volatile uint8_t *)(uintptr_t)gpa) : "memory");
}

static inline uint8_t mmio_read8(uint64_t gpa) {
    uint8_t v;
    __asm__ volatile("movb (%1), %0"
                     : "=q"(v)
                     : "r"((const volatile uint8_t *)(uintptr_t)gpa)
                     : "memory");
    return v;
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    uint64_t base = 0ull;
    uint64_t probe;
    uint8_t st, got, got2;

    (void)zero_page_gpa;
    micro_puts("\n");

    /* Probe downward from 4 GB for the firmware volume that starts the VARS region. */
    for (probe = FOUR_GB - STEP; probe >= FOUR_GB - MAX_PROBE; probe -= STEP) {
        if (mmio_read8(probe + FVH_OFFSET + 0u) == (uint8_t)'_' &&
            mmio_read8(probe + FVH_OFFSET + 1u) == (uint8_t)'F' &&
            mmio_read8(probe + FVH_OFFSET + 2u) == (uint8_t)'V' &&
            mmio_read8(probe + FVH_OFFSET + 3u) == (uint8_t)'H') {
            base = probe; /* keep going -- the LOWEST match is the window base */
        }
    }

    if (base == 0ull) {
        micro_fail(NAME, "no EFI firmware volume found below 4 GB -- the flash window is not mapped, "
                         "or its VARS region does not start with an FV header");
        micro_halt();
    }

    micro_puts("micro/" NAME ": flash window base ");
    micro_put_hex(base);
    micro_puts(" (found by its _FVH signature; window ends at 4 GB, so combined_size is ");
    micro_put_uint((FOUR_GB - base) / (1024ull * 1024ull));
    micro_puts(" MiB)\n");

    /*
     * A direct read first, with no write before it. The window is mapped read-only until a write
     * engages trap mode, so this must succeed WITHOUT faulting -- and it proves the mapping is
     * present rather than merely trapped, which is the half of #457 a write-only test cannot see.
     */
    got = mmio_read8(base + FVH_OFFSET);
    if (got != (uint8_t)'_') {
        micro_fail(NAME, "a direct read of the flash window did not return the byte the probe just "
                         "saw -- the read-only mapping is not stable");
        micro_halt();
    }

    /* Device ID mode, then back to array mode. This is a MODE TRANSITION test: the model must answer
     * differently after 0x90 than after 0xFF at the same address. */
    mmio_write8(base + TEST_OFFSET, PFLASH_CMD_READ_DEVID);
    got = mmio_read8(base + TEST_OFFSET);
    mmio_write8(base + TEST_OFFSET, PFLASH_CMD_READ_ARRAY);
    got2 = mmio_read8(base + TEST_OFFSET);
    micro_puts("micro/" NAME ": devid-mode read ");
    micro_put_hex(got);
    micro_puts(", array-mode read ");
    micro_put_hex(got2);
    micro_puts("\n");
    if (got == got2) {
        micro_fail(NAME, "READ_DEVID and READ_ARRAY returned the same byte -- the CFI model is not "
                         "changing mode, so a guest cannot tell a device ID from array data");
        micro_halt();
    }

    /* Status register: the model never reports busy, so READY must be set. */
    mmio_write8(base + TEST_OFFSET, PFLASH_CMD_READ_STATUS);
    st = mmio_read8(base + TEST_OFFSET);
    mmio_write8(base + TEST_OFFSET, PFLASH_CMD_READ_ARRAY);
    micro_puts("micro/" NAME ": status ");
    micro_put_hex(st);
    micro_puts("\n");
    if ((st & PFLASH_STATUS_READY) == 0u) {
        micro_fail(NAME, "the CFI status register does not report the write state machine ready");
        micro_halt();
    }

    /*
     * The round trip the original tested: WRITE_BYTE then the data, read it back, then write what
     * was read to a second offset and read THAT back. Doing both halves in the guest means a read
     * that silently returned a stale value cannot pass -- the second write carries the value the
     * first read produced.
     */
    mmio_write8(base + TEST_OFFSET, PFLASH_CMD_WRITE_BYTE);
    mmio_write8(base + TEST_OFFSET, 0xABu);
    got = mmio_read8(base + TEST_OFFSET);
    micro_puts("micro/" NAME ": wrote 0xab, read back ");
    micro_put_hex(got);
    micro_puts("\n");
    if (got != 0xABu) {
        micro_fail(NAME, "the byte programmed through the CFI model did not read back");
        micro_halt();
    }

    mmio_write8(base + TEST_OFFSET + 0x100ull, PFLASH_CMD_WRITE_BYTE);
    mmio_write8(base + TEST_OFFSET + 0x100ull, got);
    got2 = mmio_read8(base + TEST_OFFSET + 0x100ull);
    micro_puts("micro/" NAME ": second offset carries ");
    micro_put_hex(got2);
    micro_puts("\n");
    if (got2 != 0xABu) {
        micro_fail(NAME, "the value read back from the first offset did not survive a second write "
                         "-- so the read delivered something other than what it appeared to");
        micro_halt();
    }

    micro_pass(NAME);
    micro_halt();
}
