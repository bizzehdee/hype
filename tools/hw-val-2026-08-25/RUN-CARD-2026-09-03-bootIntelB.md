# Boot Intel-B (i5-13420H) -- the APICv build, one guest, login or the #708 evidence

**Different machine: the Intel box.** Its only NVMe is the user's BitLocker Windows install.
Nothing in this run names a physical target; the guest boots the live Alpine ISO from this
stick's data partition. Boot from the stick, nothing else.

Build: **APICv** (`-DHYPE_ENABLE_APICV=1`) is the active `\EFI\BOOT\BOOTX64.EFI`; the default
and AVIC builds sit beside it under `\EFI\hype\`. First tree carrying the #708 change: the BSP's
HLT wake now reads RVI from the VMCS when APICv is live, and `HLTSHADOW` counts BSP HLT exits
that re-saved blocking-by-STI. Active config `\hype.cfg` = `hype2c.cfg`: one Alpine live guest,
2 vCPUs (`run2c`), `\input\vm0.txt` = `input-2c/vm0.txt` (login only, no restart, 20-minute
timeout).

## Why this boot exists

Boot 2c (2026-08-25, tree 976e71a) hung in kernel boot with `TIMERSTALL vm0/0`, the vCPU in a HLT
loop at one RIP, `pending_valid=0`, and the timer vector 0xec pending in the virtual-APIC page
(`gis=0x30ec`). The BSP loop's wake test read the software IRR, which APICv never fills, so the
HLT was never retired and the re-saved STI shadow blocked the hardware's own delivery. That is
the change under test. #599's bar and #605's gate both ride on it.

## Before you boot

- Banner sha matches the staging output below.
- `vmx: apicv=ON (slot 0) ... (HYPE_ENABLE_APICV set)`. If it prints `apicv=off`, the part
  did not grant the controls and the run tests the silent fallback instead; note it and go on.
- Admission grants `run2c` two cores.

## The sequence

1. Boot, stay on the dashboard. Do not type into the guest.
2. Wait for `localhost login:`. The script logs in and prints `BOOT-OK-localhost`, then
   `fresh-boot-login`.
3. **From the login, leave it 10 minutes idle.** That is the window where 2c froze.
4. Power off. Bring back `HYPE.LOG` and `RUN2C.LOG`.

If the guest never reaches login inside the script's 20-minute timeout, power off then. That
outcome is the #708 evidence, not a harness fault.

## What to read

| Ticket | Read | Passes when |
| --- | --- | --- |
| #708 | `SCRIPT vm0: PASS`; `HLTSHADOW: bsp hlt exits with STI blocking=N of those with RVI pending=M | rvi wakes=W` | login reached and no `TIMERSTALL`. `W` > 0 with `M` ~ `W` says the wake fired for the case 2c died in. If it hangs: `M` climbing with `W` = 0 means the wake did not run; `M` = 0 means the deadlock is somewhere else and `apicv-state` (`gis=`, `virr:`, `isr:`) is the lead |
| #599 | login with `apicv=ON`; `EXHIST` `npf` vs boot 2a | its bar: a 2-vCPU Linux login with the flag on |
| #605 | `VECSTAT` / `INTDIAG` delivery counts, no `PANIC`, no `WATCHDOG`, no `TIMERSTALL` | a previously working guest boots to the same point with APICv on. On PASS the Intel default flips to ON (decision 58) in a follow-up commit; on FAIL the default stays off and the signature goes on #708 |
| #698 (APICv face) | `TMRLATE vm0/1` climbing after login | the AP's timer keeps firing under APICv |

## Re-staging AMD-1 afterwards

`./tools/hw-val-2026-08-25/stage.sh --boot amd1` after archiving this boot's logs. Same tree.
