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
Section kinds `hype`, `vm`, `disk`, `nic`, `switch` are defined here; `router`,
`media`, `snapshot`, `profile` are **reserved** for future use (e.g. `router` for
the deferred L3 element) so third-party/experimental keys don't collide. Unknown
kinds are ignored (§4.1).

---

## 5. Sections & fields

### 5.1 `[hype]` — hypervisor-global (optional; all keys optional)

| key | type / domain | default | notes |
|---|---|---|---|
| `config_version` | int | `1` | §4.2 |
| `host_cpu_budget` | cpu-list (`0-3`, `0,1,2`) | all cores | **physical cores** hype may dispatch VMs on (plan.md §5 `cpu_set` is the per-VM subset of this). A listed core is granted whole, so on an SMT host each entry supplies all of that core's hardware threads to the one VM that owns it (plan.md §10 decisions 40, 47) |
| `default_net_mode` | `none` \| `nat` | `none` | per-VM `net_mode` overrides |
| `uplink_ip` | dotted quad | (none) | **hype's own address on the physical network** (HNET-8 #405). NAPT cannot masquerade guests behind a port that has no address, so this is what makes `net_mode = nat` functional |
| `uplink_mask` | dotted quad | (none) | netmask for `uplink_ip` |
| `uplink_gateway` | dotted quad | (none) | next hop for anything off-link |
| `dashboard_default_view` | `dashboard` \| `vm:<name>` | `dashboard` | which view the GOP shows at boot (TERM) |
| `autostart` | `all` \| `none` \| list | `all` | which VMs to Start at boot (plan.md §6h/§9) |
| `shared_overcommit_ratio` | float, `>= 1.0` | `4.0` | max vCPU:thread over-commit ratio for the shared scheduling tier's pool (plan.md §10 decision 39). Admission (§6i) refuses startup if any `cpu_mode = shared` VM is configured while this is `< 1.0` — that would forbid the pool from over-committing at all, which is the tier's whole point |
| `log_level` | `error` \| `warn` \| `info` \| `debug` | `debug` | post-`ExitBootServices` log verbosity (#533). **Defaults to `debug` (everything), and every failure to read or understand the config leaves it there** — a host that cannot read its config is the host whose log matters most, so a broken value must never quiet it. Named, not numbered: a number in a config file is unreadable six months later. Phase 0 always logs in full; it runs before the config exists (plan.md §10 decision 37) and it is short. A panic is never filtered at any level. The level in force is printed before any line it could filter, so a reader can tell a quiet host from a quiet logger |
| `shared_timeslice_us` | int, microseconds | `4000` | LAPIC one-shot preemption-timer slice length for the shared tier (plan.md §10 decision 39, SMP-20). Global only — no per-VM override; a shorter slice for one latency-sensitive shared VM is a plausible future knob, but nothing today asks for it, and it would multiply SMP-20's timing proof surface |

**The three `uplink_*` keys are all-or-nothing** (#405). A partial set — an
address with no gateway, say — parses, but leaves hype with **no** uplink rather
than a half-configured one: a NAT plane that translates packets and has nowhere
to send them is worse than one that plainly is not running. All three absent is a
supported configuration, not a failure; a host running only offline guests needs
no address. Either way the state is logged, because a guest whose network
silently does not work is the case this project has paid for most often.

There is deliberately **no `uplink_mode = dhcp`** key yet. DHCP is #405's other
half and is not implemented, and a key that parsed and then acquired nothing
would read as "networking is configured" while nothing worked — worse than a
config that says nothing. When the DHCP client lands it gets its own key and its
own default.

Note the asymmetry with §5.5's `[nic.*]`: those keys describe the addresses
**guests** use, which hype learns from their own traffic rather than being told.
These describe the address **hype itself** answers to on the physical network,
which nothing can tell it but the operator or a DHCP server.

### 5.2 `[vm.<name>]` — per VM

The section id `<name>` is the VM's **canonical name** (slug `[A-Za-z0-9_-]`,
unique): it is used for cross-references (`net`/`peers`), for the state file, and
as the default display name.

| key | type / domain | default | notes |
|---|---|---|---|
| `label` | free text | the section `<name>` | **human display name surfaced in every management interface** (dashboard NAME column, TUI, future SSH mgmt). Lets a friendly "Windows 11 Workstation" show for id `win11`. |
| `vcpus` | int, **1 .. host physical cores − 1** | `1` | ≥1; counts **PHYSICAL CORES**, not threads (plan.md §10 decision 47). The guest sees `vcpus × threads_per_core` logical CPUs — 2 per core with SMT, 1 without — so SMT is a bonus it was never promised. Admission caps at the cores in `host_cpu_budget` / `cpu_set`, with the BSP's core reserved (§10) |
| `cpu_set` | cpu-list | (unpinned) | optional explicit pin subset of `host_cpu_budget`, naming **physical cores**. Each listed core is granted whole, and since a vCPU *is* a core the entry count must equal `vcpus` — the operator is naming exactly the cores asked for. SMT does not enter it |
| `cpu_mode` | `dedicated` \| `shared` | `dedicated` | scheduling tier (plan.md §3, §10 decision 39). `dedicated` = exclusive 1:1 pinning, no scheduler on the dispatch path. `shared` = time-sliced onto a pooled set of cores, so the host may run more vCPUs than it has cores. A core may never be in both a dedicated `cpu_set` and the shared pool — admission refuses it (§6i). |
| `isolation_group` | free text | the section `<name>` | trust group for core sharing. VMs naming the same group may share cores and SMT siblings freely, with no cross-VM µarch flush. Distrusting groups never occupy one physical core simultaneously, and L1D + IBPB are flushed when a core changes group. Enforced by allocating **whole physical cores** to one group at a time, **not** by disabling SMT (plan.md §10 decision 40) — so the pool's allocation quantum is a core, and many small distrusting groups leave sibling threads idle. Naming one group for mutually-trusting VMs restores full thread density. The default — each VM its own group — is **default-deny**: configuring nothing gives the strict behaviour. Only meaningful with `cpu_mode = shared`. |
| `mem_mb` | int, **1 .. host usable MB** | — (required) | ≥1 MB; admission caps at host RAM (§10) |
| `boot` | `installer` \| `disk` \| `kernel` | `installer` | two-phase (§5.4 / plan.md §6d); `kernel` is the firmware-free direct kernel boot of §5.4b (#535) |
| `kernel` | path | (none) | required when `boot = kernel`, and rejected otherwise — a raw guest kernel image loaded straight into guest RAM (§5.4b, #535) |
| `cmdline` | free text | (none) | kernel command line, `boot = kernel` only and rejected otherwise (§5.4b, #546). Absent, empty and set are three distinct states |
| `initrd` | path | (none) | **`boot = kernel` only** (#545): the initramfs, read through hype's own FS stack like `kernel` and placed as high as the image's `initrd_addr_max` allows. Rejected for the firmware boot modes, same rule as `kernel`/`cmdline` |
| `firmware` | `uefi` \| `legacy` | `uefi` | |
| `firmware` (contd) | `uefi-secboot` | — | **#432**: boots the vendored SECURE_BOOT_ENABLE OVMF with the enrolled varstore (`fw/OVMF_CODE.secboot.fd` + `OVMF_VARS.secboot.fd`, produced by `FW_SECBOOT=1 tools/build-fw.sh` + `tools/enroll-secboot.sh`). Unsigned media is refused by the guest firmware BY DESIGN — a per-VM choice, never a default |
| `os_hint` | `windows`\|`linux`\|`bsd`\|`none` | `none` | drives `bus` defaults (§5.6) **and the NIC frontend** (§5.5, #82): `windows` gets an e1000, everything else virtio-net |
| `disks` | `<disk-id>` list | (empty) | ordered hard disks (`type=disk` `[disk.*]`); **0..N**, mixed bus allowed (§5.7) |
| `cdroms` | `<disk-id>` list | (empty) | ordered optical drives (`type=cdrom` `[disk.*]`); **0..N** (§5.4) |
| `install_media` | path | (none) | sugar: an implicit boot `cdrom` (§5.4) — *which* ISO |
| `media_disk` | serial-or-GUID string | (auto-detect) | *which host drive* `install_media` lives on (§5.4a, #323) |
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
| `source_disk` | serial-or-GUID string | (auto-detect) | `backing=file`: **which host drive** holds `path` (#222/#323). Same axis and same rules as a VM's `media_disk` (§5.4a) — exact match, unidentified drives never match, positional selection refused. Distinct from `id_match`, which is this device's *own* identity when `backing=physical`. |
| `format` | `raw` \| `qcow2` | (detected) | **an assertion, not a selector** (#336). hype identifies the format by header magic + full validation; if this key is given and disagrees with the image, hype **refuses** rather than sniffing on. Omit it and detection decides — which is what lets you swap a raw image for a qcow2 without editing the config. Ignored for cdrom (ISO). |
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

### 5.4a `media_disk` — which host drive the ISO lives on (#323)

`install_media` says *which* ISO; `media_disk` says *which drive it is on*. They
are separate axes because the target deployment (plan.md §6d) is hype on a USB
stick plus a **separate** drive holding the ISOs, and a host may have several
such drives.

- Value: the drive's **serial or GUID**, matched **exactly** against the serial
  host discovery enumerated — the same identity `id_match` uses (§6). A drive
  that reports no serial can never be selected, because selecting it would mean
  guessing which drive was meant.
- **Positional selection (`disk0`, `disk1`, …) is not offered.** Enumeration
  order depends on controller probe order, so a positional value silently means
  a different drive after a hardware or firmware change.
- Omitted (the default, and what every pre-#323 config does): hype searches
  every enumerated drive and uses the first whose filesystem holds the path.
- Set but **not present**: hype **refuses that VM**. It does not fall back to
  another drive — handing a guest media from a drive nobody named is worse than
  not starting. Enforced at startup by `hype_adm_check_media_disk` (§6i) and
  again at resolve time.

The drive named here needs a filesystem hype reads (FAT32, exFAT, or ext2/3/4)
and may be SATA/AHCI, NVMe, or the USB medium hype claims at boot (#326). USB
identity (#340) is the SCSI INQUIRY VPD page 0x80 Unit Serial Number — the same
identity axis as the ATA and NVMe serials — falling back to the USB device
descriptor's iSerialNumber string when page 0x80 is unsupported; the boot log's
`MSC identity` line names which source was used. A stick that reports neither
remains auto-detectable but can never be named — the identity is never
synthesised from port or enumeration position. The ISO is **streamed** from the
drive, never copied into RAM (#322/#326), so ISO size is not bounded by guest or
host memory.

`[disk.<id>]` carries the same axis as `source_disk` (§5.3), added with the
stanza in #222 — so a `[disk.*]` image or ISO can also name the drive it lives
on, not just a VM's `install_media`.

### 5.4b `boot = kernel` — direct kernel boot, no guest firmware (#535)

`boot = kernel` loads the image named by `kernel` into the VM's guest RAM through
the Linux/x86_64 boot protocol (`core/linux_boot.h`) and enters it in long mode at
its 64-bit entry point, with `RSI` pointing at the zero page. No guest firmware
runs: there is no OVMF, no BDS, no boot order, and nothing has touched PCI or the
framebuffer before the guest's first instruction.

The image must be bzImage-shaped — a valid `setup_header` at file offset `0x1F1`
with `boot_flag = 0xAA55`, `header = "HdrS"`, `version >= 2.10` and the
`XLF_KERNEL_64` bit set. A 32-bit-only kernel is unsupported, not degraded
(plan.md: x86_64 guests only).

Such a VM needs no storage and no firmware, so `target_disk`/`disks`/`cdroms` and
`firmware` are **not required** for it — the only mode where that is true. They
stay legal: a kernel VM may be given a disk to exercise the block path. What is
not legal is naming both a `kernel` and an `install_media`, which is two answers to
"what does this VM boot"; that is refused, not resolved by precedence.

A kernel that cannot be loaded — missing file, bad header, payload too large for
the VM's RAM — refuses **that VM** and says why. It is never fatal to the host, so
one bad artifact in a suite config cannot take the other VMs down with it.

### `cmdline` (#546)

`cmdline = <string>` is the kernel command line, and applies to `boot = kernel` only —
the firmware modes have no kernel to hand it to, so the key is a config error there
rather than a silent no-op.

Three states are deliberately distinguishable, because a kernel reads all three
differently:

| config | what the kernel gets |
|---|---|
| no `cmdline` key | `cmd_line_ptr = 0` — no command line at all |
| `cmdline =` | a valid pointer to an empty string — explicitly nothing |
| `cmdline = console=ttyS0` | a pointer to that string |

The value is bounded twice: by `HYPE_CFG_CMDLINE_MAX`, which is kept inside
`HYPE_CFG_LINE_MAX` so the write-back serializer can always emit it as one line, and
by the image's own `cmdline_size` when it declares one — the kernel stating how much
it will read. Exceeding either **refuses that VM and names the limit**; it is never
truncated, because a truncated command line silently means something else
(`console=ttyS0` cut to `console=tty` is a valid, wrong setting).

For a real Linux kernel this is not a convenience. Without `console=ttyS0` the kernel
writes nothing to the UART, and silence is indistinguishable from never having
started — see #545 for the rest of what a real kernel needs.

The originating use is #534's microtest suites: each in-binary self-test guest
becomes its own build artifact, selected by a config rather than compiled into the
hypervisor. The mode itself is general — it is the M3 direct-boot path reaching
the config, not a test-only hook.

### 5.5 `[nic.<id>]` — a network device + `[switch.<id>]` — a virtual network

**Implemented (#583):** parsed, serialized (write-back round-trips), and checked by startup
admission — every `nics` id must name a real `[nic.*]`, every `[nic.*].switch` a real
`[switch.*]`, no two VMs may attach the same `[nic.*]`, and a VM's NIC count is capped at what
hype can present. Sharing a **switch** is deliberately never an error: that is the feature.

**Forwarded (#223):** a switch is now a live L2 segment (plan.md §10 decision 53). Members'
frames bridge with their real MACs — learned unicast to exactly one member, broadcast and
unknown unicast flooded to the rest. `uplink = nat` additionally lets member traffic take the
host-NAT path (hype answers gateway ARP only for addresses no member owns); `uplink = none` is a
fully private inter-VM LAN. A NIC with no `switch =` keeps its own isolated segment — the
default-deny posture is unchanged for everyone not configured onto a switch.

A VM attaches **0..N** NICs via `nics =`. Each `[nic.<id>]`:

| key | type / domain | default | notes |
|---|---|---|---|
| `switch` | `<switch-id>` | an implicit **private, isolated** per-NIC segment | which virtual network this NIC is on |
| `mac` | MAC string | derived (stable per id) | optional explicit MAC |

A `[switch.<id>]` is one **isolated L2 broadcast domain** (NET-6 #223); every NIC
that names it in `switch =` is on the **same shared network** and can communicate
freely (unicast + broadcast + ARP + DHCP). VMs NOT on that switch stay fully
isolated from its members.

| key | type / domain | default | notes |
|---|---|---|---|
| `uplink` | `none` \| `nat` | `none` | `none` = fully private inter-VM LAN; `nat` = members also get outbound WAN via the host NIC (plan.md §6e) |

**Isolation is the default** (§6e): a NIC with no `switch` sits on its own private
segment — put 3 VMs' NICs on the same `[switch.lan0]` and those 3 (and only those
3) share a network. `net_mode`/`net_peers` on `[vm.*]` remain **sugar**
(`net_mode = nat` → one implicit NIC on an implicit `uplink=nat` switch;
`net_peers` → the legacy pairwise point-to-point forward). Zero NICs = no `nics`
(a network-less VM). L3 routing *between* switches is deferred (future NET-7).

**What `net_mode = nat` does today (#81/#82).** It presents the VM one virtual
NIC, on PCI device 4, with a MAC derived from the VM index and stable for the life
of the VM — the forwarding plane identifies a guest by its source address, so a
MAC that moved between boots would look like a different guest to every mapping.

**Which** NIC is derived from `os_hint`, mirroring §5.6's storage split exactly:

| `os_hint` | NIC | why |
|---|---|---|
| `windows` | **e1000** (`8086:100E`, 82540EM, registers in BAR0) | Windows has driven an 82540EM out of the box for twenty years; virtio-net needs virtio-win injected |
| anything else | **virtio-net** (`1AF4:1041`, regions in BAR4) | inbox on Linux and BSD, and the better device |

Derived rather than configured, for §5.6's own reason: each OS has exactly one
sensible answer, so a key's only correct value would be the one hype can work
out. That is the opposite of `display` (decision 49), where the operator
genuinely has a choice. **The MAC is the same either way** — changing `os_hint`
changes which device the guest sees, not who the guest *is* to every conntrack
entry and peer rule.

The default stays `none`, and a VM with `net_mode = none` has **no** virtual NIC
at all, rather than a NIC with no uplink: an offline install must not depend on
the host NIC driver or the NAT path working, and a device the operator did not
ask for must not appear (the same rule as `display`).

Both frontends are deliberately minimal, and the omissions are the same for both:
**no checksum or segmentation offload** (hype would have to perform what it
claimed, and the NAT path already recomputes checksums after rewriting
addresses — advertising an offload hype does not do hands the guest a frame the
wire rejects), **no multicast filter** (hype delivers what the forwarding plane
decided belongs to this guest, so a filter could only discard frames hype had
already decided to deliver), **no VLAN**, and **no MSI-X** (all 24 IO-APIC pins
are allocated — decision 51 — so a single shared legacy line is what the
arrangement supports). virtio-net additionally offers no control queue, and with
it no multiqueue or MAC programming.

Both sit behind one interface (`hype_guest_nic_ops_t`), so everything above the
device — proxy ARP, address learning, the on-link check, NAPT, the peer mailbox —
is written once and does not know which NIC a guest has.

**Where frames go (#83/#84/#85).** hype routes them:

| destination | what happens |
|---|---|
| an ARP request, for anything | hype answers with its own per-VM router MAC (proxy ARP), and learns this guest's MAC+IP from the request. hype is **never told** the guest's subnet, mask or gateway — whatever the guest is configured with resolves to hype |
| another VM's address, pair in `net_peers` | forwarded directly between the two isolated segments. Never touches the physical network, so it works on a host with no uplink at all |
| another VM's address, pair **not** listed | **dropped**, and counted as `DENIED`. This is the default (#84): guests are never reachable from each other by accident |
| an address the guest ARPed for that no VM owns | dropped. The guest said it believed that address was on its link, so it must not be translated onto the physical network |
| anything else | NAPT: source address becomes `uplink_ip`, source port or ICMP identifier is rewritten, checksums fixed. Return traffic is matched against the mapping this guest created — nothing else reaches it |

Neither guest needs an address hype knows about. Two guests only have to agree
with **each other**, which is why the test configs put both on `192.168.77.0/24`
with no mention of that subnet anywhere in `[hype]`.

The per-VM `fw-1 NAT`, `fw-1 PEER` and `fw-1 UPLINK` log lines report every
counter above, including each drop reason separately — "NAT dropped it" is not a
diagnosis.

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
| `vcpus` | 1 .. host **physical core** count | parser ≥1; **admission** caps at the cores in `host_cpu_budget` with the BSP's reserved. A vCPU is a physical core; SMT multiplies what the guest SEES, not what it costs (plan.md §10 decision 47) |
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
vcpus = 4                             ; four PHYSICAL cores; on an SMT host the
                                      ; guest sees eight logical CPUs
cpu_set = 3-6
mem_mb = 8192
boot = installer
install_media = \iso\win11.iso        ; sugar: the boot CD -- which ISO
media_disk = SN-SAMSUNG-980-1TB       ; ...and which host drive it is on (§5.4a)
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

Three VMs sharing one network (opt-in), with WAN uplink — only these three can
see each other; every other VM stays isolated:

```ini
[switch.lab-lan]
uplink = nat                 ; members also reach the WAN via the host NIC

[vm.db]
label = Database
mem_mb = 4096
os_hint = linux
nics = db-eth0
[nic.db-eth0]
switch = lab-lan

[vm.app]
label = App Server
mem_mb = 4096
os_hint = linux
nics = app-eth0
[nic.app-eth0]
switch = lab-lan

[vm.web]
label = Web Frontend
mem_mb = 2048
os_hint = linux
nics = web-eth0
[nic.web-eth0]
switch = lab-lan
```

---

## 10. Validation layers

- **Parser (CONFIG-2):** single-file well-formedness + each field in-domain +
  §4 tolerance. No cross-entity checks.
- **Admission (ADM / §6i):** cross-VM — `cpu_set` within `host_cpu_budget` and
  non-overlapping (or intentionally shared), `vcpus` == the number of cores in
  `cpu_set` (a vCPU is a core), total `mem_mb` ≤ host RAM, `disks`
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

Resolved 2026-08-19 (plan.md §10 decision 47, #564): `cpu_set` and
`host_cpu_budget` name **physical cores**, **`vcpus` also counts physical
cores**, and a listed core is granted whole — so on an SMT host one core is one
vCPU that the guest sees as two logical CPUs. SMT is a bonus, not extra vCPUs.

Superseded: an earlier pass (2026-08-16, #560) read decision 40 as making
`vcpus` count hardware threads. It does not; decision 40 fixes the allocation
quantum only. Any statement that two cores support four vCPUs predates this.

Resolved this pass: one-file + `[hype]` (§2); `bus` defaults per `os_hint`
(§5.5); `install_media` kept, extra optical reserved as `[disk.*] bus=ahci-atapi`
(§5.4); `hype.d/` precedence + GUI-writes-only-`hype.cfg` (§2); runtime state in a
sibling `hype.state` (§11).

Still open (fold in as the implementation firms up): exact `hype.state` format;
how a VM's `vcpus` map onto the SMP milestone's per-vCPU model in detail
(#185+) now that the core/thread units are settled above; and whether
`net_peers` graduates to a richer `[net]` section when NET lands.
