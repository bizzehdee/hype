# Invariant: a vCPU never loses CPU-time isolation

**Hard invariant. Do not weaken without updating `plan.md` §10 first.** Which
mechanism assures it depends on the tier (`plan.md` §3, §6g, §10 decision 39),
chosen per VM by `cpu_mode`:

- **`dedicated` (the default): 1:1 exclusive vCPU-to-pCPU pinning.** No shared
  pCPU between two VMs, ever. Isolation holds *by construction* — no mechanism
  has to work correctly for it to hold. Do not weaken this; it is what
  latency-sensitive and security-critical guests are for.
- **`shared`: cores are pooled and time-sliced.** Isolation holds *only because
  preemption is mandatory*. Any change that lets a guest defer or suppress its
  own preemption breaks the guarantee outright, however harmless it looks
  locally.

Two things follow, and neither is negotiable: a core may never appear in both a
dedicated `cpu_set` and the shared pool, and distrusting `isolation_group`s may
never occupy one physical core simultaneously (default is one group per VM, so
configuring nothing is the strict case).
