# Where QEMU and the 5950X actually diverge

Written after eight hardware boots in which every USB fix "passed in QEMU" and then failed
on the desk. The question worth answering is whether QEMU is an inadequate model or whether
the rigs were.

**Mostly the rigs.** Of the nine defects found in this series, two needed hardware, one was
blocked by a false claim I had written into a rig, and the rest were reachable in QEMU with
a topology closer to the real machine.

## The topology gap, which explains most of it

| | rigs | the 5950X |
|---|---|---|
| xHCI controllers | 1 | 2 |
| hub devices | 1 | 5 |
| USB devices | 4 | 12 |
| interrupt-IN endpoints polled | 3 | 8 |
| composite (multi-interface) devices | 0 | 2 |
| devices that fail to enumerate | 0 | 1 |

Several bugs are *invisible below a threshold* the rigs never crossed. #759's idle-poll cost
is per endpoint, so three endpoints hid what eight made obvious. #765's capacity limit of 6
hub devices is ample for one hub. #757 needs enough enumeration activity to bank stale port
bits.

## Per defect

| # | found on | could QEMU have caught it | why it did not |
|---|---|---|---|
| 755 Context Entries lowered | HW | **yes** | `usb-kbd`/`usb-mouse` are single-interface, so nothing is ever claimed twice on one slot. The Logitech receiver is a keyboard AND a mouse |
| 757 stale port bits swept as departures | HW | **yes** | four devices enumerating cleanly bank almost nothing; twelve with a failure bank eight |
| 759 19,531-spin idle poll | HW | **yes** | cost is per interrupt-IN endpoint; 3 vs 8 |
| 761 dropped transfer events | HW | **yes, and it did reproduce** | the rig asserted QEMU could not test it. That claim was false and is the single most expensive mistake in the series |
| 762 only C_PORT_CONNECTION cleared | QEMU | yes | found within minutes of #761 |
| 763 unaddressable device retried forever | HW | **no** | QEMU has no device that fails Address Device |
| 765 hub/endpoint pool too small | HW | **yes** | 6 hub devices is generous for one hub |
| 766 completion claimed by the wrong TRB | HW | **yes — QEMU showed 31-46 per run** | the detector that made it visible did not exist until hardware forced it |
| 743 recycled slot id fails | HW | **no** | genuine controller behaviour |

## Genuine QEMU-vs-hardware deviations

Short list, and none of them is why this series went the way it did.

1. **Slot-id recycling** (#743). On the 5950X a HID on a recycled slot id fails every
   transfer with cc=4. QEMU reuses slot ids with no such fault. Controller behaviour; not
   modelled.
2. **No device that refuses to be addressed** (#763). A real SuperSpeed hub below another
   hub returns cc=4 to Address Device here. Nothing in QEMU does.
3. **xHCI BAR placement** (#240, earlier). QEMU puts it at 0x380000000000; this firmware
   uses a low BAR, and an off-by-8x guard left [64 GiB, 512 GiB) unmapped. The QEMU value
   was on the safe side of the bug.
4. **The TR Dequeue Pointer in a Running endpoint context.** QEMU keeps it updated during
   normal operation; real hardware need not, and this one does not -- xHCI 4.12.2 only
   requires it to be valid on a Stopped or Halted endpoint. A #764 diagnostic built on
   reading it looked sound in QEMU and produced nothing but false positives on the desk,
   five across two boots, on endpoints that were working. Withdrawn.

5. **Transfer-event ordering.** Real controllers deliver command and transfer events later
   and more out-of-order than QEMU (#254, #266). Both needed leniency QEMU never demanded.

## What I wrongly blamed on QEMU

> "QEMU's usb-hub does not deliver a status-change report for a runtime attach or detach.
> Measured, not assumed -- polls=18400 errors=0 reports=0."

Every number was real. The conclusion was wrong. `hw/usb/dev-hub.c` returns the port-change
bitmap on an IN token to endpoint 1 and NAKs when idle, and its detach path sets
`wPortChange`; QEMU's own `usb_hub_status_report` tracepoint prints it. `reports=0` measured
**hype**, and I attributed it to the emulator -- which then justified not asserting the
behind-hub cycle, which is how #746 shipped broken to hardware twice.

**The rule that follows: before writing "QEMU cannot test this" into a rig, read QEMU's
source.** It is at `/mnt/data/dev/qemu-build/qemu-11.1.0/`, and `-trace enable=<name>`
writes tracepoints to the rig's `qemu.err`. A measurement of zero is a measurement of the
whole system, not of the component you happen to suspect.

## What was done about it

`tools/767/run-767-qemu.sh` now runs that topology: two controllers, four hub devices, a hub
behind a hub, the boot medium on one controller and every input device on the other, a
composite HID claimed twice on one slot, and a device that cannot be enumerated.

The last two needed additions to QEMU itself (`tools/767/qemu-composite-hid.patch`):
`usb-kbd-mouse`, one device with a boot mouse on endpoint 1 and a boot keyboard on endpoint
2; and `usb-badaddr`, which stalls every control transfer. Neither exists in stock QEMU, and
their absence is why #755, #763, #770 and #771 all reached hardware unreproduced.

**The rig passes.** Which means the topology is no longer the difference. Boot 15 still killed
the keyboard within seconds with three ring-drift reports, and the rig reproduces none of
that -- so whatever is left is a property of the hardware rather than of the test bench, and
#764 is where it is tracked.

That is worth stating plainly: closing the topology gap did not close the bug list. It closed
the excuse.
