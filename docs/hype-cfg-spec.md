# hype.cfg — configuration specification (DRAFT)

Status: **draft / CONFIG-1 (#220)**. Extends the M1-1 parser (`core/cfg.{c,h}`,
plan.md §5) — it does **not** replace it. This is the contract that the parser
extension (CONFIG-2), the round-trip serializer / GUI write-back (CONFIG-3,
#221), and startup admission (ADM / plan.md §6i) build to.

---

## 1. Scope & goals

`hype.cfg` is the single operator-authored file, sitting on the ESP next to
`hype.efi`, that declares **which VMs exist and how each is configured**, plus
**hypervisor-global settings**. The product UX is: *drop `hype.efi` + `hype.cfg`
+ some ISOs on a FAT drive → the declared VMs come up*.

Design goals, in priority order:

1. **Extensible.** New keys, new value forms, and new section kinds can be added
   over time without breaking older or newer hype builds (§4).
2. **Round-trippable.** The GUI/TUI edits VM config live; those edits are written
   back to `hype.cfg` losslessly, preserving comments and keys this build does
   not understand (§8, CONFIG-3).
3. **Safe by construction.** Nothing in the file is *by itself* sufficient to do
   something destructive (writing a real disk always also requires runtime
   confirmation — §6, plan.md §6d/§10).
4. **Human-editable.** Simple `key = value` / TOML-ish, hand-parsed, no external
   library.

---

## 2. One file, not many (decision)

**v1 is a single `hype.cfg`** containing a `[hype]` master section plus the
`[vm.<name>]` / `[disk.<id>]` sections. Hypervisor-global vs per-VM is a
**section** distinction, not a **file** distinction.

Rationale: best serves the FAT-drop UX; makes write-back (CONFIG-3) a
parse-mutate-reserialize of one file rather than a multi-file merge; matches the
current single-file parser; v1 scale is ≤16 VMs.

**Reserved for later, additive:** an optional drop-in directory (`hype.d/*.cfg`)
for operators who prefer physical per-VM files. Not in v1; the format must not
preclude it (§4). Precedence when it lands: `hype.cfg` is read first, then
`hype.d/*.cfg` in ascending filename order; **last definition wins per key**
(systemd-style), and new sections accumulate. The **GUI/TUI writes only
`hype.cfg`** — never into `hype.d/` (those are operator-managed overrides); an
edit to a VM defined in a drop-in is written as an overriding entry in
`hype.cfg`, which then wins by precedence. Blast-radius isolation that per-VM
files would give is instead provided by **per-section lenient parsing** (§4.3).

---

## 3. Lexical format

- UTF-8 text; `\n` or `\r\n` line endings.
- `[section.name]` opens a section. Section kind is the part before the first
  `.` (`hype`, `vm`, `disk`); the remainder is the instance name.
- `key = value` inside a section. Whitespace around `key`, `=`, `value` trimmed.
- `;` or `#` begins a comment to end of line. Blank lines ignored.
- Value grammar per key (§5): scalar (`8192`), enum (`nat`), path
  (`\iso\win11.iso`), list (`4-7`, `a, b`), or a **prefixed compound**
  (`file:...`, `physical:...`) for extensible typed values.
- Names (`<name>`, `<id>`): `[A-Za-z0-9_-]`, ≤ `HYPE_CFG_NAME_MAX`.

---

## 4. Extensibility & compatibility (core)

The compatibility contract that makes the format safe to evolve:

### 4.1 Unknown keys and sections are tolerated, never fatal
A key or section kind the running build does not recognize is **ignored with a
warning**, not an error. This lets a newer `hype.cfg` run on an older build
(ignores new keys) and an older file on a newer build (defaults for absent
keys). **This is a change from the current parser**, which returns
`HYPE_CFG_ERR_UNKNOWN_KEY` — CONFIG-2 relaxes it to warn-and-retain (see §8 for
why retain, not drop).

### 4.2 Optional versioning
`[hype] config_version = N` (default `1` when absent). The parser keys behavioral
differences off it only when a genuinely incompatible change is unavoidable; the
default path assumes latest. Bumping it is a last resort — §4.1 handles most
evolution without a version bump.

### 4.3 Per-section resilience
A malformed `[vm.<name>]` (or `[disk.<id>]`) is reported and **skipped**, leaving
the other VMs loadable — the isolation benefit separate per-VM files would give,
without the multi-file cost. (A malformed `[hype]` falls back to global
defaults.)

### 4.4 Safe defaults
Every field has a documented default (§5) chosen so omission preserves prior
behavior. New fields must default to their backward-compatible value.

### 4.5 Additive-only evolution
Once shipped, a key's meaning and value domain are frozen; new capability is a
new key or a new value in an enum (readers ignore unknown enum values → fall to
default, per §4.1). Renames/removals require a `config_version` bump + a
migration in the serializer.

### 4.6 Reserved namespaces
Section kinds `hype`, `vm`, `disk` are defined here; `net`, `media`, `snapshot`,
`profile` are **reserved** for future use so third-party/experimental keys don't
collide. Unknown kinds are ignored (§4.1).

---

## 5. Sections & fields

### 5.1 `[hype]` — hypervisor-global (optional; all keys optional)

| key | type / domain | default | notes |
|---|---|---|---|
| `config_version` | int | `1` | §4.2 |
| `host_cpu_budget` | cpu-list (`0-3`, `0,1,2`) | all cores | cores hype may dispatch VMs on (plan.md §5 `cpu_set` is the per-VM subset of this) |
| `default_net_mode` | `none` \| `nat` | `none` | per-VM `net_mode` overrides |
| `dashboard_default_view` | `dashboard` \| `vm:<name>` | `dashboard` | which view the GOP shows at boot (TERM) |
| `autostart` | `all` \| `none` \| list | `all` | which VMs to Start at boot (plan.md §6h/§9) |

### 5.2 `[vm.<name>]` — per VM

The section id `<name>` is the VM's **canonical name** (slug `[A-Za-z0-9_-]`,
unique): it is used for cross-references (`net`/`peers`), for the state file, and
as the default display name.

| key | type / domain | default | notes |
|---|---|---|---|
| `label` | free text | the section `<name>` | **human display name surfaced in every management interface** (dashboard NAME column, TUI, future SSH mgmt). Lets a friendly "Windows 11 Workstation" show for id `win11`. |
| `vcpus` | int, **1 .. host cores** | `1` | ≥1; admission caps at `host_cpu_budget` size (§10) |
| `cpu_set` | cpu-list | (unpinned) | optional explicit pin subset of `host_cpu_budget` |
| `mem_mb` | int, **1 .. host usable MB** | — (required) | ≥1 MB; admission caps at host RAM (§10) |
| `boot` | `installer` \| `disk` | `installer` | two-phase (§5.4 / plan.md §6d) |
| `firmware` | `uefi` \| `legacy` | `uefi` | |
| `os_hint` | `windows`\|`linux`\|`bsd`\|`none` | `none` | drives `bus` defaults (§5.6) |
| `disks` | `<disk-id>` list | (empty) | ordered hard disks (`type=disk` `[disk.*]`); **0..N**, mixed bus allowed (§5.7) |
| `cdroms` | `<disk-id>` list | (empty) | ordered optical drives (`type=cdrom` `[disk.*]`); **0..N** (§5.4) |
| `install_media` | path | (none) | sugar: an implicit boot `cdrom` (§5.4) |
| `nics` | `<nic-id>` list | (empty) | ordered network devices (`[nic.*]`); **0..N** (§5.5) |
| `boot_order` | device-id list | `cdroms` then `disks` | order BDS tries bootable targets |

A VM may have **zero** disks, **zero** cdroms, and **zero** nics (a diskless
and/or network-less guest is valid). `target_disk`/`target_disk_size_gb` (the
inline single-disk form) stay valid as **sugar** for a one-disk VM (§7).

### 5.3 `[disk.<id>]` — a named storage device (NEW)

Decouples device definitions from VMs so a VM can attach several, of mixed type
and bus (e.g. one SATA + two NVMe, or five SATA, or three NVMe). Each `[disk.*]`
is one device; a VM references them by id in `disks =` / `cdroms =`.

| key | type / domain | default | maps to |
|---|---|---|---|
| `type` | `disk` \| `cdrom` | `disk` | hard disk vs optical drive |
| `backing` | `file` \| `physical` | `file` for cdrom; else required | blk_backend kind (#89) |
| `path` | path | — | `backing=file`: image/ISO path on the ESP/host FS |
| `format` | `raw` \| `qcow2` | `raw` | disk image format (#199 raw / #200 qcow2); ignored for cdrom (ISO) |
| `size_gb` | int | — | `type=disk backing=file`: create at this size if absent |
| `id_match` | serial-or-GUID string | — | `backing=physical`: identity phys_guard requires (#122/#124) |
| `partition` | int (1-based) \| `whole` | `whole` | `backing=physical`: GPT partition vs whole disk |
| `bus` | disk: `virtio-blk`\|`ahci-sata`\|`nvme`; cdrom: `ahci-atapi` | disk: per `os_hint` (§5.6); cdrom: `ahci-atapi` | guest-facing front-end (#196/#202) |
| `read_only` | bool | `false` (disk); always `true` (cdrom) | |
| `allow_overwrite` | bool | `false` | `backing=physical`: explicit per-disk override of the non-empty-table guard (#124/#195). Still ALSO needs runtime confirm (§6). |

### 5.4 Boot media / optical (CD/DVD)

Optical drives are first-class `[disk.<id>] type=cdrom` devices; a VM attaches
**any number** via `cdroms =` (`bus=ahci-atapi`, read-only, `backing=file` ISO).
`install_media` stays as **sugar** for the common single-installer case: it
creates one implicit boot cdrom and places it first in `boot_order`
(maps to the per-VM ISO backing, #140). Use explicit `cdroms =` when a VM needs
more than one (e.g. an installer ISO + a Windows storage-driver ISO).

### 5.5 `[nic.<id>]` — a network device (NEW)

A VM attaches **0..N** NICs via `nics =`. Each `[nic.<id>]`:

| key | type / domain | default | notes |
|---|---|---|---|
| `mode` | `none` \| `nat` | `[hype] default_net_mode` | plan.md §6e |
| `mac` | MAC string | derived (stable per id) | optional explicit MAC |
| `peers` | VM-name list | (none) | guest-to-guest forwarding (plan.md §6e `net_peers`) |

`net_mode`/`net_peers` on `[vm.*]` remain **sugar** for a single implicit NIC
(backward compat). Zero NICs = no `nics` and no `net_mode` (a network-less VM).

### 5.6 `bus` default derivation

When a `type=disk` device's `bus` is not given, it defaults from the owning VM's
`os_hint`: **`windows` → `ahci-sata`** (no inbox virtio-blk — a virtio system
disk is invisible at Windows install; AHCI/SATA is inbox on every supported
Windows), **`linux`/`bsd`/`none` → `virtio-blk`** (inbox + fastest). An explicit
`bus =` always wins. (Until the AHCI-SATA / NVMe guest front-ends land — #202 +
a guest-AHCI-disk ticket — only `virtio-blk` is realizable; the default is
correct for when they exist.)

### 5.7 Resource multiplicity & ranges

| resource | range | enforced |
|---|---|---|
| hard disks / VM (`disks`) | 0 .. `HYPE_CFG_MAX_DISKS_PER_VM` (~24) | parser cap; guest bus limits (AHCI ≤ 32 ports, PCI slots) at admission |
| optical / VM (`cdroms`) | 0 .. `HYPE_CFG_MAX_CDROMS_PER_VM` (~4) | parser cap |
| NICs / VM (`nics`) | 0 .. `HYPE_CFG_MAX_NICS_PER_VM` (~8) | parser cap |
| `vcpus` | 1 .. host core count | parser ≥1; **admission** caps at `host_cpu_budget` |
| `mem_mb` | 1 .. host usable RAM (MB) | parser ≥1; **admission** caps at real free RAM |

The parser accepts any value ≥ the minimum and ≤ the compile cap; the
**host-relative upper bounds** (cores, RAM) are an ADMISSION check (§10), since
they depend on the machine, not the file. VM names/labels flow into the runtime
`hype_fw_vm_t` (replacing today's hardcoded `vm0`/`vm1`) so the configured name
surfaces in the dashboard/TUI — a config→runtime wiring item, tracked with the
parser/admission integration.

---

## 6. Safety (non-negotiable, plan.md §6d/§10)

A `backing=physical` disk entry — even with `allow_overwrite=true` — is **never
by itself** sufficient to write a real drive. At arm time hype calls
`hype_phys_guard_arm` (#124): the config `id_match` must equal the *enumerated*
drive serial/GUID (#122), the non-empty-partition-table guard must pass (or
`allow_overwrite`), **and** the operator must confirm on the dashboard at runtime
(#125). Config supplies inputs to the gate; it can never open it alone.

---

## 7. Backward compatibility with the current inline form

The current single-disk keys remain accepted and mean exactly:
```
target_disk = file:\hype\disks\win11.img   →  an implicit [disk.<vm>sys] { backing=file, path=…, format=raw, bus=virtio-blk }
target_disk = physical:SN-WDC-123           →  { backing=physical, id_match=SN-WDC-123, partition=whole }
target_disk_size_gb = 128                   →  size_gb on that implicit disk
```
A VM may use *either* the inline form *or* `disks =`, not both. The serializer
(CONFIG-3) may normalize inline → `[disk.*]` on the first GUI-initiated rewrite.

---

## 8. Round-trip / write-back (CONFIG-3, #221)

The GUI/TUI edits VM config at runtime (mem, vcpus, net, the
`boot = installer → disk` two-phase flip, attach/detach disks). On save, hype
serializes the model back to `hype.cfg` on the ESP.

**Lossless round-trip is a hard requirement of the compat model (§4.1):** the
serializer MUST preserve comments, section order, and any **unknown keys/sections
retained** from the parse — so editing one VM's `mem_mb` on an *older* build
cannot silently drop a key a *newer* build wrote (or the operator's comments).
This is why §4.1 says *warn-and-retain*, not *warn-and-drop*: the parser keeps
unrecognized lines attached to their section for the serializer to re-emit.

Mechanics: post-EBS ESP writes need the writable-FS work (#198); a pre-EBS-only
write path is the fallback. A write is atomic (write temp + rename, or full
rewrite) so a crash mid-save never truncates the config.

---

## 9. Example (the FAT-drop scenario)

```ini
[hype]
config_version = 1
host_cpu_budget = 1-6         ; leave core 0 for hype's own housekeeping
default_net_mode = nat
autostart = all

; --- a minimal Linux VM: 1 disk, no NICs, no optical ---
[vm.alpine]
label = Alpine Sandbox
vcpus = 1
mem_mb = 2048
boot = disk
os_hint = linux
disks = alpine-sys

[disk.alpine-sys]
backing = file
path = \hype\disks\alpine.img
format = qcow2
size_gb = 8

; --- a Windows VM: mixed disks (1 SATA system + 2 NVMe data), 1 NIC, installer CD ---
[vm.win11]
label = Windows 11 Workstation
vcpus = 4
cpu_set = 3-6
mem_mb = 8192
boot = installer
install_media = \iso\win11.iso        ; sugar: the boot CD
os_hint = windows
disks = win-sys, win-data1, win-data2  ; SATA + NVMe + NVMe, enumerated in this order
nics = win-net0

[disk.win-sys]
backing = file
path = \hype\disks\win-sys.img
size_gb = 128
bus = ahci-sata                        ; (default for os_hint=windows anyway)

[disk.win-data1]
backing = file
path = \hype\disks\win-data1.img
size_gb = 512
bus = nvme

[disk.win-data2]
backing = file
path = \hype\disks\win-data2.img
size_gb = 512
bus = nvme

[nic.win-net0]
mode = nat

; --- a diskless, network-less compute node: 0 disks, 0 cdroms, 0 NICs ---
[vm.compute]
label = Compute Node
vcpus = 8
mem_mb = 16384
boot = installer
install_media = \iso\alpine-standard.iso
os_hint = linux
```

---

## 10. Validation layers

- **Parser (CONFIG-2):** single-file well-formedness + each field in-domain +
  §4 tolerance. No cross-entity checks.
- **Admission (ADM / §6i):** cross-VM — `cpu_set` within `host_cpu_budget` and
  non-overlapping (or intentionally shared), total `mem_mb` ≤ host RAM, `disks`
  reference existing `[disk.*]`, no two VMs claim the same physical drive /
  partition, `net_peers` reference real VMs.

---

## 11. Operator config vs runtime state (decision)

`hype.cfg` is **operator/GUI-authored configuration only**. Hype-**written**
runtime state — which VMs were running at shutdown and their lifecycle state, for
the auto-Start-on-boot cycle (plan.md §6h/§9) — lives in a **sibling
`hype.state`** file on the ESP, NOT in `hype.cfg` and NOT in a `[snapshot.*]`
section.

Rationale: keeping machine-written volatile state out of the hand-edited config
avoids the state writer and the GUI write-back (§8) fighting over the same file
(and risking a clobber of operator edits), and keeps `hype.cfg` round-trip clean
(only config churns it, not per-boot state). `hype.state` is hype-owned, has no
round-trip/comment-preservation obligation, and can use whatever compact format
suits it.

## 12. Resolved / remaining

Resolved this pass: one-file + `[hype]` (§2); `bus` defaults per `os_hint`
(§5.5); `install_media` kept, extra optical reserved as `[disk.*] bus=ahci-atapi`
(§5.4); `hype.d/` precedence + GUI-writes-only-`hype.cfg` (§2); runtime state in a
sibling `hype.state` (§11).

Still open (fold in as the implementation firms up): exact `hype.state` format;
whether `cpu_set`/`vcpus` interact with the SMP milestone's per-vCPU model
(#185+); and whether `net_peers` graduates to a richer `[net]` section when NET
lands.
