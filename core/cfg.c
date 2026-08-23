#include "cfg.h"
#include "format.h"
#include "strutil.h"

/* Parses a boolean value: true/false, yes/no, on/off, 1/0. */
static hype_cfg_status_t parse_bool_field(const char *val, int *out) {
    if (hype_streq(val, "true") || hype_streq(val, "yes") || hype_streq(val, "on") ||
        hype_streq(val, "1")) {
        *out = 1;
        return HYPE_CFG_OK;
    }
    if (hype_streq(val, "false") || hype_streq(val, "no") || hype_streq(val, "off") ||
        hype_streq(val, "0")) {
        *out = 0;
        return HYPE_CFG_OK;
    }
    return HYPE_CFG_ERR_BAD_VALUE;
}

static char *find_char(char *s, char c) {
    while (*s && *s != c) {
        s++;
    }
    return s;
}

/*
 * Truncates at the first comment character (to end of line), then trims.
 *
 * #357: spec section 3 says "`;` or `#` begins a comment to end of line", and only ';' was
 * implemented. That is the same documented-but-absent defect as `label`, with a far worse failure:
 * a '#' comment is parsed as content, and a '#' comment ABOVE the first section is therefore a key
 * before any section -- a hard parse error that makes hype discard the WHOLE config and fall back
 * to built-in defaults. '#' is the more common comment character of the two, and the first line of
 * a file is the most likely place to write one.
 *
 * Whichever comes first wins, so a ';' inside a '#' comment and vice versa behave as documented.
 * The verbatim retention snapshot for lossless write-back (#220/#221) is taken BEFORE this runs,
 * so stripped comments are still preserved.
 */
static char *clean_line(char *line) {
    char *semi = find_char(line, ';');
    char *hash = find_char(line, '#');
    *((hash < semi) ? hash : semi) = '\0';
    return hype_str_trim(line);
}

static void zero_vm(hype_cfg_vm_t *vm) {
    unsigned char *b = (unsigned char *)vm;
    unsigned long long i;
    for (i = 0; i < sizeof(*vm); i++) {
        b[i] = 0;
    }
    vm->net_mode = HYPE_CFG_NET_NONE;
}

static hype_cfg_status_t parse_target_disk(char *val, hype_cfg_target_disk_t *td) {
    const char *prefix;
    hype_cfg_disk_kind_t kind;
    unsigned long long prefix_len;
    unsigned long long len;

    if (hype_strneq(val, "file:", 5)) {
        prefix = "file:";
        kind = HYPE_CFG_DISK_FILE;
    } else if (hype_strneq(val, "physical:", 9)) {
        prefix = "physical:";
        kind = HYPE_CFG_DISK_PHYSICAL;
    } else {
        return HYPE_CFG_ERR_BAD_VALUE;
    }

    prefix_len = hype_strlen(prefix);
    len = hype_strlcpy(td->path_or_id, val + prefix_len, HYPE_CFG_PATH_MAX);
    if (len >= HYPE_CFG_PATH_MAX) {
        return HYPE_CFG_ERR_VALUE_TOO_LONG;
    }
    if (td->path_or_id[0] == '\0') {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    td->kind = kind;
    return HYPE_CFG_OK;
}


/*
 * #222: the cpu-list destination, so `[hype] host_cpu_budget` reuses this exact range/list grammar
 * (`0-3`, `0,1,2`) rather than growing a second parser that would inevitably diverge on the edges.
 * Same single-threaded, one-key-at-a-time reasoning as g_list_dst below.
 */
static unsigned int *g_cpu_dst;
static unsigned int *g_cpu_dst_count;

static int cpu_dst_contains(unsigned int core) {
    unsigned int i;
    for (i = 0; i < *g_cpu_dst_count; i++) {
        if (g_cpu_dst[i] == core) {
            return 1;
        }
    }
    return 0;
}

static hype_cfg_status_t cpu_set_add(hype_cfg_vm_t *vm, unsigned int core) {
    (void)vm;
    if (cpu_dst_contains(core)) {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    if (*g_cpu_dst_count >= HYPE_CFG_MAX_CPUS) {
        return HYPE_CFG_ERR_TOO_MANY_ENTRIES;
    }
    g_cpu_dst[(*g_cpu_dst_count)++] = core;
    return HYPE_CFG_OK;
}

/* Splits `val` on ',' in place, trims each piece, and hands it to `handle_one`. */
static hype_cfg_status_t for_each_comma_piece(char *val, hype_cfg_vm_t *vm,
                                               hype_cfg_status_t (*handle_one)(char *, hype_cfg_vm_t *)) {
    char *tok = val;

    if (*val == '\0') {
        return HYPE_CFG_ERR_BAD_VALUE;
    }

    for (;;) {
        char *comma = find_char(tok, ',');
        int has_more = (*comma == ',');
        hype_cfg_status_t st;

        if (has_more) {
            *comma = '\0';
        }

        st = handle_one(hype_str_trim(tok), vm);
        if (st != HYPE_CFG_OK) {
            return st;
        }

        if (!has_more) {
            break;
        }
        tok = comma + 1;
    }
    return HYPE_CFG_OK;
}

static hype_cfg_status_t cpu_set_piece(char *piece, hype_cfg_vm_t *vm) {
    char *dash = find_char(piece, '-');
    unsigned long long a, b, c;

    if (*dash == '-') {
        *dash = '\0';
        if (hype_parse_uint(hype_str_trim(piece), &a) != 0 ||
            hype_parse_uint(hype_str_trim(dash + 1), &b) != 0) {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        if (a > b || b > 0xFFFFFFFFULL) {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        for (c = a; c <= b; c++) {
            hype_cfg_status_t st = cpu_set_add(vm, (unsigned int)c);
            if (st != HYPE_CFG_OK) {
                return st;
            }
        }
        return HYPE_CFG_OK;
    }

    if (hype_parse_uint(piece, &a) != 0 || a > 0xFFFFFFFFULL) {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    return cpu_set_add(vm, (unsigned int)a);
}

static hype_cfg_status_t parse_cpu_set(char *val, hype_cfg_vm_t *vm) {
    vm->cpu_set_count = 0;
    g_cpu_dst = vm->cpu_set;
    g_cpu_dst_count = &vm->cpu_set_count;
    return for_each_comma_piece(val, vm, cpu_set_piece);
}

static hype_cfg_status_t net_peer_piece(char *name, hype_cfg_vm_t *vm) {
    unsigned long long len;
    unsigned int i;

    if (*name == '\0') {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    for (i = 0; i < vm->net_peers_count; i++) {
        if (hype_streq(vm->net_peers[i], name)) {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
    }
    if (vm->net_peers_count >= HYPE_CFG_MAX_PEERS) {
        return HYPE_CFG_ERR_TOO_MANY_ENTRIES;
    }
    len = hype_strlcpy(vm->net_peers[vm->net_peers_count], name, HYPE_CFG_NAME_MAX);
    if (len >= HYPE_CFG_NAME_MAX) {
        return HYPE_CFG_ERR_VALUE_TOO_LONG;
    }
    vm->net_peers_count++;
    return HYPE_CFG_OK;
}

static hype_cfg_status_t parse_net_peers(char *val, hype_cfg_vm_t *vm) {
    vm->net_peers_count = 0;
    return for_each_comma_piece(val, vm, net_peer_piece);
}

/*
 * #222 (§5.2): the disks/cdroms/boot_order id lists.
 *
 * for_each_comma_piece only passes the VM, so which list is being filled is carried in a file-local
 * pointer. Safe because the parser is single-threaded and strictly one key at a time -- but stated,
 * because a static here would be a bug the moment that stopped being true.
 */
static char (*g_list_dst)[HYPE_CFG_NAME_MAX];
static unsigned int *g_list_count;
static unsigned int g_list_cap;

static hype_cfg_status_t id_list_piece(char *name, hype_cfg_vm_t *vm) {
    unsigned int i;

    (void)vm;
    if (*name == '\0') {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    /* A repeated id would attach the same device twice -- for a writable disk that is two guest
     * devices over one backing store, i.e. guaranteed corruption. Refused here rather than left to
     * admission, because it needs no cross-entity knowledge to see. */
    for (i = 0; i < *g_list_count; i++) {
        if (hype_streq(g_list_dst[i], name)) {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
    }
    if (*g_list_count >= g_list_cap) {
        return HYPE_CFG_ERR_TOO_MANY_ENTRIES;
    }
    if (hype_strlcpy(g_list_dst[*g_list_count], name, HYPE_CFG_NAME_MAX) >= HYPE_CFG_NAME_MAX) {
        return HYPE_CFG_ERR_VALUE_TOO_LONG;
    }
    (*g_list_count)++;
    return HYPE_CFG_OK;
}

static hype_cfg_status_t parse_id_list(char *val, hype_cfg_vm_t *vm,
                                       char (*dst)[HYPE_CFG_NAME_MAX], unsigned int *count,
                                       unsigned int cap) {
    *count = 0;
    g_list_dst = dst;
    g_list_count = count;
    g_list_cap = cap;
    return for_each_comma_piece(val, vm, id_list_piece);
}

static hype_cfg_status_t parse_uint_field(char *val, unsigned int *out) {
    unsigned long long v;
    if (hype_parse_uint(val, &v) != 0 || v == 0 || v > 0xFFFFFFFFULL) {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    *out = (unsigned int)v;
    return HYPE_CFG_OK;
}

static hype_cfg_status_t apply_field(hype_cfg_vm_t *vm, unsigned int *seen, char *key, char *val) {
    if (hype_streq(key, "vcpus")) {
        hype_cfg_status_t st;
        if (*seen & HYPE_CFG_F_VCPUS) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_uint_field(val, &vm->vcpus);
        if (st != HYPE_CFG_OK) return st;
        *seen |= HYPE_CFG_F_VCPUS;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "cpu_set")) {
        hype_cfg_status_t st;
        if (*seen & HYPE_CFG_F_CPU_SET) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_cpu_set(val, vm);
        if (st != HYPE_CFG_OK) return st;
        vm->has_cpu_set = 1;
        *seen |= HYPE_CFG_F_CPU_SET;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "mem_mb")) {
        hype_cfg_status_t st;
        if (*seen & HYPE_CFG_F_MEM_MB) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_uint_field(val, &vm->mem_mb);
        if (st != HYPE_CFG_OK) return st;
        *seen |= HYPE_CFG_F_MEM_MB;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "boot")) {
        if (*seen & HYPE_CFG_F_BOOT) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "installer")) vm->boot = HYPE_CFG_BOOT_INSTALLER;
        else if (hype_streq(val, "disk")) vm->boot = HYPE_CFG_BOOT_DISK;
        else if (hype_streq(val, "kernel")) vm->boot = HYPE_CFG_BOOT_KERNEL; /* #535 */
        else return HYPE_CFG_ERR_BAD_VALUE;
        *seen |= HYPE_CFG_F_BOOT;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "install_media")) {
        unsigned long long len;
        if (*seen & HYPE_CFG_F_INSTALL_MEDIA) return HYPE_CFG_ERR_DUPLICATE_KEY;
        len = hype_strlcpy(vm->install_media, val, HYPE_CFG_PATH_MAX);
        if (len >= HYPE_CFG_PATH_MAX) return HYPE_CFG_ERR_VALUE_TOO_LONG;
        vm->has_install_media = 1;
        *seen |= HYPE_CFG_F_INSTALL_MEDIA;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "kernel")) {
        /* #535: the raw kernel image for boot = kernel. */
        unsigned long long len;
        if (*seen & HYPE_CFG_F_KERNEL) return HYPE_CFG_ERR_DUPLICATE_KEY;
        len = hype_strlcpy(vm->kernel, val, HYPE_CFG_PATH_MAX);
        if (len >= HYPE_CFG_PATH_MAX) return HYPE_CFG_ERR_VALUE_TOO_LONG;
        if (vm->kernel[0] == '\0') return HYPE_CFG_ERR_BAD_VALUE;
        vm->has_kernel = 1;
        *seen |= HYPE_CFG_F_KERNEL;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "cmdline")) {
        /* #546: the kernel command line. An EMPTY value is accepted and means an empty command
         * line -- distinct from the key being absent, which is why has_cmdline exists separately.
         * `cmdline =` is a legitimate way to say "explicitly nothing". */
        unsigned long long len;
        if (*seen & HYPE_CFG_F_CMDLINE) return HYPE_CFG_ERR_DUPLICATE_KEY;
        len = hype_strlcpy(vm->cmdline, val, HYPE_CFG_CMDLINE_MAX);
        if (len >= HYPE_CFG_CMDLINE_MAX) return HYPE_CFG_ERR_VALUE_TOO_LONG;
        vm->has_cmdline = 1;
        *seen |= HYPE_CFG_F_CMDLINE;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "tpm")) {
        /* #433: a guest TPM 2.0. Default off -- most guests neither need nor probe one, and a VM
         * that does not ask should not pay a device window. */
        if (*seen & HYPE_CFG_F_TPM) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "on") || hype_streq(val, "2.0") || hype_streq(val, "crb")) {
            vm->tpm = 1;
        } else if (hype_streq(val, "off") || hype_streq(val, "none")) {
            vm->tpm = 0;
        } else {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        *seen |= HYPE_CFG_F_TPM;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "initrd")) {
        /* #545: the initramfs for boot = kernel. A path, so empty is meaningless (unlike
         * cmdline's legitimate empty): omit the key for "no initrd". */
        unsigned long long len;
        if (*seen & HYPE_CFG_F_INITRD) return HYPE_CFG_ERR_DUPLICATE_KEY;
        len = hype_strlcpy(vm->initrd, val, HYPE_CFG_PATH_MAX);
        if (len >= HYPE_CFG_PATH_MAX) return HYPE_CFG_ERR_VALUE_TOO_LONG;
        if (vm->initrd[0] == '\0') return HYPE_CFG_ERR_BAD_VALUE;
        vm->has_initrd = 1;
        *seen |= HYPE_CFG_F_INITRD;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "media_disk")) {
        /* #323: the drive the media lives on, by serial/GUID. Optional -- absent means
         * auto-detect, preserving the pre-#323 behaviour for every existing config. */
        unsigned long long len;
        if (*seen & HYPE_CFG_F_MEDIA_DISK) return HYPE_CFG_ERR_DUPLICATE_KEY;
        len = hype_strlcpy(vm->media_disk, val, HYPE_CFG_PATH_MAX);
        if (len >= HYPE_CFG_PATH_MAX) return HYPE_CFG_ERR_VALUE_TOO_LONG;
        if (vm->media_disk[0] == '\0') return HYPE_CFG_ERR_BAD_VALUE;
        vm->has_media_disk = 1;
        *seen |= HYPE_CFG_F_MEDIA_DISK;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "disks")) {
        hype_cfg_status_t st;
        if (*seen & HYPE_CFG_F_DISKS) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_id_list(val, vm, vm->disks, &vm->disks_count, HYPE_CFG_MAX_VM_DISKS);
        if (st != HYPE_CFG_OK) return st;
        *seen |= HYPE_CFG_F_DISKS;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "nics")) {
        /* #583 (§5.2): ordered network devices, by [nic.<id>] id. Whether those ids EXIST is
         * admission's question, not the parser's -- the same split `disks` already follows, and it
         * is what keeps a parse lossless when a config names a device defined further down. */
        hype_cfg_status_t st;
        if (*seen & HYPE_CFG_F_NICS) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_id_list(val, vm, vm->nics, &vm->nics_count, HYPE_CFG_MAX_VM_NICS);
        if (st != HYPE_CFG_OK) return st;
        *seen |= HYPE_CFG_F_NICS;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "cdroms")) {
        hype_cfg_status_t st;
        if (*seen & HYPE_CFG_F_CDROMS) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_id_list(val, vm, vm->cdroms, &vm->cdroms_count, HYPE_CFG_MAX_VM_DISKS);
        if (st != HYPE_CFG_OK) return st;
        *seen |= HYPE_CFG_F_CDROMS;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "boot_order")) {
        hype_cfg_status_t st;
        if (*seen & HYPE_CFG_F_BOOT_ORDER) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_id_list(val, vm, vm->boot_order, &vm->boot_order_count,
                           HYPE_CFG_MAX_BOOT_ORDER);
        if (st != HYPE_CFG_OK) return st;
        *seen |= HYPE_CFG_F_BOOT_ORDER;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "target_disk")) {
        hype_cfg_status_t st;
        if (*seen & HYPE_CFG_F_TARGET_DISK) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_target_disk(val, &vm->target_disk);
        if (st != HYPE_CFG_OK) return st;
        *seen |= HYPE_CFG_F_TARGET_DISK;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "target_disk_size_gb")) {
        hype_cfg_status_t st;
        if (*seen & HYPE_CFG_F_TARGET_DISK_SIZE_GB) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_uint_field(val, &vm->target_disk_size_gb);
        if (st != HYPE_CFG_OK) return st;
        vm->has_target_disk_size_gb = 1;
        *seen |= HYPE_CFG_F_TARGET_DISK_SIZE_GB;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "firmware")) {
        if (*seen & HYPE_CFG_F_FIRMWARE) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "uefi")) vm->firmware = HYPE_CFG_FW_UEFI;
        else if (hype_streq(val, "uefi-secboot")) vm->firmware = HYPE_CFG_FW_UEFI_SECBOOT; /* #432 */
        else if (hype_streq(val, "legacy")) vm->firmware = HYPE_CFG_FW_LEGACY;
        else return HYPE_CFG_ERR_BAD_VALUE;
        *seen |= HYPE_CFG_F_FIRMWARE;
        return HYPE_CFG_OK;
    }
    /* #565 / decision 49: an extra Bochs VBE adapter, off unless asked for. */
    if (hype_streq(key, "display")) {
        if (*seen & HYPE_CFG_F_DISPLAY) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "none")) vm->display = HYPE_CFG_DISPLAY_NONE;
        else if (hype_streq(val, "bochs")) vm->display = HYPE_CFG_DISPLAY_BOCHS;
        else return HYPE_CFG_ERR_BAD_VALUE;
        *seen |= HYPE_CFG_F_DISPLAY;
        return HYPE_CFG_OK;
    }
    /*
     * #357: `label` was documented in the spec's own worked examples and not implemented, so a
     * config copied out of the documentation produced "line(s) not understood" and the setting
     * silently did nothing. Accepts any prose; empty is rejected like every other string value,
     * because `label =` with nothing after it is a mistake rather than a way to clear it.
     */
    if (hype_streq(key, "label")) {
        if (*seen & HYPE_CFG_F_LABEL) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_strlcpy(vm->label, val, HYPE_CFG_LABEL_MAX) >= HYPE_CFG_LABEL_MAX) {
            return HYPE_CFG_ERR_VALUE_TOO_LONG;
        }
        if (vm->label[0] == '\0') return HYPE_CFG_ERR_BAD_VALUE;
        *seen |= HYPE_CFG_F_LABEL;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "os_hint")) {
        if (*seen & HYPE_CFG_F_OS_HINT) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "windows")) vm->os_hint = HYPE_CFG_OS_WINDOWS;
        else if (hype_streq(val, "linux")) vm->os_hint = HYPE_CFG_OS_LINUX;
        else if (hype_streq(val, "bsd")) vm->os_hint = HYPE_CFG_OS_BSD;
        else if (hype_streq(val, "none")) vm->os_hint = HYPE_CFG_OS_NONE;
        else return HYPE_CFG_ERR_BAD_VALUE;
        *seen |= HYPE_CFG_F_OS_HINT;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "net_mode")) {
        if (*seen & HYPE_CFG_F_NET_MODE) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "none")) vm->net_mode = HYPE_CFG_NET_NONE;
        else if (hype_streq(val, "nat")) vm->net_mode = HYPE_CFG_NET_NAT;
        else return HYPE_CFG_ERR_BAD_VALUE;
        *seen |= HYPE_CFG_F_NET_MODE;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "net_peers")) {
        hype_cfg_status_t st;
        if (*seen & HYPE_CFG_F_NET_PEERS) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_net_peers(val, vm);
        if (st != HYPE_CFG_OK) return st;
        *seen |= HYPE_CFG_F_NET_PEERS;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "partition")) {
        if (*seen & HYPE_CFG_F_PARTITION) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "whole")) {
            vm->target_disk.partition = 0; /* whole disk */
        } else {
            hype_cfg_status_t st = parse_uint_field(val, &vm->target_disk.partition);
            if (st != HYPE_CFG_OK) return st;
            if (vm->target_disk.partition == 0) return HYPE_CFG_ERR_BAD_VALUE; /* 1-based */
        }
        *seen |= HYPE_CFG_F_PARTITION;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "allow_overwrite")) {
        hype_cfg_status_t st;
        if (*seen & HYPE_CFG_F_ALLOW_OVERWRITE) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_bool_field(val, &vm->target_disk.allow_overwrite);
        if (st != HYPE_CFG_OK) return st;
        *seen |= HYPE_CFG_F_ALLOW_OVERWRITE;
        return HYPE_CFG_OK;
    }
    return HYPE_CFG_ERR_UNKNOWN_KEY;
}

static hype_cfg_status_t validate_required(const hype_cfg_vm_t *vm, unsigned int seen) {
    if (!(seen & HYPE_CFG_F_VCPUS)) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (!(seen & HYPE_CFG_F_MEM_MB)) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (!(seen & HYPE_CFG_F_BOOT)) return HYPE_CFG_ERR_MISSING_REQUIRED;
    /*
     * #222 (§7): a VM says where its storage comes from EITHER inline (target_disk) OR by reference
     * (disks/cdroms) -- never both, since two answers to "which disk" cannot be reconciled and
     * picking one silently would be exactly the #285 failure again.
     */
    if ((seen & HYPE_CFG_F_TARGET_DISK) && ((seen & HYPE_CFG_F_DISKS) || (seen & HYPE_CFG_F_CDROMS))) {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    /*
     * One of the two forms is required. §5.2 does say a VM with zero disks and zero cdroms is valid,
     * and this deliberately does NOT permit that yet: nothing can launch such a VM, and accepting it
     * would turn a misspelled `target_disk` line into a silently disk-less guest -- a config that
     * looks accepted and produces a machine that cannot boot. Revisit with #329, which is what will
     * make an attached-device list mean anything at launch.
     */
    if (vm->boot != HYPE_CFG_BOOT_KERNEL && !(seen & HYPE_CFG_F_TARGET_DISK) &&
        !(seen & HYPE_CFG_F_DISKS) && !(seen & HYPE_CFG_F_CDROMS)) {
        return HYPE_CFG_ERR_MISSING_REQUIRED;
    }
    /*
     * #535: boot = kernel waives storage and firmware, and requires `kernel`.
     *
     * Waived rather than defaulted-and-ignored: such a VM has nothing to boot from a disk and
     * no firmware in its path, so demanding `target_disk` and `firmware` would force every
     * microtest config to carry two lines that do nothing -- which is how a reader learns to
     * stop believing what a config says. The keys stay legal (a kernel VM may still be given a
     * disk to exercise the block path); they are simply no longer required.
     */
    if (vm->boot != HYPE_CFG_BOOT_KERNEL && !(seen & HYPE_CFG_F_FIRMWARE)) {
        return HYPE_CFG_ERR_MISSING_REQUIRED;
    }
    if (!(seen & HYPE_CFG_F_OS_HINT)) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (vm->boot == HYPE_CFG_BOOT_INSTALLER && !(seen & HYPE_CFG_F_INSTALL_MEDIA)) {
        return HYPE_CFG_ERR_MISSING_REQUIRED;
    }
    if (vm->boot == HYPE_CFG_BOOT_KERNEL && !(seen & HYPE_CFG_F_KERNEL)) {
        return HYPE_CFG_ERR_MISSING_REQUIRED;
    }
    /* Two answers to "what does this VM boot" is a contradiction, not a precedence question. */
    if (vm->boot != HYPE_CFG_BOOT_KERNEL && (seen & HYPE_CFG_F_KERNEL)) {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    if (vm->boot == HYPE_CFG_BOOT_KERNEL && (seen & HYPE_CFG_F_INSTALL_MEDIA)) {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    /* #546: a command line with no kernel to give it to. */
    if (vm->boot != HYPE_CFG_BOOT_KERNEL && (seen & HYPE_CFG_F_CMDLINE)) {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    /* #545: same rule for an initrd -- the firmware boot modes have no kernel to hand it to. */
    if (vm->boot != HYPE_CFG_BOOT_KERNEL && (seen & HYPE_CFG_F_INITRD)) {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    return HYPE_CFG_OK;
}

/*
 * #405: dotted-quad IPv4. Returns 0 and fills `out` on success, -1 otherwise.
 *
 * Strict on purpose: exactly four decimal octets, each 0-255, separated by single dots, nothing
 * before or after. A lenient parser here would accept "10.0.2" or "10.0.2.15 " and silently give
 * hype the wrong address, and the symptom of a wrong uplink address is a network that does not work
 * with nothing pointing at the config.
 */
static int parse_ipv4_quad(const char *s, uint8_t out[4]) {
    unsigned int part = 0;
    unsigned int value = 0;
    unsigned int digits = 0;

    if (s == 0) {
        return -1;
    }
    for (;;) {
        if (*s >= '0' && *s <= '9') {
            if (digits >= 3u) {
                return -1; /* more than three digits cannot be an octet */
            }
            value = value * 10u + (unsigned int)(*s - '0');
            digits++;
            s++;
            continue;
        }
        if (digits == 0u) {
            return -1; /* an empty octet: a leading dot, a double dot, or a trailing dot */
        }
        if (value > 255u) {
            return -1;
        }
        if (part >= 4u) {
            return -1;
        }
        out[part] = (uint8_t)value;
        part++;
        value = 0;
        digits = 0;
        if (*s == '.') {
            s++;
            continue;
        }
        if (*s == '\0') {
            break;
        }
        return -1; /* any other trailing character */
    }
    return (part == 4u) ? 0 : -1;
}

/* #222 (§5.1): per-[hype] duplicate-key tracking. */
enum {
    H_CONFIG_VERSION = 1u << 0,
    H_HOST_CPU_BUDGET = 1u << 1,
    H_DEFAULT_NET_MODE = 1u << 2,
    H_DASHBOARD_VIEW = 1u << 3,
    H_AUTOSTART = 1u << 4,
    /* #529: bit 5 was H_RESOLUTION. Left as a gap rather than renumbered -- the values are a
     * duplicate-key bitmask, not a wire format, and shifting them buys nothing. */
    H_CPU_AVG_WINDOW = 1u << 6,
    H_LOG_LEVEL = 1u << 7, /* #533 */
    H_UPLINK_IP = 1u << 8,      /* #405 */
    H_UPLINK_MASK = 1u << 9,
    H_UPLINK_GATEWAY = 1u << 10,
    H_FS_SELFTEST_DISK = 1u << 11 /* #709 */
};

static void hype_globals_defaults(hype_cfg_hype_t *h) {
    unsigned char *p = (unsigned char *)h;
    unsigned long long i;
    for (i = 0; i < sizeof(*h); i++) {
        p[i] = 0;
    }
    /* Zero is the right default for every enum here (NET_NONE, VIEW_DASHBOARD, AUTOSTART_ALL) and
     * for the empty cpu budget meaning "all cores". config_version and cpu_avg_window_secs are
     * the exceptions -- both have a real, non-zero default. */
    h->config_version = 1u;
    h->cpu_avg_window_secs = 1u;
    /*
     * #533: DEBUG, not zero. HYPE_LOG_ERROR is 0, so zeroing this struct would silently make the
     * quietest level the default -- and the default has to be the loudest, because it is what a
     * host with no readable config gets.
     */
    h->log_level = HYPE_LOG_DEBUG;
}

/* `autostart = a, b` -- the list form. `all` / `none` are handled by the caller. */
static hype_cfg_status_t autostart_piece(char *name, hype_cfg_vm_t *vm) {
    (void)vm;
    return id_list_piece(name, vm);
}

static hype_cfg_status_t apply_hype_field(hype_cfg_hype_t *h, unsigned int *seen, const char *key,
                                         char *val) {
    if (hype_streq(key, "config_version")) {
        hype_cfg_status_t st;
        if (*seen & H_CONFIG_VERSION) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_uint_field(val, &h->config_version);
        if (st != HYPE_CFG_OK) return st;
        *seen |= H_CONFIG_VERSION;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "host_cpu_budget")) {
        hype_cfg_status_t st;
        if (*seen & H_HOST_CPU_BUDGET) return HYPE_CFG_ERR_DUPLICATE_KEY;
        h->host_cpu_budget_count = 0;
        g_cpu_dst = h->host_cpu_budget;
        g_cpu_dst_count = &h->host_cpu_budget_count;
        /* Reuses cpu_set's own range/list grammar; the VM argument is unused by the destination. */
        st = for_each_comma_piece(val, (hype_cfg_vm_t *)0, cpu_set_piece);
        if (st != HYPE_CFG_OK) return st;
        h->has_host_cpu_budget = 1;
        *seen |= H_HOST_CPU_BUDGET;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "default_net_mode")) {
        if (*seen & H_DEFAULT_NET_MODE) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "none")) {
            h->default_net_mode = HYPE_CFG_NET_NONE;
        } else if (hype_streq(val, "nat")) {
            h->default_net_mode = HYPE_CFG_NET_NAT;
        } else {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        *seen |= H_DEFAULT_NET_MODE;
        return HYPE_CFG_OK;
    }
    /*
     * #405: hype's own uplink address, statically configured. Parsed here rather than in a
     * [network] section of its own because there is exactly one physical uplink (plan.md 6e: "the
     * one physical NIC the hypervisor owns"), so a section would be a section with one member.
     */
    if (hype_streq(key, "uplink_ip") || hype_streq(key, "uplink_mask") ||
        hype_streq(key, "uplink_gateway")) {
        uint8_t quad[4];
        unsigned int flag;
        uint8_t *dst;

        if (hype_streq(key, "uplink_ip")) {
            flag = H_UPLINK_IP;
            dst = h->uplink_ip;
        } else if (hype_streq(key, "uplink_mask")) {
            flag = H_UPLINK_MASK;
            dst = h->uplink_mask;
        } else {
            flag = H_UPLINK_GATEWAY;
            dst = h->uplink_gateway;
        }
        if (*seen & flag) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (parse_ipv4_quad(val, quad) != 0) return HYPE_CFG_ERR_BAD_VALUE;
        dst[0] = quad[0];
        dst[1] = quad[1];
        dst[2] = quad[2];
        dst[3] = quad[3];
        /*
         * has_uplink is set only once ALL THREE are present. A partial uplink -- an address with no
         * gateway, say -- is not a usable configuration, and treating it as one would produce a NAT
         * plane that translated packets and had nowhere to send them. Requiring the set is the same
         * rule the per-VM keys already follow (#225: all six or the config is ignored).
         */
        h->has_uplink = ((*seen | flag) & (H_UPLINK_IP | H_UPLINK_MASK | H_UPLINK_GATEWAY)) ==
                        (H_UPLINK_IP | H_UPLINK_MASK | H_UPLINK_GATEWAY);
        *seen |= flag;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "dashboard_default_view")) {
        if (*seen & H_DASHBOARD_VIEW) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "dashboard")) {
            h->dashboard_default_view = HYPE_CFG_VIEW_DASHBOARD;
        } else if (hype_strneq(val, "vm:", 3)) {
            /* The VM named here need not exist yet -- sections may come later in the file, and
             * whether it exists at all is a cross-entity question for admission. */
            if (val[3] == '\0') return HYPE_CFG_ERR_BAD_VALUE;
            if (hype_strlcpy(h->dashboard_default_vm, val + 3, HYPE_CFG_NAME_MAX) >=
                HYPE_CFG_NAME_MAX) {
                return HYPE_CFG_ERR_VALUE_TOO_LONG;
            }
            h->dashboard_default_view = HYPE_CFG_VIEW_VM;
        } else {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        *seen |= H_DASHBOARD_VIEW;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "fs_selftest_disk")) {
        /* #709: a disk serial, not a path/index -- same selection convention as `mkdisk` and
         * target_disk.path_or_id. An empty value is meaningless (there is no "unset via empty
         * string" here; absence of the key already means that). */
        if (*seen & H_FS_SELFTEST_DISK) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (val[0] == '\0') return HYPE_CFG_ERR_BAD_VALUE;
        if (hype_strlcpy(h->fs_selftest_disk, val, sizeof h->fs_selftest_disk) >=
            sizeof h->fs_selftest_disk) {
            return HYPE_CFG_ERR_VALUE_TOO_LONG;
        }
        *seen |= H_FS_SELFTEST_DISK;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "autostart")) {
        if (*seen & H_AUTOSTART) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "all")) {
            h->autostart = HYPE_CFG_AUTOSTART_ALL;
        } else if (hype_streq(val, "none")) {
            h->autostart = HYPE_CFG_AUTOSTART_NONE;
        } else {
            hype_cfg_status_t st;
            h->autostart_count = 0;
            g_list_dst = h->autostart_vms;
            g_list_count = &h->autostart_count;
            g_list_cap = HYPE_CFG_MAX_VMS;
            st = for_each_comma_piece(val, (hype_cfg_vm_t *)0, autostart_piece);
            if (st != HYPE_CFG_OK) return st;
            h->autostart = HYPE_CFG_AUTOSTART_LIST;
        }
        *seen |= H_AUTOSTART;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "log_level")) {
        /* #533: an unparseable level leaves DEBUG in place rather than quieting the log, and is
         * still reported as malformed so the operator knows the key did nothing. */
        if (*seen & H_LOG_LEVEL) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_log_level_parse(val, &h->log_level) != 0) {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        *seen |= H_LOG_LEVEL;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "cpu_avg_window_secs")) {
        /*
         * #429: unlike a syntax error (non-numeric, which still falls through to §4.3's whole-
         * section malformed reset below), a successfully-parsed 0 is CLAMPED to 1 rather than
         * rejected -- it is out of range, not unparseable, and the ticket's own requirement is
         * "no less than 1 second", a floor, not a hard refusal.
         */
        /*
         * NOT parse_uint_field(): that helper rejects 0 outright (BAD_VALUE) for the many
         * fields where 0 is always meaningless (vcpus, mem_mb, ...), which would make the
         * clamp below unreachable -- 0 has to actually parse here so it can be clamped
         * instead of falling into the whole-section malformed reset a genuine BAD_VALUE
         * triggers.
         */
        unsigned long long v;
        if (*seen & H_CPU_AVG_WINDOW) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_parse_uint(val, &v) != 0 || v > 0xFFFFFFFFULL) {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        h->cpu_avg_window_secs = (v == 0u) ? 1u : (unsigned int)v;
        *seen |= H_CPU_AVG_WINDOW;
        return HYPE_CFG_OK;
    }
    return HYPE_CFG_ERR_UNKNOWN_KEY;
}

/* #222 (§5.3): per-[disk.*] duplicate-key tracking, independent of the [vm.*] flags above. */
enum {
    D_TYPE = 1u << 0,
    D_BACKING = 1u << 1,
    D_PATH = 1u << 2,
    D_FORMAT = 1u << 3,
    D_SIZE_GB = 1u << 4,
    D_ID_MATCH = 1u << 5,
    D_PARTITION = 1u << 6,
    D_BUS = 1u << 7,
    D_READ_ONLY = 1u << 8,
    D_ALLOW_OVERWRITE = 1u << 9,
    D_SOURCE_DISK = 1u << 10
};

static void zero_disk(hype_cfg_disk_t *d) {
    unsigned char *p = (unsigned char *)d;
    unsigned long long i;
    for (i = 0; i < sizeof(*d); i++) {
        p[i] = 0;
    }
    /* The zero values ARE the documented defaults: type=disk, backing=file, format=raw,
     * partition=0 (whole), bus=DEFAULT (resolved per os_hint, §5.6), read_only=false. */
}

static hype_cfg_status_t apply_disk_str(char *dst, int *has, unsigned int *seen, unsigned int bit,
                                        const char *val) {
    if (*seen & bit) return HYPE_CFG_ERR_DUPLICATE_KEY;
    if (hype_strlcpy(dst, val, HYPE_CFG_PATH_MAX) >= HYPE_CFG_PATH_MAX) {
        return HYPE_CFG_ERR_VALUE_TOO_LONG;
    }
    if (dst[0] == '\0') return HYPE_CFG_ERR_BAD_VALUE;
    *has = 1;
    *seen |= bit;
    return HYPE_CFG_OK;
}

/* #583 (§5.5): the per-section seen-masks, one bit per key, so a duplicate is refused rather than
 * silently letting the later value win. Same shape as the D_* masks below. */
#define N_SWITCH (1u << 0)
#define N_MAC (1u << 1)
#define SW_UPLINK (1u << 0)

static void zero_nic(hype_cfg_nic_t *n) {
    unsigned char *p = (unsigned char *)n;
    unsigned long long i;
    for (i = 0; i < sizeof(*n); i++) p[i] = 0;
    /* Zero IS the documented default: no switch (an implicit private per-NIC segment, §5.5) and no
     * explicit MAC (derived, stable per id). */
}

static void zero_switch(hype_cfg_switch_t *w) {
    unsigned char *p = (unsigned char *)w;
    unsigned long long i;
    for (i = 0; i < sizeof(*w); i++) p[i] = 0;
    /* Zero is HYPE_CFG_UPLINK_NONE -- §6e's isolation-by-default. */
}

/*
 * #583: one MAC, "aa:bb:cc:dd:ee:ff". Hex only, exactly six octets, colon-separated.
 *
 * Strict on purpose. A MAC is an identity the forwarding plane keys guests off (#81), so a
 * half-parsed one that silently became 00:00:00:00:00:00 would make two guests indistinguishable to
 * every mapping -- and a broadcast/multicast source address would be worse. Rejected rather than
 * coerced, and the multicast bit is refused by name.
 */
static hype_cfg_status_t parse_mac(const char *val, uint8_t *out) {
    unsigned int i;
    const char *p = val;
    for (i = 0; i < 6u; i++) {
        unsigned int j, byte = 0u;
        for (j = 0; j < 2u; j++) {
            char c = *p++;
            unsigned int d;
            if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned int)(c - 'a') + 10u;
            else if (c >= 'A' && c <= 'F') d = (unsigned int)(c - 'A') + 10u;
            else return HYPE_CFG_ERR_BAD_VALUE;
            byte = (byte << 4) | d;
        }
        out[i] = (uint8_t)byte;
        if (i < 5u) {
            if (*p++ != ':') return HYPE_CFG_ERR_BAD_VALUE;
        }
    }
    if (*p != '\0') return HYPE_CFG_ERR_BAD_VALUE; /* trailing junk is not a MAC */
    if ((out[0] & 0x01u) != 0u) {
        return HYPE_CFG_ERR_BAD_VALUE; /* multicast/broadcast bit set: not a source address */
    }
    return HYPE_CFG_OK;
}

static hype_cfg_status_t apply_nic_field(hype_cfg_nic_t *n, unsigned int *seen, const char *key,
                                         char *val) {
    if (hype_streq(key, "switch")) {
        if (*seen & N_SWITCH) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (*val == '\0') return HYPE_CFG_ERR_BAD_VALUE;
        if (hype_strlcpy(n->switch_id, val, HYPE_CFG_NAME_MAX) >= HYPE_CFG_NAME_MAX) {
            return HYPE_CFG_ERR_VALUE_TOO_LONG;
        }
        n->has_switch = 1;
        *seen |= N_SWITCH;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "mac")) {
        hype_cfg_status_t st;
        if (*seen & N_MAC) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_mac(val, n->mac);
        if (st != HYPE_CFG_OK) return st;
        n->has_mac = 1;
        *seen |= N_MAC;
        return HYPE_CFG_OK;
    }
    return HYPE_CFG_ERR_UNKNOWN_KEY;
}

static hype_cfg_status_t apply_switch_field(hype_cfg_switch_t *w, unsigned int *seen,
                                            const char *key, char *val) {
    if (hype_streq(key, "uplink")) {
        if (*seen & SW_UPLINK) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "none")) {
            w->uplink = HYPE_CFG_UPLINK_NONE;
        } else if (hype_streq(val, "nat")) {
            w->uplink = HYPE_CFG_UPLINK_NAT;
        } else {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        w->has_uplink = 1;
        *seen |= SW_UPLINK;
        return HYPE_CFG_OK;
    }
    return HYPE_CFG_ERR_UNKNOWN_KEY;
}

static hype_cfg_status_t apply_disk_field(hype_cfg_disk_t *d, unsigned int *seen, const char *key,
                                          char *val) {
    if (hype_streq(key, "type")) {
        if (*seen & D_TYPE) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "disk")) {
            d->type = HYPE_CFG_DISK_TYPE_DISK;
        } else if (hype_streq(val, "cdrom")) {
            d->type = HYPE_CFG_DISK_TYPE_CDROM;
            /* §5.3: a cdrom is always read-only, and the operator does not get to say otherwise --
             * a writable ISO is not a thing, and honouring read_only=false would arm a write path
             * against installer media. */
            d->read_only = 1;
        } else {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        *seen |= D_TYPE;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "backing")) {
        if (*seen & D_BACKING) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "file")) {
            d->backing = HYPE_CFG_BACKING_FILE;
        } else if (hype_streq(val, "physical")) {
            d->backing = HYPE_CFG_BACKING_PHYSICAL;
        } else {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        d->has_backing = 1;
        *seen |= D_BACKING;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "path")) {
        return apply_disk_str(d->path, &d->has_path, seen, D_PATH, val);
    }
    if (hype_streq(key, "source_disk")) {
        return apply_disk_str(d->source_disk, &d->has_source_disk, seen, D_SOURCE_DISK, val);
    }
    if (hype_streq(key, "id_match")) {
        return apply_disk_str(d->id_match, &d->has_id_match, seen, D_ID_MATCH, val);
    }
    if (hype_streq(key, "format")) {
        if (*seen & D_FORMAT) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "raw")) {
            d->format = HYPE_CFG_FORMAT_RAW;
        } else if (hype_streq(val, "qcow2")) {
            d->format = HYPE_CFG_FORMAT_QCOW2;
        } else {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        d->has_format = 1; /* #336: distinct from format==RAW, which is also the default */
        *seen |= D_FORMAT;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "size_gb")) {
        hype_cfg_status_t st;
        if (*seen & D_SIZE_GB) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_uint_field(val, &d->size_gb);
        if (st != HYPE_CFG_OK) return st;
        d->has_size_gb = 1;
        *seen |= D_SIZE_GB;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "partition")) {
        if (*seen & D_PARTITION) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "whole")) {
            d->partition = 0u; /* 0 is the sentinel for whole-disk, matching target_disk */
        } else {
            hype_cfg_status_t st = parse_uint_field(val, &d->partition);
            if (st != HYPE_CFG_OK) return st; /* parse_uint_field already rejects 0 */
        }
        *seen |= D_PARTITION;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "bus")) {
        if (*seen & D_BUS) return HYPE_CFG_ERR_DUPLICATE_KEY;
        if (hype_streq(val, "virtio-blk")) {
            d->bus = HYPE_CFG_BUS_VIRTIO_BLK;
        } else if (hype_streq(val, "ahci-sata")) {
            d->bus = HYPE_CFG_BUS_AHCI_SATA;
        } else if (hype_streq(val, "nvme")) {
            d->bus = HYPE_CFG_BUS_NVME;
        } else if (hype_streq(val, "ahci-atapi")) {
            d->bus = HYPE_CFG_BUS_AHCI_ATAPI;
        } else if (hype_streq(val, "usb-msc")) {
            d->bus = HYPE_CFG_BUS_USB_MSC;
        } else {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        *seen |= D_BUS;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "read_only")) {
        hype_cfg_status_t st;
        int v = 0;
        if (*seen & D_READ_ONLY) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_bool_field(val, &v);
        if (st != HYPE_CFG_OK) return st;
        /* A cdrom stays read-only whatever this says (see `type` above); anything else applies. */
        if (d->type != HYPE_CFG_DISK_TYPE_CDROM) {
            d->read_only = v;
        }
        d->has_read_only = 1;
        *seen |= D_READ_ONLY;
        return HYPE_CFG_OK;
    }
    if (hype_streq(key, "allow_overwrite")) {
        hype_cfg_status_t st;
        if (*seen & D_ALLOW_OVERWRITE) return HYPE_CFG_ERR_DUPLICATE_KEY;
        st = parse_bool_field(val, &d->allow_overwrite);
        if (st != HYPE_CFG_OK) return st;
        *seen |= D_ALLOW_OVERWRITE;
        return HYPE_CFG_OK;
    }
    return HYPE_CFG_ERR_UNKNOWN_KEY;
}

/*
 * §5.3 required fields, checked when the section ends. Returns OK or the reason the device is
 * unusable -- the caller SKIPS it rather than failing the whole parse (§4.3).
 */
static hype_cfg_status_t validate_disk(const hype_cfg_disk_t *d, unsigned int seen) {
    if (d->type == HYPE_CFG_DISK_TYPE_CDROM) {
        /* backing defaults to file for a cdrom, so only the path is genuinely required. */
        if (!d->has_path) return HYPE_CFG_ERR_MISSING_REQUIRED;
        return HYPE_CFG_OK;
    }
    /* A hard disk must say where its bytes come from: `backing` is required rather than defaulted,
     * because guessing between a file and a REAL DRIVE is not a guess worth making. */
    if (!(seen & D_BACKING)) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (d->backing == HYPE_CFG_BACKING_FILE && !d->has_path) return HYPE_CFG_ERR_MISSING_REQUIRED;
    if (d->backing == HYPE_CFG_BACKING_PHYSICAL) {
        /* Without id_match the phys_guard (#122/#124) has nothing to match the enumerated drive
         * against, so the write could never be armed anyway -- and a physical target that silently
         * matched "any drive" is the worst possible reading of an omission. */
        if (!d->has_id_match) return HYPE_CFG_ERR_MISSING_REQUIRED;
        if (d->has_path) return HYPE_CFG_ERR_BAD_VALUE; /* path is meaningless for a real drive */
    }
    return HYPE_CFG_OK;
}

/*
 * #222: retain a line verbatim, attached to the section it appeared in.
 *
 * Capacity exhaustion sets retained_overflow rather than failing the parse: a config must not become
 * unbootable because it carries one comment too many. The flag is what stops the CONFIG-3 serializer
 * writing back a file whose tail it never captured.
 */
static void retain_line(hype_cfg_t *out, const char *raw, int raw_truncated) {
    unsigned long long len;

    if (raw_truncated) {
        /* The caller's snapshot already lost bytes, so what is here is a PREFIX of the operator's
         * line. Writing a prefix back would corrupt the file, so keep none of it -- and flag it. */
        out->retained_overflow = 1;
        return;
    }
    if (out->retained_count >= HYPE_CFG_MAX_RETAINED) {
        out->retained_overflow = 1;
        return;
    }
    len = hype_strlcpy(out->retained[out->retained_count].text, raw, HYPE_CFG_LINE_MAX);
    if (len >= HYPE_CFG_LINE_MAX) {
        out->retained_overflow = 1;
        return;
    }
    out->retained[out->retained_count].section = (int)out->section_count - 1;
    out->retained_count++;
}

/* Records a section header in file order. Returns the new section index, or -1 if full. */
static int add_section(hype_cfg_t *out, hype_cfg_section_kind_t kind, const char *name,
                       const char *raw, int index) {
    hype_cfg_section_t *sec;

    if (out->section_count >= HYPE_CFG_MAX_SECTIONS) {
        out->retained_overflow = 1;
        return -1;
    }
    sec = &out->sections[out->section_count];
    sec->kind = kind;
    sec->index = index;
    if (hype_strlcpy(sec->name, name, HYPE_CFG_NAME_MAX) >= HYPE_CFG_NAME_MAX ||
        hype_strlcpy(sec->raw, raw, HYPE_CFG_LINE_MAX) >= HYPE_CFG_LINE_MAX) {
        out->retained_overflow = 1;
    }
    out->section_count++;
    return (int)out->section_count - 1;
}

/* #450: *cur_over_cap is raised for a [vm.*] past capacity and cleared by any other section, so
 * that VM's keys are ignored instead of read as keys outside a section. */
static hype_cfg_status_t process_section_header(char *line, hype_cfg_t *out, int *cur,
                                                 const char *raw, int raw_truncated,
                                                 int *cur_disk,
                                                 unsigned int *disk_seen, int *cur_is_hype,
                                                 unsigned int *in_hype_seen,
                                                 unsigned int line_no, int *cur_over_cap,
                                                 int *cur_nic, int *cur_switch,
                                                 unsigned int *nic_seen) {
    *cur_over_cap = 0;
    unsigned long long len = hype_strlen(line);
    char *body;
    char *name;
    unsigned long long name_len;
    unsigned int i;

    if (len < 2 || line[len - 1] != ']') {
        return HYPE_CFG_ERR_SYNTAX;
    }
    line[len - 1] = '\0';
    body = line + 1;
    if (hype_streq(body, "hype")) {
        /* §5.1: the master section has no instance name. A second one is a duplicate, and silently
         * merging two would make which value wins depend on file order. */
        if (*in_hype_seen != 0u || out->hype.malformed) {
            return HYPE_CFG_ERR_DUPLICATE_KEY;
        }
        *cur = -1;
        *cur_disk = -1;
        *cur_nic = -1;   /* #583 */
        *cur_switch = -1;
        *cur_is_hype = 1;
        (void)add_section(out, HYPE_CFG_SECTION_HYPE, "", raw, -1);
        return HYPE_CFG_OK;
    }
    if (hype_strneq(body, "disk.", 5)) {
        /* #222 (§5.3): a named device. Same shape as a VM section: the id must be non-empty and
         * unique, since `disks = a, b` resolves by id and a duplicate would make which device is
         * meant depend on parse order. */
        name = body + 5;
        if (*name == '\0') {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        for (i = 0; i < out->disk_count; i++) {
            if (hype_streq(out->disks[i].id, name)) {
                return HYPE_CFG_ERR_DUPLICATE_VM_NAME; /* same class of error: a duplicated id */
            }
        }
        if (out->disk_count >= HYPE_CFG_MAX_DISKS) {
            return HYPE_CFG_ERR_TOO_MANY_ENTRIES;
        }
        *cur = -1; /* not a VM: VM keys must not land in a disk section */
        *cur_is_hype = 0;
        *cur_nic = -1;   /* #583 */
        *cur_switch = -1;
        *cur_disk = (int)out->disk_count;
        zero_disk(&out->disks[*cur_disk]);
        if (hype_strlcpy(out->disks[*cur_disk].id, name, HYPE_CFG_NAME_MAX) >= HYPE_CFG_NAME_MAX) {
            return HYPE_CFG_ERR_VALUE_TOO_LONG;
        }
        *disk_seen = 0;
        (void)add_section(out, HYPE_CFG_SECTION_DISK, name, raw, *cur_disk);
        out->disk_count++;
        return HYPE_CFG_OK;
    }
    if (hype_strneq(body, "nic.", 4) || hype_strneq(body, "switch.", 7)) {
        /*
         * #583 (§5.5). Same shape as [disk.*]: a non-empty, unique id, because `nics = a, b` and
         * `switch = lan0` both resolve by id and a duplicate would make which device is meant depend
         * on parse order.
         *
         * A malformed one is skipped and COUNTED (§4.3), like a bad [disk.*] -- one typo in a NIC
         * nothing may even reference must not stop the machine booting, and a silently absent NIC is
         * how a VM ends up with no network for no visible reason.
         */
        int is_nic = hype_strneq(body, "nic.", 4);
        name = body + (is_nic ? 4 : 7);
        if (*name == '\0') {
            return HYPE_CFG_ERR_BAD_VALUE;
        }
        if (is_nic) {
            for (i = 0; i < out->nic_count; i++) {
                if (hype_streq(out->nics[i].id, name)) return HYPE_CFG_ERR_DUPLICATE_VM_NAME;
            }
            if (out->nic_count >= HYPE_CFG_MAX_NICS) return HYPE_CFG_ERR_TOO_MANY_ENTRIES;
        } else {
            for (i = 0; i < out->switch_count; i++) {
                if (hype_streq(out->switches[i].id, name)) return HYPE_CFG_ERR_DUPLICATE_VM_NAME;
            }
            if (out->switch_count >= HYPE_CFG_MAX_SWITCHES) return HYPE_CFG_ERR_TOO_MANY_ENTRIES;
        }
        *cur = -1; /* not a VM, and not a disk: those sections' keys must not land here */
        *cur_disk = -1;
        *cur_is_hype = 0;
        if (is_nic) {
            *cur_nic = (int)out->nic_count;
            *cur_switch = -1;
            zero_nic(&out->nics[*cur_nic]);
            if (hype_strlcpy(out->nics[*cur_nic].id, name, HYPE_CFG_NAME_MAX) >= HYPE_CFG_NAME_MAX) {
                return HYPE_CFG_ERR_VALUE_TOO_LONG;
            }
            *nic_seen = 0;
            (void)add_section(out, HYPE_CFG_SECTION_NIC, name, raw, *cur_nic);
            out->nic_count++;
        } else {
            *cur_switch = (int)out->switch_count;
            *cur_nic = -1;
            zero_switch(&out->switches[*cur_switch]);
            if (hype_strlcpy(out->switches[*cur_switch].id, name, HYPE_CFG_NAME_MAX) >=
                HYPE_CFG_NAME_MAX) {
                return HYPE_CFG_ERR_VALUE_TOO_LONG;
            }
            *nic_seen = 0;
            (void)add_section(out, HYPE_CFG_SECTION_SWITCH, name, raw, *cur_switch);
            out->switch_count++;
        }
        return HYPE_CFG_OK;
    }
    if (!hype_strneq(body, "vm.", 3)) {
        /*
         * #678: `raw` is `line` truncated to HYPE_CFG_LINE_MAX for retention -- fine for a plain
         * key/value or comment line (any shorter text is still valid), but a SECTION HEADER's
         * validity depends on its LAST byte being ']' (the check at the top of this function,
         * against the full untruncated `line`). Truncating a long header for storage can cut off
         * the closing ']' that made the original line acceptable, so hype_cfg_serialize()'s
         * re-emission of `raw` -- which the parser sees as a fresh, `\n`-terminated line next time
         * -- fails that same check and the round-trip turns an OK config into a syntax error.
         * Refuse here, at ORIGINAL parse time, rather than retain something that cannot reparse
         * to what it meant: the same "detect at the point of truncation, not after" discipline the
         * raw-snapshot comment above already applies to key/value retention.
         */
        if (raw_truncated) {
            return HYPE_CFG_ERR_VALUE_TOO_LONG;
        }
        /*
         * #222 (spec §4.1): an unknown section KIND is retained, not rejected. A `[disk.*]` or
         * `[nic.*]` section from a newer hype must not stop an older one booting, and the lines
         * inside it must survive a write-back. Its keys are retained too -- see process_key_value's
         * cur < 0 path, which is why `cur` is cleared here.
         */
        *cur = -1;
        *cur_disk = -1;
        *cur_nic = -1;   /* #583 */
        *cur_switch = -1;
        *cur_is_hype = 0;
        (void)add_section(out, HYPE_CFG_SECTION_UNKNOWN, "", raw, -1);
        out->unknown_count++;
        return HYPE_CFG_OK;
    }
    name = body + 3;
    if (*name == '\0') {
        return HYPE_CFG_ERR_BAD_VALUE;
    }
    for (i = 0; i < out->vm_count; i++) {
        if (hype_streq(out->vms[i].name, name)) {
            return HYPE_CFG_ERR_DUPLICATE_VM_NAME;
        }
    }
    if (out->vm_count >= out->vm_cap) {
        /*
         * #450: BOUNDED, not rejected.
         *
         * This used to return HYPE_CFG_ERR_TOO_MANY_VMS, which aborts the whole parse -- so a
         * config with one VM too many was discarded entirely and hype fell back to its built-in
         * two, silently losing the fifteen VMs it had already read. Measured: a 20-VM config on
         * a build whose post-EBS storage holds 16 produced 'parse error (status=7, line=148)'
         * and then booted two default VMs.
         *
         * The VMs that fit are kept, and the ones past capacity are reported through the same
         * skipped-VM channel a VM missing a required key uses -- the mechanism that exists
         * precisely so a VM never vanishes without being named (#341).
         */
        if (out->skipped_vms == 0u) {
            (void)hype_strlcpy(out->skipped_vm_name, name, HYPE_CFG_NAME_MAX);
            out->skipped_vm_line = line_no;
        }
        out->skipped_vms++;
        *cur = -1;      /* subsequent keys belong to a VM that does not exist */
        *cur_disk = -1;
        *cur_nic = -1;   /* #583 */
        *cur_switch = -1;
        *cur_is_hype = 0;
        *cur_over_cap = 1; /* ...and are IGNORED rather than reported outside-a-section */
        return HYPE_CFG_OK;
    }

    *cur = (int)out->vm_count;
    *cur_disk = -1;
    *cur_nic = -1;   /* #583 */
    *cur_switch = -1;
    *cur_is_hype = 0;
    zero_vm(&out->vms[*cur]);
    name_len = hype_strlcpy(out->vms[*cur].name, name, HYPE_CFG_NAME_MAX);
    if (name_len >= HYPE_CFG_NAME_MAX) {
        return HYPE_CFG_ERR_VALUE_TOO_LONG;
    }
    /* #444 (TERM-6): seen_fields lives in the struct itself now, so operators can query
     * default-vs-explicit later at runtime -- already cleared by zero_vm() above, nothing
     * further to reset here. */
    (void)add_section(out, HYPE_CFG_SECTION_VM, name, raw, *cur);
    out->vm_count++;
    return HYPE_CFG_OK;
}

static hype_cfg_status_t process_key_value(char *line, hype_cfg_t *out, int cur,
                                            const char *raw, int raw_truncated,
                                            int cur_disk, unsigned int *disk_seen, int cur_is_hype,
                                            unsigned int *in_hype_seen, int cur_nic, int cur_switch,
                                            unsigned int *nic_seen) {
    char *eq;
    char *key;
    char *val;
    hype_cfg_status_t st;

    if (cur < 0 && cur_disk < 0 && cur_nic < 0 && cur_switch < 0 && !cur_is_hype) {
        /*
         * #222: no CURRENT typed section. Two different cases, deliberately distinguished: inside a
         * retained unknown section the line is retained (section_count > 0), whereas a key before
         * ANY section header is still a hard error -- it belongs to nothing and is far more likely a
         * typo'd header than a forward-compatible extension.
         */
        if (out->section_count > 0u &&
            out->sections[out->section_count - 1u].kind == HYPE_CFG_SECTION_UNKNOWN) {
            retain_line(out, raw, raw_truncated);
            out->unknown_count++;
            return HYPE_CFG_OK;
        }
        return HYPE_CFG_ERR_KEY_BEFORE_SECTION;
    }

    eq = find_char(line, '=');
    if (*eq != '=') {
        return HYPE_CFG_ERR_SYNTAX;
    }
    *eq = '\0';
    key = hype_str_trim(line);
    val = hype_str_trim(eq + 1);
    if (*key == '\0') {
        return HYPE_CFG_ERR_SYNTAX;
    }

    if (cur_is_hype) {
        st = apply_hype_field(&out->hype, in_hype_seen, key, val);
        if (st != HYPE_CFG_OK && st != HYPE_CFG_ERR_UNKNOWN_KEY) {
            /*
             * §4.3: a malformed [hype] falls back to global DEFAULTS instead of failing the parse.
             * Deliberately a different rule from [vm.*]: a bad global cannot make a VM wrong, it can
             * only make hype behave as it did before the section existed. The flag is what stops
             * that being silent -- a rejected `host_cpu_budget` that just vanished would look like
             * it had been applied.
             */
            hype_globals_defaults(&out->hype);
            out->hype.malformed = 1;
            *in_hype_seen = 0;
            return HYPE_CFG_OK;
        }
    } else if (cur_nic >= 0) {
        st = apply_nic_field(&out->nics[cur_nic], nic_seen, key, val); /* #583 */
    } else if (cur_switch >= 0) {
        st = apply_switch_field(&out->switches[cur_switch], nic_seen, key, val); /* #583 */
    } else {
        st = (cur_disk >= 0) ? apply_disk_field(&out->disks[cur_disk], disk_seen, key, val)
                             : apply_field(&out->vms[cur], &out->vms[cur].seen_fields, key, val);
    }
    if (st == HYPE_CFG_ERR_UNKNOWN_KEY) {
        /* #222: retained, not rejected -- the whole point of the keystone. Note `line` has been
         * mutated (the '=' overwritten), which is why retention works off the untouched `raw`. */
        retain_line(out, raw, raw_truncated);
        out->unknown_count++;
        return HYPE_CFG_OK;
    }
    return st;
}

/*
 * #393: count the [vm.*] section headers, so a caller can size storage before parsing. Read-only
 * and deliberately dumb -- it counts declarations, and the parse is what judges them.
 */
void hype_cfg_init(hype_cfg_t *out) {
    unsigned char *b = (unsigned char *)out;
    unsigned long long i;
    if (out == 0) {
        return;
    }
    for (i = 0; i < sizeof(*out); i++) {
        b[i] = 0;
    }
    out->vms = out->vms_default;
    out->vm_cap = HYPE_CFG_MAX_VMS;
    /*
     * #533: SAFE DEFAULTS (spec section 4.4), not zeroes.
     *
     * Zeroing alone made log_level come out as HYPE_LOG_ERROR, which is 0 -- so a host with no
     * config, the one whose log matters most, got the QUIETEST level instead of the loudest. Caught
     * by the line that prints which level is in force, which is the whole reason that line exists.
     */
    hype_globals_defaults(&out->hype);
}

int hype_cfg_append_vm(hype_cfg_t *cfg, const hype_cfg_vm_t *vm) {
    hype_cfg_section_t *sec;
    unsigned int ni;
    unsigned char *dst;
    const unsigned char *src;
    unsigned long long k;

    if (cfg == 0 || vm == 0) {
        return -1;
    }
    if (cfg->vm_count >= cfg->vm_cap || cfg->section_count >= HYPE_CFG_MAX_SECTIONS) {
        return -1;
    }
    ni = cfg->vm_count;
    dst = (unsigned char *)&cfg->vms[ni];
    src = (const unsigned char *)vm;
    for (k = 0; k < sizeof(*vm); k++) {
        dst[k] = src[k];
    }
    cfg->vm_count = ni + 1u;

    sec = &cfg->sections[cfg->section_count];
    sec->kind = HYPE_CFG_SECTION_VM;
    sec->index = (int)ni;
    (void)hype_strlcpy(sec->name, vm->name, HYPE_CFG_NAME_MAX);
    /* The header is written the way an operator would write it, because it IS re-emitted verbatim. */
    hype_snprintf(sec->raw, HYPE_CFG_LINE_MAX, "[vm.%s]", vm->name);
    cfg->section_count++;
    return 0;
}


/* #491: freestanding build -- struct assignment of these large types lowers to memcpy, which
 * does not exist here (the #freestanding-memcpy trap). Byte copy instead. */
static void cfg_byte_copy(void *dst, const void *src, unsigned long long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    unsigned long long i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

/* #491: does any OTHER non-deleted VM reference device `name` in list (disks/cdroms/nics)? */
static int cfg_dev_referenced_elsewhere(const hype_cfg_t *cfg, unsigned int skip_vm,
                                        const char *name, int is_nic) {
    unsigned int vi, di;

    for (vi = 0; vi < cfg->vm_count; vi++) {
        const hype_cfg_vm_t *vm = &cfg->vms[vi];
        if (vi == skip_vm || vm->deleted) {
            continue;
        }
        if (is_nic) {
            for (di = 0; di < vm->nics_count; di++) {
                if (hype_streq(vm->nics[di], name)) {
                    return 1;
                }
            }
        } else {
            for (di = 0; di < vm->disks_count; di++) {
                if (hype_streq(vm->disks[di], name)) {
                    return 1;
                }
            }
            for (di = 0; di < vm->cdroms_count; di++) {
                if (hype_streq(vm->cdroms[di], name)) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* #491: should sections[si] be removed when deleting vm_index? */
static int cfg_section_belongs_to_vm(const hype_cfg_t *cfg, unsigned int si,
                                     unsigned int vm_index) {
    const hype_cfg_section_t *sec = &cfg->sections[si];
    const hype_cfg_vm_t *vm = &cfg->vms[vm_index];
    unsigned int di;

    if (sec->kind == HYPE_CFG_SECTION_VM) {
        return sec->index == (int)vm_index;
    }
    if (sec->kind == HYPE_CFG_SECTION_DISK) {
        for (di = 0; di < vm->disks_count; di++) {
            if (hype_streq(sec->name, vm->disks[di]) &&
                !cfg_dev_referenced_elsewhere(cfg, vm_index, sec->name, 0)) {
                return 1;
            }
        }
        for (di = 0; di < vm->cdroms_count; di++) {
            if (hype_streq(sec->name, vm->cdroms[di]) &&
                !cfg_dev_referenced_elsewhere(cfg, vm_index, sec->name, 0)) {
                return 1;
            }
        }
        return 0;
    }
    if (sec->kind == HYPE_CFG_SECTION_NIC) {
        for (di = 0; di < vm->nics_count; di++) {
            if (hype_streq(sec->name, vm->nics[di]) &&
                !cfg_dev_referenced_elsewhere(cfg, vm_index, sec->name, 1)) {
                return 1;
            }
        }
        return 0;
    }
    /* [hype], [switch.*] and unknown sections are never a VM's to take down. */
    return 0;
}

int hype_cfg_delete_vm(hype_cfg_t *cfg, unsigned int vm_index) {
    /* old section index -> new (or -1 = removed); bounded by the fixed section table. */
    int remap[HYPE_CFG_MAX_SECTIONS];
    unsigned int si, w, ri, rw;

    if (cfg == 0 || vm_index >= cfg->vm_count || cfg->vms[vm_index].deleted) {
        return -1;
    }

    w = 0;
    for (si = 0; si < cfg->section_count; si++) {
        if (cfg_section_belongs_to_vm(cfg, si, vm_index)) {
            remap[si] = -1;
            continue;
        }
        remap[si] = (int)w;
        if (w != si) {
            cfg_byte_copy(&cfg->sections[w], &cfg->sections[si], sizeof(cfg->sections[w]));
        }
        w++;
    }
    if (w == cfg->section_count) {
        /* No [vm.*] section found for this index: the config and the caller disagree about what
         * exists, and deleting nothing while reporting success would hide that. */
        return -1;
    }
    cfg->section_count = w;

    /*
     * Retained lines: comments inside a removed section leave with it (the section is gone, and a
     * comment about a machine that no longer exists anchored to nothing is noise at best); every
     * other line's anchor is remapped so it stays with ITS section across the compaction.
     */
    rw = 0;
    for (ri = 0; ri < cfg->retained_count; ri++) {
        int sec = cfg->retained[ri].section;
        if (sec >= 0) {
            if (remap[sec] < 0) {
                continue; /* belonged to a removed section */
            }
            cfg->retained[ri].section = remap[sec];
        }
        if (rw != ri) {
            cfg_byte_copy(&cfg->retained[rw], &cfg->retained[ri], sizeof(cfg->retained[rw]));
        }
        rw++;
    }
    cfg->retained_count = rw;

    cfg->vms[vm_index].deleted = 1;
    return 0;
}


/*
 * #488 (TERM-12): attach a device to a VM at runtime, writing a [disk.<id>] section and adding it
 * to the VM's device list. The change persists through hype_cfg_serialize() (which emits the disk
 * from the struct) and takes effect at the VM's next start. `backing == PHYSICAL` uses
 * `path_or_serial` as id_match (the host device's identity); otherwise as the image path. Returns
 * 0 on success, -1 on a bad argument, an unknown VM, a duplicate id, or a full table.
 */
int hype_cfg_attach_disk(hype_cfg_t *cfg, const char *vm_name, const char *id,
                         hype_cfg_disk_type_t type, hype_cfg_backing_t backing,
                         const char *path_or_serial, hype_cfg_bus_t bus) {
    unsigned int i;
    int vmi = -1, di;
    char raw[HYPE_CFG_LINE_MAX];
    hype_cfg_disk_t *d;
    hype_cfg_vm_t *vm;

    if (cfg == 0 || vm_name == 0 || id == 0 || id[0] == '\0' || path_or_serial == 0) {
        return -1;
    }
    for (i = 0; i < cfg->vm_count; i++) {
        if (!cfg->vms[i].deleted && hype_streq(cfg->vms[i].name, vm_name)) {
            vmi = (int)i;
            break;
        }
    }
    if (vmi < 0) {
        return -1;
    }
    for (i = 0; i < cfg->disk_count; i++) {
        if (hype_streq(cfg->disks[i].id, id)) {
            return -1; /* id already exists */
        }
    }
    if (cfg->disk_count >= HYPE_CFG_MAX_DISKS) {
        return -1;
    }
    vm = &cfg->vms[vmi];
    if (vm->disks_count >= HYPE_CFG_MAX_VM_DISKS) {
        return -1;
    }
    di = (int)cfg->disk_count;
    d = &cfg->disks[di];
    zero_disk(d);
    if (hype_strlcpy(d->id, id, HYPE_CFG_NAME_MAX) >= HYPE_CFG_NAME_MAX) {
        return -1;
    }
    d->type = type;
    d->backing = backing;
    d->has_backing = 1;
    d->bus = bus;
    if (backing == HYPE_CFG_BACKING_PHYSICAL) {
        if (hype_strlcpy(d->id_match, path_or_serial, HYPE_CFG_PATH_MAX) >= HYPE_CFG_PATH_MAX) {
            return -1;
        }
        d->has_id_match = 1;
    } else {
        if (hype_strlcpy(d->path, path_or_serial, HYPE_CFG_PATH_MAX) >= HYPE_CFG_PATH_MAX) {
            return -1;
        }
        d->has_path = 1;
    }
    /* Section header line, as it would be written: "[disk.<id>]". Built by hand -- no strlcat in
     * this freestanding build. */
    {
        unsigned int n = 0;
        const char *pfx = "[disk.";
        while (pfx[n] != '\0' && n + 1u < sizeof(raw)) {
            raw[n] = pfx[n];
            n++;
        }
        {
            unsigned int j = 0;
            while (id[j] != '\0' && n + 1u < sizeof(raw)) {
                raw[n++] = id[j++];
            }
        }
        if (n + 1u < sizeof(raw)) {
            raw[n++] = ']';
        }
        raw[n] = '\0';
    }
    if (add_section(cfg, HYPE_CFG_SECTION_DISK, id, raw, di) < 0) {
        return -1;
    }
    cfg->disk_count++;
    (void)hype_strlcpy(vm->disks[vm->disks_count], id, HYPE_CFG_NAME_MAX);
    vm->disks_count++;
    return 0;
}

/*
 * #488: detach device `id` from VM `vm_name`. Removes the [disk.<id>] section (compacting
 * sections[] and remapping retained lines, exactly as hype_cfg_delete_vm does) and drops the id
 * from the VM's device list. The disk entry stays in disks[] uncompacted -- nothing references it,
 * so it is never re-emitted -- matching the delete_vm convention. Returns 0, or -1 if the VM or
 * the device is not found.
 */
int hype_cfg_detach_disk(hype_cfg_t *cfg, const char *vm_name, const char *id) {
    unsigned int si, ri, di_i;
    unsigned int w, rw;
    int vmi = -1, di = -1;
    int remap[HYPE_CFG_MAX_SECTIONS];
    hype_cfg_vm_t *vm;

    if (cfg == 0 || vm_name == 0 || id == 0) {
        return -1;
    }
    for (di_i = 0; di_i < cfg->vm_count; di_i++) {
        if (!cfg->vms[di_i].deleted && hype_streq(cfg->vms[di_i].name, vm_name)) {
            vmi = (int)di_i;
            break;
        }
    }
    if (vmi < 0) {
        return -1;
    }
    vm = &cfg->vms[vmi];
    /* The id must currently be on THIS VM's list. */
    {
        unsigned int k;
        int on_list = -1;
        for (k = 0; k < vm->disks_count; k++) {
            if (hype_streq(vm->disks[k], id)) {
                on_list = (int)k;
                break;
            }
        }
        if (on_list < 0) {
            return -1;
        }
        /* Drop it from the list (compact the small fixed array). */
        for (k = (unsigned int)on_list; k + 1u < vm->disks_count; k++) {
            (void)hype_strlcpy(vm->disks[k], vm->disks[k + 1u], HYPE_CFG_NAME_MAX);
        }
        vm->disks_count--;
    }
    for (di_i = 0; di_i < cfg->disk_count; di_i++) {
        if (hype_streq(cfg->disks[di_i].id, id)) {
            di = (int)di_i;
            break;
        }
    }
    /* Remove the disk's section (compact + remap retained), mirroring hype_cfg_delete_vm. */
    w = 0;
    for (si = 0; si < cfg->section_count; si++) {
        if (cfg->sections[si].kind == HYPE_CFG_SECTION_DISK && di >= 0 &&
            cfg->sections[si].index == di) {
            remap[si] = -1;
            continue;
        }
        remap[si] = (int)w;
        if (w != si) {
            cfg_byte_copy(&cfg->sections[w], &cfg->sections[si], sizeof(cfg->sections[w]));
        }
        w++;
    }
    cfg->section_count = w;
    rw = 0;
    for (ri = 0; ri < cfg->retained_count; ri++) {
        int sec = cfg->retained[ri].section;
        if (sec >= 0) {
            if (remap[sec] < 0) {
                continue;
            }
            cfg->retained[ri].section = remap[sec];
        }
        if (rw != ri) {
            cfg_byte_copy(&cfg->retained[rw], &cfg->retained[ri], sizeof(cfg->retained[rw]));
        }
        rw++;
    }
    cfg->retained_count = rw;
    return 0;
}

unsigned int hype_cfg_count_vms(const char *text) {
    unsigned int n = 0;
    const char *p = text;
    while (p != 0 && *p) {
        const char *ls = p;
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '[' && p[1] == 'v' && p[2] == 'm' && p[3] == '.') {
            n++;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        if (p == ls) break;
    }
    return n;
}

hype_cfg_result_t hype_cfg_parse(char *text, hype_cfg_t *out) {
    /* #393: the struct's own storage, which is a convenience default and not a cap on hype --
     * see the note in cfg.h. hype itself calls hype_cfg_parse_into() with pool storage. */
    return hype_cfg_parse_into(text, out, out->vms_default, HYPE_CFG_MAX_VMS);
}

hype_cfg_result_t hype_cfg_parse_into(char *text, hype_cfg_t *out, hype_cfg_vm_t *vms,
                                      unsigned int vm_cap) {
    hype_cfg_result_t res;
    unsigned int disk_seen = 0;
    unsigned int in_hype_seen = 0;
    hype_cfg_status_t first_err = HYPE_CFG_OK;
    int in_vm_key = 0;
    unsigned int first_err_line = 0;
    int cur = -1;
    int cur_disk = -1;
    int cur_nic = -1;         /* #583 */
    int cur_switch = -1;      /* #583 */
    unsigned int nic_seen = 0u; /* one mask: a NIC and a switch section are never both current */
    int cur_is_hype = 0;
    int cur_over_cap = 0;
    unsigned int line_no = 0;
    unsigned int unknown_before = 0;
    char *p = text;
    unsigned int i;

    res.status = HYPE_CFG_OK;
    res.line = 0;
    out->vms = vms;
    out->vm_cap = vm_cap;
    out->vm_count = 0;
    hype_globals_defaults(&out->hype);
    out->disk_count = 0;
    out->skipped_disks = 0;
    out->skipped_nics = 0;      /* #583 */
    out->skipped_switches = 0;
    out->nic_count = 0;
    out->switch_count = 0;
    out->skipped_vms = 0;
    out->skipped_vm_name[0] = '\0';
    out->skipped_vm_line = 0;
    out->section_count = 0;
    out->retained_count = 0;
    out->retained_overflow = 0;
    out->unknown_count = 0;
    /* #357: reset with the count, or a later parse reports a line from an earlier one. Caught by
     * a test that parses twice into the same struct -- which is exactly how the loader uses it. */
    out->unknown_first_line = 0u;
    out->unknown_first[0] = '\0';

    while (*p) {
        char *line_start = p;
        char *line;
        char raw[HYPE_CFG_LINE_MAX];
        int raw_truncated;
        hype_cfg_status_t st;

        while (*p && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            *p = '\0';
            p++;
        }
        line_no++;

        /* #222: snapshot the line BEFORE clean_line() strips its comment and trims it in place.
         * Retention has to re-emit what the operator wrote, comment included, so it cannot work off
         * the cleaned form. Truncation MUST be detected here rather than inside retain_line(): by
         * then the bytes are already gone and the prefix looks like a complete short line. */
        raw_truncated = hype_strlcpy(raw, line_start, HYPE_CFG_LINE_MAX) >= HYPE_CFG_LINE_MAX;

        line = clean_line(line_start);
        if (*line == '\0') {
            /* Comment-only lines are retained (a serializer must preserve them); pure blanks are
             * not -- they carry nothing a re-emitted file needs, and capacity is finite. */
            if (raw[0] != '\0') {
                retain_line(out, raw, raw_truncated);
            }
            continue;
        }

        /*
         * #357: remember the FIRST line the parser did not understand, with its number.
         *
         * The old warning said "N line(s) not understood -- a misspelled key looks exactly like
         * this" and then did not say which line. It is the counter-example to its own advice: with
         * a 40-line config and a real typo the operator has nothing to go on. Captured here rather
         * than at the three unknown_count++ sites because `raw` and `line_no` are both in scope
         * only in this loop, and the first one is the one worth reporting -- a cascade usually
         * starts with a single mistake.
         */
        unknown_before = out->unknown_count;

        in_vm_key = 0;
        if (line[0] == '[') {
            st = process_section_header(line, out, &cur, raw, raw_truncated, &cur_disk, &disk_seen,
                                        &cur_is_hype, &in_hype_seen, line_no, &cur_over_cap,
                                        &cur_nic, &cur_switch, &nic_seen);
        } else {
            in_vm_key = (cur >= 0);
            st = cur_over_cap
                     ? HYPE_CFG_OK /* #450: a VM past capacity was reported; its keys are noise */
                     : process_key_value(line, out, cur, raw, raw_truncated, cur_disk,
                                         &disk_seen, cur_is_hype, &in_hype_seen, cur_nic,
                                         cur_switch, &nic_seen);
        }
        if (out->unknown_count > unknown_before && out->unknown_first_line == 0u) {
            out->unknown_first_line = line_no;
            (void)hype_strlcpy(out->unknown_first, raw, HYPE_CFG_LINE_MAX);
        }

        if (st != HYPE_CFG_OK) {
            /*
             * #341 (§4.3): an error INSIDE a [vm.*] drops that VM and keeps going -- one typo must
             * not cost every VM in the file. Everything else (a key before any section, a duplicate
             * VM name, a broken section header) is still fatal: those are not attributable to one VM,
             * so there is nothing to isolate.
             *
             * The FIRST error is remembered rather than discarded, because if every VM ends up
             * skipped the parse still fails with it -- which is what keeps a single-VM config's
             * behaviour, and every existing error test, exactly as before.
             *
             * `in_vm_key`, not `cur >= 0`: after a bad SECTION HEADER `cur` still points at the
             * PREVIOUS VM, so keying off it blamed that VM for an error it did not cause -- which is
             * how the first attempt turned "too many VMs" into a silently dropped 16th VM and a
             * successful parse.
             */
            if (in_vm_key) {
                if (!out->vms[cur].parse_bad) {
                    out->vms[cur].parse_bad = 1;
                    if (out->skipped_vms == 0u) {
                        (void)hype_strlcpy(out->skipped_vm_name, out->vms[cur].name,
                                           HYPE_CFG_NAME_MAX);
                        out->skipped_vm_line = line_no;
                    }
                    out->skipped_vms++;
                }
                if (first_err == HYPE_CFG_OK) {
                    first_err = st;
                    first_err_line = line_no;
                }
                continue;
            }
            res.status = st;
            res.line = line_no;
            return res;
        }
    }

    /*
     * §4.3: a malformed [disk.*] is dropped, not fatal. Compacted in place so disks[] stays dense
     * and `disk_count` means what it says -- a caller iterating 0..disk_count must not have to know
     * some entries are corpses. Dropping loses which id was bad, which is why skipped_disks exists
     * for the caller to report: a device that vanishes silently is how a VM ends up with no boot
     * disk for no visible reason.
     *
     * disk_seen only tracks the LAST disk section's keys, so required-field validation can only be
     * done for that one; every earlier disk is validated on the fields it actually set. That is
     * enough: the required-field checks read has_* flags, and only D_BACKING needs the seen mask
     * (has_backing carries the same information).
     */
    {
        int remap[HYPE_CFG_MAX_DISKS];
        unsigned int w = 0;
        unsigned int si;

        /*
         * Two passes with an explicit remap, NOT an index comparison after the fact. The obvious
         * version -- retarget moved entries, then clear any section index >= the new count -- is
         * wrong: with disks[0] bad and disks[1] good, the good one moves to 0 and the BAD section's
         * stale index 0 is still in range, so two headers end up pointing at the same device.
         */
        for (i = 0; i < out->disk_count; i++) {
            unsigned int sm = out->disks[i].has_backing ? D_BACKING : 0u;
            if (validate_disk(&out->disks[i], sm) != HYPE_CFG_OK) {
                remap[i] = -1;
                out->skipped_disks++;
                continue;
            }
            remap[i] = (int)w;
            if (w != i) {
                /* Byte copy, not `out->disks[w] = out->disks[i]`: freestanding hype has no libc, and
                 * whole-struct assignment of anything containing an array emits a memcpy call that
                 * fails at EFI link. Only `make all` catches it -- the host test build links fine. */
                unsigned char *dst = (unsigned char *)&out->disks[w];
                const unsigned char *src = (const unsigned char *)&out->disks[i];
                unsigned long long b;
                for (b = 0; b < sizeof(out->disks[0]); b++) {
                    dst[b] = src[b];
                }
            }
            w++;
        }
        for (si = 0; si < out->section_count; si++) {
            if (out->sections[si].kind == HYPE_CFG_SECTION_DISK && out->sections[si].index >= 0) {
                out->sections[si].index = remap[out->sections[si].index];
            }
        }
        out->disk_count = w;
    }

    for (i = 0; i < out->vm_count; i++) {
        hype_cfg_status_t st;

        if (out->vms[i].parse_bad) {
            continue; /* already counted; do not report the same VM twice */
        }
        st = validate_required(&out->vms[i], out->vms[i].seen_fields);
        if (st != HYPE_CFG_OK) {
            out->vms[i].parse_bad = 1;
            if (out->skipped_vms == 0u) {
                (void)hype_strlcpy(out->skipped_vm_name, out->vms[i].name, HYPE_CFG_NAME_MAX);
                out->skipped_vm_line = 0; /* a missing key has no line of its own */
            }
            out->skipped_vms++;
            if (first_err == HYPE_CFG_OK) {
                first_err = st;
                first_err_line = 0;
            }
        }
    }

    /* Compact the survivors, keeping the section table pointing at them. Same explicit-remap shape as
     * the disk compaction above, and for the same reason: comparing indices against the new count
     * aliases when index 0 is the dropped one. */
    {
        int remap[HYPE_CFG_MAX_VMS];
        unsigned int w = 0;
        unsigned int si;

        for (i = 0; i < out->vm_count; i++) {
            if (out->vms[i].parse_bad) {
                remap[i] = -1;
                continue;
            }
            remap[i] = (int)w;
            if (w != i) {
                unsigned char *dst = (unsigned char *)&out->vms[w];
                const unsigned char *src = (const unsigned char *)&out->vms[i];
                unsigned long long b;
                for (b = 0; b < sizeof(out->vms[0]); b++) {
                    dst[b] = src[b]; /* byte copy: no libc, and hype_cfg_vm_t contains arrays */
                }
            }
            w++;
        }
        for (si = 0; si < out->section_count; si++) {
            if (out->sections[si].kind == HYPE_CFG_SECTION_VM && out->sections[si].index >= 0) {
                out->sections[si].index = remap[out->sections[si].index];
            }
        }
        out->vm_count = w;
    }

    /*
     * If EVERY declared VM was skipped, the config as a whole is unusable -- "hype running with zero
     * VMs" is never what anyone wanted -- so fail with the first error, exactly as before #341. This
     * is what makes the change invisible to a single-VM config and to every existing error test:
     * isolation only shows up when there is something left to isolate.
     */
    if (out->vm_count == 0u && first_err != HYPE_CFG_OK) {
        res.status = first_err;
        res.line = first_err_line;
        return res;
    }

    return res;
}

/*
 * SMP-1 (#185). See cfg.h. Clamps into [1, max_vcpus] and reports which of the four things
 * happened, so the caller can say so rather than silently applying a different number than the
 * operator configured -- the #428 lesson.
 */
hype_cfg_ram_status_t hype_cfg_resolve_vcpus(unsigned int cfg_vcpus, unsigned int max_vcpus,
                                             unsigned int *out_vcpus) {
    unsigned int applied;
    hype_cfg_ram_status_t st;

    if (max_vcpus == 0u) {
        max_vcpus = 1u; /* a caller that passes 0 still gets a runnable VM, not zero vCPUs */
    }

    if (cfg_vcpus == 0u) {
        applied = 1u; /* unconfigured: one vCPU, hype's behaviour before SMP existed */
        st = HYPE_CFG_RAM_DEFAULTED;
    } else if (cfg_vcpus > max_vcpus) {
        applied = max_vcpus;
        st = HYPE_CFG_RAM_CLAMPED_HIGH;
    } else {
        applied = cfg_vcpus;
        st = HYPE_CFG_RAM_APPLIED;
    }

    if (out_vcpus != (unsigned int *)0) {
        *out_vcpus = applied;
    }
    return st;
}

hype_cfg_ram_status_t hype_cfg_resolve_mem_mb(unsigned int cfg_mem_mb, unsigned int default_mb,
                                              unsigned int min_mb, unsigned int max_mb,
                                              unsigned int *out_mb) {
    unsigned int applied;
    hype_cfg_ram_status_t st;

    /* Inverted limits: the floor wins. A guest that is too small fails loudly at
     * its own boot; one sized past the platform's ceiling corrupts whatever lives
     * above it, which is far harder to attribute. */
    if (max_mb < min_mb) {
        max_mb = min_mb;
    }

    if (cfg_mem_mb == 0u) {
        applied = default_mb;
        st = HYPE_CFG_RAM_DEFAULTED;
    } else if (cfg_mem_mb < min_mb) {
        applied = min_mb;
        st = HYPE_CFG_RAM_CLAMPED_LOW;
    } else if (cfg_mem_mb > max_mb) {
        applied = max_mb;
        st = HYPE_CFG_RAM_CLAMPED_HIGH;
    } else {
        applied = cfg_mem_mb;
        st = HYPE_CFG_RAM_APPLIED;
    }

    /* The DEFAULT is clamped too. A built-in default that exceeds the platform
     * ceiling is still an overrun, and the compile-time override
     * (-DHYPE_FW_1_GUEST_RAM_MB=N) can set it to anything. */
    if (applied < min_mb) {
        applied = min_mb;
    }
    if (applied > max_mb) {
        applied = max_mb;
    }

    if (out_mb != (unsigned int *)0) {
        *out_mb = applied;
    }
    return st;
}

const char *hype_cfg_ram_status_str(hype_cfg_ram_status_t st) {
    switch (st) {
        case HYPE_CFG_RAM_DEFAULTED: return "built-in default (no hype.cfg value)";
        case HYPE_CFG_RAM_APPLIED: return "from hype.cfg";
        case HYPE_CFG_RAM_CLAMPED_LOW: return "hype.cfg value RAISED to the minimum";
        case HYPE_CFG_RAM_CLAMPED_HIGH: return "hype.cfg value LOWERED to the maximum";
        default: return "unknown";
    }
}

uint64_t hype_cfg_size_gb_to_bytes(unsigned int gb) {
    /* unsigned int caps at ~4.3e9, so 4.3e9 * 2^30 stays inside 64 bits -- no overflow guard
     * needed, and one would be dead code. */
    return (uint64_t)gb * 1024ull * 1024ull * 1024ull;
}

hype_cfg_bus_t hype_cfg_resolve_bus(const hype_cfg_disk_t *disk, hype_cfg_os_hint_t os_hint) {
    if (disk == 0) {
        return HYPE_CFG_BUS_DEFAULT;
    }
    /* An optical drive is ATAPI whatever anything else says: it is the only optical front-end hype
     * has, and a cdrom on virtio-blk is not a thing a guest would recognise. */
    if (disk->type == HYPE_CFG_DISK_TYPE_CDROM) {
        return HYPE_CFG_BUS_AHCI_ATAPI;
    }
    if (disk->bus != HYPE_CFG_BUS_DEFAULT) {
        return disk->bus; /* explicit always wins */
    }
    if (os_hint == HYPE_CFG_OS_WINDOWS) {
        return HYPE_CFG_BUS_AHCI_SATA;
    }
    return HYPE_CFG_BUS_VIRTIO_BLK;
}

hype_cfg_format_check_t hype_cfg_check_format(const hype_cfg_disk_t *disk, int sniffed_is_qcow2) {
    if (disk == 0 || !disk->has_format) {
        return HYPE_CFG_FORMAT_AGREES; /* nothing asserted -- detection decides, as it always has */
    }
    if (disk->format == HYPE_CFG_FORMAT_QCOW2 && !sniffed_is_qcow2) {
        return HYPE_CFG_FORMAT_MISMATCH_WANTED_QCOW2;
    }
    if (disk->format == HYPE_CFG_FORMAT_RAW && sniffed_is_qcow2) {
        return HYPE_CFG_FORMAT_MISMATCH_WANTED_RAW;
    }
    return HYPE_CFG_FORMAT_AGREES;
}

const char *hype_cfg_format_check_str(hype_cfg_format_check_t c) {
    switch (c) {
        case HYPE_CFG_FORMAT_AGREES:
            return "format matches the image";
        case HYPE_CFG_FORMAT_MISMATCH_WANTED_QCOW2:
            return "config says format = qcow2 but the image is RAW -- refusing (a stale path is far "
                   "more likely than a conversion you wanted)";
        case HYPE_CFG_FORMAT_MISMATCH_WANTED_RAW:
            return "config says format = raw but the image is QCOW2 -- refusing (writing a qcow2 as "
                   "raw would corrupt it)";
        default:
            return "unknown";
    }
}

/* #357: see the header. One decision, so every display site agrees. */
const char *hype_cfg_vm_display_name(const hype_cfg_vm_t *vm) {
    if (vm == 0) return "";
    return (vm->label[0] != '\0') ? vm->label : vm->name;
}

int hype_cfg_vm_has_target_disk(const hype_cfg_vm_t *vm) {
    if (vm == 0) return 0;
    /* A physical target is named by id/serial, a file target by path -- both land in path_or_id,
     * so its emptiness is the whole test. */
    return (vm->target_disk.path_or_id[0] != '\0') ? 1 : 0;
}

/* ==================== CONFIG-3 (#221): serializer ==================== */

typedef struct {
    char *buf;
    unsigned int cap;
    unsigned int len;
    int truncated;
} hype_cfg_w_t;

static void w_init(hype_cfg_w_t *w, char *buf, unsigned int cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->truncated = 0;
    if (cap > 0u) {
        buf[0] = '\0';
    }
}

static void w_raw(hype_cfg_w_t *w, const char *s, unsigned long long n) {
    unsigned long long i;
    if (w->truncated) {
        return;
    }
    for (i = 0; i < n; i++) {
        if (w->len + 1u >= w->cap) {
            w->truncated = 1;
            return;
        }
        w->buf[w->len++] = s[i];
    }
    w->buf[w->len] = '\0';
}

static void w_str(hype_cfg_w_t *w, const char *s) {
    w_raw(w, s, hype_strlen(s));
}

static void w_line(hype_cfg_w_t *w, const char *s) {
    w_str(w, s);
    w_str(w, "\n");
}

static void w_kv(hype_cfg_w_t *w, const char *key, const char *val) {
    w_str(w, key);
    w_str(w, " = ");
    w_line(w, val);
}

static void w_kv_uint(hype_cfg_w_t *w, const char *key, unsigned int val) {
    char tmp[24];
    hype_snprintf(tmp, sizeof(tmp), "%u", val);
    w_kv(w, key, tmp);
}

/* Comma-joins up to `count` NAME_MAX entries. A count of 0 emits nothing (an
 * empty list has no textual form the parser accepts back -- see id_list_piece
 * / cpu_set_piece, both of which reject an empty piece). */
static void w_kv_list(hype_cfg_w_t *w, const char *key, const char (*items)[HYPE_CFG_NAME_MAX],
                      unsigned int count) {
    unsigned int i;
    if (count == 0u) {
        return;
    }
    w_str(w, key);
    w_str(w, " = ");
    for (i = 0; i < count; i++) {
        if (i > 0u) {
            w_str(w, ",");
        }
        w_str(w, items[i]);
    }
    w_str(w, "\n");
}

/*
 * host_cpu_budget / cpu_set: written back as an expanded comma list ("1,2,3"),
 * never the operator's original range notation ("1-3") -- the struct only
 * ever stored the expanded set, so the range is not recoverable. Reparsing
 * the expanded form yields the identical set, which is what round-trip
 * correctness requires here (see hype_cfg_serialize's own header comment).
 */
static void w_kv_cpu_list(hype_cfg_w_t *w, const char *key, const unsigned int *cores,
                          unsigned int count) {
    unsigned int i;
    char tmp[12];
    if (count == 0u) {
        return;
    }
    w_str(w, key);
    w_str(w, " = ");
    for (i = 0; i < count; i++) {
        if (i > 0u) {
            w_str(w, ",");
        }
        hype_snprintf(tmp, sizeof(tmp), "%u", cores[i]);
        w_str(w, tmp);
    }
    w_str(w, "\n");
}

/* Emits every retained line still attached to `section` (-1 = before any section header), starting
 * from *ri, advancing *ri past them. Relies on retained[] being in file order (the parser only ever
 * appends), so a single forward pass over the whole array, resumed section by section, visits every
 * entry exactly once. */
static void serialize_retained_upto(hype_cfg_w_t *w, const hype_cfg_t *cfg, int section,
                                    unsigned int *ri) {
    while (*ri < cfg->retained_count && cfg->retained[*ri].section == section) {
        w_line(w, cfg->retained[*ri].text);
        (*ri)++;
    }
}

static void serialize_hype(hype_cfg_w_t *w, const hype_cfg_hype_t *h) {
    w_kv_uint(w, "config_version", h->config_version);
    if (h->has_host_cpu_budget) {
        w_kv_cpu_list(w, "host_cpu_budget", h->host_cpu_budget, h->host_cpu_budget_count);
    }
    w_kv(w, "default_net_mode", h->default_net_mode == HYPE_CFG_NET_NAT ? "nat" : "none");
    /* #405: emitted only when configured, like every other non-default -- writing an uplink of
     * 0.0.0.0 into a config that had none would turn "no uplink" into "a broken uplink". */
    if (h->has_uplink) {
        char q[16];
        hype_snprintf(q, sizeof(q), "%u.%u.%u.%u", (unsigned)h->uplink_ip[0],
                      (unsigned)h->uplink_ip[1], (unsigned)h->uplink_ip[2],
                      (unsigned)h->uplink_ip[3]);
        w_kv(w, "uplink_ip", q);
        hype_snprintf(q, sizeof(q), "%u.%u.%u.%u", (unsigned)h->uplink_mask[0],
                      (unsigned)h->uplink_mask[1], (unsigned)h->uplink_mask[2],
                      (unsigned)h->uplink_mask[3]);
        w_kv(w, "uplink_mask", q);
        hype_snprintf(q, sizeof(q), "%u.%u.%u.%u", (unsigned)h->uplink_gateway[0],
                      (unsigned)h->uplink_gateway[1], (unsigned)h->uplink_gateway[2],
                      (unsigned)h->uplink_gateway[3]);
        w_kv(w, "uplink_gateway", q);
    }
    if (h->dashboard_default_view == HYPE_CFG_VIEW_VM) {
        char tmp[HYPE_CFG_NAME_MAX + 4];
        hype_snprintf(tmp, sizeof(tmp), "vm:%s", h->dashboard_default_vm);
        w_kv(w, "dashboard_default_view", tmp);
    } else {
        w_kv(w, "dashboard_default_view", "dashboard");
    }
    if (h->autostart == HYPE_CFG_AUTOSTART_NONE) {
        w_kv(w, "autostart", "none");
    } else if (h->autostart == HYPE_CFG_AUTOSTART_LIST) {
        w_kv_list(w, "autostart", h->autostart_vms, h->autostart_count);
    } else {
        w_kv(w, "autostart", "all");
    }
    /* #429: always has a real, meaningful value (default 1) -- same "always emit" treatment as
     * config_version, no has_* flag needed. */
    w_kv(w, "log_level", hype_log_level_name(h->log_level)); /* #533 */
    w_kv_uint(w, "cpu_avg_window_secs", h->cpu_avg_window_secs);
    if (h->fs_selftest_disk[0] != '\0') {
        w_kv(w, "fs_selftest_disk", h->fs_selftest_disk); /* #709 */
    }
}

static void serialize_vm(hype_cfg_w_t *w, const hype_cfg_vm_t *vm) {
    w_kv_uint(w, "vcpus", vm->vcpus);
    if (vm->has_cpu_set) {
        w_kv_cpu_list(w, "cpu_set", vm->cpu_set, vm->cpu_set_count);
    }
    w_kv_uint(w, "mem_mb", vm->mem_mb);
    w_kv(w, "boot",
         (vm->boot == HYPE_CFG_BOOT_DISK)     ? "disk"
         : (vm->boot == HYPE_CFG_BOOT_KERNEL) ? "kernel"
                                              : "installer");
    if (vm->has_kernel) {
        w_kv(w, "kernel", vm->kernel);
    }
    if (vm->has_cmdline) {
        w_kv(w, "cmdline", vm->cmdline); /* #546 */
    }
    if (vm->has_initrd) {
        w_kv(w, "initrd", vm->initrd); /* #545 */
    }
    if (vm->tpm) {
        w_kv(w, "tpm", "on"); /* #433 */
    }
    if (vm->has_install_media) {
        w_kv(w, "install_media", vm->install_media);
    }
    if (vm->has_media_disk) {
        w_kv(w, "media_disk", vm->media_disk);
    }
    /* target_disk and disks/cdroms are mutually exclusive (validate_required, §7) -- exactly one
     * of these two branches ever has anything to emit for a VM that passed validation. */
    if (hype_cfg_vm_has_target_disk(vm)) {
        char tmp[HYPE_CFG_PATH_MAX + 10];
        hype_snprintf(tmp, sizeof(tmp), "%s:%s",
                     vm->target_disk.kind == HYPE_CFG_DISK_PHYSICAL ? "physical" : "file",
                     vm->target_disk.path_or_id);
        w_kv(w, "target_disk", tmp);
        if (vm->target_disk.partition == 0u) {
            w_kv(w, "partition", "whole");
        } else {
            w_kv_uint(w, "partition", vm->target_disk.partition);
        }
        if (vm->target_disk.allow_overwrite) {
            w_kv(w, "allow_overwrite", "true");
        }
        if (vm->has_target_disk_size_gb) {
            w_kv_uint(w, "target_disk_size_gb", vm->target_disk_size_gb);
        }
    } else {
        if (vm->disks_count > 0u) {
            w_kv_list(w, "disks", vm->disks, vm->disks_count);
        }
        if (vm->cdroms_count > 0u) {
            w_kv_list(w, "cdroms", vm->cdroms, vm->cdroms_count);
        }
    }

    /*
     * #583: NICs are emitted OUTSIDE the storage branch, and that is not a style choice -- the first
     * cut put them beside `disks`, inside the else of "does this VM use an inline target_disk". A VM
     * with a target_disk took the other branch, so its `nics` line was silently dropped on
     * write-back: the round-trip test caught a config that lost its network by being saved.
     * Networking has nothing to do with which storage form a VM uses.
     */
    if (vm->nics_count > 0u) {
        /* Only when non-empty: zero NICs is a network-less VM (§5.5), and writing `nics = ` would
         * turn that into an empty list nobody asked for. */
        w_kv_list(w, "nics", vm->nics, vm->nics_count);
    }
    if (vm->boot_order_count > 0u) {
        w_kv_list(w, "boot_order", vm->boot_order, vm->boot_order_count);
    }
    w_kv(w, "firmware", vm->firmware == HYPE_CFG_FW_LEGACY
                            ? "legacy"
                            : (vm->firmware == HYPE_CFG_FW_UEFI_SECBOOT ? "uefi-secboot"
                                                                        : "uefi")); /* #432 */
    /* #565: emitted only when set, like every other non-default -- writing `display = none` into
     * every config would add a line the operator never wrote. */
    if (vm->display != HYPE_CFG_DISPLAY_NONE) {
        w_kv(w, "display", "bochs");
    }
    if (vm->label[0] != '\0') {
        w_kv(w, "label", vm->label);
    }
    {
        const char *oh = "none";
        if (vm->os_hint == HYPE_CFG_OS_WINDOWS) {
            oh = "windows";
        } else if (vm->os_hint == HYPE_CFG_OS_LINUX) {
            oh = "linux";
        } else if (vm->os_hint == HYPE_CFG_OS_BSD) {
            oh = "bsd";
        }
        w_kv(w, "os_hint", oh);
    }
    w_kv(w, "net_mode", vm->net_mode == HYPE_CFG_NET_NAT ? "nat" : "none");
    if (vm->net_peers_count > 0u) {
        w_kv_list(w, "net_peers", vm->net_peers, vm->net_peers_count);
    }
}

/* NULL for HYPE_CFG_BUS_DEFAULT: that sentinel has no textual form the parser accepts (§5.6 derives
 * it from the owning VM's os_hint at resolve time, not at parse time -- see the enum's own header
 * comment), so a device left at the default must have its `bus` key OMITTED entirely rather than
 * writing a value that would fail to reparse. */
static const char *disk_bus_str(hype_cfg_bus_t bus) {
    switch (bus) {
        case HYPE_CFG_BUS_VIRTIO_BLK: return "virtio-blk";
        case HYPE_CFG_BUS_AHCI_SATA: return "ahci-sata";
        case HYPE_CFG_BUS_NVME: return "nvme";
        case HYPE_CFG_BUS_AHCI_ATAPI: return "ahci-atapi";
        case HYPE_CFG_BUS_USB_MSC: return "usb-msc";
        default: return 0;
    }
}

/*
 * #583: a NIC and a switch round-trip like every other section -- emitted from their CURRENT struct
 * values, so a GUI edit lands and a comment nobody touched survives (§4.1's write-back contract).
 *
 * Only what was SET is emitted. `switch` absent means the documented default -- an implicit private
 * per-NIC segment -- and writing `switch = ` for it would turn a default into a value, which is the
 * shape of bug this file keeps calling out: something that looks configured and is not.
 */
static void serialize_nic(hype_cfg_w_t *w, const hype_cfg_nic_t *n) {
    if (n->has_switch) {
        w_kv(w, "switch", n->switch_id);
    }
    if (n->has_mac) {
        char mac[18];
        static const char hexd[] = "0123456789abcdef";
        unsigned int i;
        for (i = 0; i < 6u; i++) {
            mac[i * 3u] = hexd[(n->mac[i] >> 4) & 0x0Fu];
            mac[i * 3u + 1u] = hexd[n->mac[i] & 0x0Fu];
            if (i < 5u) mac[i * 3u + 2u] = ':';
        }
        mac[17] = '\0';
        w_kv(w, "mac", mac);
    }
}

static void serialize_switch(hype_cfg_w_t *w, const hype_cfg_switch_t *sw) {
    if (sw->has_uplink) {
        w_kv(w, "uplink", sw->uplink == HYPE_CFG_UPLINK_NAT ? "nat" : "none");
    }
}

static void serialize_disk(hype_cfg_w_t *w, const hype_cfg_disk_t *d) {
    w_kv(w, "type", d->type == HYPE_CFG_DISK_TYPE_CDROM ? "cdrom" : "disk");
    w_kv(w, "backing", d->backing == HYPE_CFG_BACKING_PHYSICAL ? "physical" : "file");
    if (d->has_path) {
        w_kv(w, "path", d->path);
    }
    if (d->has_source_disk) {
        w_kv(w, "source_disk", d->source_disk);
    }
    if (d->has_id_match) {
        w_kv(w, "id_match", d->id_match);
    }
    if (d->has_format) {
        w_kv(w, "format", d->format == HYPE_CFG_FORMAT_QCOW2 ? "qcow2" : "raw");
    }
    if (d->has_size_gb) {
        w_kv_uint(w, "size_gb", d->size_gb);
    }
    if (d->partition == 0u) {
        w_kv(w, "partition", "whole");
    } else {
        w_kv_uint(w, "partition", d->partition);
    }
    {
        const char *bs = disk_bus_str(d->bus);
        if (bs != 0) {
            w_kv(w, "bus", bs);
        }
    }
    if (d->has_read_only) {
        w_kv(w, "read_only", d->read_only ? "true" : "false");
    }
    /* Meaningful only for a physical target (plan.md §10 decision #8's destructive-write guard);
     * a file backing that somehow had it set (not reachable through the parser today) still round-
     * trips correctly by omission, since `allow_overwrite`'s only consumer is the physical path. */
    if (d->backing == HYPE_CFG_BACKING_PHYSICAL && d->allow_overwrite) {
        w_kv(w, "allow_overwrite", "true");
    }
}

hype_cfg_serialize_result_t hype_cfg_serialize(const hype_cfg_t *cfg, char *out,
                                               unsigned int out_cap) {
    hype_cfg_serialize_result_t res;
    hype_cfg_w_t w;
    unsigned int ri = 0;
    unsigned int si;

    res.len = 0;
    res.truncated = 0;
    res.refused_overflow = 0;

    if (cfg->retained_overflow) {
        /* §8: some of the ORIGINAL file's content never made it into `cfg` at all -- writing back
         * would silently delete it. Refuse rather than produce a file that looks complete. */
        res.refused_overflow = 1;
        return res;
    }

    w_init(&w, out, out_cap);

    serialize_retained_upto(&w, cfg, -1, &ri);

    for (si = 0; si < cfg->section_count; si++) {
        const hype_cfg_section_t *sec = &cfg->sections[si];
        w_line(&w, sec->raw);
        switch (sec->kind) {
            case HYPE_CFG_SECTION_HYPE:
                serialize_hype(&w, &cfg->hype);
                break;
            /*
             * #673: a section whose VM/disk was dropped as malformed (§4.3 -- a malformed
             * section is skipped, not fatal) keeps its `kind` but has its `index` remapped to
             * -1 by the compaction pass in load_hype_cfg(). Indexing straight through was an
             * OOB read: on the fixed-size `disks[HYPE_CFG_MAX_DISKS]` array it is a crash
             * (UBSan array-bounds); on the caller-owned `vms` pointer (decision 33, no
             * compile-time bound for a sanitizer to check) it silently read whatever host
             * memory sits before the VM storage and serialized it into the output as if it
             * were a real section -- an information leak into the saved config, not a crash.
             * A dropped section emits nothing beyond its already-preserved raw/retained lines.
             */
            case HYPE_CFG_SECTION_VM:
                if (sec->index >= 0) {
                    serialize_vm(&w, &cfg->vms[sec->index]);
                }
                break;
            case HYPE_CFG_SECTION_DISK:
                if (sec->index >= 0) {
                    serialize_disk(&w, &cfg->disks[sec->index]);
                }
                break;
            case HYPE_CFG_SECTION_NIC: /* #583 */
                if (sec->index >= 0) {
                    serialize_nic(&w, &cfg->nics[sec->index]);
                }
                break;
            case HYPE_CFG_SECTION_SWITCH:
                if (sec->index >= 0) {
                    serialize_switch(&w, &cfg->switches[sec->index]);
                }
                break;
            case HYPE_CFG_SECTION_UNKNOWN:
            default:
                /* An unknown section's entire content lives in retained[] -- there is nothing
                 * else to emit; the loop below still re-emits its retained lines. */
                break;
        }
        serialize_retained_upto(&w, cfg, (int)si, &ri);
    }

    res.len = w.len;
    res.truncated = w.truncated;
    return res;
}
