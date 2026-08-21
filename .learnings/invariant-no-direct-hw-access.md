# Invariant: no guest gets direct hardware access

**Hard invariant. Do not weaken without updating `plan.md` §10 first.**

Physical disk/NIC access is always mediated through a host-side driver plus an
emulated guest-facing frontend — never PCI passthrough or guest-initiated DMA to
real hardware.

This is why v1 needs no IOMMU; do not add passthrough without revisiting that
decision in `plan.md` §10. (USB "passthrough" is mediated at the device-model
level, not raw PCI passthrough — it still goes host-driver → emulated frontend.)
