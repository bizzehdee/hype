/*
 * Shared virtio-pci capability discovery for the microtest guests (#569).
 *
 * VIRTIO 1.x §4.1.4 has a driver locate the four virtio-pci regions -- common configuration,
 * notify, ISR and device-specific configuration -- by WALKING THE PCI CAPABILITY LIST for vendor
 * capabilities (ID 0x09) whose cfg_type says which region each one is. Each capability names the
 * BAR and the offset and length within it. That is how Linux's virtio_pci_modern and FreeBSD's
 * virtio_pci find anything at all, so it is the only discovery path that proves a real driver
 * could bind.
 *
 * This lives in its own header because BOTH virtio front-ends need it and a test that hardcodes
 * hype's own offsets is written against hype rather than against the spec -- the weaker of the
 * two. virtionet.c grew this walk first; virtioblk.c used fixed offsets and said so. Two copies of
 * a bus walk would drift, which is the failure #294 is open about across the drivers, so there is
 * one.
 *
 * A virtio_pci_cap is:
 *   cap_vndr(1) cap_next(1) cap_len(1) cfg_type(1) bar(1) padding(3) offset(4) length(4)
 * and the NOTIFY capability adds notify_off_multiplier(4) after that.
 */
#ifndef MICRO_VIRTIO_H
#define MICRO_VIRTIO_H

#include "micro_pci.h"

#define MICRO_VIRTIO_PCI_STATUS_CAP_LIST 0x0010u
#define MICRO_VIRTIO_PCI_CAP_POINTER 0x34u
#define MICRO_VIRTIO_PCI_CAP_ID_VENDOR 0x09u

#define MICRO_VIRTIO_CAP_COMMON_CFG 1u
#define MICRO_VIRTIO_CAP_NOTIFY_CFG 2u
#define MICRO_VIRTIO_CAP_ISR_CFG 3u
#define MICRO_VIRTIO_CAP_DEVICE_CFG 4u

#define MICRO_VIRTIO_NOT_FOUND 0xFFFFFFFFu

typedef struct micro_virtio_caps {
    uint32_t common_off;
    uint32_t notify_off;
    uint32_t isr_off;
    uint32_t device_off;
    uint32_t common_len;
    uint32_t notify_len;
    uint32_t isr_len;
    uint32_t device_len;
    uint32_t notify_mult;
} micro_virtio_caps_t;

/*
 * Discover the four regions on `dev`. `want_bar` is the BAR every region must live in; a
 * capability pointing anywhere else is a failure rather than a thing to follow, because the caller
 * has already sized and placed that one BAR and would map nothing.
 *
 * Returns 0 with *out filled, or -1 having ALREADY reported the verdict naming which part of the
 * chain failed. Each field is MICRO_VIRTIO_NOT_FOUND until a capability sets it, so a partial
 * chain can say which region is missing instead of only that the chain is incomplete.
 */
static inline int micro_virtio_walk_caps(unsigned dev, unsigned want_bar, const char *name,
                                         micro_virtio_caps_t *out) {
    uint32_t status = micro_pci_read32(dev, 0x04u);
    unsigned cap;
    unsigned guard = 0;
    int found = 0;

    out->common_off = MICRO_VIRTIO_NOT_FOUND;
    out->notify_off = MICRO_VIRTIO_NOT_FOUND;
    out->isr_off = MICRO_VIRTIO_NOT_FOUND;
    out->device_off = MICRO_VIRTIO_NOT_FOUND;
    out->common_len = 0u;
    out->notify_len = 0u;
    out->isr_len = 0u;
    out->device_len = 0u;
    out->notify_mult = 0u;

    if (((status >> 16) & MICRO_VIRTIO_PCI_STATUS_CAP_LIST) == 0u) {
        micro_fail(name, "the device does not advertise a capability list, so a real virtio driver "
                         "has no way to find its configuration regions (PCI status bit 4 clear)");
        return -1;
    }
    cap = micro_pci_read32(dev, MICRO_VIRTIO_PCI_CAP_POINTER) & 0xFFu;

    /* The chain is device-supplied, so it is bounded: 48 links is far more than any real device
     * has, and a chain that loops must not spin here forever. */
    while (cap >= 0x40u && cap < 0x100u && guard++ < 48u) {
        uint32_t w0 = micro_pci_read32(dev, cap); /* vndr | next | len | cfg_type */
        uint32_t bar = micro_pci_read32(dev, cap + 4u) & 0xFFu;
        uint32_t off = micro_pci_read32(dev, cap + 8u);
        uint32_t len = micro_pci_read32(dev, cap + 12u);
        unsigned id = w0 & 0xFFu;
        unsigned next = (w0 >> 8) & 0xFFu;
        unsigned type = (w0 >> 24) & 0xFFu;

        if (id == MICRO_VIRTIO_PCI_CAP_ID_VENDOR) {
            micro_puts("micro/");
            micro_puts(name);
            micro_puts(": cap at ");
            micro_put_hex(cap);
            micro_puts(" type ");
            micro_put_uint(type);
            micro_puts(" bar ");
            micro_put_uint(bar);
            micro_puts(" off ");
            micro_put_hex(off);
            micro_puts(" len ");
            micro_put_hex(len);
            micro_puts("\n");

            if (bar != want_bar) {
                micro_fail(name, "a virtio capability points at a BAR this test did not place -- "
                                 "the driver would map nothing");
                return -1;
            }
            if (type == MICRO_VIRTIO_CAP_COMMON_CFG) {
                out->common_off = off;
                out->common_len = len;
                found++;
            } else if (type == MICRO_VIRTIO_CAP_NOTIFY_CFG) {
                out->notify_off = off;
                out->notify_len = len;
                out->notify_mult = micro_pci_read32(dev, cap + 16u);
                found++;
            } else if (type == MICRO_VIRTIO_CAP_ISR_CFG) {
                out->isr_off = off;
                out->isr_len = len;
                found++;
            } else if (type == MICRO_VIRTIO_CAP_DEVICE_CFG) {
                out->device_off = off;
                out->device_len = len;
                found++;
            }
        }
        if (next == 0u || next == cap) {
            break;
        }
        cap = next;
    }

    if (out->common_off == MICRO_VIRTIO_NOT_FOUND || out->notify_off == MICRO_VIRTIO_NOT_FOUND ||
        out->isr_off == MICRO_VIRTIO_NOT_FOUND || out->device_off == MICRO_VIRTIO_NOT_FOUND) {
        /* Which ones were found is printed, because "the chain is incomplete" sends a reader to
         * the whole chain while "three of four, device-cfg missing" names the capability to look
         * at. */
        micro_puts("micro/");
        micro_puts(name);
        micro_puts(": vendor caps found=");
        micro_put_uint((unsigned)found);
        micro_puts(" common=");
        micro_put_hex(out->common_off);
        micro_puts(" notify=");
        micro_put_hex(out->notify_off);
        micro_puts(" isr=");
        micro_put_hex(out->isr_off);
        micro_puts(" device=");
        micro_put_hex(out->device_off);
        micro_puts("\n");
        micro_fail(name, "the capability chain does not describe all four virtio regions "
                         "(common/notify/isr/device) -- a real driver binds by walking this chain, "
                         "so whatever is missing here is a region no driver can find");
        return -1;
    }
    /*
     * The multiplier is what turns a queue's notify_off into an address. Zero puts every queue's
     * doorbell at one address: legal for a single-queue device, and wrong for any device hype
     * presents, since it publishes 4 for both front-ends. Checked here rather than per test so a
     * future multi-queue front-end cannot inherit a silent 0.
     */
    if (out->notify_mult == 0u) {
        micro_fail(name, "notify_off_multiplier is 0, so every queue's doorbell would be at one "
                         "address and the device could not tell one queue's kick from another's");
        return -1;
    }
    return 0;
}

#endif /* MICRO_VIRTIO_H */
