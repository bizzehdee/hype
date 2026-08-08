#ifndef HYPE_CORE_CPU_TOPOLOGY_H
#define HYPE_CORE_CPU_TOPOLOGY_H

#include <stdint.h>

/*
 * #360: the host's REAL local-APIC IDs, enumerated rather than assumed.
 *
 * hype used to start its APs by literal ID -- hype_ap_start(..., 1u, ...) for
 * the first and 2u for the second -- i.e. it assumed APIC IDs run 0, 1, 2 from
 * the BSP. That holds on the AMD laptop, where two VMs work. It does not hold
 * on a hybrid Intel part (P-cores + E-cores, SMT on some of them), where the
 * 8-bit initial-APIC-ID space is not densely packed. On an i5-13420H, ID 1
 * exists and ID 2 does not answer at all: the second AP never even entered the
 * real-mode trampoline (last_phase=0), so the VM bound to it never ran while
 * every other part of its setup succeeded -- RAM, firmware and media all
 * resolved. The failure looked like a configuration problem, not a missing CPU.
 *
 * The IDs come from EFI_MP_SERVICES_PROTOCOL, which is only available BEFORE
 * ExitBootServices, so enumeration happens early and the result is carried
 * forward. This module is the carrier, and it is pure: no EFI, no MMIO, no
 * globals. The firmware call fills it; everything else asks it questions.
 *
 * Fully unit tested, including the sparse layout that exposed the bug.
 */

/* 64 logical processors is well past anything hype targets today (the AMD
 * laptop has 16, the Intel box 12) and keeps the table a few hundred bytes. */
#define HYPE_CPU_TOPOLOGY_MAX 64u

/*
 * Where a processor physically sits, as firmware reports it
 * (EFI_CPU_PHYSICAL_LOCATION). Two threads sharing one (package, core) are SMT
 * siblings.
 *
 * This does NOT say whether a core is a P-core or an E-core. UEFI has no notion
 * of core TYPE -- every enabled processor is just an AP. The definitive answer
 * is CPUID leaf 0x1A (Hybrid Information), whose EAX[31:24] is 0x40 for a
 * "Core" (P) and 0x20 for an "Atom" (E), and which is only present when CPUID
 * leaf 7 EDX bit 15 (Hybrid) is set. Leaf 0x1A reports the type of the core
 * EXECUTING it, so it must be run on each core in turn -- it cannot be read for
 * another core from the BSP.
 *
 * Until that is done, SMT is a useful free proxy on current Intel hybrid parts:
 * P-cores have two threads, E-cores have one. It is a heuristic, not a
 * guarantee, and it is reported as such.
 */
typedef struct {
    uint32_t package;
    uint32_t core;
    uint32_t thread;
} hype_cpu_location_t;

typedef struct {
    uint32_t apic_id[HYPE_CPU_TOPOLOGY_MAX];
    hype_cpu_location_t loc[HYPE_CPU_TOPOLOGY_MAX];
    unsigned int count;    /* usable (enabled) processors recorded */
    unsigned int bsp_index;/* index into apic_id[] of the bootstrap processor */
    int have_bsp;          /* 0 until a processor was added with is_bsp set */
    unsigned int dropped;  /* processors seen but not recorded (table full) */
} hype_cpu_topology_t;

/* Empty the table. Must be called before the first add. */
void hype_cpu_topology_reset(hype_cpu_topology_t *t);

/*
 * Record one processor. DISABLED processors are skipped entirely -- firmware
 * reports them, but they cannot be started, and including one would make
 * hype_cpu_topology_ap() hand out an ID that never answers, which is exactly
 * the failure this module exists to prevent.
 *
 * Returns 0 if recorded, -1 if skipped (disabled) or dropped (table full);
 * `dropped` counts only the latter, so a full table can be reported rather than
 * silently truncating the machine.
 */
int hype_cpu_topology_add(hype_cpu_topology_t *t, uint32_t apic_id, int is_bsp, int is_enabled);

/* As hype_cpu_topology_add(), also recording where the processor sits. */
int hype_cpu_topology_add_at(hype_cpu_topology_t *t, uint32_t apic_id, int is_bsp, int is_enabled,
                             uint32_t package, uint32_t core, uint32_t thread);

/*
 * Distinct physical cores, and how many of them have more than one thread.
 * On a current Intel hybrid part the SMT count is the P-core count, because
 * E-cores are single-threaded -- a useful signal until CPUID leaf 0x1A is read
 * per core (see hype_cpu_location_t). Either pointer may be null.
 */
void hype_cpu_topology_core_summary(const hype_cpu_topology_t *t, unsigned int *cores,
                                    unsigned int *smt_cores);

/*
 * The APIC ID of the `n`-th application processor -- every recorded processor
 * except the BSP, in enumeration order, zero-based. Returns -1 if there is no
 * such AP.
 *
 * Returned as int64 so a legitimate 32-bit APIC ID can never collide with the
 * -1 "no such AP" answer. x2APIC IDs are full 32-bit values.
 */
int64_t hype_cpu_topology_ap(const hype_cpu_topology_t *t, unsigned int n);

/* How many APs are available to run guests on (recorded processors minus BSP). */
unsigned int hype_cpu_topology_ap_count(const hype_cpu_topology_t *t);

/* The BSP's own APIC ID, or -1 if no processor was marked as the BSP. */
int64_t hype_cpu_topology_bsp(const hype_cpu_topology_t *t);

/*
 * 1 when the recorded IDs are exactly 0,1,2,... in order -- the layout hype used
 * to assume. Purely diagnostic: it lets a log say whether the old hardcoded
 * behaviour would have worked on this machine, which is the difference between
 * the AMD laptop and the Intel box.
 */
int hype_cpu_topology_is_consecutive(const hype_cpu_topology_t *t);

#endif /* HYPE_CORE_CPU_TOPOLOGY_H */
