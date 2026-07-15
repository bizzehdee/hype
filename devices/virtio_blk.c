#include "virtio_blk.h"

static void reset_negotiation_state(hype_virtio_blk_t *dev) {
    dev->device_feature_select = 0;
    dev->driver_feature_select = 0;
    dev->driver_features = 0;
    dev->device_status = 0;
    dev->queue_select = 0;
    dev->queue_size = HYPE_VIRTIO_BLK_QUEUE_SIZE_MAX;
    dev->queue_enable = 0;
    dev->queue_desc = 0;
    dev->queue_driver = 0;
    dev->queue_device = 0;
    dev->isr_status = 0;
    dev->last_avail_idx = 0;
}

void hype_virtio_blk_reset(hype_virtio_blk_t *dev, uint64_t capacity_sectors) {
    reset_negotiation_state(dev);
    dev->capacity_sectors = capacity_sectors;
}

int hype_virtio_blk_common_cfg_read(const hype_virtio_blk_t *dev, uint32_t offset, uint8_t size_bytes,
                                     uint32_t *out_value) {
    if (offset >= HYPE_VIRTIO_COMMON_CFG_SIZE) {
        return -1;
    }

    switch (offset) {
        case HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT:
            if (size_bytes != 4u) return -1;
            *out_value = dev->device_feature_select;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE:
            if (size_bytes != 4u) return -1;
            if (dev->device_feature_select == 1u) {
                *out_value = 1u << (HYPE_VIRTIO_F_VERSION_1_BIT - 32u);
            } else {
                *out_value = 0u;
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT:
            if (size_bytes != 4u) return -1;
            *out_value = dev->driver_feature_select;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE:
            if (size_bytes != 4u) return -1;
            if (dev->driver_feature_select == 0u) {
                *out_value = (uint32_t)(dev->driver_features & 0xFFFFFFFFu);
            } else if (dev->driver_feature_select == 1u) {
                *out_value = (uint32_t)(dev->driver_features >> 32);
            } else {
                *out_value = 0u;
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_MSIX_CONFIG:
            if (size_bytes != 2u) return -1;
            *out_value = 0xFFFFu; /* NO_VECTOR -- MSI-X not modeled */
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_NUM_QUEUES:
            if (size_bytes != 2u) return -1;
            *out_value = 1u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS:
            if (size_bytes != 1u) return -1;
            *out_value = dev->device_status;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_CONFIG_GENERATION:
            if (size_bytes != 1u) return -1;
            *out_value = 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT:
            if (size_bytes != 2u) return -1;
            *out_value = dev->queue_select;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE:
            if (size_bytes != 2u) return -1;
            *out_value = (dev->queue_select == 0u) ? dev->queue_size : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_MSIX_VECTOR:
            if (size_bytes != 2u) return -1;
            *out_value = 0xFFFFu;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE:
            if (size_bytes != 2u) return -1;
            *out_value = (dev->queue_select == 0u) ? dev->queue_enable : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_NOTIFY_OFF:
            if (size_bytes != 2u) return -1;
            *out_value = 0u; /* single queue -- notify_off is always 0 */
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO:
            if (size_bytes != 4u) return -1;
            *out_value = (dev->queue_select == 0u) ? (uint32_t)(dev->queue_desc & 0xFFFFFFFFu) : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI:
            if (size_bytes != 4u) return -1;
            *out_value = (dev->queue_select == 0u) ? (uint32_t)(dev->queue_desc >> 32) : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO:
            if (size_bytes != 4u) return -1;
            *out_value = (dev->queue_select == 0u) ? (uint32_t)(dev->queue_driver & 0xFFFFFFFFu) : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI:
            if (size_bytes != 4u) return -1;
            *out_value = (dev->queue_select == 0u) ? (uint32_t)(dev->queue_driver >> 32) : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO:
            if (size_bytes != 4u) return -1;
            *out_value = (dev->queue_select == 0u) ? (uint32_t)(dev->queue_device & 0xFFFFFFFFu) : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI:
            if (size_bytes != 4u) return -1;
            *out_value = (dev->queue_select == 0u) ? (uint32_t)(dev->queue_device >> 32) : 0u;
            return 0;
        default:
            /* Reserved/unmodeled byte within the 56-byte structure --
             * reads as 0, the standard "reserved" convention. */
            *out_value = 0u;
            return 0;
    }
}

int hype_virtio_blk_common_cfg_write(hype_virtio_blk_t *dev, uint32_t offset, uint8_t size_bytes,
                                      uint32_t value) {
    if (offset >= HYPE_VIRTIO_COMMON_CFG_SIZE) {
        return -1;
    }

    switch (offset) {
        case HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT:
            if (size_bytes != 4u) return -1;
            dev->device_feature_select = value;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT:
            if (size_bytes != 4u) return -1;
            dev->driver_feature_select = value;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE:
            if (size_bytes != 4u) return -1;
            if (dev->driver_feature_select == 0u) {
                dev->driver_features = (dev->driver_features & ~0xFFFFFFFFull) | value;
            } else if (dev->driver_feature_select == 1u) {
                dev->driver_features = (dev->driver_features & 0xFFFFFFFFull) | ((uint64_t)value << 32);
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS:
            if (size_bytes != 1u) return -1;
            if ((uint8_t)value == 0u) {
                reset_negotiation_state(dev);
            } else {
                dev->device_status = (uint8_t)value;
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT:
            if (size_bytes != 2u) return -1;
            dev->queue_select = (uint16_t)value;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE:
            if (size_bytes != 2u) return -1;
            if (dev->queue_select == 0u) {
                dev->queue_size =
                    ((uint16_t)value <= HYPE_VIRTIO_BLK_QUEUE_SIZE_MAX) ? (uint16_t)value : HYPE_VIRTIO_BLK_QUEUE_SIZE_MAX;
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE:
            if (size_bytes != 2u) return -1;
            if (dev->queue_select == 0u) {
                dev->queue_enable = (uint16_t)value;
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO:
            if (size_bytes != 4u) return -1;
            if (dev->queue_select == 0u) {
                dev->queue_desc = (dev->queue_desc & 0xFFFFFFFF00000000ull) | value;
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI:
            if (size_bytes != 4u) return -1;
            if (dev->queue_select == 0u) {
                dev->queue_desc = (dev->queue_desc & 0xFFFFFFFFull) | ((uint64_t)value << 32);
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO:
            if (size_bytes != 4u) return -1;
            if (dev->queue_select == 0u) {
                dev->queue_driver = (dev->queue_driver & 0xFFFFFFFF00000000ull) | value;
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI:
            if (size_bytes != 4u) return -1;
            if (dev->queue_select == 0u) {
                dev->queue_driver = (dev->queue_driver & 0xFFFFFFFFull) | ((uint64_t)value << 32);
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO:
            if (size_bytes != 4u) return -1;
            if (dev->queue_select == 0u) {
                dev->queue_device = (dev->queue_device & 0xFFFFFFFF00000000ull) | value;
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI:
            if (size_bytes != 4u) return -1;
            if (dev->queue_select == 0u) {
                dev->queue_device = (dev->queue_device & 0xFFFFFFFFull) | ((uint64_t)value << 32);
            }
            return 0;
        /* Read-only registers (DEVICE_FEATURE, NUM_QUEUES,
         * CONFIG_GENERATION, QUEUE_NOTIFY_OFF) and unmodeled ones
         * (MSIX_CONFIG, QUEUE_MSIX_VECTOR) silently ignore a write --
         * the standard "not this project's concern" convention, same
         * as devices/ahci.h's own read-only-register handling. */
        default:
            return 0;
    }
}

int hype_virtio_blk_device_cfg_read(const hype_virtio_blk_t *dev, uint32_t offset, uint8_t size_bytes,
                                     uint32_t *out_value) {
    if (offset >= HYPE_VIRTIO_BLK_CFG_SIZE) {
        return -1;
    }

    switch (offset) {
        case HYPE_VIRTIO_BLK_CFG_CAPACITY_LO:
            if (size_bytes != 4u) return -1;
            *out_value = (uint32_t)(dev->capacity_sectors & 0xFFFFFFFFu);
            return 0;
        case HYPE_VIRTIO_BLK_CFG_CAPACITY_HI:
            if (size_bytes != 4u) return -1;
            *out_value = (uint32_t)(dev->capacity_sectors >> 32);
            return 0;
        default:
            /* size_max/seg_max/geometry/blk_size are all gated behind
             * optional feature bits this project deliberately doesn't
             * offer (see this header's own top comment) -- a
             * compliant driver has no reason to read them, and this
             * project has no real value to report for them. */
            *out_value = 0u;
            return 0;
    }
}

uint8_t hype_virtio_blk_isr_read(hype_virtio_blk_t *dev) {
    uint8_t value = dev->isr_status;
    dev->isr_status = 0;
    return value;
}

int hype_virtio_blk_is_queue_ready(const hype_virtio_blk_t *dev) {
    return (dev->device_status & HYPE_VIRTIO_STATUS_DRIVER_OK) != 0 && dev->queue_enable != 0 &&
           dev->queue_size != 0 && dev->queue_desc != 0;
}

void hype_virtq_decode_desc(const uint8_t raw[16], hype_virtq_desc_t *out) {
    out->addr = (uint64_t)raw[0] | ((uint64_t)raw[1] << 8) | ((uint64_t)raw[2] << 16) |
                ((uint64_t)raw[3] << 24) | ((uint64_t)raw[4] << 32) | ((uint64_t)raw[5] << 40) |
                ((uint64_t)raw[6] << 48) | ((uint64_t)raw[7] << 56);
    out->len = (uint32_t)raw[8] | ((uint32_t)raw[9] << 8) | ((uint32_t)raw[10] << 16) |
               ((uint32_t)raw[11] << 24);
    out->flags = (uint16_t)(raw[12] | (raw[13] << 8));
    out->next = (uint16_t)(raw[14] | (raw[15] << 8));
}
