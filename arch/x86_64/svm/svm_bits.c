#include "svm.h"

#include "../cpu/lapic.h"
#include "../../../core/avic.h"
#include "../../../core/cfg.h" /* HYPE_CFG_MAX_VMS -- one AVIC ID table pair per VM (decision 67) */

uint32_t hype_svm_nasid_from_cpuid_ebx(uint32_t ebx) {
    return ebx;
}

uint32_t hype_svm_asid_for_slot(unsigned slot, uint32_t nasid) {
    uint32_t max_guest_asid;

    /* NASID 0 or 1 means no usable guest ASID at all (0 belongs to the host). Report
     * 0 so the caller refuses to run rather than silently sharing the host's tag. */
    if (nasid < 2u) {
        return 0u;
    }
    max_guest_asid = nasid - 1u;
    /* slot 0 -> 1, never 0. Slots beyond the CPU's range wrap onto the top usable
     * ASID: vCPU slots are already aliased loudly when the pool is exhausted
     * (svm_alloc_vcpu_slot), and inventing an out-of-range ASID would fail VMRUN
     * instead of degrading. A shared ASID is then flushed by TLB_CONTROL. */
    if ((uint32_t)slot + 1u > max_guest_asid) {
        return max_guest_asid;
    }
    return (uint32_t)slot + 1u;
}

uint64_t hype_svm_efer_with_svme(uint64_t old_efer) {
    return old_efer | HYPE_EFER_SVME;
}

int hype_svm_guest_efer_write(uint64_t current_efer, uint64_t requested, uint64_t cr0,
                              uint64_t cr4, uint64_t *out) {
    uint64_t value;

    /* MBZ bits: a real WRMSR raises #GP, and letting one through would fail VMRUN's own
     * "Any MBZ bit of EFER is set" check -- killing hype instead of the guest. */
    if ((requested & HYPE_EFER_MBZ) != 0ull) {
        return -1;
    }
    /*
     * LMA is hardware-owned. APM §3.1.7: "When writing the EFER register the value of this bit
     * must be preserved. [...] An attempt to write a value that differs from the state
     * determined by hardware results in a #GP fault." Faulting a mismatch is what lets a guest
     * that reads-modifies-writes correctly through, while refusing one that invents long-mode
     * state it has not earned by enabling paging.
     */
    if ((requested & HYPE_EFER_LMA) != (current_efer & HYPE_EFER_LMA)) {
        return -1;
    }
    /*
     * Enabling long mode requires paging to be OFF at the moment LME is set -- software sets
     * LME first and CR0.PG second. Accepting LME while PG is already set would produce
     * exactly APM §15.5.1's "EFER.LME and CR0.PG are both set and CR4.PAE is zero" /
     * "...and CR0.PE is zero" illegal states on the next entry.
     */
    if ((requested & HYPE_EFER_LME) != 0ull && (current_efer & HYPE_EFER_LME) == 0ull &&
        (cr0 & HYPE_CR0_PG) != 0ull) {
        return -1;
    }
    /* And catch the illegal combinations directly, for the case where LME was already set. */
    if ((requested & HYPE_EFER_LME) != 0ull && (cr0 & HYPE_CR0_PG) != 0ull &&
        ((cr4 & HYPE_CR4_PAE) == 0ull || (cr0 & HYPE_CR0_PE) == 0ull)) {
        return -1;
    }

    /* RAZ bits read as zero, so they are dropped rather than faulted; SVME is non-negotiable. */
    value = (requested & ~HYPE_EFER_RAZ) | HYPE_EFER_SVME;
    *out = value;
    return 0;
}

uint64_t hype_svm_guest_efer_read(uint64_t stored_efer) {
    return stored_efer & ~HYPE_EFER_SVME;
}

/*
 * #640 (cause 2): AVIC's per-VM ID tables (plan.md decision 67) -- one physical + one logical
 * table per VM, sized to HYPE_CFG_MAX_VMS rather than a new allocation scheme, shared by every
 * vCPU of that VM. Each row is exactly 4 KiB (512 * 8B / 1024 * 4B), so aligning the whole array
 * to 4 KiB keeps every row page-aligned too, same trick boot/main.c's g_pd[gb][512] uses.
 */
static uint64_t g_avic_physical_table[HYPE_CFG_MAX_VMS][512] __attribute__((aligned(4096)));
static uint32_t g_avic_logical_table[HYPE_CFG_MAX_VMS][1024] __attribute__((aligned(4096)));

/*
 * #640 (cause 2): one backing page per vCPU pool slot (mirrors g_vmcb_pool/g_ctx_pool's own
 * per-slot shape, and VMX APICv's g_virtual_apic_page[slot]) -- AVIC requires this to be that
 * vCPU's own register file, never shared. Dynamically allocated (hype_svm_avic_pool_alloc,
 * called from boot only in a HYPE_ENABLE_AVIC build) rather than a fixed array, so a default
 * build pays nothing for it.
 */
static uint8_t *g_avic_backing_pages; /* g_avic_backing_pool_n * 4096 bytes, page-aligned */
static unsigned g_avic_backing_pool_n;
/* Which VM + guest-physical-APIC-id each pool slot's AVIC state belongs to, so a later NOACCEL
 * exit (which only has the slot, via ctx) can find the right row of g_avic_logical_table to
 * update on an LDR write. Parallel arrays, same slot indexing as the backing-page pool. */
static unsigned g_avic_slot_vm_idx[HYPE_SVM_AVIC_MAX_SLOTS];
static unsigned g_avic_slot_guest_id[HYPE_SVM_AVIC_MAX_SLOTS];
static uint8_t g_avic_slot_active[HYPE_SVM_AVIC_MAX_SLOTS];

void hype_svm_avic_pool_alloc(unsigned count, uint64_t (*alloc_zeroed_pages)(unsigned pages)) {
    if (count == 0u) count = 1u;
    if (count > HYPE_SVM_AVIC_MAX_SLOTS) count = HYPE_SVM_AVIC_MAX_SLOTS;
    g_avic_backing_pages = (uint8_t *)(uintptr_t)alloc_zeroed_pages(count);
    g_avic_backing_pool_n = count;
}

void hype_svm_vcpu_enable_apic_accel(hype_vmcb_t *vmcb, unsigned slot, unsigned vm_idx,
                                     unsigned guest_apic_id, uint32_t host_apic_id,
                                     unsigned max_physical_id) {
    uint8_t *backing;
    uint64_t backing_phys;

    if (g_avic_backing_pages == 0 || slot >= g_avic_backing_pool_n || vm_idx >= HYPE_CFG_MAX_VMS ||
        guest_apic_id >= 512u) {
        return; /* out of range -- refuse rather than write past a table or alias a neighbor */
    }
    backing = g_avic_backing_pages + (uint64_t)slot * 4096u;
    backing_phys = (uint64_t)(uintptr_t)backing;

    g_avic_physical_table[vm_idx][guest_apic_id] =
        hype_avic_physical_entry(host_apic_id, backing_phys, 1 /* running */, 1 /* valid */);
    g_avic_slot_vm_idx[slot] = vm_idx;
    g_avic_slot_guest_id[slot] = guest_apic_id;
    g_avic_slot_active[slot] = 1u;

    hype_vmcb_configure_avic(vmcb, HYPE_LAPIC_DEFAULT_BASE, backing_phys,
                             (uint64_t)(uintptr_t)g_avic_logical_table[vm_idx],
                             (uint64_t)(uintptr_t)g_avic_physical_table[vm_idx],
                             (uint8_t)max_physical_id);
}

void *hype_svm_avic_backing_page_by_slot(unsigned slot) {
    if (g_avic_backing_pages == 0 || slot >= g_avic_backing_pool_n || !g_avic_slot_active[slot]) {
        return 0;
    }
    return g_avic_backing_pages + (uint64_t)slot * 4096u;
}

void hype_svm_avic_update_logical_by_slot(unsigned slot, uint32_t new_ldr) {
    int bit;
    if (slot >= g_avic_backing_pool_n || !g_avic_slot_active[slot]) {
        return;
    }
    /* #640: flat mode only (AMD APM Vol 2 §15.29.5.3) -- cluster mode's own table format is a
     * separate, less common encoding no guest this project has booted under AVIC has needed
     * yet; a cluster-mode LDR write just leaves the logical table unentried for this vCPU,
     * same as before this function existed. */
    bit = hype_avic_ldr_flat_index(new_ldr);
    if (bit < 0) {
        return;
    }
    g_avic_logical_table[g_avic_slot_vm_idx[slot]][bit] =
        hype_avic_logical_entry(g_avic_slot_guest_id[slot], 1);
}

void hype_svm_decode_exitintinfo(uint64_t exitintinfo, int will_inject, hype_svm_evtinfo_t *out) {
    out->valid = (exitintinfo & HYPE_SVM_EVENTINJ_V) != 0ULL;
    out->type = (unsigned int)((exitintinfo & HYPE_SVM_EVENTINJ_TYPE_MASK) >>
                               HYPE_SVM_EVENTINJ_TYPE_SHIFT);
    out->vector = (unsigned int)(exitintinfo & HYPE_SVM_EVENTINJ_VECTOR_MASK);
    out->has_error_code = (exitintinfo & HYPE_SVM_EVENTINJ_EV) != 0ULL;
    out->error_code = (uint32_t)(exitintinfo >> HYPE_SVM_EVENTINJ_ERRORCODE_SHIFT);
    out->hypervisor_will_inject = will_inject ? 1 : 0;
}

hype_svm_evtreplay_t hype_svm_decide_event_replay(const hype_svm_evtinfo_t *in) {
    if (in == 0 || !in->valid) {
        return HYPE_SVM_EVTREPLAY_NONE;
    }
    /*
     * An exception is reproduced by restarting the faulting instruction, so re-staging it would
     * deliver it twice. Checked BEFORE the will_inject case: it is a property of the recorded event,
     * not of what hype is doing, and reporting "refused" for something that self-heals would be
     * noise on a path that is already rare.
     */
    if (in->type == (unsigned int)HYPE_SVM_EVENTINJ_TYPE_EXCEPTION) {
        return HYPE_SVM_EVTREPLAY_SELF_HEALS;
    }
    /* EVENTINJ carries one event; re-staging over an injection hype has already decided on would
     * silently drop whichever lost. */
    if (in->hypervisor_will_inject) {
        return HYPE_SVM_EVTREPLAY_REFUSE;
    }
    if (in->type == (unsigned int)HYPE_SVM_EVENTINJ_TYPE_INTR ||
        in->type == (unsigned int)HYPE_SVM_EVENTINJ_TYPE_NMI) {
        return HYPE_SVM_EVTREPLAY_REINJECT; /* ack'd at the PIC/APIC: unrecoverable if dropped */
    }
    /* Software interrupts (type 4) and the reserved encodings: next-RIP semantics hype does not
     * model, so acting would be a guess. */
    return HYPE_SVM_EVTREPLAY_REFUSE;
}

const char *hype_svm_evtreplay_str(hype_svm_evtreplay_t d) {
    switch (d) {
        case HYPE_SVM_EVTREPLAY_NONE: return "none";
        case HYPE_SVM_EVTREPLAY_REINJECT: return "re-staged (interrupt already acknowledged)";
        case HYPE_SVM_EVTREPLAY_SELF_HEALS: return "left alone (instruction restart reproduces it)";
        case HYPE_SVM_EVTREPLAY_REFUSE: return "REFUSED -- cannot re-stage safely";
        default: return "unknown";
    }
}
