#ifndef HYPE_CFG_H
#define HYPE_CFG_H

#include <stdint.h>

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
 *   boot = installer         ; installer | disk
 *   install_media = \EFI\hype\win11.iso   ; required when boot=installer
 *   target_disk = file:\hype\disks\win11.img   ; file:<path> | physical:<id>
 *   target_disk_size_gb = 128            ; optional, only for new file: targets
 *   firmware = uefi          ; uefi | legacy
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
#define HYPE_CFG_NAME_MAX 32
#define HYPE_CFG_PATH_MAX 256
#define HYPE_CFG_MAX_CPUS 256
#define HYPE_CFG_MAX_PEERS 8

/* #222 (CONFIG-2, spec §4.1): retention limits. A line longer than
 * HYPE_CFG_LINE_MAX cannot be retained verbatim, so it is treated as an overflow
 * (see retained_overflow) rather than silently truncated -- a truncated line
 * written back would corrupt the operator's file. */
/* #222: named [disk.<id>] devices. 16 matches HYPE_CFG_MAX_VMS -- a one-disk-per-VM floor
 * with room for the mixed-bus multi-disk case §5.3 exists for. */
#define HYPE_CFG_MAX_DISKS 16

#define HYPE_CFG_MAX_SECTIONS 32
#define HYPE_CFG_MAX_RETAINED 64
#define HYPE_CFG_LINE_MAX 192

typedef enum {
    HYPE_CFG_BOOT_INSTALLER,
    HYPE_CFG_BOOT_DISK
} hype_cfg_boot_t;

typedef enum {
    HYPE_CFG_FW_UEFI,
    HYPE_CFG_FW_LEGACY
} hype_cfg_firmware_t;

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

    unsigned int vcpus;

    int has_cpu_set;
    unsigned int cpu_set[HYPE_CFG_MAX_CPUS];
    unsigned int cpu_set_count;

    unsigned int mem_mb;

    hype_cfg_boot_t boot;

    int has_install_media;
    char install_media[HYPE_CFG_PATH_MAX];

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

    hype_cfg_net_mode_t net_mode; /* defaults to HYPE_CFG_NET_NONE */

    unsigned int net_peers_count;
    char net_peers[HYPE_CFG_MAX_PEERS][HYPE_CFG_NAME_MAX];
} hype_cfg_vm_t;

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
    HYPE_CFG_BUS_AHCI_ATAPI
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

typedef enum {
    HYPE_CFG_SECTION_VM = 0,
    HYPE_CFG_SECTION_DISK,
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
    hype_cfg_vm_t vms[HYPE_CFG_MAX_VMS];
    unsigned int vm_count;

    /* #222 (§5.3): named devices, referenced by VMs via disks =/cdroms =. */
    hype_cfg_disk_t disks[HYPE_CFG_MAX_DISKS];
    unsigned int disk_count;

    /*
     * §4.3: a malformed [disk.*] is REPORTED AND SKIPPED, not fatal -- one bad device must not stop
     * the rest of the config loading, because the alternative is a machine that boots nothing over a
     * typo in a disk nothing may even reference. Counted so the operator is told, since a silently
     * absent disk is how a VM ends up with no boot device for no visible reason.
     */
    unsigned int skipped_disks;

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
hype_cfg_result_t hype_cfg_parse(char *text, hype_cfg_t *out);


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

#endif /* HYPE_CFG_H */
