# Invariant: the host↔guest and guest↔guest security boundaries are paramount

**Hard invariant. Do not weaken without updating `plan.md` §10 first.** Above
performance, features, or convenience.

Nothing may cross either boundary unintentionally. The host must never expose
its own state, memory, or hardware to a guest except through a deliberately
designed, mediated interface; one guest must never observe or affect another
(its memory, its I/O, its timing side-channels, shared emulation state that
should be per-VM) except where the operator has *explicitly* configured a
channel (e.g. `net_peers`).

The rule is against *unintentional* leakage, not designed communication.
Intentional, configured inter-VM or external communication (net_peers, VMs over
a real network) is fine. When in doubt, treat a potential cross-boundary path as
a leak and prove it isn't.

A performance or simplicity win that erodes a boundary is not a win — see
[file-global-state-leak.md](file-global-state-leak.md) (the rejected port-0x80
passthrough and the per-vCPU de-globalization work).
