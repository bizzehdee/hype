# Validate hype.cfg against the real parser before every physical boot

**Tickets:** #687/#688/#689 (USB-SATA partition sweep, 2026-08-22).

## What happened

Writing `hype.cfg` by hand for a real-hardware run, on a machine that costs a
full cold-boot cycle per attempt, produced four separate mistakes across three
config edits — each one only discovered after a boot, from the log:

- Omitted `firmware = uefi` on a `boot = disk` VM. It's required for any
  non-`kernel`-boot VM (`core/cfg.c`'s `validate_required()`), but the failure
  mode is not a helpful per-key error: since #532 removed hype's old built-in
  default VM, a config that fails to parse at all leaves **zero VMs** and hype
  just idles at the terminal ("no usable \hype.cfg... type 'create'"). A single
  missing key silently downgraded to "nothing runs," not "this VM is wrong."
- Set `source_disk` directly on `[vm.*]`. It only exists on `[disk.*]` — the
  parser didn't error, it RETAINED the line verbatim and ignored it
  (`cfg: 1 line(s) not understood -- RETAINED verbatim, not applied`), which
  only surfaces if you read the parse-summary log line, not the terminal.
- Mixed `target_disk` sugar with an explicit `disks =` list on the same VM.
  These are mutually exclusive (#222) — adding a second disk to a VM already
  using `target_disk` sugar requires converting the whole VM to the explicit
  `disks = a, b` + `[disk.a]`/`[disk.b]` form, not just adding a second key.
- Assumed a disk's resolved `bus` would match a previous successful boot.
  `os_hint = linux` defaults an unspecified `bus` to `virtio-blk`
  (`core/cfg.c:2160`), but a prior working boot of the exact same disk image
  showed an AHCI/SATA device path in the log. The discrepancy was never fully
  explained; the fix was to pin `bus = ahci-sata` explicitly to match the
  proven-working boot, not to keep guessing at the default.
- Assumed the physical drive's serial as `hype.cfg`'s `media_disk`/`source_disk`
  would match `udevadm ID_SERIAL_SHORT`. It does not — hype enumerates a USB
  Mass Storage device by its own SCSI/USB descriptor serial, which is a
  **different string** from what the host kernel's udev layer derives for the
  same physical drive. Get the real value from hype's own log
  (`media: registered host device N = usb serial='...'`) on a first boot,
  never from the host OS's device identity.

## The lesson

- **Build a tiny host-native harness that links the real `core/cfg.c` and
  calls `hype_cfg_parse()` directly**, and run every config edit through it
  before staging it for a physical boot. It catches missing-required-field
  and unknown-key mistakes for free, and can be extended to print resolved
  `bus`/`disks` to catch the rest (see the harness built for this sweep,
  reconstructable in a few minutes: parse a file, print `status`/`line` on
  failure, and for success enumerate each VM's `disks_count` and call
  `hype_cfg_resolve_bus()` per disk).
- A parse status of `HYPE_CFG_ERR_MISSING_REQUIRED` with `line=0` is a
  whole-VM check failing (firmware/boot-media/os_hint-shaped), not a specific
  line — check `validate_required()`'s exact condition order in `core/cfg.c`
  rather than guessing which field is missing.
- When something that worked in a previous boot stops working after an
  unrelated-looking config change, re-check every value the two configs
  actually share (bus, serial, path) rather than assuming only the changed
  lines matter.
- Get a device's real identity from hype's own enumeration log, not from the
  host OS reporting on the same physical hardware — the two do not
  necessarily agree.
