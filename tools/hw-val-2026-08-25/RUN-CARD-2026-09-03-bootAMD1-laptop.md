# Boot AMD-1L (AMD laptop, 4 cores) -- the input run with the pinned restart, Pico on a root port

Build: default, tree at da3e93d or later. Active config `\hype.cfg` = `hype1a.cfg` (2 vCPUs, the
boot-42 guest); `\input\vm0.txt` = `input-1a/vm0.txt` (reboot-pin). hype1g.cfg needs seven cores
and will be refused here, so #603's SVM leg stays with the 5950X.

**What is different on this machine, and what it means for the readings**
- The laptop keyboard is PS/2 (i8042). That is the #796 path: `KBDIRQ ps2reads=` should be about
  4,000 a second and `BSPCOST input` well under half.
- The Pico sits on a root port, not behind the boot-42 hub. Its scheduled self hot-plug now
  exercises the root-port arrival/departure path (#744/#746), and its typing runs through a
  different controller than every #788 measurement so far.
- The stick and the Pico probably share the laptop's only xHCI controller, so `XHCIOWN` will
  name it for the log sink and boot medium. If it stalls, #783 refuses the reset by design and
  input stays dead; the log records the refusal. That is a valid result for #783, not a failure
  of the run.
- No Logitech, no Keychron: `keyboards=` counts will be 1 (the Pico) plus the PS/2 keyboard's
  separate path.

## The sequence

1. Boot, stay on the dashboard. Confirm the banner sha and `XHCIOWN`.
2. The script logs in, pins the reboot to CPU 1, reboots, logs in again and prints
   `reboot-pin-nonbsp`. **Do not type until it has.**
3. Then **BOOTSEL once** on the Pico; confirm `a0001` appears in the guest.
4. Leave it 90 minutes. Type on the laptop keyboard now and then (leader chord, a few letters).
5. Power off. Bring back `HYPE.LOG` and `RUN1A.LOG`.

## What to read

| Ticket | Read | It says |
| --- | --- | --- |
| #525 #698 (SVM, hardware) | the restart chain, `TMRLATE vm0/1` climbing after `restarted (M8-4)` | the SVM record on the current tree |
| #788 | doubled characters per 1,000 in `KBDCHARS` | boot 42 was 2.7 through the hub on the 5950X's ctrl[2]; a clean count here localises the doubling to that path, the same count says it is hype |
| #796 | `KBDIRQ ps2reads= max=`, `BSPCOST input` | the PS/2 gate on a second laptop |
| #744 #746 | `port N changed -- now empty` / `something is attached` on a ROOT port every ~6.4 min, re-claim each time | root-port hot-plug on hardware |
| #426 | 90 minutes with input live at the end | a real-hardware long run of the shared facility; the 5950X form is still the gate |
| #641 | `APVCPU vm0/N: exits=`, `PERF: hlt_wait=` | recorded |
| #792 | `XHCIRESET` lines | probably none: the stall is the 5950X controller's habit. No stall is no result, not a pass |
