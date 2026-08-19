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

**Boot 2 (about 2 minutes)** — the complete microtest sweep, ten VMs, no Alpine.

```sh
mv hype.cfg hype-alpine.cfg && mv hype-micro.cfg hype.cfg
mv input input-alpine       && mv input-micro input
```

Then boot again. The second swap matters: without it the Alpine scripts get fed to microtests that
never print a login prompt, and you get two harmless-but-confusing script timeouts.

Why two boots rather than one: a microtest costs 196 MiB of the guest pool whatever its `mem_mb`
says (a 128 MiB floor — #290 — plus a 64 MiB vdisk carve and a 4 MiB firmware copy), and the pool
on an 8 GiB host measures 4590 MiB. Everything at once does not fit, and an over-committed config
loses its **tail**, which would be the microtests. Both halves as shipped sit inside comfortable
margins (8% and 57%), and both were rehearsed under QEMU at `QEMU_MEM=8192 SMP=12` — matching this
laptop — with **zero VMs dropped**.

The logs are on the stick because this machine has no serial port. They contain invalid UTF-8, so
read them with `LC_ALL=C grep -a`.

## What this single boot is trying to settle

Eight VMs, in this order — **order is load-bearing**, because admission caps from the end and names
whatever it drops (#396). The two Alpine guests come first: they carry the highest-value ticket and
need the whole run, while every microtest finishes in seconds.

| # | VM | Ticket | What to look for |
|---|---|---|---|
| 0 | `alpine-smp8` | **#527** | both APs `live=1`, AP exits in the millions, `Brought up 1 node, 2 CPUs`, `VMCSRELOAD ... steals=0`, entry failures 0, sustained past 8 min |
| 0,1 | both Alpines | **#526** | how many `soft lockup` lines appear. The nested rig gives **2 per 240 s**; bare metal is the comparison that has never been taken |
| 0,1 | both Alpines | **#461** | watch-only: a host `#GP (vector 13)` with RIP inside the AP trampoline, if it recurs |
| 1 | `alpine-load-cf9` | **#525** | after the 10-minute delay: `HAVE-TASKSET`, then a `0xcf9` write in HYPE.LOG and vm1 restarting. `NO-TASKSET` means the arm did not run and #525 is untouched |
| 2 | `micro/intdeliver` | **#553** (High) | `resumes past HLT=` against the tick count. QEMU gives ~1154 ticks to **1** resume. If bare metal shows resumes ≈ ticks, the bug is a nested-KVM artefact and #553 narrows sharply |
| 3 | `micro/pflash` | **#556** (High) | expected to **FAIL** on QEMU (`status 0x00`, where 0x80 is unconditional). If it PASSES here, #556 is nested-only |
| 4 | `micro/cpumsr` | **#552** | `vmx = absent`. The fix was found and made on the nested box, so this is the first assertion of it on silicon that genuinely has VT-x |
| 5 | `micro/pci` | — | first hardware run of a real guest bus walk: ECAM, BAR sizing, a self-programmed BAR, and the MSI capability #512's delivery depends on |
| 6 | `micro/ram1` | — | every page written then verified in two passes, so **page aliasing** in the real EPT would show |

### Boot 2 adds

| VM | Ticket | What to look for |
|---|---|---|
| `micro/pausespin` | #555 | preemption of a spinning guest; the implied TSC rate should be this laptop's **base** clock, and is boost-independent |
| `micro/fwcfg` | — | the fw_cfg directory, and `etc/e820` read both by PIO and by DMA with the two required to agree byte for byte. Its in-binary predecessor was skipped on VMX entirely |
| `micro/hello` | — | the load path itself, plus a `cmdline` echoed back |
| `micro/ps2` | — | keyboard **and** mouse driven by `\input\vm8.txt`, distinguished by the AUX_DATA status bit |
| `micro/faulter` | **#538** | expected to **FAIL**, deliberately. It triple-faults, and the point is that every verdict above it is still present and the host survived — proving a guest fault stops only its own VM |

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

Expected results, so a real regression is not lost in the noise: on **boot 1** everything should
PASS except `pflash`; on **boot 2** everything should PASS except `pflash` and `faulter`. Any other
FAIL, or any missing verdict, is new.

**A missing microtest verdict is a failure, not an absence of news** — a guest that wedges or
triple-faults prints neither PASS nor FAIL. Two verdicts are expected to be interesting rather than
green: `pflash` should FAIL (that is #556's reproduction), and `intdeliver`'s resume count is the
measurement rather than its PASS.

## If the run comes up short

- **A 113-byte log with no `hype: build` banner** is #371: roughly one boot in four never reaches
  hype at all. It is neither a pass nor a failure — just re-run.
- **`WILL NOT RUN` lines** mean admission capped the count. That is working as designed; the VMs
  lost are the ones lowest in the table, and the log names them with real numbers.
