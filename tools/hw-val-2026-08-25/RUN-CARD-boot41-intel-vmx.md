# Boot 41 -- Intel i5-13420H. VMX correctness. Two tickets, one guest boot.

**Different machine.** The Intel box. Its only NVMe is the user's BitLocker Windows install --
never a write target.

Default build, no `EXTRA_CFLAGS`. A Linux guest with more than one vCPU, so there is a non-BSP
vCPU to drive #525 from.

## The two

| Ticket | What to read | Passes when |
| --- | --- | --- |
| **#525** | a guest reboot driven from a **non-BSP** vCPU | the 0xCF9 path works from vCPU 1. `#482` moved the ACPI reset register into `fw_1_shared_port_io()` so a guest can reboot from any vCPU, and closed on a bare-metal Intel run -- but its own notes flagged the VMX side of that function as **compile-tested only**. This is its first execution |
| **#729** | guest MTRR, PAT and pvclock MSR reads | modelled values, not zeros, and writes that are not dropped. `3f59e4c` landed one guest MTRR model shared by both backends (`core/guest_mtrr.c`); the SVM path is proven and the VMX path is not |

## Why one boot covers both

Both are VMX-side gaps in code the SVM side already exercises, both need a Linux guest on
bare-metal Intel, and neither disturbs the other: #729 is read at any point during the run and
#525 is the last thing you do, because it reboots the guest.

Use `input-scripts/reboot-pin.txt`, which pins the reboot to vCPU 1 after an idle observation
window at the login prompt. Driving it through emulated keystrokes proved unreliable on the
QEMU rig (chords=0, most presses lost), which is exactly why the pinned script exists.
