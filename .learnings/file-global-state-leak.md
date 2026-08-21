# File-global emulation state is a guest↔guest leak

**Backs:** the host↔guest / guest↔guest boundary invariant in `AGENTS.md`.

## What happened

Two design pressures were rejected because they eroded the isolation boundary:

- **Port-0x80 passthrough.** Letting a guest's port-0x80 writes reach real
  hardware was a simplicity/performance win locally, but it created an
  unmediated host path. Rejected.
- **File-global emulation state.** Any device-emulation state kept in a
  file-global (not per-VM / per-vCPU) is shared between guests. One guest can
  then observe or affect another through it. This drove the per-vCPU
  de-globalization work.

## The lesson

- A performance or simplicity win that erodes a boundary is not a win.
- Emulation state that should be per-VM but is file-global is a guest↔guest
  leak, even when nothing visibly breaks. Treat a `static` in a per-VM code path
  as a suspect: it is shared by every VM, and it will hide its own bug (see the
  NAT port-space and per-VM-function `static` incidents).
- When in doubt, treat a potential cross-boundary path as a leak and prove it is
  not before relying on it.
