#ifndef HYPE_DEVICES_E1000_DEV_H
#define HYPE_DEVICES_E1000_DEV_H

#include <stdint.h>

#include "../core/e1000.h"

/*
 * NET-3 (#82): the GUEST-FACING e1000 NIC. The register plane a guest driver programs.
 *
 * WHY A SECOND NIC AT ALL, when virtio-net (#81) works. Windows has no inbox virtio-net driver:
 * virtio-win has to be injected, which is exactly the "the guest needs a driver hype cannot ship"
 * problem §6a already solved for storage by giving Windows AHCI and Linux/BSD virtio. This is the
 * same split one layer over: an 82540EM is a device Windows has driven out of the box for twenty
 * years.
 *
 * WHY THE REGISTER DEFINITIONS COME FROM core/e1000.h. That header was written for the HOST driver
 * (#80), and it describes the 82540EM -- offsets, descriptor layouts, ring arithmetic, the
 * EEPROM/MAC encoding. It is the same device seen from the other side, so the definitions are the
 * same definitions. Redeclaring them here would be two statements of one fact whose failure mode
 * when they drift is a guest reading the wrong register with no error anywhere.
 *
 * WHAT THIS FILE IS NOT. It holds no ring walking: that dereferences guest-supplied addresses and
 * so needs the bounds-checked gpa map, which lives in core/e1000_dev_ring.c for exactly the reason
 * core/virtio_net_ring.c is separate from devices/virtio_net.c.
 *
 * THE MODEL IS DELIBERATELY MINIMAL, and each omission is a decision:
 *
 *   no offloads          RCTL/TCTL checksum and segmentation offload bits are accepted and ignored.
 *                        hype would have to perform what it claimed, and the NAT plane already
 *                        recomputes checksums after rewriting addresses.
 *   no multicast filter  the MTA is accepted and ignored; hype delivers what the forwarding plane
 *                        decided belongs to this guest, so a filter here would only be able to
 *                        DISCARD frames hype had already decided to deliver.
 *   no VLAN              plan.md §6e defers VLAN tagging.
 *   no MSI-X             the guest-facing virtio and AHCI models answer NO_VECTOR too; a single
 *                        legacy line is what the shared-GSI arrangement (decision 51) supports.
 *   one queue each way   the 82540EM has one, which is a reason to model it rather than a newer part.
 */

#define HYPE_E1000_DEV_PCI_VENDOR 0x8086u
#define HYPE_E1000_DEV_PCI_DEVICE 0x100Eu /* 82540EM */
#define HYPE_E1000_DEV_PCI_CLASS_BASE 0x02u
#define HYPE_E1000_DEV_PCI_CLASS_SUB 0x00u
#define HYPE_E1000_DEV_PCI_CLASS_INTERFACE 0x00u

/* BAR0 is the register window. 128 KiB is what the part decodes, and a driver sizes the BAR to find
 * out -- advertising less would make a correct driver map less than it needs. */
#define HYPE_E1000_DEV_BAR_SIZE 0x20000u

/* Registers this model does not implement read back as 0 and absorb writes. That is the 82540EM's
 * own behaviour for reserved space, and it is what lets a driver probe without faulting. */

/* CTRL bits a driver actually uses. */
#define HYPE_E1000_CTRL_RST (1u << 26)  /* Device Reset -- self-clearing */
#define HYPE_E1000_CTRL_SLU (1u << 6)   /* Set Link Up */

/* STATUS bits hype reports. LU must be set or a driver waits for link forever. */
#define HYPE_E1000_STATUS_LU (1u << 1)      /* Link Up */
#define HYPE_E1000_STATUS_FD (1u << 0)      /* Full Duplex */
#define HYPE_E1000_STATUS_SPEED_1000 (2u << 6)

#define HYPE_E1000_RCTL_EN (1u << 1)  /* Receiver Enable */
#define HYPE_E1000_TCTL_EN (1u << 1)  /* Transmitter Enable */

/* Interrupt causes this model raises. */
#define HYPE_E1000_ICR_TXDW (1u << 0) /* Transmit Descriptor Written back */
#define HYPE_E1000_ICR_LSC (1u << 2)  /* Link Status Change */
#define HYPE_E1000_ICR_RXT0 (1u << 7) /* Receiver Timer -- "packets are waiting" */

typedef struct {
    uint32_t ctrl;
    uint32_t rctl;
    uint32_t tctl;
    uint32_t ims;  /* enabled interrupt causes */
    uint32_t icr;  /* pending causes; READ-TO-CLEAR */
    uint32_t rdbal;
    uint32_t rdbah;
    uint32_t rdlen;
    uint32_t rdh;
    uint32_t rdt;
    uint32_t tdbal;
    uint32_t tdbah;
    uint32_t tdlen;
    uint32_t tdh;
    uint32_t tdt;
    uint32_t eerd;
    uint8_t mac[6];
    /* #372's rule, as for every other DMA-capable model here: the guest's PCI Bus Master Enable
     * mirrored in, so this model needs no PCI. A NIC reaches its rings by mastering the bus. */
    int bus_master;
} hype_e1000_dev_t;

/*
 * Post-power-on state. `mac` is device IDENTITY and is installed here rather than in the register
 * reset, for the same reason virtio-net's is: a driver writing CTRL.RST resets the device it is
 * talking to, and a card whose address changed under that write would look to the guest like the
 * card had been swapped -- and to hype's forwarding plane like a different guest.
 *
 * An all-zero or multicast `mac` is refused and the previous one kept; a NULL one keeps it too.
 */
void hype_e1000_dev_reset(hype_e1000_dev_t *dev, const uint8_t *mac);
int hype_e1000_dev_set_mac(hype_e1000_dev_t *dev, const uint8_t *mac);
void hype_e1000_dev_set_bus_master(hype_e1000_dev_t *dev, int enabled);

/*
 * The register window. `size_bytes` must be 4: every register in this part is a 32-bit register, and
 * a driver doing a byte access to one is a driver bug hype should report rather than paper over.
 *
 * Returns 0 when handled, -1 otherwise. An unimplemented offset inside the BAR is HANDLED -- it
 * reads 0 and absorbs writes, which is the part's own behaviour for reserved space and is what lets
 * a real driver probe without faulting.
 */
int hype_e1000_dev_reg_read(hype_e1000_dev_t *dev, uint32_t offset, uint8_t size_bytes,
                            uint32_t *out_value);
int hype_e1000_dev_reg_write(hype_e1000_dev_t *dev, uint32_t offset, uint8_t size_bytes,
                             uint32_t value);

/* Raises `cause` in ICR. The caller delivers the actual interrupt; this model has no idea what a
 * vector is. Returns nonzero if the cause is unmasked in IMS, i.e. if a line should be asserted. */
int hype_e1000_dev_raise(hype_e1000_dev_t *dev, uint32_t cause);

/* Whether a line should be asserted right now: something pending AND unmasked. Level-triggered, so
 * the caller can ask this every pass rather than tracking edges. */
int hype_e1000_dev_irq_pending(const hype_e1000_dev_t *dev);

/*
 * Whether each ring is usable for DMA: enabled, addressed, a legal length, and bus mastering on.
 * Every clause is something the driver does, and a ring walker that skipped one would be reading a
 * ring the guest has not finished publishing.
 */
int hype_e1000_dev_tx_ready(const hype_e1000_dev_t *dev);
int hype_e1000_dev_rx_ready(const hype_e1000_dev_t *dev);

/* Descriptor count implied by RDLEN/TDLEN. 0 if the length is not a whole number of descriptors --
 * refused rather than rounded, because a rounded ring length makes the device and the driver
 * disagree about where the ring ends. */
unsigned int hype_e1000_dev_rx_count(const hype_e1000_dev_t *dev);
unsigned int hype_e1000_dev_tx_count(const hype_e1000_dev_t *dev);

#endif /* HYPE_DEVICES_E1000_DEV_H */
