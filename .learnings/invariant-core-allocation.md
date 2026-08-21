# Invariant: a hardware thread executes; a physical core is the unit of allocation

**Hard invariant. Do not weaken without updating `plan.md` §10 first.** (`plan.md`
§10 decision 40) — **and a vCPU IS a physical core** (decision 47).

`vcpus = N` costs exactly N cores on every host, and SMT is a **bonus**: a
granted core is granted whole, so a dedicated VM given one 2-thread core gets one
vCPU whose guest sees two logical CPUs, and the same config on a non-SMT host
costs the same core and yields one.

Never idle a sibling thread to satisfy an isolation rule and never disable SMT
for the pool — the rule forbids two *distrusting* owners on one core at the same
time, which core-granular allocation already delivers.

If (package, core, thread) cannot be proven for this host, fall back to treating
every logical processor as its own single-threaded core; wasting threads is
safe, pairing distrusting owners by accident is not.

Both admission and placement must call `hype_smp_pack()`. See the
`reference_bare_metal_vm_capacity` memory for the derived capacities (#564
supersedes #560).
