#include "ap_boot.h"
#include "lapic.h"

/* The relocatable trampoline blob + its parameter-block labels (ap_trampoline.S). */
extern uint8_t hype_ap_tramp_start[];
extern uint8_t hype_ap_tramp_end[];
extern uint8_t hype_ap_tramp_cr3[];
extern uint8_t hype_ap_tramp_entry[];
extern uint8_t hype_ap_tramp_stack[];
extern uint8_t hype_ap_tramp_alive[];
extern uint8_t hype_ap_tramp_phase[];
extern uint8_t hype_ap_tramp_arg[];

volatile uint32_t g_hype_ap_c_alive = 0;
volatile uint32_t g_hype_ap_last_phase = 0;

/*
 * The AP's 64-bit C landing. Runs on the AP with its own stack, on hype's
 * paging, interrupts disabled, and (deliberately) no IDT yet. For M8-0b-i
 * this just proves the core came all the way up through the C call boundary,
 * then parks. M8-0b-ii will instead set up per-AP GDT/IDT/TSS and run a guest
 * (run_fw_1_test(&g_vms[1], ...)) from here.
 *
 * Never returns: the trampoline reaches it with `callq`, but there is nowhere
 * sane to return to, so it parks in cli/hlt.
 */
void hype_ap_entry(void *arg) {
    (void)arg;
    g_hype_ap_c_alive = 1;
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static inline uint64_t ap_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* Busy-wait ~`us` microseconds. Uses the TSC when a frequency is known;
 * otherwise a coarse fixed spin (the exact INIT->SIPI spacing is not critical
 * on modern parts, which latch the Startup IPI regardless). */
static void ap_busy_wait_us(uint64_t tsc_hz, uint64_t us) {
    if (tsc_hz == 0) {
        volatile uint64_t i;
        for (i = 0; i < 2000000ULL; i++) {
            __asm__ volatile("pause");
        }
        return;
    }
    uint64_t cycles = (tsc_hz / 1000000ULL) * us;
    uint64_t start = ap_rdtsc();
    while (ap_rdtsc() - start < cycles) {
        __asm__ volatile("pause");
    }
}

int hype_ap_start(volatile uint32_t *lapic_base, uint8_t apic_id, void *tramp_page, uint64_t cr3,
                  uint64_t stack_top, uint64_t tsc_hz, void (*entry)(void *), void *entry_arg) {
    unsigned int size = (unsigned int)(hype_ap_tramp_end - hype_ap_tramp_start);
    unsigned int i;
    uint8_t *dst = (uint8_t *)tramp_page;
    volatile uint32_t *alive;
    uint8_t vec;
    uint64_t start, limit;

    /* Copy the trampoline into the low page, then fill its parameter block
     * (offsets are label-minus-start within the blob). */
    for (i = 0; i < size; i++) {
        dst[i] = hype_ap_tramp_start[i];
    }
    *(volatile uint64_t *)(dst + (hype_ap_tramp_cr3 - hype_ap_tramp_start)) = cr3;
    *(volatile uint64_t *)(dst + (hype_ap_tramp_entry - hype_ap_tramp_start)) =
        (uint64_t)(uintptr_t)entry;
    *(volatile uint64_t *)(dst + (hype_ap_tramp_arg - hype_ap_tramp_start)) =
        (uint64_t)(uintptr_t)entry_arg;
    *(volatile uint64_t *)(dst + (hype_ap_tramp_stack - hype_ap_tramp_start)) = stack_top;
    alive = (volatile uint32_t *)(dst + (hype_ap_tramp_alive - hype_ap_tramp_start));
    *alive = 0;
    g_hype_ap_c_alive = 0;

    /* SIPI vector is the trampoline page's physical page number: the AP starts
     * executing in real mode at CS = vector << 8, IP = 0 (linear vector<<12). */
    vec = (uint8_t)((uintptr_t)tramp_page >> 12);

    /* INIT-SIPI-SIPI (Intel SDM Vol 3 / AMD APM MP-init sequence). */
    hype_lapic_send_ipi(lapic_base, hype_lapic_icr_high(apic_id), hype_lapic_icr_low_init());
    ap_busy_wait_us(tsc_hz, 10000); /* 10 ms after INIT */
    hype_lapic_send_ipi(lapic_base, hype_lapic_icr_high(apic_id), hype_lapic_icr_low_sipi(vec));
    ap_busy_wait_us(tsc_hz, 200); /* 200 us between the two SIPIs */
    hype_lapic_send_ipi(lapic_base, hype_lapic_icr_high(apic_id), hype_lapic_icr_low_sipi(vec));

    /* Wait up to ~100 ms for the trampoline to reach long mode (alive flag). */
    {
        volatile uint32_t *phase = (volatile uint32_t *)(dst + (hype_ap_tramp_phase - hype_ap_tramp_start));
        int rc = -1;
        start = ap_rdtsc();
        limit = (tsc_hz != 0) ? (tsc_hz / 10ULL) : 200000000ULL;
        while (ap_rdtsc() - start < limit) {
            if (*alive) {
                rc = 0;
                break;
            }
            __asm__ volatile("pause");
        }
        /* Report how far the AP got even on timeout (0=never started, 1=real,
         * 2=protected, 3=long) -- turns a bare "didn't come up" into a
         * pinpoint of which mode transition failed. */
        g_hype_ap_last_phase = *phase;
        return rc;
    }
}
