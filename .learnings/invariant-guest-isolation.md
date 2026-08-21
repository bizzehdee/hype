# Invariant: guest isolation is the point of this project

**Hard invariant. Do not weaken without updating `plan.md` §10 first.** Each
clause exists because of `plan.md` §6g/§6j/§10's security-review decisions
(#19–22).

- Every device-emulation path that touches a guest-supplied address, offset, or
  length (virtio descriptors, AHCI/NVMe command buffers, block I/O LBA+count)
  **must** validate it against that specific VM's own EPT/NPT-mapped range and
  the backing resource's real size before the host dereferences it or performs
  the I/O. No raw guest pointer is ever trusted. **This is the actual
  guest-escape vector — EPT/NPT alone does not prevent it.**
- No two VMs' `cpu_set` ranges, `target_disk` paths, or varstore files may
  overlap/collide — enforced at startup admission control, not assumed.
- Guest-to-guest networking is default-deny; a pairing is allowed only when
  explicitly named via `net_peers`, validated at startup. Never make
  guest-to-guest traffic possible as a side effect of how NAT/switching happens
  to be implemented.
- A misbehaving/faulted guest is torn down alone (Force power off) — never a
  hypervisor-wide halt or reset in response to one guest's fault.
- A fault-isolation watchdog catches hangs/anomalies; it is **not** a substitute
  for the input-validation rule above.
