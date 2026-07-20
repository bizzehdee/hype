#include <stdio.h>
#include "../../devices/guest_lapic.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_reset_defaults(void) {
    hype_guest_lapic_t l;
    uint32_t v;
    hype_guest_lapic_reset(&l);
    CHECK_HEX("timer masked at reset", HYPE_GUEST_LAPIC_LVT_MASKED, l.lvt_timer);
    CHECK_HEX("no IRQ pending at reset", 0, l.timer_irq_pending);
    CHECK_HEX("nothing in service at reset", 0, l.timer_in_service);
    CHECK_HEX("ID reads 0", 0, (hype_guest_lapic_read(&l, HYPE_GUEST_LAPIC_REG_ID, 4, &v), v));
    CHECK_HEX("VERSION reads the synthesized value", HYPE_GUEST_LAPIC_VERSION_VALUE,
              (hype_guest_lapic_read(&l, HYPE_GUEST_LAPIC_REG_VERSION, 4, &v), v));
}

static void test_svr_read_write_roundtrip(void) {
    hype_guest_lapic_t l;
    uint32_t v;
    hype_guest_lapic_reset(&l);
    CHECK_HEX("SVR write ok", 0, hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_SVR, 4, 0x1FFu));
    hype_guest_lapic_read(&l, HYPE_GUEST_LAPIC_REG_SVR, 4, &v);
    CHECK_HEX("SVR reads back", 0x1FFu, v);
}

static void test_non_dword_access_rejected(void) {
    hype_guest_lapic_t l;
    uint32_t v;
    hype_guest_lapic_reset(&l);
    CHECK_HEX("byte read rejected", (unsigned long long)(long long)-1,
              (unsigned long long)(long long)hype_guest_lapic_read(&l, HYPE_GUEST_LAPIC_REG_SVR, 1, &v));
    CHECK_HEX("word write rejected", (unsigned long long)(long long)-1,
              (unsigned long long)(long long)hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_SVR, 2, 0));
}

static void test_read_only_regs_ignore_writes(void) {
    hype_guest_lapic_t l;
    uint32_t v;
    hype_guest_lapic_reset(&l);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_ID, 4, 0xDEAD);
    hype_guest_lapic_read(&l, HYPE_GUEST_LAPIC_REG_ID, 4, &v);
    CHECK_HEX("ID still 0 after write attempt", 0, v);
}

/* Arm a periodic timer (vector 32) and confirm it fires exactly once
 * per HYPE_GUEST_LAPIC_TICK_EXITS ticks, and that current_count moves. */
static void test_timer_fires_periodically(void) {
    hype_guest_lapic_t l;
    uint8_t vec = 0;
    unsigned int i;
    uint32_t first_count, later_count;

    hype_guest_lapic_reset(&l);
    /* vector 32, periodic, unmasked */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_LVT_TIMER, 4, 32u | HYPE_GUEST_LAPIC_LVT_PERIODIC);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT, 4, 1000000u);

    hype_guest_lapic_read(&l, HYPE_GUEST_LAPIC_REG_TIMER_CURRENT_COUNT, 4, &first_count);

    /* A few ticks into the period (not at the boundary, where a periodic
     * timer correctly reloads): current_count must have visibly moved. */
    for (i = 0; i < 3; i++) {
        hype_guest_lapic_tick(&l);
    }
    hype_guest_lapic_read(&l, HYPE_GUEST_LAPIC_REG_TIMER_CURRENT_COUNT, 4, &later_count);
    if (later_count >= first_count) {
        printf("FAIL: current_count did not decrease mid-period (%u -> %u)\n", first_count, later_count);
        failures++;
    }

    /* Continue to one tick shy of a full period: still no IRQ. */
    for (i = 3; i < HYPE_GUEST_LAPIC_TICK_EXITS - 1; i++) {
        hype_guest_lapic_tick(&l);
    }
    CHECK_HEX("no timer IRQ before a full period", 0, hype_guest_lapic_take_timer_irq(&l, &vec));

    /* The tick that completes the period fires it. */
    hype_guest_lapic_tick(&l);
    CHECK_HEX("timer IRQ fires at the period boundary", 1, hype_guest_lapic_take_timer_irq(&l, &vec));
    CHECK_HEX("delivered vector is the LVT vector", 32u, vec);
}

/* A second IRQ must not be taken until the guest EOIs the first. */
static void test_in_service_gates_next_irq(void) {
    hype_guest_lapic_t l;
    uint8_t vec = 0;
    unsigned int i;

    hype_guest_lapic_reset(&l);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_LVT_TIMER, 4, 32u | HYPE_GUEST_LAPIC_LVT_PERIODIC);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT, 4, 1000000u);

    for (i = 0; i < HYPE_GUEST_LAPIC_TICK_EXITS; i++) {
        hype_guest_lapic_tick(&l);
    }
    CHECK_HEX("first IRQ taken", 1, hype_guest_lapic_take_timer_irq(&l, &vec));

    /* Another full period elapses, but the first is still in service. */
    for (i = 0; i < HYPE_GUEST_LAPIC_TICK_EXITS; i++) {
        hype_guest_lapic_tick(&l);
    }
    CHECK_HEX("second IRQ blocked until EOI", 0, hype_guest_lapic_take_timer_irq(&l, &vec));

    /* EOI clears in-service; the pending one can now be taken. */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_EOI, 4, 0);
    for (i = 0; i < HYPE_GUEST_LAPIC_TICK_EXITS; i++) {
        hype_guest_lapic_tick(&l);
    }
    CHECK_HEX("IRQ flows again after EOI", 1, hype_guest_lapic_take_timer_irq(&l, &vec));
}

static void test_masked_timer_never_fires(void) {
    hype_guest_lapic_t l;
    uint8_t vec = 0;
    unsigned int i;
    hype_guest_lapic_reset(&l);
    /* armed (init_count set) but LVT masked */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_LVT_TIMER, 4, 32u | HYPE_GUEST_LAPIC_LVT_MASKED);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT, 4, 1000u);
    for (i = 0; i < HYPE_GUEST_LAPIC_TICK_EXITS * 4u; i++) {
        hype_guest_lapic_tick(&l);
    }
    CHECK_HEX("masked timer never raises an IRQ", 0, hype_guest_lapic_take_timer_irq(&l, &vec));
}

static void test_disarmed_timer_never_fires(void) {
    hype_guest_lapic_t l;
    uint8_t vec = 0;
    unsigned int i;
    hype_guest_lapic_reset(&l);
    /* unmasked vector, but init_count == 0 (disarmed) */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_LVT_TIMER, 4, 32u);
    for (i = 0; i < HYPE_GUEST_LAPIC_TICK_EXITS * 4u; i++) {
        hype_guest_lapic_tick(&l);
    }
    CHECK_HEX("disarmed timer never raises an IRQ", 0, hype_guest_lapic_take_timer_irq(&l, &vec));
}

/* Exercise every modeled register's read and write path, plus the
 * benign default (unmodeled offset) paths, for coverage of the full
 * dispatch. */
static void test_all_registers_roundtrip(void) {
    hype_guest_lapic_t l;
    uint32_t v;
    struct {
        uint32_t off;
        uint32_t val;
    } rw[] = {
        {HYPE_GUEST_LAPIC_REG_LDR, 0x01000000u},
        {HYPE_GUEST_LAPIC_REG_DFR, 0xF0000000u},
        {HYPE_GUEST_LAPIC_REG_LVT_TIMER, 0x00020020u},
        {HYPE_GUEST_LAPIC_REG_LVT_LINT0, 0x00000700u},
        {HYPE_GUEST_LAPIC_REG_LVT_LINT1, 0x00000400u},
        {HYPE_GUEST_LAPIC_REG_TIMER_DIVIDE_CONFIG, 0x0000000Bu},
        {HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT, 0x00123456u},
    };
    unsigned int i;

    hype_guest_lapic_reset(&l);
    for (i = 0; i < sizeof(rw) / sizeof(rw[0]); i++) {
        CHECK_HEX("rw write ok", 0, hype_guest_lapic_write(&l, rw[i].off, 4, rw[i].val));
        hype_guest_lapic_read(&l, rw[i].off, 4, &v);
        CHECK_HEX("rw reads back", rw[i].val, v);
    }

    /* EOI reads back as 0 (write-only), and an unmodeled offset in the
     * window reads 0 and absorbs writes. */
    v = 0xFFFFFFFFu;
    hype_guest_lapic_read(&l, HYPE_GUEST_LAPIC_REG_EOI, 4, &v);
    CHECK_HEX("EOI reads 0", 0, v);
    v = 0xFFFFFFFFu;
    CHECK_HEX("unmodeled offset write absorbed", 0, hype_guest_lapic_write(&l, 0x040u, 4, 0xDEADBEEFu));
    hype_guest_lapic_read(&l, 0x040u, 4, &v);
    CHECK_HEX("unmodeled offset reads 0", 0, v);
    /* VERSION write is ignored (read-only). */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_VERSION, 4, 0x1234u);
    hype_guest_lapic_read(&l, HYPE_GUEST_LAPIC_REG_VERSION, 4, &v);
    CHECK_HEX("VERSION unchanged after write attempt", HYPE_GUEST_LAPIC_VERSION_VALUE, v);
}

/* M4-6b1: real-time-proportional advance-by-N. */
static void test_advance_periodic_fires_at_count(void) {
    hype_guest_lapic_t l;
    uint8_t vec = 0;
    hype_guest_lapic_reset(&l);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_LVT_TIMER, 4, 32u | HYPE_GUEST_LAPIC_LVT_PERIODIC);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_DIVIDE_CONFIG, 4, 0xBu); /* divide-by-1 */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT, 4, 1000u);

    hype_guest_lapic_advance(&l, 400);
    CHECK_HEX("current_count decremented by N", 600, l.current_count);
    CHECK_HEX("no IRQ before terminal count", 0, hype_guest_lapic_take_timer_irq(&l, &vec));

    hype_guest_lapic_advance(&l, 700); /* crosses 0 (600 -> terminal, +100 over) */
    CHECK_HEX("periodic IRQ fires at terminal count", 1, hype_guest_lapic_take_timer_irq(&l, &vec));
    CHECK_HEX("fires at the programmed vector", 32u, vec);
    CHECK_HEX("periodic reloaded near init_count (phase carried)", 900, l.current_count);
}

static void test_advance_one_shot_fires_once(void) {
    hype_guest_lapic_t l;
    uint8_t vec = 0;
    hype_guest_lapic_reset(&l);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_LVT_TIMER, 4, 48u); /* one-shot (no PERIODIC bit) */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_DIVIDE_CONFIG, 4, 0xBu); /* divide-by-1 */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT, 4, 500u);

    hype_guest_lapic_advance(&l, 10000); /* far past terminal */
    CHECK_HEX("one-shot IRQ fires", 1, hype_guest_lapic_take_timer_irq(&l, &vec));
    CHECK_HEX("one-shot count stays at 0", 0, l.current_count);
    /* After EOI, a one-shot at terminal does not re-fire. */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_EOI, 4, 0);
    hype_guest_lapic_advance(&l, 10000);
    CHECK_HEX("one-shot does not re-fire", 0, hype_guest_lapic_take_timer_irq(&l, &vec));
}

/* M4-6d4: the LAPIC one-shot CYCLE. When a tickless Linux uses the LAPIC
 * timer as its clockevent, it re-arms after every expiry: take the IRQ,
 * EOI, write a fresh init_count, take the next IRQ, repeat. QEMU runs the
 * FW-1 guest with the LAPIC timer MASKED (it uses the periodic PIT), so
 * this cycle is never exercised there -- a re-arm bug would only surface on
 * real hardware. Drive several consecutive cycles and confirm each re-arm
 * fires exactly one fresh IRQ. */
static void test_advance_one_shot_rearm_refires(void) {
    hype_guest_lapic_t l;
    uint8_t vec = 0;
    int cycle;
    hype_guest_lapic_reset(&l);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_LVT_TIMER, 4, 48u); /* one-shot */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_DIVIDE_CONFIG, 4, 0xBu); /* divide-by-1 */

    for (cycle = 0; cycle < 4; cycle++) {
        /* Re-arm by writing a fresh initial count, even from a spent
         * (current_count == 0) state. */
        hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT, 4, 500u);
        CHECK_HEX("re-arm reloads current_count", 500, l.current_count);

        hype_guest_lapic_advance(&l, 200);
        CHECK_HEX("no IRQ before terminal this cycle", 0, hype_guest_lapic_take_timer_irq(&l, &vec));
        hype_guest_lapic_advance(&l, 400); /* crosses terminal (200+400 > 500) */
        CHECK_HEX("re-armed one-shot fires", 1, hype_guest_lapic_take_timer_irq(&l, &vec));
        CHECK_HEX("count spent again", 0, l.current_count);

        /* EOI so the next cycle's IRQ isn't gated by in-service. */
        hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_EOI, 4, 0);
        /* Still spent until the next re-arm: no phantom IRQ. */
        hype_guest_lapic_advance(&l, 10000);
        CHECK_HEX("spent between cycles, no re-fire", 0, hype_guest_lapic_take_timer_irq(&l, &vec));
    }
}

static void test_advance_masked_or_disarmed_never_fires(void) {
    hype_guest_lapic_t l;
    uint8_t vec = 0;
    hype_guest_lapic_reset(&l);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_LVT_TIMER, 4, 32u | HYPE_GUEST_LAPIC_LVT_MASKED);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT, 4, 100u);
    hype_guest_lapic_advance(&l, 100000);
    CHECK_HEX("masked timer never fires under advance", 0, hype_guest_lapic_take_timer_irq(&l, &vec));

    hype_guest_lapic_reset(&l);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_LVT_TIMER, 4, 32u); /* armed but init_count 0 */
    hype_guest_lapic_advance(&l, 100000);
    CHECK_HEX("disarmed timer never fires under advance", 0, hype_guest_lapic_take_timer_irq(&l, &vec));
}

/* Real hardware: a MASKED but armed timer's count register STILL decrements
 * (and a periodic one still reloads) -- the mask only suppresses the IRQ. Linux
 * calibrates the LAPIC timer this way (mask the LVT, read current_count to
 * measure the rate), so the counter must move or an ACPI-mode guest hangs. */
static void test_advance_masked_timer_still_counts(void) {
    hype_guest_lapic_t l;
    uint8_t vec = 0;
    hype_guest_lapic_reset(&l);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_LVT_TIMER, 4,
                           32u | HYPE_GUEST_LAPIC_LVT_MASKED); /* masked, one-shot */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_DIVIDE_CONFIG, 4, 0xBu); /* divide-by-1 */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT, 4, 1000u);

    hype_guest_lapic_advance(&l, 400);
    CHECK_HEX("masked timer's counter still decrements", 600, l.current_count);
    CHECK_HEX("but raises no IRQ while masked", 0, hype_guest_lapic_take_timer_irq(&l, &vec));

    /* A masked PERIODIC timer still reloads on expiry (counter keeps moving). */
    hype_guest_lapic_reset(&l);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_LVT_TIMER, 4,
                           32u | HYPE_GUEST_LAPIC_LVT_MASKED | HYPE_GUEST_LAPIC_LVT_PERIODIC);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_DIVIDE_CONFIG, 4, 0xBu);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT, 4, 500u);
    hype_guest_lapic_advance(&l, 700); /* crosses terminal: 500 -> reload, 200 over */
    CHECK_HEX("masked periodic still reloads", 300, l.current_count);
    CHECK_HEX("masked periodic raises no IRQ", 0, hype_guest_lapic_take_timer_irq(&l, &vec));
}

/* M4-6b5: the Divide Configuration Register decode (SDM bits [3,1,0]). */
static void test_divisor_decode(void) {
    CHECK_HEX("DCR 0x0 -> div 2", 2u, hype_guest_lapic_divisor(0x0u));
    CHECK_HEX("DCR 0x1 -> div 4", 4u, hype_guest_lapic_divisor(0x1u));
    CHECK_HEX("DCR 0x2 -> div 8", 8u, hype_guest_lapic_divisor(0x2u));
    CHECK_HEX("DCR 0x3 -> div 16", 16u, hype_guest_lapic_divisor(0x3u));
    CHECK_HEX("DCR 0x8 -> div 32", 32u, hype_guest_lapic_divisor(0x8u));
    CHECK_HEX("DCR 0x9 -> div 64", 64u, hype_guest_lapic_divisor(0x9u));
    CHECK_HEX("DCR 0xA -> div 128", 128u, hype_guest_lapic_divisor(0xAu));
    CHECK_HEX("DCR 0xB -> div 1", 1u, hype_guest_lapic_divisor(0xBu));
    /* bit 2 is reserved and must not affect the decode */
    CHECK_HEX("DCR 0xF (bit2 set) -> div 1", 1u, hype_guest_lapic_divisor(0xFu));
}

/* M4-6b5: advance honours the divisor -- divide-by-16 needs 16x the base-rate
 * ticks to reach terminal count, and the fractional remainder carries. */
static void test_advance_honours_divide(void) {
    hype_guest_lapic_t l;
    uint8_t vec = 0;
    hype_guest_lapic_reset(&l);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_LVT_TIMER, 4, 32u);            /* one-shot */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_DIVIDE_CONFIG, 4, 0x3u); /* divide-by-16 */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT, 4, 100u);

    hype_guest_lapic_advance(&l, 1600); /* 1600 base / 16 = 100 counts -> exactly terminal */
    CHECK_HEX("divide-by-16: 1600 base ticks = 100 counts -> fires", 1,
              hype_guest_lapic_take_timer_irq(&l, &vec));

    /* Remainder carry: 8 base ticks/call * 16 calls = 128 base = 8 counts. */
    hype_guest_lapic_reset(&l);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_LVT_TIMER, 4, 32u);
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_DIVIDE_CONFIG, 4, 0x3u); /* div-16 */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT, 4, 100u);
    {
        int i;
        for (i = 0; i < 16; i++) {
            hype_guest_lapic_advance(&l, 8); /* 8/16 = 0 each call, but remainder accrues */
        }
    }
    CHECK_HEX("fractional remainder carries across calls (16x8=128 base=8 counts)", 92,
              l.current_count);
}

int main(void) {
    test_divisor_decode();
    test_advance_honours_divide();
    test_reset_defaults();
    test_all_registers_roundtrip();
    test_svr_read_write_roundtrip();
    test_non_dword_access_rejected();
    test_read_only_regs_ignore_writes();
    test_timer_fires_periodically();
    test_in_service_gates_next_irq();
    test_masked_timer_never_fires();
    test_disarmed_timer_never_fires();
    test_advance_periodic_fires_at_count();
    test_advance_one_shot_rearm_refires();
    test_advance_one_shot_fires_once();
    test_advance_masked_or_disarmed_never_fires();
    test_advance_masked_timer_still_counts();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
