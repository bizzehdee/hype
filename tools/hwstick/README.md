# hype hardware-validation stick — multi-ticket run

Build: **a86e313** (`hype: build a86e313-dirty`).

The `-dirty` is the **vendored `edk2` submodule**, which carries local #436 research patches and is
**not compiled into `hype.efi`**. hype.efi is built by the clang/lld pipeline from `boot/`, `core/`,
`arch/` and `devices/` only; the guest firmware on this stick is the pre-built `fw/*.fd` pair. So the
binary corresponds exactly to a86e313 — but the stamp is left honest rather than forced clean.

## SAFETY — read this first

**Nothing in this run writes to a physical disk.** Every `target_disk` is `file:` on this stick;
there is no `physical:` target anywhere in `hype.cfg`. That is deliberate: this laptop's only
internal NVMe is your BitLocker Windows install, and it is never a write target.

If you ever edit `hype.cfg` on this stick, re-check that before booting.

## How to run — two boots off the same stick

**Boot 1 (about 12 minutes)** — `hype.cfg` as shipped. Two 2-vCPU Alpine guests plus the five
highest-value microtests.

1. Boot the laptop from this stick (Secure Boot off).
2. Let it run **at least 12 minutes**. Ten of those are a deliberate `delay` in vm1's script that
   banks #527's window before the reboot arm fires; a shorter run loses #525 and weakens #527.
3. Power off. **Copy `HYPE.LOG`, `VM0.LOG` and `VM1.LOG` off the stick, or rename them** — boot 2
   overwrites them.

**Boot 2 (about 1 minute, optional)** — the suite alone, one VM. Use it when the Alpine guests are
not the point, e.g. re-checking #556 or #557 after a fix.

```sh
mv hype.cfg hype-alpine.cfg && mv hype-micro.cfg hype.cfg
mv input input-alpine       && mv input-micro input
```

The second swap matters: with no Alpine guests, `ps2`'s input script must land at `\input\vm0.txt`,
and `input-micro/` is what puts it there. That config also includes `ps2`, which the primary one
cannot (vm0 and vm1's scripts already occupy those indices).

Both configs were rehearsed under QEMU at `QEMU_MEM=12288 SMP=8` — matching the AMD laptop — with
**zero VMs dropped** and both input scripts armed. Note that a QEMU rehearsal cannot reproduce the
core count that actually binds here: `-smp 8` gives eight single-thread cores, not four cores with
two threads each. The VM count was therefore derived from the machine's real topology rather than
from a rehearsal.

The logs are on the stick because this machine has no serial port. They contain invalid UTF-8, so
read them with `LC_ALL=C grep -a`.

## Why only three VMs

Admission grants one **whole physical core** per VM with the BSP's core reserved, so a host runs at
most `physical cores - 1` VMs however much RAM it has:

| machine | logical | physical | RAM | max VMs |
|---|---|---|---|---|
| AMD laptop | 8 | **4** | 12 GB | **3** |
| Intel i5-13420H | 12 | 8 | 8 GB | 7 |

The first version of this stick listed seven VMs. The AMD run dropped four of them — including both
High-priority reproductions it existed to carry — and RAM was never close (pool 7012 of 10165 MiB
usable). So the layout is now two Alpine guests plus **one VM running the suite kernel** (#554),
which runs the microtests in turn inside a single guest. Three VMs fits the smaller machine, and the
bigger one simply has cores to spare.

## What this is trying to settle

| VM | Ticket | What to look for |
|---|---|---|
| `vm0` alpine | **#527** | both APs `live=1`, AP exits in the millions, `Brought up 1 node, 2 CPUs`, `VMCSRELOAD ... steals=0`, entry failures 0, sustained past 8 min |
| `vm0`,`vm1` | **#526** | how many `soft lockup` lines appear. The nested rig gives 2 per 240 s; the AMD baseline from the last run was **zero** |
| `vm0`,`vm1` | **#461** | watch-only: a host `#GP (vector 13)` with RIP inside the AP trampoline |
| `vm1` | **#525** | after the 10-minute delay: `HAVE-TASKSET`, then a `0xcf9` write and vm1 restarting. `NO-TASKSET` means the arm did not run |
| `vm2` suite | **#553** | `intdeliver`: `resumes past HLT=` vs the tick count |
| `vm2` suite | **#557** | `intdeliver` on the last AMD run got **38M HLT exits and zero interrupts**. If it PASSES now, #557 was mis-scoped |
| `vm2` suite | **#556** | `pflash` is expected to **FAIL** |
| `vm2` suite | **#552** | `cpumsr`: `vmx = absent` |
| `vm2` suite | — | `ram1` (page aliasing in the real nested tables), `pci` (ECAM/BAR/MSI), `fwcfg` (PIO vs DMA agreement), `pausespin` (preemption + invariant TSC) |

### Reading the suite

Each member is announced **before** it runs:

```
MICRO RUN: ram1
MICRO PASS: ram1
...
MICRO SUITE: ran=8 passed=7 failed=1 noverdict=0 skipped=0 unknown=0
```

**The summary line's absence is the signal** that the sweep was truncated — a suite that died halfway
otherwise looks like a suite with fewer members. And a `MICRO RUN` with no matching verdict names
the member that took the VM down.

## Reading the result quickly

```sh
LC_ALL=C grep -a "hype: build" HYPE.LOG                 # confirm which binary ran
LC_ALL=C grep -a -oE "MICRO (PASS|FAIL): [a-z0-9]+" HYPE.LOG | sort -u
LC_ALL=C grep -a -E "WILL NOT RUN|adm: REFUSED" HYPE.LOG   # anything admission dropped
LC_ALL=C grep -a -c "soft lockup" VM0.LOG VM1.LOG           # #526
LC_ALL=C grep -a -E "live=|VMCSRELOAD" HYPE.LOG | tail -20  # #527
LC_ALL=C grep -a -iE "0xcf9|reset" HYPE.LOG | head           # #525
LC_ALL=C grep -a -E "resumes past HLT|TSC (rate|cycles)" HYPE.LOG   # #553, #555
```

Expected results, so a real regression is not lost in the noise: **`pflash` FAILS** (#556) and
**`intdeliver` may FAIL** (#557 — it got zero interrupts on the last AMD run). Everything else should
PASS. Any other FAIL, any missing verdict, or a missing `MICRO SUITE:` summary is new.

**A missing microtest verdict is a failure, not an absence of news** — a guest that wedges or
triple-faults prints neither PASS nor FAIL. Two verdicts are expected to be interesting rather than
green: `pflash` should FAIL (that is #556's reproduction), and `intdeliver`'s resume count is the
measurement rather than its PASS.

## If the run comes up short

- **A 113-byte log with no `hype: build` banner** is #371: roughly one boot in four never reaches
  hype at all. It is neither a pass nor a failure — just re-run.
- **`WILL NOT RUN` lines** mean admission capped the count. That is working as designed; the VMs
  lost are the ones lowest in the table, and the log names them with real numbers.
