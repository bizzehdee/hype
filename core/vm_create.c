#include "vm_create.h"
#include "strutil.h"
#include "format.h"

/*
 * TERM-10 (#486). See vm_create.h for why this is a pure state machine.
 *
 * The validation here deliberately mirrors the config parser's rules rather than inventing looser
 * ones: a wizard that accepts what hype.cfg would reject writes a file that fails to parse on the
 * next boot, which is the worst possible time to find out.
 */

static void wiz_err(hype_vm_wizard_t *w, const char *msg) {
    (void)hype_strlcpy(w->error, msg, sizeof(w->error));
    w->have_error = 1;
}

static void wiz_ok(hype_vm_wizard_t *w) {
    w->error[0] = '\0';
    w->have_error = 0;
}

static int line_is_empty(const char *s) {
    unsigned int i = 0;
    if (s == 0) {
        return 1;
    }
    while (s[i] == ' ' || s[i] == '\t') {
        i++;
    }
    return s[i] == '\0';
}

void hype_vm_wizard_begin(hype_vm_wizard_t *w) {
    unsigned char *b = (unsigned char *)w;
    unsigned long long i;
    if (w == 0) {
        return;
    }
    for (i = 0; i < sizeof(*w); i++) {
        b[i] = 0;
    }
    w->step = HYPE_VMW_NAME;
    /* Defaults that match hype's own built-ins, so an operator can accept every prompt and get a
     * working Linux installer VM. */
    w->vm.vcpus = 1u;
    w->vm.mem_mb = 1024u;
    w->vm.firmware = HYPE_CFG_FW_UEFI;
    w->vm.os_hint = HYPE_CFG_OS_LINUX;
    w->vm.boot = HYPE_CFG_BOOT_INSTALLER;
    w->vm.target_disk.kind = HYPE_CFG_DISK_FILE;
}

const char *hype_vm_wizard_prompt(const hype_vm_wizard_t *w) {
    if (w == 0) {
        return "";
    }
    switch (w->step) {
        case HYPE_VMW_NAME:    return "name (letters, digits, - and _):";
        case HYPE_VMW_OS_HINT: return "os_hint [linux] (windows|linux|bsd|none):";
        case HYPE_VMW_VCPUS:   return "vcpus [1]:";
        case HYPE_VMW_MEM_MB:  return "mem_mb [1024]:";
        case HYPE_VMW_DISK:    return "target_disk path, file: assumed [\\hype\\disks\\<name>.img]:";
        case HYPE_VMW_MEDIA:   return "install_media [\\iso\\<name>.iso], or '-' for none:";
        case HYPE_VMW_BOOT:    return "boot [installer] (installer|disk):";
        case HYPE_VMW_CPU_SET: return "cpu_set, e.g. 2 or 2-3, or '-' to let hype choose:";
        case HYPE_VMW_CONFIRM: return "create this VM? (yes|no):";
        case HYPE_VMW_DONE:    return "created";
        case HYPE_VMW_CANCELLED: return "cancelled";
    }
    return "";
}

/* #570: see the header. Bounded copy helper -- no libc in a freestanding build. */
static unsigned wiz_cat(char *out, unsigned pos, unsigned cap, const char *s) {
    while (*s != '\0' && pos + 1u < cap) {
        out[pos++] = *s++;
    }
    out[pos] = '\0';
    return pos;
}

const char *hype_vm_wizard_render(const hype_vm_wizard_t *w, char *out, unsigned cap) {
    unsigned pos = 0;

    if (out == 0 || cap == 0u) {
        return "";
    }
    out[0] = '\0';
    if (w == 0) {
        return out;
    }
    if (w->step == HYPE_VMW_DONE || w->step == HYPE_VMW_CANCELLED) {
        pos = wiz_cat(out, pos, cap, "create: ");
        (void)wiz_cat(out, pos, cap, hype_vm_wizard_prompt(w));
        return out;
    }
    if (w->have_error) {
        pos = wiz_cat(out, pos, cap, "INVALID -- ");
        pos = wiz_cat(out, pos, cap, w->error);
        pos = wiz_cat(out, pos, cap, "\n");
    }
    pos = wiz_cat(out, pos, cap, "create> ");
    pos = wiz_cat(out, pos, cap, hype_vm_wizard_prompt(w));
    (void)wiz_cat(out, pos, cap, "  ('cancel' abandons)");
    return out;
}

/* The config parser accepts a section name of letters, digits, '-' and '_'. Same rule here. */
static int name_is_valid(const char *s) {
    unsigned int i;
    if (s == 0 || s[0] == '\0') {
        return 0;
    }
    for (i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_';
        if (!ok) {
            return 0;
        }
    }
    return i < HYPE_CFG_NAME_MAX;
}

/* "2" or "2-3" or "2,4" -- the same shapes cfg's cpu_set accepts. */
static int parse_cpu_set(hype_vm_wizard_t *w, const char *s) {
    unsigned int n = 0;
    unsigned long long lo, hi;
    char buf[32];
    unsigned int bi = 0;
    const char *p = s;

    for (;;) {
        char c = *p;
        if (c == ',' || c == '\0') {
            char *dash;
            buf[bi] = '\0';
            if (bi == 0) {
                return -1;
            }
            dash = buf;
            while (*dash != '\0' && *dash != '-') {
                dash++;
            }
            if (*dash == '-') {
                *dash = '\0';
                if (hype_parse_uint(buf, &lo) != 0 || hype_parse_uint(dash + 1, &hi) != 0) {
                    return -1;
                }
            } else {
                if (hype_parse_uint(buf, &lo) != 0) {
                    return -1;
                }
                hi = lo;
            }
            if (hi < lo || hi >= HYPE_CFG_MAX_CPUS) {
                return -1;
            }
            while (lo <= hi) {
                if (n >= HYPE_CFG_MAX_CPUS) {
                    return -1;
                }
                w->vm.cpu_set[n++] = (unsigned int)lo;
                lo++;
            }
            bi = 0;
            if (c == '\0') {
                break;
            }
        } else if (bi + 1u < sizeof(buf)) {
            buf[bi++] = c;
        } else {
            return -1;
        }
        p++;
    }
    w->vm.cpu_set_count = n;
    return 0;
}

hype_vmw_step_t hype_vm_wizard_feed(hype_vm_wizard_t *w, const char *line,
                                    const hype_cfg_t *existing) {
    if (w == 0) {
        return HYPE_VMW_CANCELLED;
    }
    if (w->step == HYPE_VMW_DONE || w->step == HYPE_VMW_CANCELLED) {
        return w->step;
    }
    /* Available at every step: an operator who realises the machine is wrong must not have to
     * finish inventing it before they can abandon it. */
    if (line != 0 && hype_streq(line, "cancel")) {
        wiz_ok(w);
        w->step = HYPE_VMW_CANCELLED;
        return w->step;
    }
    wiz_ok(w);

    switch (w->step) {
        case HYPE_VMW_NAME:
            if (!name_is_valid(line)) {
                wiz_err(w, "a name is required: letters, digits, - and _ only");
                return w->step;
            }
            if (existing != 0) {
                unsigned int i;
                for (i = 0; i < existing->vm_count; i++) {
                    if (hype_streq(existing->vms[i].name, line)) {
                        /* Rejected here rather than at write time: two [vm.*] sections with one
                         * name is a config the parser refuses outright, so the whole file would
                         * stop loading on the next boot. */
                        wiz_err(w, "a VM with that name already exists");
                        return w->step;
                    }
                }
            }
            (void)hype_strlcpy(w->vm.name, line, HYPE_CFG_NAME_MAX);
            w->step = HYPE_VMW_OS_HINT;
            return w->step;

        case HYPE_VMW_OS_HINT:
            if (line_is_empty(line) || hype_streq(line, "linux")) {
                w->vm.os_hint = HYPE_CFG_OS_LINUX;
            } else if (hype_streq(line, "windows")) {
                w->vm.os_hint = HYPE_CFG_OS_WINDOWS;
            } else if (hype_streq(line, "bsd")) {
                w->vm.os_hint = HYPE_CFG_OS_BSD;
            } else if (hype_streq(line, "none")) {
                w->vm.os_hint = HYPE_CFG_OS_NONE;
            } else {
                wiz_err(w, "os_hint must be windows, linux, bsd or none");
                return w->step;
            }
            w->step = HYPE_VMW_VCPUS;
            return w->step;

        case HYPE_VMW_VCPUS: {
            unsigned long long v;
            if (line_is_empty(line)) {
                w->step = HYPE_VMW_MEM_MB;
                return w->step;
            }
            if (hype_parse_uint(line, &v) != 0 || v == 0ull || v > 255ull) {
                wiz_err(w, "vcpus must be a whole number from 1 to 255");
                return w->step;
            }
            w->vm.vcpus = (unsigned int)v;
            w->step = HYPE_VMW_MEM_MB;
            return w->step;
        }

        case HYPE_VMW_MEM_MB: {
            unsigned long long v;
            if (line_is_empty(line)) {
                w->step = HYPE_VMW_DISK;
                return w->step;
            }
            if (hype_parse_uint(line, &v) != 0 || v == 0ull) {
                wiz_err(w, "mem_mb must be a whole number of megabytes, at least 1");
                return w->step;
            }
            w->vm.mem_mb = (unsigned int)v;
            w->step = HYPE_VMW_DISK;
            return w->step;
        }

        case HYPE_VMW_DISK:
            w->vm.target_disk.kind = HYPE_CFG_DISK_FILE;
            if (line_is_empty(line)) {
                hype_snprintf(w->vm.target_disk.path_or_id, HYPE_CFG_PATH_MAX,
                              "\\hype\\disks\\%s.img", w->vm.name);
            } else {
                (void)hype_strlcpy(w->vm.target_disk.path_or_id, line, HYPE_CFG_PATH_MAX);
            }
            w->step = HYPE_VMW_MEDIA;
            return w->step;

        case HYPE_VMW_MEDIA:
            if (hype_streq(line, "-")) {
                w->vm.install_media[0] = '\0';
            } else if (line_is_empty(line)) {
                hype_snprintf(w->vm.install_media, HYPE_CFG_PATH_MAX, "\\iso\\%s.iso", w->vm.name);
            } else {
                (void)hype_strlcpy(w->vm.install_media, line, HYPE_CFG_PATH_MAX);
            }
            w->step = HYPE_VMW_BOOT;
            return w->step;

        case HYPE_VMW_BOOT:
            if (line_is_empty(line) || hype_streq(line, "installer")) {
                w->vm.boot = HYPE_CFG_BOOT_INSTALLER;
            } else if (hype_streq(line, "disk")) {
                w->vm.boot = HYPE_CFG_BOOT_DISK;
            } else {
                wiz_err(w, "boot must be installer or disk");
                return w->step;
            }
            if (w->vm.boot == HYPE_CFG_BOOT_INSTALLER && w->vm.install_media[0] == '\0') {
                /* Caught here because the guest-visible symptom -- a VM that boots to a firmware
                 * shell -- looks like a hype bug from inside the VM. */
                wiz_err(w, "boot=installer needs install_media; go back with cancel, or use disk");
                return w->step;
            }
            w->step = HYPE_VMW_CPU_SET;
            return w->step;

        case HYPE_VMW_CPU_SET:
            if (line_is_empty(line) || hype_streq(line, "-")) {
                w->vm.cpu_set_count = 0u; /* hype picks an isolated core */
            } else if (parse_cpu_set(w, line) != 0) {
                wiz_err(w, "cpu_set must be like 2, 2-3 or 2,4 with cores under 256");
                return w->step;
            } else if (w->vm.cpu_set_count != w->vm.vcpus) {
                /* The same rule admission enforces (HYPE_ADM_ERR_CPU_SET_COUNT_MISMATCH): one
                 * cpu_set entry per vCPU, because a vCPU IS a physical core (§10 decision 47)
                 * and cpu_set names cores. */
                wiz_err(w, "cpu_set must name exactly as many cores as vcpus");
                w->vm.cpu_set_count = 0u;
                return w->step;
            }
            w->step = HYPE_VMW_CONFIRM;
            return w->step;

        case HYPE_VMW_CONFIRM:
            if (hype_streq(line, "yes") || hype_streq(line, "y")) {
                w->step = HYPE_VMW_DONE;
            } else if (hype_streq(line, "no") || hype_streq(line, "n")) {
                w->step = HYPE_VMW_CANCELLED;
            } else {
                wiz_err(w, "answer yes or no");
            }
            return w->step;

        case HYPE_VMW_DONE:
        case HYPE_VMW_CANCELLED:
            return w->step;
    }
    return w->step;
}

void hype_vm_wizard_summary(const hype_vm_wizard_t *w, char *out, unsigned int out_len) {
    const char *os = "none";
    const char *boot = "installer";
    if (w == 0 || out == 0 || out_len == 0u) {
        return;
    }
    if (w->vm.os_hint == HYPE_CFG_OS_LINUX) os = "linux";
    else if (w->vm.os_hint == HYPE_CFG_OS_WINDOWS) os = "windows";
    else if (w->vm.os_hint == HYPE_CFG_OS_BSD) os = "bsd";
    if (w->vm.boot == HYPE_CFG_BOOT_DISK) boot = "disk";
    hype_snprintf(out, out_len, "%s: %s, %u vcpu(s), %u MiB, boot=%s, disk=%s, media=%s, cpu_set=%u",
                  w->vm.name[0] ? w->vm.name : "(unnamed)", os, w->vm.vcpus, w->vm.mem_mb, boot,
                  w->vm.target_disk.path_or_id[0] ? w->vm.target_disk.path_or_id : "(none)",
                  w->vm.install_media[0] ? w->vm.install_media : "(none)", w->vm.cpu_set_count);
}
