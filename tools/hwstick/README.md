# ⚠ PHYSICAL-WRITE RUN (build a502863+) — reads the AMD laptop's spare disks

**This stick is configured to WRITE to the AMD laptop's two spare disks, by serial:**
- `vm0 phys-write-ahci` → SATA SSD **2132E5BF4EAE** (Crucial), via a `[disk.*] backing=physical`
  section (validates #586) presented as virtio-blk; a 48 MiB `dd` drives #295 vectored writes and a
  marker read-back proves the physical AHCI write path. Watch hype's `BLK WRITE ... vec=` DIAG.
- `vm1 phys-write-nvme` → NVMe **5ME3N005713803V2W** (SKHynix); a direct-boot Linux writes a marker
  and reads it back — physical NVMe write path + #588 kernel-VM PCI BARs.
- `suite` → the micro test suite (regression baseline).

Destructive writes to those two serials are **operator-authorized and auto-confirmed** (no `confirm`
keystroke needed). By-serial targeting means this can only ever touch those two disks — on any other
machine the serials do not match and nothing is armed. **AMD LAPTOP ONLY.**

Verdicts per VM land in `VM0.LOG` / `VM1.LOG`:
`SCRIPT vm0: PASS pass (physical-ahci-write-verified)` and `SCRIPT vm1: PASS (physical-nvme-write-verified)`.
Also check `HYPE.LOG`: `AUTO-CONFIRMED (operator-authorized serial ...)`, the `vec=` counter in the
`BLK WRITE` DIAG (#295), and `#586 ... PHYSICAL AHCI/SATA backend ... [writable]`.

---

# hype hardware-validation stick — multi-ticket run

Build: **`make clean && make all && make micro`, default flags, no `EXTRA_CFLAGS`.** Every ticket
this stick carries is satisfied by a default build; nothing here needs a variant, and #527's
instruction to build with `-DHYPE_SMP_STARTABLE_VCPUS=2` is stale (that knob has raised only the
NO-CONFIG default since #192, and this stick ships a config — see the correction on #527).

`stage.sh` echoes the binary's own stamp (`hype: build <sha>-dirty`), and that stamp is the
authority, not this line. **Check it against the banner in the log after the run**; a stick staged
from a stale `build/hype.efi` is indistinguishable from a fix that did not work.

The `-dirty` is the **vendored `edk2` submodule**, which carries local #436 research patches and is
**not compiled into `hype.efi`**. hype.efi is built by the clang/lld pipeline from `boot/`, `core/`,
`arch/` and `devices/` only; the guest firmware on this stick is the pre-built `fw/*.fd` pair. So the
binary corresponds exactly to its sha — the stamp is left honest rather than forced clean.

## Before the first boot: three files this repo does not carry

`stage.sh` does not put these on the stick, and **without them that VM does not run at all** —
hype refuses to substitute a scratch disk, which is correct, but it means the ticket riding on
that VM produces no evidence:

| path | what it is | if missing |
|---|---|---|
| `\iso\test.iso` | vm0's Alpine install media | **fatal** — vm0 has no boot device |
| `\hype\disks\vm1-alpine-disk.img` | vm1's own bootable disk (#120) | **fatal** — vm1 has no boot device |
| `\hype\disks\vm0.img` | vm0's install target | warning — vm0 still boots its media, it just cannot install |

`stage.sh` checks these last, and **the severity depends on the VM's own `boot` mode** — a missing
`target_disk` is a warning for a `boot = installer` VM and fatal for a `boot = disk` one. It creates
nothing: a blank image is not a bootable disk, and guessing an install size is the kind of
substitution the rest of that script refuses to make.

**vm1 boots its OWN DISK, not installer media (#120).** That is the M9-5 case — "already-installed
guest disks, not just fresh installers" — and every other guest here boots from media, so without
this VM the disk-boot path is never exercised on hardware. Build the artefact with:

```sh
tools/make-guest-disk-from-iso.sh disk-images/alpine-virt-3.21.7-x86_64.iso \
    rig/guestdisk/alpine-disk.img
```

It checks the ISO really is a hybrid image (an MBR partition of type 0xEF plus a GPT — which is why
a dd'd Alpine ISO boots on a USB stick), copies it fully allocated, and then **control-boots it in
bare QEMU with no cdrom attached**, refusing to hand you an image that does not reach a login
prompt. That control is #262's lesson: without it, "hype's disk model refused it" and "the image was
never bootable" look identical, and on a cold-boot-only laptop that mistake costs a whole run.

What it is NOT: the product of a `setup-disk` install. The guest runs Alpine's live mode off its own
disk. That is exactly the disk-versus-media distinction #120 draws, and it is not enough for
anything needing a partitioned persistent install (#126, #164) — those still need a real install.

A stick staged before this has `\iso\vm1.iso` and `\hype\disks\vm1.img`; both are now unused
and can be deleted to free 64 MiB and change nothing.

## SAFETY — read this first

**Nothing in this run writes to a physical disk.** Every `target_disk` is `file:` on this stick;
there is no `physical:` target anywhere in `hype.cfg`. That is deliberate: this laptop's only
internal NVMe is your BitLocker Windows install, and it is never a write target.

If you ever edit `hype.cfg` on this stick, re-check that before booting.

## How to run — two boots off the same stick

**Boot 1 (about 12 minutes)** — `hype.cfg` as shipped. Two Alpine guests (one from installer
media, one from its own disk) plus the microtest suite. Each guest gets one physical core and sees
2 CPUs where SMT is on.

1. Boot the laptop from this stick (Secure Boot off).
2. Let it run **at least 12 minutes**. Ten of those are a deliberate `delay` in vm1's script that
   banks #527's window before the reboot arm fires; a shorter run loses #525 and weakens #527.
3. Power off. **Copy `HYPE.LOG`, `VM0.LOG` and `VM1.LOG` off the stick, or rename them** — boot 2
   overwrites them.

**Boot 2 (about 1 minute, optional)** — the suite alone, one VM. Use it when the Alpine guests are
not the point, e.g. re-checking #557 after a fix.

```sh
mv hype.cfg hype-alpine.cfg && mv hype-micro.cfg hype.cfg
mv input input-alpine       && mv input-micro input
```

The second swap matters: with no Alpine guests, `ps2`'s input script must land at `\input\vm0.txt`,
and `input-micro/` is what puts it there. That config also includes `ps2`, which the primary one
cannot (vm0 and vm1's scripts already occupy those indices).

Rehearse this stick before shipping it with:

```sh
tools/560/rehearse-560.sh            # 4 cores x 2 threads, the AMD laptop's real shape
tools/560/rehearse-560.sh 150 8      # 8 single-threaded cores, the sibling-detection fallback
```

It stages this directory exactly as `stage.sh` does, boots it, and prints the three things that
decide whether the stick is worth a cold boot: VMs dropped, the placement lines, and whether both
input scripts armed.

Use the explicit SMT layout, not plain `SMP=8`: `-smp 8` gives eight **single-threaded** cores, so
the SMT placement path never runs and a config that fits only because of #560 appears to fit for
the wrong reason. Both layouts are worth running — the config is designed to fit either way.

The logs are on the stick because this machine has no serial port. They contain invalid UTF-8, so
read them with `LC_ALL=C grep -a`.

## Why only three VMs

**A vCPU is a physical core** (#564). `vcpus = N` costs exactly N cores on any host, with the BSP's
core reserved — so the budget is simply:

| machine | logical | physical | RAM | max vCPUs |
|---|---|---|---|---|
| AMD laptop | 8 | **4** | 12 GB | **3** |
| Intel i5-13420H | 12 | 8 | 8 GB | 7 |
| Ryzen 5950X | 32 | 16 | — | 15 |

This stick is 1 + 1 + 1 = **three cores, exactly the AMD laptop's budget**.

**SMT is a bonus, not extra vCPUs.** Each Alpine asks for one core and its guest still reports
**2 CPUs** on this laptop, so #527's bar (`Brought up 1 node, 2 CPUs`) is met — at a third of what
the same evidence used to cost. With SMT off in the BIOS each would report 1 CPU and the config
would **still fit**, which is the point of pricing in cores: this file cannot stop fitting because
of someone's firmware setting.

Which case happened is in the log:

```
fw-1 SMP: vm0 granted 1 whole physical core(s) -> 2 logical CPU(s), 2 thread(s)/core (SMT bonus)
fw-1 SMP: 3 whole physical core(s) granted -> 6 logical CPU(s) ... siblings known
```

`siblings UNPROVEN (one thread per core)` is the #378 fallback, and is not a failure — the guests
simply lose the bonus.

The first version of this stick listed seven VMs. The AMD run dropped four of them — including both
High-priority reproductions it existed to carry — and RAM was never close (pool 7012 of 10165 MiB
usable). So the layout is two Alpine guests plus **one VM running the suite kernel** (#554), which
runs the microtests in turn inside a single guest.

## What this is trying to settle

| VM | Ticket | What to look for |
|---|---|---|
| `vm0` alpine | **#527** | `fw-1 SMP: vm0 granted 1 whole physical core(s) -> 2 logical CPU(s)`, then both APs `live=1`, AP exits in the millions, the guest's own `Brought up 1 node, 2 CPUs`, `VMCSRELOAD ... steals=0`, entry failures 0, sustained past 8 min. **Do not look for `fw-1: vm0 vcpus 2`** — the ticket's original gate names a line hype does not emit for a one-core config |
| `vm0`,`vm1` | **#526** | how many `soft lockup` lines appear. The nested rig gives 2 per 240 s; the AMD baseline from the last run was **zero** |
| `vm0`,`vm1` | **#461** | watch-only: a host `#GP (vector 13)` with RIP inside the AP trampoline |
| `vm1` | **#120** | `vm1 boot=disk -- no installer media attached`, then the guest reaching a login prompt. It booted its own disk with nothing to stream from |
| `vm1` | **#525** | after the 10-minute delay: `HAVE-TASKSET`, then a `0xcf9` write **naming vCPU 1**, vm1 restarting, and `VM1-REBOOTED-up<small>` + `VM1-RENPROC-2` from the fresh boot. `NO-TASKSET` means the arm did not run |
| `vm2` suite | **#580** | `intdeliver`: `at_hlt=` must be **0** and `past_hlt=` must equal the tick count. Fixed in 22997ce; this is the bare-metal confirmation of its bar ("a real guest still boots on both vendors") |
| `vm2` suite | **#557** | the **`PITROUTE`** line, not the verdict. `edges=` is the denominator this ticket lacked; `coalesced=` accounts for the PIC's one-bit IRR merging edges a guest is not acknowledging. A guest served by the 8259 shows `pic_delivered` climbing; one genuinely starved shows `pic_delivered=0` **with `coalesced=0`**, and `mIMR`/`mISR` on the same line say why |
| `vm2` suite | **#552** | `cpumsr`: `vmx = absent` |
| `vm2` suite | — | `ram1` (page aliasing in the real nested tables), `pci` (ECAM/BAR/MSI), `fwcfg` (PIO vs DMA agreement), `pausespin` (preemption + invariant TSC) |

### Reading the suite

Each member is announced **before** it runs:

```
MICRO RUN: ram1
MICRO PASS: ram1
...
MICRO SUITE: ran=8 passed=8 failed=0 noverdict=0 skipped=0 unknown=0
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
LC_ALL=C grep -a -iE "0xcf9|reset" HYPE.LOG | head           # #525 -- must NAME a vCPU
LC_ALL=C grep -ac "0xCF9" HYPE.LOG                          # #525 "exactly once" -> must be 1
LC_ALL=C grep -ac "vm1 restarted (M8-4)" HYPE.LOG           # #525 "exactly once" -> must be 1
LC_ALL=C grep -a -E "VM1-REBOOTED-up|VM1-RENPROC" VM1.LOG   # #525 the guest came BACK
LC_ALL=C grep -a "boot=disk" HYPE.LOG                       # #120
LC_ALL=C grep -a -E "resumes past HLT|TSC (rate|cycles)" HYPE.LOG   # #553, #555
```

Expected results, so a real regression is not lost in the noise: **all 8 PASS.** That is a change —
this section used to say `pflash` FAILS (#556) and `intdeliver` may FAIL (#557), and both are out of
date: #556 is closed, and the QEMU rehearsal of this exact stick now reports

```
MICRO SUITE: ran=8 passed=8 failed=0 noverdict=0 skipped=0 unknown=0
```

Leaving "expected to fail" text in place after the failure is gone is worse than having none: a real
`pflash` regression would be dismissed as expected, and 8/8 would read as suspicious. **Any FAIL,
any missing verdict, or a missing `MICRO SUITE:` summary is now new.**

`intdeliver` passing here says nothing about #557, which is a BARE-METAL ticket — the same binary
and the same test got 38M HLT exits on the last AMD run. Read its `PITROUTE` line, not its verdict.

**A missing microtest verdict is a failure, not an absence of news** — a guest that wedges or
triple-faults prints neither PASS nor FAIL. One verdict is still not the interesting part:
`intdeliver`'s `PITROUTE` funnel and its `at_hlt`/`past_hlt` split are the measurement, and it can
PASS while carrying the answer to #557 either way.

## If the run comes up short

- **A 113-byte log with no `hype: build` banner** is #371: roughly one boot in four never reaches
  hype at all. It is neither a pass nor a failure — just re-run.
- **`WILL NOT RUN` lines** mean admission capped the count. That is working as designed; the VMs
  lost are the ones lowest in the table, and the log names them with real numbers.
