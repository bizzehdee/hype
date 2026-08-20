#!/usr/bin/env python3
"""
#576: fail the build's checks on a guest MMIO window served from outside the ONE shared dispatch.

#482 gave a VM's shared device MMIO one entry point, fw_1_shared_mmio_npf(), and then left the BSP
loop's own copy of the chain in place. So for a while there were two hand-maintained lists of the
same windows, and nothing failed when they drifted: #81's NIC went into the shared list only, so it
was served on every vCPU EXCEPT the BSP, and a single-vCPU guest read its device_status as 0xff
after sizing BAR4 and walking the whole capability chain successfully. Every symptom pointed at the
device model. The device model was fine.

The asymmetry is what makes this expensive. A window in one list works on whichever vCPU that list
serves and fails on the others, so the same defect presents as "works with 1 vCPU, hangs with 2" or
the exact reverse, depending on which list got the addition -- #511's shape, and #511 was expensive.

#576 merged the lists. This guard keeps them merged: every guest MMIO handler below must be called
from inside fw_1_shared_mmio_npf() and nowhere else. The guest LAPIC is deliberately excluded --
it is per-vCPU state, needs no device lock, and belongs to whichever vCPU faulted, so each loop
handles its own.
"""
import sys

PATH = "boot/main.c"
DEF = "static hype_fw_dev_t fw_1_shared_mmio_npf("

# Every handler that serves a window of guest-physical address space out of per-VM device state.
HANDLERS = (
    "vmm_handle_pci_ecam_npf_insn",
    "hype_svm_vcpu_handle_hpet_npf",
    "vmm_handle_ioapic_npf",
    "vmm_handle_ahci_disk_npf_map",
    "vmm_handle_ahci_npf_map",
    "vmm_handle_nvme_npf",
    "vmm_handle_bochs_vbe_npf",
    "vmm_handle_virtio_blk_npf_map",
    "vmm_handle_guest_nic_npf",
    "fw_1_flash_npf",
)


def main() -> int:
    lines = open(PATH, encoding="utf-8", errors="surrogateescape").read().split("\n")

    start = next((i for i, l in enumerate(lines) if l.startswith(DEF)), None)
    if start is None:
        print("check-one-mmio-list: fw_1_shared_mmio_npf() not found -- the guard has gone blind, "
              "which is worse than a failure. Fix the pattern in tools/check-one-mmio-list.py.")
        return 2
    end = next((i for i in range(start + 1, len(lines)) if lines[i] == "}"), None)
    if end is None:
        print("check-one-mmio-list: could not find the end of fw_1_shared_mmio_npf()")
        return 2

    bad = []
    for i, line in enumerate(lines):
        if start <= i <= end:
            continue
        stripped = line.strip()
        if stripped.startswith("*") or stripped.startswith("//"):
            continue
        # A declaration or definition is not a dispatch site.
        if stripped.startswith("static "):
            continue
        for h in HANDLERS:
            if h + "(" in line:
                bad.append((i + 1, h, stripped))

    if bad:
        print("check-one-mmio-list: a guest MMIO window is served from outside "
              "fw_1_shared_mmio_npf() (#576).")
        print("A second dispatch means that window is served on some vCPUs and absorbed as")
        print("all-ones on the others, and which ones depends on which list got the addition.")
        print("That is #81 and #511: the guest places its BAR, walks its capability chain, then")
        print("reads a register as 0xff. Add the window to the shared dispatch instead -- every")
        print("loop calls it.")
        for ln, h, text in bad:
            print(f"  {PATH}:{ln}: {h} -- {text}")
        return 1

    print(f"check-one-mmio-list: OK -- all {len(HANDLERS)} guest MMIO windows are served from the "
          "one shared dispatch (#576)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
