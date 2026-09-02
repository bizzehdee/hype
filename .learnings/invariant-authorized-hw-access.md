# Invariant: no guest gets UNAUTHORIZED direct hardware access

**Hard invariant. Do not weaken without updating `plan.md` §10 first.**

**Amended 2026-08-23** (was "no guest gets direct hardware access", flat and
absolute) to make room for PCI-e device passthrough (#699) without weakening
the rule for every device that isn't explicitly assigned. The word that
matters is **unauthorized**: a VM may only ever reach hardware it has been
specifically, explicitly granted — never anything else, and never by
guessing, positional inference, or a default.

By default, every device is still mediated exactly as before: physical
disk/NIC access always goes through a host-side driver plus an emulated
guest-facing frontend — never PCI passthrough or guest-initiated DMA to real
hardware. (USB "passthrough" is mediated at the device-model level, not raw
PCI passthrough — it still goes host-driver → emulated frontend.)

**Authorization**, when it exists at all (passthrough, #699), means all of:

- The device is named in that VM's own config, by durable hardware identity
  (serial/BDF+vendor+device-ID, the same no-positional-matching discipline
  `physical:` disk targets already use, decision 8) — never by slot order,
  never a default, never inferred.
- Its DMA is constrained to that VM's own GPA range through the IOMMU — a
  passthrough device can physically only ever address memory the owning VM
  itself owns, so a misprogrammed or malicious device driver in the guest
  cannot reach another VM or the host.
- Its interrupts are remapped so they can only target that VM's own vCPUs.
- Every device NOT explicitly authorized this way remains exactly as
  absolute as before this amendment — this is not a general loosening, it is
  one narrow, IOMMU-backed exception with its own ceremony (`plan.md` §10,
  the decision this amendment points to).

This is why v1 needed no IOMMU; passthrough is the one thing that changes
that, and only for a device an operator explicitly assigned.

**Ratified as `plan.md` §10 decision #80 (2026-09-03, #700).** That decision
fixes what this note leaves open: AMD-Vi first behind one IOMMU interface,
the `[device.*]` config block (`id_match` AND `bdf` both mandatory, whole
IOMMU groups only, `allow_passthrough = true`), the boot-time dashboard
confirmation and the refusal list (never the controller carrying the boot
medium or log sink, the input xHCI, the uplink NIC, the IOMMU, a host bridge,
or the GOP device without a USB console), and reset ownership (hype resets
before assignment and after every teardown; a failed reset quarantines the
function for the run). Read decision #80 before touching any of it.
