#ifndef HYPE_CFG_H
#define HYPE_CFG_H

#include <stdint.h>
#include "log_level.h"

/*
 * hype.cfg parser (M1-1, plan.md §5). Parses a whole config file already
 * read into memory (how it gets there -- ESP file read via UEFI Simple
 * File System -- is a separate, thin, hardware-facing concern for
 * whatever boot-time code loads it; this module is pure text-in,
 * struct-out so it's fully unit-testable with no UEFI dependency at
 * all).
 *
 * Format, from plan.md §5:
 *   [vm.<name>]
 *   vcpus = 4
 *   cpu_set = 4-7            ; optional; ranges and comma lists both work
 *   mem_mb = 8192
 *   boot = installer         ; installer | disk | kernel
 *   install_media = \EFI\hype\win11.iso   ; required when boot=installer
 *   kernel = \EFI\hype\micro\ram1.bin    ; required when boot=kernel
 *   cmdline = console=ttyS0 acpi=off      ; optional, only for boot=kernel
 *   target_disk = file:\hype\disks\win11.img   ; file:<path> | physical:<id>
 *   target_disk_size_gb = 128            ; optional, only for new file: targets
 *   firmware = uefi          ; uefi | legacy
 *   display = none           ; none | bochs -- an extra Bochs VBE adapter (decision 49);
 *                            ; ramfb is always present regardless
 *   os_hint = windows        ; windows | linux | bsd | none
 *   net_mode = nat           ; none | nat, default none
 *   net_peers = freebsd      ; optional, comma-separated VM names
 *
 * ';' starts a comment (rest of line ignored). Cross-VM validation
 * (cpu_set overlap, target_disk collisions, net_peers referring to a
 * VM that doesn't exist, physical RAM/core budget, ...) is ADM-*'s job,
 * not this parser's -- this only validates that a single file is
 * internally well-formed and each VM's own fields are in-domain.
 */

#define HYPE_CFG_MAX_VMS 16
/* Longer than NAME_MAX because a label is prose ("Windows 11 Workstation"), not an identifier. */
#define HYPE_CFG_LABEL_MAX 64

/*
 * #546: the kernel command line. Deliberately shorter than a kernel's own limit (which
 * cmdline_size in the image's setup header states, typically 2048): HYPE_CFG_LINE_MAX bounds what
 * the write-back serializer can emit as one line, so a longer value could be parsed and then not
 * survive a round trip -- which is #221's own refusal condition, and worse than refusing it here.
 * "cmdline = " is 10 bytes, so this leaves comfortable margin inside a 192-byte line.
 */
#define HYPE_CFG_CMDLINE_MAX 160

#define HYPE_CFG_NAME_MAX 32
#define HYPE_CFG_PATH_MAX 256
#define HYPE_CFG_MAX_CPUS 256
#define HYPE_CFG_MAX_PEERS 8

/* #222 (§5.2): how many devices one VM may attach. Separate from HYPE_CFG_MAX_DISKS (how many may be
 * DEFINED) -- several VMs can reference the same read-only device, and one VM needing all 16 is not
 * the case worth sizing for. boot_order may name every disk and cdrom, hence the sum. */
#define HYPE_CFG_MAX_VM_DISKS 8
#define HYPE_CFG_MAX_BOOT_ORDER (HYPE_CFG_MAX_VM_DISKS * 2)

/* #222 (CONFIG-2, spec §4.1): retention limits. A line longer than
 * HYPE_CFG_LINE_MAX cannot be retained verbatim, so it is treated as an overflow
 * (see retained_overflow) rather than silently truncated -- a truncated line
 * written back would corrupt the operator's file. */
/* #222: named [disk.<id>] devices. 16 matches HYPE_CFG_MAX_VMS -- a one-disk-per-VM floor
 * with room for the mixed-bus multi-disk case §5.3 exists for. */
#define HYPE_CFG_MAX_DISKS 16

/*
 * #583 (§5.5): named network devices and the virtual networks they sit on.
 *
 * 16 NICs matches HYPE_CFG_MAX_DISKS for the same reason -- one per VM plus headroom. 8 switches
 * because a switch is a shared L2 segment, so the useful count is far lower than the device count:
 * the point of §5.5 is several NICs on ONE switch.
 *
 * HYPE_CFG_MAX_VM_NICS is what a single VM may attach, and it is deliberately small. hype presents
 * a guest exactly one NIC today (PCI device 4, #81/#82), so a config asking for four is already
 * ahead of the machine model; admission is where that is refused with the real number rather than
 * here, so the parse stays lossless and the diagnostic names the VM.
 */
#define HYPE_CFG_MAX_NICS 16
#define HYPE_CFG_MAX_SWITCHES 8
#define HYPE_CFG_MAX_VM_NICS 4

#define HYPE_CFG_MAX_SECTIONS 32
/*
 * #567: 256, raised from 64, and the number is CHOSEN rather than doubled by reflex.
 *
 * Comment-only lines are retained, because a lossless serializer must preserve them. 64 is about
 * one screen of comments, so any config with real explanation in it -- which is precisely what the
 * spec's lossless round-trip exists to protect -- overflowed and could never be written back.
 * tools/hwstick/hype.cfg, the config hype ships on its own validation stick, has 68 comment lines:
 * four over, so write-back was permanently unavailable on that stick.
 *
 * Why 256 and not more: it is the largest cap whose WORST-CASE serialization still fits the 64 KiB
 * buffer this header already tells callers to provide. Measured, by parsing a config built to every
 * structural maximum (MAX_RETAINED lines at LINE_MAX-1 chars, 16 VMs, 16 disks, 32 sections) and
 * serializing it:
 *
 *     cap=128  ->  29.1 KiB out,  struct 104.5 KiB
 *     cap=256  ->  53.0 KiB out,  struct 129.0 KiB   <- fits 64 KiB, 11 KiB spare
 *     cap=384  ->  76.9 KiB out,  struct 153.5 KiB   <- would BREAK the 64 KiB guidance
 *
 * Cost: the retained array goes 12.2 KiB -> 49.0 KiB, so hype_cfg_t goes 92.3 KiB -> 129.0 KiB.
 * Every instance is a static, so this is .bss in a UEFI application that carves guest RAM in
 * hundreds of megabytes.
 *
 * 256 is also 3.8x the shipped config, which is the headroom that matters in practice: it holds a
 * config where all 16 VMs and all 16 disks carry a commented block AND a substantial preamble.
 */
#define HYPE_CFG_MAX_RETAINED 256
#define HYPE_CFG_LINE_MAX 192

typedef enum {
    HYPE_CFG_BOOT_INSTALLER,
    HYPE_CFG_BOOT_DISK,
    /*
     * #535: a raw guest kernel image, loaded straight into guest RAM through the Linux
     * x86_64 boot protocol (core/linux_boot.h) with no guest firmware in the path at all.
     * Needs `kernel = <path>` and no storage: such a VM has no disk to boot from by
     * construction, which is why validate_required() waives the storage requirement for it.
     */
    HYPE_CFG_BOOT_KERNEL
} hype_cfg_boot_t;

typedef enum {
    HYPE_CFG_FW_UEFI,
    HYPE_CFG_FW_LEGACY,
    HYPE_CFG_FW_UEFI_SECBOOT /* #432: the SECURE_BOOT_ENABLE OVMF + enrolled varstore */
} hype_cfg_firmware_t;

/*
 * #565 / §10 decision 49: which guest-visible display adapter this VM is offered.
 *
 * `none` (the default) does NOT mean "no display": every VM gets a ramfb surface through fw_cfg
 * (`etc/ramfb`), which needs no PCI device. This key adds the Bochs VBE adapter
 * (PCI 0x1234:0x1111) as a second, independent surface, and it is off by default because a Linux
 * guest with bochs-drm inbox would bind it and move its console there -- away from the ramfb
 * surface hype renders and away from the serial console the scripted-input runner drives. A
 * display device is the thing the operator watches, so it must not appear uninvited.
 */
typedef enum {
    HYPE_CFG_DISPLAY_NONE = 0,
    HYPE_CFG_DISPLAY_BOCHS
} hype_cfg_display_t;

typedef enum {
    HYPE_CFG_OS_WINDOWS,
    HYPE_CFG_OS_LINUX,
    HYPE_CFG_OS_BSD,
    HYPE_CFG_OS_NONE
} hype_cfg_os_hint_t;

typedef enum {
    HYPE_CFG_NET_NONE,
    HYPE_CFG_NET_NAT
} hype_cfg_net_mode_t;

typedef enum {
    HYPE_CFG_DISK_FILE,
    HYPE_CFG_DISK_PHYSICAL
} hype_cfg_disk_kind_t;

typedef struct {
    hype_cfg_disk_kind_t kind;
    char path_or_id[HYPE_CFG_PATH_MAX];
    /* M10-4/#124 physical-target qualifiers (docs/hype-cfg-spec.md §5.3). Only
     * meaningful for kind==PHYSICAL; harmless/ignored for a file target. */
    unsigned int partition;    /* 1-based GPT partition to scope to; 0 = whole disk (default) */
    int allow_overwrite;       /* explicit override of the non-empty-partition-table guard */
} hype_cfg_target_disk_t;

typedef struct {
    char name[HYPE_CFG_NAME_MAX];

    /*
     * #491 (TERM-15): set by hype_cfg_delete_vm(). The entry STAYS in vms[] so every other VM's
     * index -- which the runtime uses as its own -- is undisturbed until the next boot re-parses
     * the written config and compacts naturally. The serializer walks sections[], and delete
     * removes the section, so a deleted VM is never written back; this flag is for the RUNTIME
     * (list/resolve skip it) and for admission-time callers that want to ignore the ghost.
     */
    int deleted;

    /*
     * #357: the human-readable display name from `label = ...` (spec section 5).
     *
     * The spec used `label` in its own worked examples while core/cfg.c did not parse it, so a
     * config copied straight out of the documentation produced "1 line(s) not understood" and the
     * setting silently did nothing. That is the #285/#331/#339 class the codebase keeps calling
     * out: a documented setting that appears accepted and has no effect.
     *
     * Empty when unset. Callers wanting something to show should fall back to `name` (the section
     * id), which is what the dashboard and log lines used before this existed.
     */
    char label[HYPE_CFG_LABEL_MAX];

    unsigned int vcpus;

    int has_cpu_set;
    unsigned int cpu_set[HYPE_CFG_MAX_CPUS];
    unsigned int cpu_set_count;

    unsigned int mem_mb;

    hype_cfg_boot_t boot;

    int has_install_media;
    char install_media[HYPE_CFG_PATH_MAX];

    /*
     * #535: the guest kernel image for boot = kernel. Required for that mode and rejected
     * for the other two -- a config naming both a kernel and an installer ISO is giving two
     * answers to "what does this VM boot", and picking one silently is the #285 class of
     * defect this parser keeps being extended to avoid.
     */
    int has_kernel;
    char kernel[HYPE_CFG_PATH_MAX];

    /*
     * #546: the kernel command line, for boot = kernel only. Optional -- a micro-kernel needs
     * none, and a real Linux needs at least console=ttyS0 to be visible at all. Rejected for the
     * firmware boot modes: they have no kernel to hand it to, and accepting a setting that does
     * nothing is the defect class this parser keeps being extended to avoid (#285/#331/#339/#357).
     */
    int has_cmdline;
    char cmdline[HYPE_CFG_CMDLINE_MAX];

    /*
     * #545: the initramfs for boot = kernel, read through hype's own FS stack like the kernel
     * image, placed high in guest RAM bounded by the image's initrd_addr_max. Optional -- a
     * microtest needs none -- and rejected for the firmware boot modes, same rule as `kernel`
     * and `cmdline`.
     */
    int has_initrd;
    char initrd[HYPE_CFG_PATH_MAX];

    int tpm; /* #433: guest TPM 2.0 (CRB) present */

    /*
     * #323: WHICH host drive the media (and file-backed image) lives on, matched by drive
     * serial/GUID -- the "where" axis to install_media's "which". Optional: unset means
     * auto-detect, which is what hype did exclusively before, so every existing config is
     * unaffected.
     *
     * Never a positional index. §6d already rules that out for `physical:` targets because an
     * index is fragile against cable/port reordering, and the same operator typo picks the wrong
     * drive here -- a read-only source makes a wrong guess confusing rather than destructive, but
     * no easier to diagnose.
     */
    int has_media_disk;
    char media_disk[HYPE_CFG_PATH_MAX];

    hype_cfg_target_disk_t target_disk;

    int has_target_disk_size_gb;
    unsigned int target_disk_size_gb;

    hype_cfg_firmware_t firmware;
    hype_cfg_os_hint_t os_hint;
    hype_cfg_display_t display; /* #565: defaults to HYPE_CFG_DISPLAY_NONE */

    hype_cfg_net_mode_t net_mode; /* defaults to HYPE_CFG_NET_NONE */

    unsigned int net_peers_count;
    char net_peers[HYPE_CFG_MAX_PEERS][HYPE_CFG_NAME_MAX];

    /*
     * #222 (§5.2/§5.4/§5.7): ordered references to [disk.<id>] devices. ORDER IS MEANINGFUL -- it is
     * the order the guest enumerates them, so `disks = sys, data` and `disks = data, sys` are
     * different machines, which is why these are lists and not sets.
     *
     * Ids only; whether each names a device that exists (and of the right `type`) is a CROSS-entity
     * check and therefore admission's job, not the parser's -- the same split target_disk follows.
     */
    unsigned int disks_count;
    char disks[HYPE_CFG_MAX_VM_DISKS][HYPE_CFG_NAME_MAX];

    unsigned int cdroms_count;
    char cdroms[HYPE_CFG_MAX_VM_DISKS][HYPE_CFG_NAME_MAX];

    /*
     * #583 (§5.2/§5.5): ordered network devices, by [nic.<id>] id. 0..N, and zero is a real answer
     * -- a network-less VM has no `nics` key at all.
     *
     * `net_mode`/`net_peers` remain sugar over this (§5.5): `net_mode = nat` is one implicit NIC on
     * an implicit uplink=nat switch. They are NOT merged into this list, because the sugar is what
     * every existing config uses and rewriting it here would change what a write-back emits.
     */
    unsigned int nics_count;
    char nics[HYPE_CFG_MAX_VM_NICS][HYPE_CFG_NAME_MAX];

    /* Which bootable targets BDS tries, in order. Empty means the documented default: cdroms then
     * disks. Entries may name either kind. */
    unsigned int boot_order_count;
    char boot_order[HYPE_CFG_MAX_BOOT_ORDER][HYPE_CFG_NAME_MAX];

    /*
     * #444 (TERM-6): which of the bits below the operator actually WROTE, vs. what the parser
     * defaulted -- so a caller (the dashboard's `config <vm>` query) can tell "vcpus = 1 because
     * you configured it" from "vcpus = 1 because you didn't say". Every field in `hype_cfg_vm_t`
     * already resolves to a valid value once a VM survives validate_required() (see §5.2's
     * required-field table), which is why this had no consumer before now; it existed only as a
     * local parse-time duplicate-key check (`apply_field`'s own `seen` parameter) until TERM-6
     * needed it to outlive the parse.
     */
    unsigned int seen_fields;
    /*
     * #393: the parser's "this VM is invalid" marker, moved out of a parallel
     * int vm_bad[HYPE_CFG_MAX_VMS] stack array so nothing in the parse path is sized by a
     * compile-time VM cap. Internal to parsing; callers have no reason to read it.
     */
    int parse_bad;
} hype_cfg_vm_t;

/*
 * #222/#444: the bits `hype_cfg_vm_t.seen_fields` accumulates, one per `[vm.*]` key -- also used
 * internally by the parser (`apply_field`'s duplicate-key check) and by `validate_required()`.
 * Public so a caller outside cfg.c (TERM-6's config query) can test which fields were explicitly
 * written versus defaulted.
 */
enum {
    HYPE_CFG_F_VCPUS = 1u << 0,
    HYPE_CFG_F_CPU_SET = 1u << 1,
    HYPE_CFG_F_MEM_MB = 1u << 2,
    HYPE_CFG_F_BOOT = 1u << 3,
    HYPE_CFG_F_INSTALL_MEDIA = 1u << 4,
    HYPE_CFG_F_TARGET_DISK = 1u << 5,
    HYPE_CFG_F_TARGET_DISK_SIZE_GB = 1u << 6,
    HYPE_CFG_F_FIRMWARE = 1u << 7,
    HYPE_CFG_F_OS_HINT = 1u << 8,
    HYPE_CFG_F_NET_MODE = 1u << 9,
    HYPE_CFG_F_NET_PEERS = 1u << 10,
    HYPE_CFG_F_PARTITION = 1u << 11,
    HYPE_CFG_F_ALLOW_OVERWRITE = 1u << 12,
    HYPE_CFG_F_MEDIA_DISK = 1u << 13,
    HYPE_CFG_F_DISKS = 1u << 14,
    HYPE_CFG_F_CDROMS = 1u << 15,
    HYPE_CFG_F_BOOT_ORDER = 1u << 16, /* #323 */
    HYPE_CFG_F_LABEL = 1u << 17,      /* #357 */
    HYPE_CFG_F_KERNEL = 1u << 18,     /* #535 */
    HYPE_CFG_F_CMDLINE = 1u << 19,    /* #546 */
    HYPE_CFG_F_DISPLAY = 1u << 20,    /* #565 */
    HYPE_CFG_F_NICS = 1u << 21,       /* #583 */
    HYPE_CFG_F_INITRD = 1u << 22,     /* #545 */
    HYPE_CFG_F_TPM = 1u << 23         /* #433 */
};

/*
 * #222 (CONFIG-2, spec §4.1): the extensibility keystone.
 *
 * An unknown key or an unknown SECTION KIND used to be fatal (HYPE_CFG_ERR_UNKNOWN_KEY), which made
 * the format un-extendable in both directions: a config written by a newer hype could not be read by
 * an older one at all, and -- worse -- the CONFIG-3 serializer (#221) would write back a file with
 * the operator's unrecognised lines silently DELETED. So unknown lines are now non-fatal and
 * RETAINED verbatim, attached to the section they appeared in.
 *
 * `sections` records every section header in FILE ORDER regardless of kind, which is what lets a
 * serializer re-emit `[vm.*]`, `[disk.*]` and unknown sections interleaved as the operator wrote
 * them; `vms[]`/`disks[]` alone cannot express that ordering.
 *
 * Comments are retained the same way. Pure blank lines are not: they carry no information a
 * serializer needs, and retention capacity is finite.
 */

/*
 * #222 (CONFIG-2, spec §5.1): hypervisor-global settings. Every key is optional, and the whole
 * section may be absent -- so the defaults here must reproduce today's behaviour exactly.
 *
 * §4.3: a malformed [hype] falls back to these defaults rather than failing the parse. That is a
 * different resilience rule from [vm.*] on purpose: a bad global cannot make a VM wrong, it can only
 * make hype fall back to what it did before the section existed.
 */
typedef enum {
    HYPE_CFG_VIEW_DASHBOARD = 0,
    HYPE_CFG_VIEW_VM
} hype_cfg_view_t;

typedef enum {
    HYPE_CFG_AUTOSTART_ALL = 0, /* the default: every VM starts, which is what hype does today */
    HYPE_CFG_AUTOSTART_NONE,
    HYPE_CFG_AUTOSTART_LIST
} hype_cfg_autostart_t;

typedef struct {
    unsigned int config_version; /* §4.2; 1 when absent */

    /* Cores hype may dispatch VMs on. Empty means "all cores"; a per-VM cpu_set is a subset of this
     * (validated by admission, not here). */
    int has_host_cpu_budget;
    unsigned int host_cpu_budget[HYPE_CFG_MAX_CPUS];
    unsigned int host_cpu_budget_count;

    hype_cfg_net_mode_t default_net_mode; /* a per-VM net_mode overrides it */

    /*
     * HNET-8 (#405), the static half: hype's OWN address on the physical network, which NAPT needs
     * before it can masquerade anything. plan.md 6e admits a DHCP client as the single sanctioned
     * host endpoint and requires this static path alongside it, "so an operator who does not want
     * even this can turn it off entirely".
     *
     * All three absent means NO UPLINK, and that is a supported configuration rather than a
     * failure: a host running only offline guests must not need an address. It is LOGGED, because a
     * guest whose network silently does not work is the case this project has paid for repeatedly.
     *
     * DHCP is not implemented yet. Until it is, absent means absent -- there is deliberately no
     * `uplink_mode = dhcp` key that would parse and then do nothing, because a config that reads as
     * "networking is configured" while nothing acquires an address is worse than one that says
     * nothing at all.
     */
    int has_uplink;
    uint8_t uplink_ip[4];
    uint8_t uplink_mask[4];
    uint8_t uplink_gateway[4];

    hype_cfg_view_t dashboard_default_view;
    char dashboard_default_vm[HYPE_CFG_NAME_MAX]; /* only meaningful for VIEW_VM */

    hype_cfg_autostart_t autostart;
    unsigned int autostart_count;
    char autostart_vms[HYPE_CFG_MAX_VMS][HYPE_CFG_NAME_MAX]; /* only for AUTOSTART_LIST */

    /*
     * #529 (plan.md section 10 decision 44): there is NO resolution key. It was the one setting
     * that had to be read before ExitBootServices -- applying it goes through the GOP protocol --
     * and decision 37 moves config reading into Phase 1. hype now aims at 1920x1080 on every host
     * and takes the nearest mode the firmware offers (hype_gop_mode_find_nearest), so there is
     * nothing per-machine left to configure. A config still carrying `resolution` is reported by
     * the existing unknown-key path rather than silently ignored.
     */

    /*
     * #429: how many seconds the dashboard's CPU%% column averages over. Always holds a valid
     * value -- hype_globals_defaults() sets it to 1 (§4.3's usual "the default when absent"), and
     * an explicitly-configured 0 is CLAMPED up to 1 rather than triggering the whole-[hype]-
     * section malformed fallback a syntax error would: a 0-second window is out of range, not
     * unparseable, and clamping just this one field is more precise than resetting every global.
     * No `has_*` flag needed, unlike resolution -- every value (including the default) is
     * meaningful and directly usable, there is no "unset" state to distinguish.
     */
    unsigned int cpu_avg_window_secs;

    /*
     * #533: the post-ExitBootServices log level, named not numbered. DEFAULTS TO DEBUG, and every
     * failure to read or understand the config leaves it there -- a host that cannot read its
     * config is the host whose log matters most.
     */
    hype_log_level_t log_level;

    /* Set when the section was present but something in it was rejected: the defaults above apply,
     * and the caller should say so rather than let the operator believe a global took effect. */
    int malformed;
} hype_cfg_hype_t;

/*
 * #222 (CONFIG-2, spec §5.3): a named storage device, decoupled from any VM.
 *
 * Exists so one VM can attach several devices of mixed type and bus (one SATA + two NVMe, five
 * SATA, ...) which the inline `target_disk` sugar cannot express at all -- it describes exactly one
 * disk with an implied bus.
 *
 * Domains are validated here; CROSS-entity checks are not. Whether `disks = a, b` names devices that
 * exist, whether two VMs claim the same physical drive, and whether the bus is one hype can actually
 * present are admission's job (§10), the same split target_disk already follows.
 */
typedef enum {
    HYPE_CFG_DISK_TYPE_DISK = 0,
    HYPE_CFG_DISK_TYPE_CDROM
} hype_cfg_disk_type_t;

typedef enum {
    HYPE_CFG_BACKING_FILE = 0,
    HYPE_CFG_BACKING_PHYSICAL
} hype_cfg_backing_t;

typedef enum {
    HYPE_CFG_FORMAT_RAW = 0,
    HYPE_CFG_FORMAT_QCOW2
} hype_cfg_format_t;

/*
 * HYPE_CFG_BUS_DEFAULT is a real state, not a placeholder for virtio-blk: §5.6 derives the default
 * from the owning VM's os_hint, and a [disk.*] is parsed BEFORE it is known which VM (and therefore
 * which os_hint) attaches it. Collapsing it to a concrete bus at parse time would silently pin every
 * Windows disk to virtio-blk, which Windows cannot boot from without a driver.
 */
typedef enum {
    HYPE_CFG_BUS_DEFAULT = 0,
    HYPE_CFG_BUS_VIRTIO_BLK,
    HYPE_CFG_BUS_AHCI_SATA,
    HYPE_CFG_BUS_NVME,
    HYPE_CFG_BUS_AHCI_ATAPI,
    HYPE_CFG_BUS_USB_MSC /* #593: removable USB mass storage over the guest xHCI (#446) */
} hype_cfg_bus_t;

typedef struct {
    char id[HYPE_CFG_NAME_MAX]; /* the <id> in [disk.<id>] */

    hype_cfg_disk_type_t type;
    hype_cfg_backing_t backing;
    int has_backing;

    /* backing=file: the image/ISO path on a host filesystem. */
    int has_path;
    char path[HYPE_CFG_PATH_MAX];

    /*
     * WHICH host drive holds `path`, by serial/GUID -- the same axis media_disk (#323) added for
     * install_media, and for the same reason: the deployment is hype on a USB stick with images and
     * ISOs on a separate drive (§6d), and a host may have several. Unset means auto-detect.
     *
     * Distinct from id_match, which is this device's OWN identity when backing=physical. Here the
     * device is a file, and this names the drive whose filesystem it lives on.
     */
    int has_source_disk;
    char source_disk[HYPE_CFG_PATH_MAX];

    /*
     * #336: `has_format` is not redundant with `format`. The default is RAW, so without this flag
     * "declared raw" and "said nothing" are the same value -- and under the assertion semantics below
     * that would make every un-annotated qcow2 image a mismatch and refuse it.
     */
    int has_format;
    hype_cfg_format_t format;

    int has_size_gb;
    unsigned int size_gb;

    /* backing=physical: the identity phys_guard (#122/#124) must match against the ENUMERATED drive
     * before any write is armed. */
    int has_id_match;
    char id_match[HYPE_CFG_PATH_MAX];

    unsigned int partition; /* 1-based GPT partition; 0 = whole disk (the `whole` default) */

    hype_cfg_bus_t bus;

    int read_only;
    int has_read_only;

    int allow_overwrite;
} hype_cfg_disk_t;

/*
 * #583 (§5.5): `[switch.<id>] uplink = none | nat`.
 *
 * `none` is a fully private inter-VM LAN; `nat` additionally gives its members outbound WAN through
 * the host NIC (plan.md §6e). Default `none`, because §6e makes isolation the default and a switch
 * that silently reached the internet would be the opposite of what its name promises.
 */
typedef enum {
    HYPE_CFG_UPLINK_NONE = 0,
    HYPE_CFG_UPLINK_NAT
} hype_cfg_uplink_t;

typedef struct {
    char id[HYPE_CFG_NAME_MAX]; /* the <id> in [switch.<id>] */
    hype_cfg_uplink_t uplink;
    int has_uplink;
} hype_cfg_switch_t;

typedef struct {
    char id[HYPE_CFG_NAME_MAX]; /* the <id> in [nic.<id>] */
    /*
     * §5.5: which virtual network this NIC is on. EMPTY is not "unset pending a default" -- it is
     * the documented default and it MEANS something: an implicit private, isolated per-NIC segment.
     * So `has_switch` distinguishes "on switch lan0" from "on its own private segment", and the
     * second is a real configuration rather than a missing value.
     */
    int has_switch;
    char switch_id[HYPE_CFG_NAME_MAX];
    /* Optional explicit MAC. Absent means derived and stable per id -- the forwarding plane
     * identifies a guest by its source address, so a MAC that moved between boots would look like
     * a different guest to every mapping (#81). */
    int has_mac;
    uint8_t mac[6];
} hype_cfg_nic_t;

typedef enum {
    HYPE_CFG_SECTION_VM = 0,
    HYPE_CFG_SECTION_DISK,
    HYPE_CFG_SECTION_HYPE,
    HYPE_CFG_SECTION_NIC,    /* #583 */
    HYPE_CFG_SECTION_SWITCH, /* #583 */
    HYPE_CFG_SECTION_UNKNOWN
} hype_cfg_section_kind_t;

typedef struct {
    hype_cfg_section_kind_t kind;
    char name[HYPE_CFG_NAME_MAX]; /* the part after the '.'; "" when there is none */
    char raw[HYPE_CFG_LINE_MAX];  /* the header line as written, for re-emission */
    int index;                    /* into vms[] (kind==VM) or disks[] (kind==DISK); -1 otherwise */
} hype_cfg_section_t;

typedef struct {
    int section;                  /* index into sections[]; -1 = before any section header */
    char text[HYPE_CFG_LINE_MAX]; /* the line as written, comment included */
} hype_cfg_retained_t;

typedef struct {
    /*
     * #393 (plan.md section 10 decision 33): VM storage is BOUND, not fixed.
     *
     * `vms` points at whatever the caller gave hype_cfg_parse_into(), and `vm_cap` says how
     * many it holds. hype_cfg_parse() binds `vms_default` below, which is a convenience for
     * tests and small configs -- NOT a cap on the hypervisor, which sizes its own storage from
     * a pre-pass count and the host's detected topology. A parser that refused VM #17 was the
     * same silent clamp as the old two-VM firmware array, one layer up.
     */
    hype_cfg_vm_t *vms;
    unsigned int vm_cap;
    unsigned int vm_count;

    /* #222 (§5.1): hypervisor-global settings; defaults applied when the section is absent. */
    hype_cfg_hype_t hype;

    /* #222 (§5.3): named devices, referenced by VMs via disks =/cdroms =. */
    hype_cfg_disk_t disks[HYPE_CFG_MAX_DISKS];
    unsigned int disk_count;

    /* #583 (§5.5): named NICs and switches, referenced by VMs via nics = and by NICs via switch =. */
    hype_cfg_nic_t nics[HYPE_CFG_MAX_NICS];
    unsigned int nic_count;
    hype_cfg_switch_t switches[HYPE_CFG_MAX_SWITCHES];
    unsigned int switch_count;

    /* #393: default VM storage, bound by hype_cfg_parse(). See the note on `vms` above. */
    hype_cfg_vm_t vms_default[HYPE_CFG_MAX_VMS];

    /*
     * §4.3: a malformed [disk.*] is REPORTED AND SKIPPED, not fatal -- one bad device must not stop
     * the rest of the config loading, because the alternative is a machine that boots nothing over a
     * typo in a disk nothing may even reference. Counted so the operator is told, since a silently
     * absent disk is how a VM ends up with no boot device for no visible reason.
     */
    unsigned int skipped_disks;

    /*
     * #583: same §4.3 treatment for a malformed [nic.*] or [switch.*] -- reported and skipped, not
     * fatal. One bad device must not stop the rest of the config loading, and a silently absent NIC
     * is how a VM ends up with no network for no visible reason.
     */
    unsigned int skipped_nics;
    unsigned int skipped_switches;

    /*
     * #341 (§4.3): VMs dropped because something in their section was malformed. The rest of the
     * config still loads -- one typo must not cost every VM.
     *
     * `skipped_vm_name`/`skipped_vm_line` describe the FIRST one, because a VM that vanishes silently
     * is how an operator ends up with a machine that simply is not there, and a bare count does not
     * tell them which. hype's culture (#285, #331, #339) is that a value which looks accepted and does
     * nothing is the failure to avoid; a section that looks accepted and produces no VM is the same
     * thing one level up.
     */
    unsigned int skipped_vms;
    char skipped_vm_name[HYPE_CFG_NAME_MAX];
    unsigned int skipped_vm_line;

    /* #222: see hype_cfg_section_t above. */
    hype_cfg_section_t sections[HYPE_CFG_MAX_SECTIONS];
    unsigned int section_count;

    hype_cfg_retained_t retained[HYPE_CFG_MAX_RETAINED];
    unsigned int retained_count;

    /*
     * Set when a line that SHOULD have been retained could not be (too many lines, too long, or too
     * many sections). Parsing still succeeds -- an operator must not be locked out of booting by a
     * comment -- but a serializer MUST refuse to write this config back, because doing so would
     * delete content it never captured. Losing an operator's config silently is worse than refusing
     * to save.
     */
    int retained_overflow;

    /* Count of lines the parser did not understand, for a single summary log line. */
    unsigned int unknown_count;

    /*
     * #357: the FIRST line that was not understood, and its 1-based number. 0 / "" when there
     * were none.
     *
     * The summary used to report only a count, while telling the operator that "a misspelled key
     * looks exactly like this" -- advice it then made impossible to act on. With a typo in a long
     * config there was nothing to search for. Naming one line is enough: a cascade of unknown
     * lines almost always starts from a single mistake, and the count still says how many
     * followed.
     */
    unsigned int unknown_first_line;
    char unknown_first[HYPE_CFG_LINE_MAX];
} hype_cfg_t;

typedef enum {
    HYPE_CFG_OK = 0,
    HYPE_CFG_ERR_SYNTAX,
    HYPE_CFG_ERR_UNKNOWN_KEY,
    HYPE_CFG_ERR_BAD_VALUE,
    HYPE_CFG_ERR_DUPLICATE_KEY,
    HYPE_CFG_ERR_KEY_BEFORE_SECTION,
    HYPE_CFG_ERR_DUPLICATE_VM_NAME,
    HYPE_CFG_ERR_TOO_MANY_VMS,
    HYPE_CFG_ERR_TOO_MANY_ENTRIES,
    HYPE_CFG_ERR_VALUE_TOO_LONG,
    HYPE_CFG_ERR_MISSING_REQUIRED
} hype_cfg_status_t;

typedef struct {
    hype_cfg_status_t status;
    unsigned int line; /* 1-based; 0 if not applicable (e.g. cross-file checks) */
} hype_cfg_result_t;

/*
 * Parses `text` (NUL-terminated, may be mutated internally as scratch
 * space -- callers that need the original preserved should pass a copy)
 * into *out. On any error, *out's contents are unspecified; check
 * result.status.
 */
/*
 * #393: zero a config AND bind its default VM storage. Any hype_cfg_t built by hand rather
 * than by parsing must go through this -- `vms` is a pointer now, so a memset alone leaves it
 * null and the first cfg->vms[i] write is a fault. Callers that parse do not need it;
 * hype_cfg_parse() binds storage itself.
 */
void hype_cfg_init(hype_cfg_t *out);

hype_cfg_result_t hype_cfg_parse(char *text, hype_cfg_t *out);

/*
 * #393: parse into caller-owned VM storage. `hype_cfg_parse()` is this with the struct's own
 * default array bound, so existing callers are unchanged; hype passes pool storage sized from
 * hype_cfg_count_vms() and refuses at admission, with real numbers, rather than at VM #17.
 */
hype_cfg_result_t hype_cfg_parse_into(char *text, hype_cfg_t *out, hype_cfg_vm_t *vms,
                                      unsigned int vm_cap);

/*
 * #393: how many [vm.*] sections the text declares. Read-only pre-pass, so the caller can size
 * storage before parsing. Counts declarations, not validity -- the parse is what judges those.
 */
unsigned int hype_cfg_count_vms(const char *text);

/*
 * TERM-10 (#486): append a VM to a parsed config so the serializer will emit it.
 *
 * hype_cfg_serialize() walks sections[], not vms[] -- that is how CONFIG-3 preserves comments,
 * unknown keys and the operator's own ordering. A VM appended to vms[] alone therefore has no
 * section to emit, and the write-back silently drops it: measured as a create that started the VM
 * and wrote a hype.cfg without it, so the machine existed until the next boot and then did not.
 *
 * Returns 0 on success, -1 when either the VM array or the section table is full.
 */
int hype_cfg_append_vm(hype_cfg_t *cfg, const hype_cfg_vm_t *vm);

/*
 * #491 (TERM-15): delete a VM from the config -- the write-back half of the terminal's `delete`.
 *
 * Removes from sections[] the VM's own [vm.*] section and every [disk.*]/[nic.*] section the VM
 * references that NO other (non-deleted) VM also references -- admission forbids sharing, but a
 * config can still SAY it, and deleting a section out from under another VM would be the worse
 * failure. [switch.*] sections are never removed: switches are legitimately shared. Retained
 * lines (comments) inside a removed section go with it; retained lines elsewhere keep their
 * anchoring across the compaction.
 *
 * vms[]/disks[]/nics[] are deliberately NOT compacted (see the `deleted` flag's comment): the
 * entry is flagged instead, and section indexes into those arrays stay valid.
 *
 * Returns 0, or -1 for a null cfg, an out-of-range index, or a VM already deleted.
 */
int hype_cfg_delete_vm(hype_cfg_t *cfg, unsigned int vm_index);

/*
 * CONFIG-3 (#221): serialize `cfg` back into `hype.cfg` text, for the GUI/TUI
 * dashboard to persist a runtime edit (mem, vcpus, net, boot=installer->disk,
 * attach/detach a disk, ...) across a host reboot.
 *
 * Lossless round-trip is scoped exactly as docs/hype-cfg-spec.md §8 promises:
 * section order, comments, and unknown keys/sections the parser RETAINED are
 * reproduced verbatim (so an older hype editing a newer one's config, or vice
 * versa, cannot silently drop content it does not understand). Every KNOWN
 * field is re-emitted from its CURRENT struct value in a fixed, canonical
 * form -- not the operator's original spelling/whitespace/notation (e.g. a
 * `host_cpu_budget = 1-6` range is written back as the expanded list
 * `1,2,3,4,5,6`) and not necessarily the original line order relative to that
 * section's own comments, since the parser does not track where among the
 * known keys each comment/unknown line originally sat, only their order
 * relative to EACH OTHER within that section. Re-parsing the output always
 * recovers the same VALUES; it does not guarantee byte-identical text.
 *
 * Refuses to serialize (returns .refused_overflow = 1, `out` left untouched)
 * when `cfg->retained_overflow` is set: that flag means some retained content
 * from the ORIGINAL parse never made it into `cfg` at all (too many
 * lines/sections, or a truncated line), so writing back would silently
 * delete content the parser could not even capture, which is worse than
 * refusing to save. The caller decides what "refuse to save" means at its
 * own layer (dashboard error, keep the in-memory edit unpersisted, ...).
 *
 * `out_cap` bounds the output buffer; `.truncated` is set (and `out`'s
 * content is NOT a valid, complete config -- do not write it to disk) if the
 * serialized text would not fit. Sizing guidance: HYPE_CFG_MAX_SECTIONS
 * sections, HYPE_CFG_MAX_RETAINED retained lines at up to HYPE_CFG_LINE_MAX
 * bytes each, plus every known field of up to HYPE_CFG_MAX_VMS VMs and
 * HYPE_CFG_MAX_DISKS disks -- a 64 KiB buffer comfortably covers the
 * structure's own maximums.
 */
typedef struct {
    unsigned int len;     /* bytes written into `out`, excluding the NUL terminator */
    int truncated;        /* 1 if out_cap was too small; `out`'s content must not be used */
    int refused_overflow; /* 1 if cfg->retained_overflow was set; `out` is untouched */
} hype_cfg_serialize_result_t;

hype_cfg_serialize_result_t hype_cfg_serialize(const hype_cfg_t *cfg, char *out,
                                               unsigned int out_cap);


/*
 * #336: reconcile a DECLARED `format` against the format the image actually is.
 *
 * Decision (plan.md §10 decision 3): sniffing stays authoritative and `format` is an optional
 * ASSERTION. hype identifies qcow2 by header magic plus hype_qcow2_init's full validation -- which is
 * how every other tool does it, cannot mistake a raw image for a qcow2, and lets an operator swap a raw
 * image for a qcow2 one without also remembering to edit hype.cfg. Making the key authoritative would
 * throw that away to guard against a case that already fails loudly (a corrupt qcow2 header makes
 * hype_qcow2_init refuse, it does not silently read as raw).
 *
 * So the key's job is to catch a VM pointed at the WRONG FILE: you said qcow2, the file is raw, and
 * that is much more likely a stale path than a format you wanted converted. Same shape as #331's
 * target_disk_size_gb check.
 *
 * Returns MISMATCH only when a format was actually declared and disagrees. Absent key => AGREES.
 */
typedef enum {
    HYPE_CFG_FORMAT_AGREES = 0,
    HYPE_CFG_FORMAT_MISMATCH_WANTED_QCOW2, /* declared qcow2, the file is raw */
    HYPE_CFG_FORMAT_MISMATCH_WANTED_RAW    /* declared raw, the file is qcow2 */
} hype_cfg_format_check_t;

hype_cfg_format_check_t hype_cfg_check_format(const hype_cfg_disk_t *disk, int sniffed_is_qcow2);

/* The refusal message body, so every caller words it the same way. */
const char *hype_cfg_format_check_str(hype_cfg_format_check_t c);

/*
 * #222 (spec §5.6): the guest-facing bus a device should present on, resolving HYPE_CFG_BUS_DEFAULT
 * against the owning VM's os_hint.
 *
 * A separate call rather than something the parser bakes in, because a [disk.*] is parsed before it
 * is known which VM attaches it -- and the same device may be referenced by VMs with different
 * os_hints, which have different right answers.
 *
 * windows -> ahci-sata: there is no inbox virtio-blk driver, so a virtio system disk is INVISIBLE at
 * Windows install time, while AHCI/SATA is inbox on every supported Windows. linux/bsd/none ->
 * virtio-blk (inbox and fastest). type=cdrom is always ahci-atapi. An explicit bus always wins.
 */
hype_cfg_bus_t hype_cfg_resolve_bus(const hype_cfg_disk_t *disk, hype_cfg_os_hint_t os_hint);

/*
 * #290: resolve the guest RAM size a VM should actually get, in MB.
 *
 * Exists because `mem_mb` was a required, validated, echoed config key that was
 * then ignored -- boot/main.c assigned the VM struct's mem_mb *from* the
 * compile-time default, so the config value was overwritten before use and the
 * echo read as confirmation of a setting that had no effect.
 *
 * The status is the point, not a decoration. Reporting only the number cannot
 * distinguish "your 512 was applied" from "your 512 was clamped to 128" from
 * "your config was ignored and you got the built-in default" -- and it was
 * exactly that ambiguity (an echo that looked like confirmation) which made the
 * original bug survive. The caller is expected to LOG which of these happened.
 *
 * cfg_mem_mb == 0 means "no config value present".
 * Pure; no clock, no allocation, no I/O.
 */
typedef enum {
    HYPE_CFG_RAM_DEFAULTED = 0,   /* no config value -- built-in default applied */
    HYPE_CFG_RAM_APPLIED,         /* config value used exactly as written */
    HYPE_CFG_RAM_CLAMPED_LOW,     /* config value below min_mb; min applied */
    HYPE_CFG_RAM_CLAMPED_HIGH     /* config value above max_mb; max applied */
} hype_cfg_ram_status_t;

/*
 * Writes the MB figure to apply into *out_mb (never NULL-derefs: a NULL out_mb
 * is a no-op returning the status only) and returns which case applied.
 * min_mb/max_mb are the platform's own limits; max_mb must be >= min_mb, and if
 * a caller passes them inverted the floor wins, because booting a too-small
 * guest fails visibly whereas overrunning the address space hole corrupts.
 */
hype_cfg_ram_status_t hype_cfg_resolve_mem_mb(unsigned int cfg_mem_mb, unsigned int default_mb,
                                              unsigned int min_mb, unsigned int max_mb,
                                              unsigned int *out_mb);

/* Human-readable form of the above, for the log line that accompanies it. */
const char *hype_cfg_ram_status_str(hype_cfg_ram_status_t st);

/*
 * SMP-1 (#185): resolve a VM's vCPU count the same way mem_mb is resolved -- one pure
 * function, so the "which source won, and was it clamped" question has one answer and one
 * test, rather than being decided inline at the call site.
 *
 * Reuses hype_cfg_ram_status_t deliberately: the four outcomes (defaulted, applied, clamped
 * low, clamped high) are identical in shape, and inventing a parallel enum would mean a
 * second status_str() to keep in step. `min` is 1 -- a VM with no vCPU is not a VM.
 */
hype_cfg_ram_status_t hype_cfg_resolve_vcpus(unsigned int cfg_vcpus, unsigned int max_vcpus,
                                             unsigned int *out_vcpus);

/*
 * #331: the byte count a `target_disk_size_gb` value declares.
 *
 * GiB, not decimal GB -- the same unit tools/make-disk-image.sh allocates in, so a config and the
 * image it describes agree. That choice is the one thing here actually worth pinning: a GB/GiB
 * mismatch would make every correctly-sized image look wrong by 7%, which is exactly the sort of
 * warning an operator learns to ignore.
 *
 * Returns 0 for a 0 input (the parser already rejects that, so it means "not declared").
 */
uint64_t hype_cfg_size_gb_to_bytes(unsigned int gb);

/*
 * #357: what to CALL this VM on screen and in the log.
 *
 * Parsing `label` was only half the fix. Nothing read it, so a config copied out of the spec still
 * had no visible effect -- which is the exact defect the ticket was filed about, just moved one
 * step later. Every display site must go through here rather than reaching for one field or the
 * other, so "labelled VMs are named by their label" is one decision with one test, not a
 * convention each caller re-implements.
 *
 * Returns the label when set, otherwise the section id. Never NULL, and never an empty string for
 * a parsed VM: the parser rejects both an empty section id and an empty `label =`.
 */
const char *hype_cfg_vm_display_name(const hype_cfg_vm_t *vm);

/*
 * #357: does this VM configure a `target_disk` at all?
 *
 * The VM summary printed `target=file:` unconditionally, so a VM whose storage comes from
 * `disks = a, b` read as a configured-but-EMPTY target -- worse than saying nothing, because an
 * empty path looks like a truncated or failed value. The storage-by-reference case has to be
 * distinguishable from the no-storage case, and neither is a file target.
 */
int hype_cfg_vm_has_target_disk(const hype_cfg_vm_t *vm);

#endif /* HYPE_CFG_H */
