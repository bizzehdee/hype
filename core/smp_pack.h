/*
 * How a dedicated VM's request is priced in physical cores (#564).
 *
 * A vCPU IS A PHYSICAL CORE. `vcpus = N` asks for N whole cores, and the VM costs exactly N
 * cores on every host. What the guest then SEES depends on the hardware it was given: a core
 * with two SMT threads yields two logical CPUs, a core with one yields one. SMT is a bonus the
 * guest was never promised.
 *
 * So the fixed quantity is the ALLOCATION and the variable one is the guest's CPU count -- the
 * opposite of the usual hypervisor convention, where `-smp cpus=N` fixes what the guest sees and
 * lets the cost float. Chosen deliberately (plan.md §10 decision 47): a dedicated VM is buying
 * exclusive hardware, and the honest unit for exclusive hardware is the thing exclusion happens
 * on -- the core. It also makes one config fit every host, because the core cost never moves.
 *
 * A core is granted whole and is never split between two VMs; that is what §6g's isolation
 * argument relies on, and it is unchanged.
 *
 * This lives in core/ and is pure arithmetic on purpose. Admission and placement both need it,
 * they used to compute it separately, and they drifted -- silently starting two VMs on one core
 * (#559). One tested function is what stops that recurring.
 */
#ifndef HYPE_SMP_PACK_H
#define HYPE_SMP_PACK_H

/* Per-VM result of a packing run. */
typedef struct {
    unsigned int cores;      /* whole physical cores granted (== requested when the VM fit) */
    unsigned int vcpus;      /* logical CPUs the guest gets: cores * threads_per_core */
    unsigned int first_core; /* index into per_core[] of this VM's first core */
    /*
     * Threads per core to report to this guest, and the MINIMUM across the cores it was granted,
     * not the maximum. CPUID leaf 0xB/0x1F reports ONE threads-per-core for the whole VM, so a
     * guest handed a 2-thread and a 1-thread core cannot be described truthfully by either
     * number. Taking the minimum keeps the guest's topology uniform and honest; the surplus
     * threads of a wider core stay idle and stay owned by this VM, which is the safe direction.
     * On a homogeneous host -- every real target so far -- nothing is wasted.
     */
    unsigned int threads_per_core;
} hype_smp_pack_vm_t;

/*
 * Pack `nvms` VMs onto `ncores` cores, in order, giving each VM the whole cores it asked for.
 *
 * per_core[i] is the number of hardware threads on the i-th available core (as
 * hype_cpu_topology_select_cores reports them, BSP core already excluded). want[i] is VM i's
 * requested CORE count; 0 is read as 1. out[] receives one entry per VM and is always fully
 * written, so a caller may inspect the VMs that did not fit.
 *
 * max_vcpus_per_vm caps the LOGICAL CPUs a VM may end up with; cores are dropped to respect it,
 * because granting a core whose threads cannot be used would take hardware from another VM for
 * nothing. 0 means no cap.
 *
 * Returns the number of VMs satisfied IN FULL, counting from VM 0. Cores are spent in order, so
 * the first VM that does not fit means none after it fits either -- the return value is a
 * launchable prefix length, which is what admission needs to report.
 */
unsigned int hype_smp_pack(const unsigned int *per_core, unsigned int ncores,
                           const unsigned int *want, unsigned int nvms,
                           hype_smp_pack_vm_t *out, unsigned int max_vcpus_per_vm);

#endif /* HYPE_SMP_PACK_H */
