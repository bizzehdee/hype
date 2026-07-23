#ifndef HYPE_CFG_H
#define HYPE_CFG_H

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

    hype_cfg_target_disk_t target_disk;

    int has_target_disk_size_gb;
    unsigned int target_disk_size_gb;

    hype_cfg_firmware_t firmware;
    hype_cfg_os_hint_t os_hint;

    hype_cfg_net_mode_t net_mode; /* defaults to HYPE_CFG_NET_NONE */

    unsigned int net_peers_count;
    char net_peers[HYPE_CFG_MAX_PEERS][HYPE_CFG_NAME_MAX];
} hype_cfg_vm_t;

typedef struct {
    hype_cfg_vm_t vms[HYPE_CFG_MAX_VMS];
    unsigned int vm_count;
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

#endif /* HYPE_CFG_H */
