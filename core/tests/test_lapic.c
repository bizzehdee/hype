#include <stdio.h>
#include "../../arch/x86_64/cpu/lapic.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* Room for the LVT Timer register (offset 0x320) plus slack. */
static uint32_t g_fake_lapic[0x400 / 4];

static uint32_t *lvt_timer(void) {
    return (uint32_t *)((unsigned char *)g_fake_lapic + HYPE_LAPIC_LVT_TIMER_OFFSET);
}

static void test_mask_timer_sets_only_the_mask_bit(void) {
    *lvt_timer() = 0x000000EC; /* vector 0xEC, some mode bits, unmasked */

    hype_lapic_mask_timer(g_fake_lapic);

    CHECK_HEX("mask bit set", HYPE_LAPIC_LVT_MASKED, *lvt_timer() & HYPE_LAPIC_LVT_MASKED);
    CHECK_HEX("other bits (vector/mode) preserved", 0xEC, *lvt_timer() & 0xFFu);
}

static void test_mask_timer_idempotent(void) {
    *lvt_timer() = HYPE_LAPIC_LVT_MASKED | 0x30;

    hype_lapic_mask_timer(g_fake_lapic);

    CHECK_HEX("already-masked register stays masked", HYPE_LAPIC_LVT_MASKED,
              *lvt_timer() & HYPE_LAPIC_LVT_MASKED);
    CHECK_HEX("other bits untouched when already masked", 0x30, *lvt_timer() & 0xFFu);
}

static void test_mask_timer_does_not_touch_neighboring_registers(void) {
    uint32_t *before = (uint32_t *)((unsigned char *)g_fake_lapic + HYPE_LAPIC_LVT_TIMER_OFFSET - 4);
    uint32_t *after = (uint32_t *)((unsigned char *)g_fake_lapic + HYPE_LAPIC_LVT_TIMER_OFFSET + 4);

    *before = 0xAAAAAAAAu;
    *after = 0x55555555u;
    *lvt_timer() = 0;

    hype_lapic_mask_timer(g_fake_lapic);

    CHECK_HEX("register before LVT Timer untouched", 0xAAAAAAAAu, *before);
    CHECK_HEX("register after LVT Timer untouched", 0x55555555u, *after);
}

/* M8-0b: ICR IPI encoders + send. */

static void test_icr_high_places_apic_id_in_top_byte(void) {
    CHECK_HEX("apic id 0 -> 0", 0x00000000u, hype_lapic_icr_high(0));
    CHECK_HEX("apic id 1 -> bit 24", 0x01000000u, hype_lapic_icr_high(1));
    CHECK_HEX("apic id 0xFF -> top byte", 0xFF000000u, hype_lapic_icr_high(0xFF));
}

static void test_icr_low_init_is_canonical_value(void) {
    /* INIT, assert, edge -> delivery mode 0b101 (0x500) | level assert (0x4000). */
    CHECK_HEX("INIT-assert ICR low", 0x00004500u, hype_lapic_icr_low_init());
}

static void test_icr_low_sipi_encodes_vector_and_startup_mode(void) {
    /* Startup mode 0b110 (0x600) | assert (0x4000) | trampoline page in low 8. */
    CHECK_HEX("SIPI page 0x08", 0x00004608u, hype_lapic_icr_low_sipi(0x08));
    CHECK_HEX("SIPI page 0x00", 0x00004600u, hype_lapic_icr_low_sipi(0x00));
    CHECK_HEX("SIPI page 0xFF", 0x000046FFu, hype_lapic_icr_low_sipi(0xFF));
}

static uint32_t *icr_low(void) {
    return (uint32_t *)((unsigned char *)g_fake_lapic + HYPE_LAPIC_ICR_LOW_OFFSET);
}
static uint32_t *icr_high(void) {
    return (uint32_t *)((unsigned char *)g_fake_lapic + HYPE_LAPIC_ICR_HIGH_OFFSET);
}

static void test_send_ipi_writes_both_icr_dwords(void) {
    *icr_high() = 0;
    *icr_low() = 0; /* delivery-status bit clear, so the wait loop exits at once */

    hype_lapic_send_ipi(g_fake_lapic, hype_lapic_icr_high(3), hype_lapic_icr_low_sipi(0x08));

    CHECK_HEX("ICR_HIGH got the destination", 0x03000000u, *icr_high());
    CHECK_HEX("ICR_LOW got the SIPI command", 0x00004608u, *icr_low());
}

static void test_lvt_timer_periodic_encoding(void) {
    /* vector in low 8 bits + periodic mode bit (17), unmasked (bit 16 clear). */
    CHECK_HEX("periodic LVT vector 0xEC", 0x200ECu, hype_lapic_lvt_timer_periodic(0xEC));
    CHECK_HEX("periodic mode bit set", (1u << 17), hype_lapic_lvt_timer_periodic(0x40) & (1u << 17));
    CHECK_HEX("not masked", 0u, hype_lapic_lvt_timer_periodic(0x40) & HYPE_LAPIC_LVT_MASKED);
    CHECK_HEX("vector preserved", 0x40u, hype_lapic_lvt_timer_periodic(0x40) & 0xFFu);
}

int main(void) {
    test_mask_timer_sets_only_the_mask_bit();
    test_mask_timer_idempotent();
    test_mask_timer_does_not_touch_neighboring_registers();
    test_lvt_timer_periodic_encoding();
    test_icr_high_places_apic_id_in_top_byte();
    test_icr_low_init_is_canonical_value();
    test_icr_low_sipi_encodes_vector_and_startup_mode();
    test_send_ipi_writes_both_icr_dwords();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
