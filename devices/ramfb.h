#ifndef HYPE_DEVICES_RAMFB_H
#define HYPE_DEVICES_RAMFB_H

#include <stdint.h>

/*
 * QEMU-compatible "ramfb" device (VIDEO-2): the mechanism this
 * project's vendored, unmodified OVMF (M4-2, whose build already
 * includes OvmfPkg/QemuRamfbDxe -- confirmed present in
 * edk2/Build/OvmfX64/.../QemuRamfbDxe.efi) uses to expose a
 * pre-OS-driver Graphics Output Protocol backed by a framebuffer the
 * *guest itself* allocates in its own RAM, with no PCI BAR/MMIO device
 * at all -- just an fw_cfg file (devices/fw_cfg.h) the guest writes
 * into to tell the host where that framebuffer is.
 *
 * Struct layout, field order, and the fixed pixel format below are
 * transcribed directly from this project's own vendored driver source
 * (edk2/OvmfPkg/QemuRamfbDxe/QemuRamfb.c's `RAMFB_CONFIG`,
 * `#pragma pack(1)`) and its GUID header (Include/Guid/QemuRamfb.h) --
 * fetched and read for this task, not reconstructed from memory. The
 * driver writes every field byte-swapped to big-endian
 * (`SwapBytes64`/`SwapBytes32`) before handing the 28-byte buffer to
 * `QemuFwCfgWriteBytes()`, which prefers the DMA write path
 * (`FW_CFG_DMA_CTL_WRITE`) whenever the host advertises DMA support --
 * this project's own fw_cfg model already does (devices/fw_cfg.c's
 * `FW_CFG_VERSION_DMA` bit), so that's the path this device is
 * actually exercised through, not the classic port-based write.
 *
 * Named "etc/ramfb" -- the exact fw_cfg file name
 * `QemuFwCfgFindFile()` looks up (QemuRamfb.c's
 * `InitializeQemuRamfb()`), and must be registered as exactly
 * HYPE_RAMFB_CONFIG_SIZE bytes (that driver rejects any other size).
 */

#define HYPE_RAMFB_CONFIG_SIZE 28

/* DRM_FORMAT_XRGB8888 -- the only pixel format this project's vendored
 * OVMF driver ever sends (QemuRamfb.c's RAMFB_FORMAT); 4 bytes/pixel. */
#define HYPE_RAMFB_FORMAT_XRGB8888 0x34325258u

typedef struct {
    uint64_t address; /* guest-physical address of the framebuffer, chosen by the guest */
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride; /* bytes per scanline */
} hype_ramfb_config_t;

/*
 * Decodes HYPE_RAMFB_CONFIG_SIZE raw bytes (the exact wire layout
 * above, every field big-endian) into `*out`, native byte order. Pure
 * bit manipulation, no guest-memory access of its own -- `buf` is
 * whatever devices/fw_cfg.c's "etc/ramfb" writable-file buffer
 * currently holds (arch/x86_64/svm/svm_vcpu.c's fw_cfg DMA glue is
 * what got guest-written bytes into it in the first place).
 */
void hype_ramfb_decode_config(const uint8_t buf[HYPE_RAMFB_CONFIG_SIZE], hype_ramfb_config_t *out);

#endif /* HYPE_DEVICES_RAMFB_H */
