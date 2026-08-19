/*
 * How a dedicated VM's vCPUs are priced in physical cores (#560).
 *
 * plan.md §10 decision 40: the unit of *execution* is a hardware thread; the unit of
 * *allocation* is a physical core. A core is granted whole, and every SMT sibling thread of a
 * granted core may run a vCPU OF THE SAME VM. So a VM asking for N vCPUs costs
 * ceil(N / threads_per_core) cores, and a dedicated VM given one 2-thread core gets two vCPUs.
 *
 * What the isolation rule forbids is two DISTRUSTING owners occupying one physical core at the
 * same time (§6g) -- not a sibling thread being used. This packer never splits a core between
 * two VMs: a core touched by one VM is spent, even if only partly filled.
 *
 * This lives in core/ and is pure arithmetic on purpose. Admission and placement both need it,
 * they used to compute it separately, and they drifted -- silently starting two VMs on one core
 * (#559). One tested function is what stops that recurring.
 */
#ifndef HYPE_SMP_PACK_H
#define HYPE_SMP_PACK_H

/* Per-VM result of a packing run. */
typedef struct {
    unsigned int vcpus;      /* vCPUs actually placed (== requested when the VM fit) */
    unsigned int cores;      /* whole physical cores consumed */
    unsigned int first_core; /* index into per_core[] of this VM's first core */
    /*
     * Threads per core to report to THIS guest: the widest core it was given, never more than
     * the vCPUs it actually got. A 1-vCPU VM on a 2-thread core reports 1, because the sibling
     * is not a vCPU of that guest and advertising it would describe a CPU that does not exist.
     */
    unsigned int threads_per_core;
} hype_smp_pack_vm_t;

/*
 * Pack `nvms` VMs onto `ncores` cores, in order, giving each VM whole cores.
 *
 * per_core[i] is the number of hardware threads on the i-th available core (as
 * hype_cpu_topology_select_cores reports them, BSP core already excluded). want[i] is VM i's
 * requested vCPU count; 0 is read as 1. out[] receives one entry per VM and is always fully
 * written, so a caller may inspect the VMs that did not fit.
 *
 * Returns the number of VMs satisfied IN FULL, counting from VM 0. Because cores are spent in
 * order, the first VM that does not fit means none after it fits either, so the return value is
 * a launchable prefix length -- which is exactly what admission needs to report.
 */
unsigned int hype_smp_pack(const unsigned int *per_core, unsigned int ncores,
                           const unsigned int *want, unsigned int nvms,
                           hype_smp_pack_vm_t *out, unsigned int max_vcpus_per_vm);

#endif /* HYPE_SMP_PACK_H */
