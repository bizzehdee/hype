# Boot A (Intel, VMX) -- four VMX legs in one boot (#525 #698 #729 #603)

**Different machine: the Intel box.** If it is the i5-13420H, its only NVMe is the user's
BitLocker Windows install. Nothing in this run names a physical target; every disk is a file on
this stick. Boot from the stick, nothing else.

Build: default, no `EXTRA_CFLAGS`, first tree carrying 61921d9 (the VMX half of the STI-shadow
fix). Active config `\hype.cfg` = `hype2g.cfg`: one Alpine live guest with 2 vCPUs (`run2b`, the
reboot-pin script auto-loads as `\input\vm0.txt`) plus hype2d.cfg's three microtests. Seven
physical cores including the BSP.

## Before you boot

- Banner sha matches the staged build (the card's staging output says which).
- The admission lines grant all four VMs. **If `run2b` or a microtest is refused for cores**,
  power off, copy `\hype2b.cfg` over `\hype.cfg`, boot (that is #525/#698), then do the same
  with `\hype2d.cfg` (that is #729/#603). Two boots, same stick, no re-staging.

## The sequence

1. Boot, stay on the dashboard. The microtests run themselves: `hello` prints PASS within a
   minute, `vmexit` walks its probes and triple-faults on purpose, `vmexitstorm` trips its own
   watchdog and hype powers that VM off alone. None of that needs you.
2. The Alpine guest reaches `localhost login:`; the script logs in, pins the reboot to CPU 1 and
   reboots. Do not type into it.
3. Wait for the second login. The script logs in again, checks it is a fresh boot, and prints
   `reboot-pin-nonbsp`. **From the second login, leave it 10 minutes**: that idle window is what
   #698 measures.
4. Power off. Bring back `HYPE.LOG` and `RUN1A.LOG`.

If the second login never comes, leave it 15 minutes past the reboot, then power off. The log is
still the result: `TMRLATE vm0/1` frozen after `restarted (M8-4)` is #698 not fixed on VMX.

## What to read

| Ticket | What to read | It says |
| --- | --- | --- |
| **#525** | `vm0 vCPU 1 guest reset via ACPI reset register (0xCF9)` exactly once, then `SCRIPT vm0: PASS ... reboot-pin-nonbsp` | the restart was driven from the non-BSP vCPU and the guest came back to a fresh login |
| **#698** | every `TMRLATE vm0/1: deliveries=` after `restarted (M8-4)` | climbing every sample. Boot 2b-era logs froze at one value for 22 min. Also `INTDIAG vm0/1` must not sit at `pending=1 ... shadow=0x1` |
| **#729** | `micro/vmexit: MSR round-trip (MTRR var0 base) wrote 0x123456000, read back` | `0x123456000`. Boot 2d read back 0 |
| **#603** | `micro/hello` PASS **after** `vmexit`'s triple fault and `vmexitstorm`'s watchdog force-off; `vmexit`'s probe list with no `PROBE FAIL` | the VMX leg of the coverage table, and Sec 6g isolation |
| **#787-class noise** | none expected: no Pico on this machine | |

#599/#605 are NOT this boot. They need the `-DHYPE_ENABLE_APICV=1` binary, which is not staged;
nothing has landed on that hang since boot 2c.
