# Boot AMD-1 (5950X) -- the input run with the SMP legs folded in (#426 #775 #790 #603 #641 #788)

Build: default, no `EXTRA_CFLAGS`, tree at or after da3e93d (rate-bounded xHCI reset #792, PS/2
gate #796, reset-path fixes #797/#798). Active config `\hype.cfg` = `hype1g.cfg`: the boot-42
guest (`run1a`, 2 vCPUs, reboot pinned to CPU 1 by `\input\vm0.txt` = `input-1a/vm0.txt`) plus
hype1b.cfg's three #603 microtests. Seven physical cores including the BSP; the 5950X has 16.

## Before you boot

- Banner sha matches the staged build.
- `XHCIOWN: log sink on ctrl[1], boot medium on ctrl[1]` -- the keyboards are on ctrl[2].
- Admission grants all four VMs.

## The sequence

1. Boot, stay on the dashboard. The microtests finish themselves in the first minute (`hello`
   PASS, `vmexit` triple-faults on purpose, `vmexitstorm` is force-powered-off by its watchdog).
2. The script logs in, pins the reboot to CPU 1, reboots, logs in again and prints
   `reboot-pin-nonbsp`. **Do not type until it has.**
3. Then arm the Pico: **BOOTSEL once**, confirm `a0001` in the guest. Keep the Logitech and the
   Keychron attached; leave both spare USB drives plugged in (#780's condition).
4. Leave it 90 minutes from the second login. Type on the Logitech now and then.
5. Power off. Bring back `HYPE.LOG` and `RUN1A.LOG`.

## What to read

| Ticket | Read | Passes when |
| --- | --- | --- |
| #603 (SVM leg) | `micro/hello` PASS after `vmexit`'s triple fault and `vmexitstorm`'s force-off; no `PROBE FAIL` | the coverage suite's second leg; closes #603 |
| #426 | 90 minutes with input live at the end | no `left dead`, no `REFUSED`, keyboards typing at power-off |
| #775 | `CTRLSILENCE` / `XHCIRESET` | every stall followed by `reset #N done ... keyboards=3`; close as answered by #781-#785 |
| #790 | `cmdring timeouts=` equals the `CTRLSILENCE` count; 0 `REVIVE` | the 90-minute form the ticket asked for |
| #792 | `XHCIRESET ctrl[2]: reset #N begins (M in the last 10 min, previous S s ago)` | a fourth reset is allowed when the stalls are minutes apart |
| #525 #698 | the restart chain and `TMRLATE vm0/1` after it | SVM hardware record on the current tree |
| #641 | `APVCPU vm0/N: exits=`, `PERF: hlt_wait=` | recorded |
| #788 | doubled characters per 1,000 in `KBDCHARS` | recorded (boot 42: 2.7) |
