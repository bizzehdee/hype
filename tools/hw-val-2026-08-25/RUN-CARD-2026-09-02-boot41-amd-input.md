# Boot 41 (AMD 5950X) -- the controller-silence probe

Build: default, no `EXTRA_CFLAGS`. Commit with `#781` controller-silence probe and the `#791`
fix (after `ef7472a`). Config unchanged from boot 40.

## What boot 40 settled

- The revive was the wedge trigger (#790): 54 minutes, `cmdring timeouts=0`, no `REVIVE`.
- The input still died, at 41 minutes, and it was not the Pico: the Pico, the Logitech and the
  hubs' status endpoints on ctrl[2] all stopped completing within the same 30 seconds, with no
  port event and no error. Boot 39 did the same at 291 s. Nothing in the log could say whether
  the controller's event delivery had died or the hub behind root port 4 had.

## What changed since boot 40

- **#781 probe.** When every keyboard on a controller that has ever reported has been silent
  for 30 s, hype reads USBSTS, USBCMD, CRCR, IMAN, IMOD, ERDP, the root port's PORTSC, checks
  whether an event is sitting unconsumed at the software dequeue, and issues a **No-Op
  command**. One `CTRLSILENCE` line per silence, at most 16 per run.
- **#791 fix.** A keyboard claimed after the guest set its typematic rate now gets that rate.
  Expect `host-hid: cafe:4b44 claimed after the guest set typematic 0x00 -- re-applied` at
  every Pico re-arrival.

## Before you boot

- Confirm the banner sha matches the staged build.
- Confirm the first `HIDTICK` line says `silence_revive=0`.

## The sequence

Unchanged from boot 40.

1. Boot, stay on the dashboard.
2. **Right-Ctrl + Right-Alt + 1** (both modifiers right-hand).
3. Wait for `localhost login:`, type **`root`** and Enter, get `localhost:~#`.
4. **BOOTSEL once** -- LED solid, `a0001` appears in the guest.
5. Leave it as long as the machine can be spared. **The input has died at 291 s and at 2450 s in
   the last two runs; the probe fires 30 s after it does.** Once a `CTRLSILENCE` line has been
   written the run has done its job.
6. Power off normally. Bring back `HYPE.LOG` and `RUN1A.LOG`.

## What to read

| Ticket | What to read | It says |
| --- | --- | --- |
| **#781** | `fw-1 CTRLSILENCE ctrl[N]: ... No-Op COMPLETED cc=1` | command and event rings are alive; the fault is downstream of the root port (the hub or its link). `PORTSC` `CCS`/`PED`/`PLS` say whether the root port still sees the hub |
| **#781** | `... No-Op FAILED` and `cmd timeouts=1` | the controller itself stopped; `USBSTS` `HCH`/`HSE`/`HCE`, `CRCR.CRR` and `ERDP` vs `sw_deq` say how |
| **#781** | `pending_event=1` | an event was delivered and hype never consumed it: a software-side dequeue fault, not hardware |
| **#791** | the `hhhh` run after every 6k+5 marker in `KBDCHARS` | the same repeat count before and after the Pico's first bus drop (boot 40: 68 before, 27 after) |
| **#788** | `KBDCHARS` alphabet passes | doubled characters per 1,000; boot 40 was 4 in 11,542 |
| **#787** | `hub slot 3 port 3 changed -- now empty` | the Pico's 376 s bus-drop period, unchanged if it is Pico-side |

A `CTRLSILENCE` line 30 s after a human stops typing on an otherwise idle machine is expected
and harmless; it will show a healthy controller and a completed No-Op.
