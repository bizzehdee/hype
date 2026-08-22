#include "dump_fmt.h"
#include "format.h"

/* GPR encoding order (see dump_fmt.h): 0=RAX,1=RCX,2=RDX,3=RBX,4=RSP,5=RBP,6=RSI,7=RDI,8-15=R8..R15.
 * Printed in the conventional reading order below rather than index order, which is why this
 * table exists instead of a straight loop over gprs[]. */
static const unsigned char g_gpr_print_order[16] = {0, 3, 1, 2, 6, 7, 5, 4, 8, 9, 10, 11, 12, 13, 14, 15};
static const char *const g_gpr_print_name[16] = {
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

static void fmt_vcpu(unsigned index, const hype_dump_vcpu_t *v, hype_dash_text_t *out) {
    char buf[HYPE_DASH_TEXT_COLS];
    unsigned g;

    if (v == (const hype_dump_vcpu_t *)0 || !v->present) {
        hype_snprintf(buf, sizeof(buf), "  vcpu%u: not dispatched (no vCPU created yet)", index);
        hype_dash_text_add(out, buf);
        return;
    }

    hype_snprintf(buf, sizeof(buf), "  vcpu%u: rip=0x%016llx cr3=0x%016llx", index,
                  (unsigned long long)v->rip, (unsigned long long)v->cr3);
    hype_dash_text_add(out, buf);

    for (g = 0; g < 16u; g += 4u) {
        unsigned char i0 = g_gpr_print_order[g], i1 = g_gpr_print_order[g + 1u],
                      i2 = g_gpr_print_order[g + 2u], i3 = g_gpr_print_order[g + 3u];
        hype_snprintf(buf, sizeof(buf), "    %s=0x%016llx %s=0x%016llx %s=0x%016llx %s=0x%016llx",
                      g_gpr_print_name[g], (unsigned long long)v->gprs[i0],
                      g_gpr_print_name[g + 1u], (unsigned long long)v->gprs[i1],
                      g_gpr_print_name[g + 2u], (unsigned long long)v->gprs[i2],
                      g_gpr_print_name[g + 3u], (unsigned long long)v->gprs[i3]);
        hype_dash_text_add(out, buf);
    }

    hype_snprintf(buf, sizeof(buf),
                  "    event: can_accept=%s eventinj=0x%016llx vintr_window=%s pending=%u "
                  "(highest=0x%02x)",
                  v->can_accept ? "yes" : "no", (unsigned long long)v->eventinj,
                  v->vintr_armed ? "armed" : "idle", v->pending_count, v->pending_vector);
    hype_dash_text_add(out, buf);
}

void hype_dump_format(const hype_dump_snapshot_t *snap, hype_dash_text_t *out) {
    char buf[HYPE_DASH_TEXT_COLS];
    unsigned i;

    if (out == (hype_dash_text_t *)0) {
        return;
    }
    hype_dash_text_reset(out);
    if (snap == (const hype_dump_snapshot_t *)0) {
        return;
    }

    hype_snprintf(buf, sizeof(buf), "dump %s -- %s", snap->vm_name ? snap->vm_name : "?",
                  snap->lifecycle ? snap->lifecycle : "?");
    hype_dash_text_add(out, buf);

    for (i = 0; i < snap->n_vcpus && i < HYPE_DUMP_MAX_VCPUS; i++) {
        fmt_vcpu(i, &snap->vcpu[i], out);
    }

    /* Split across two lines: ten buckets on one line overruns HYPE_DASH_TEXT_COLS on a
     * long-running VM, where every counter can print its full 20 digits. */
    hype_snprintf(buf, sizeof(buf), "  exits (vcpu0 loop): total=%llu hlt=%llu npf=%llu ioio=%llu msr=%llu",
                  snap->ex_total, snap->ex_hlt, snap->ex_npf, snap->ex_ioio, snap->ex_msr);
    hype_dash_text_add(out, buf);
    hype_snprintf(buf, sizeof(buf), "    cpuid=%llu vintr=%llu pause=%llu intr=%llu other=%llu",
                  snap->ex_cpuid, snap->ex_vintr, snap->ex_pause, snap->ex_intr, snap->ex_other);
    hype_dash_text_add(out, buf);
}
