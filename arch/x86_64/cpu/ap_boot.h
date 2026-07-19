#ifndef HYPE_ARCH_AP_BOOT_H
#define HYPE_ARCH_AP_BOOT_H

#include <stdint.h>

/*
 * M8-0b: application-processor (AP) bring-up via INIT-SIPI-SIPI.
 *
 * Firmware MP services don't survive ExitBootServices, so hype starts a
 * second core itself: it stages the ap_trampoline.S blob in a page below
 * 1 MiB, sends INIT then two Startup IPIs (lapic.c ICR encoders), and the
 * trampoline transitions the AP real -> protected -> long mode onto hype's
 * own paging with its own stack, then calls hype_ap_entry().
 *
 * This is exempt glue (real IPIs, rdtsc pacing, mode transitions); the only
 * unit-tested logic it rests on is the ICR encoders in lapic.c.
 */

/* Brings up one AP and waits (bounded) for it to signal alive.
 *   lapic_base : host LAPIC MMIO window (HYPE_LAPIC_DEFAULT_BASE on real HW)
 *   apic_id    : the target AP's physical APIC id
 *   tramp_page : a 4 KiB-aligned page below 1 MiB to stage the trampoline in
 *   cr3        : host paging root the AP loads (must be < 4 GiB)
 *   stack_top  : top of a 16-byte-aligned stack for the AP (host address)
 *   tsc_hz     : host TSC frequency, for the INIT->SIPI->SIPI delays (0 -> a
 *                coarse fixed spin fallback)
 *   entry      : the AP's 64-bit C landing (runs on the AP with `entry_arg` in
 *                RCX). Pass hype_ap_entry for the bare bring-up smoketest, or a
 *                caller-provided AP-main to run a guest on the AP (M8-0b-ii).
 *   entry_arg  : opaque argument passed to `entry` (RCX per the MS x64 ABI).
 * Returns 0 if the AP reached the trampoline's long-mode stage within the
 * timeout, -1 otherwise. */
/* The default 64-bit C landing for a bare AP bring-up: sets g_hype_ap_c_alive
 * then parks in cli/hlt. Pass as hype_ap_start's `entry` to just prove a core
 * comes up (e.g. STEP 2a's parked apic_id=2), vs a real per-core main. */
void hype_ap_entry(void *arg);

int hype_ap_start(volatile uint32_t *lapic_base, uint8_t apic_id, void *tramp_page, uint64_t cr3,
                  uint64_t stack_top, uint64_t tsc_hz, void (*entry)(void *), void *entry_arg);

/* Set to 1 once an AP has actually entered hype_ap_entry() -- i.e. the 64-bit
 * C landing ran (proves the whole mode-switch + stack + call worked, a step
 * beyond the trampoline's own "alive" flag). */
extern volatile uint32_t g_hype_ap_c_alive;

/* The last trampoline stage the AP reached in the most recent hype_ap_start:
 * 0 = never started (no SIPI delivery / AP absent), 1 = real mode, 2 =
 * protected mode, 3 = long mode. Diagnostic for a failed bring-up. */
extern volatile uint32_t g_hype_ap_last_phase;

#endif /* HYPE_ARCH_AP_BOOT_H */
