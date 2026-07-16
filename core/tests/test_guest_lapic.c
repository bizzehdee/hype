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
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT, 4, 500u);

    hype_guest_lapic_advance(&l, 10000); /* far past terminal */
    CHECK_HEX("one-shot IRQ fires", 1, hype_guest_lapic_take_timer_irq(&l, &vec));
    CHECK_HEX("one-shot count stays at 0", 0, l.current_count);
    /* After EOI, a one-shot at terminal does not re-fire. */
    hype_guest_lapic_write(&l, HYPE_GUEST_LAPIC_REG_EOI, 4, 0);
    hype_guest_lapic_advance(&l, 10000);
    CHECK_HEX("one-shot does not re-fire", 0, hype_guest_lapic_take_timer_irq(&l, &vec));
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

int main(void) {
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
    test_advance_one_shot_fires_once();
    test_advance_masked_or_disarmed_never_fires();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
