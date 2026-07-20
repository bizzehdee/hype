#ifndef HYPE_CORE_FILE_IO_H
#define HYPE_CORE_FILE_IO_H

#include "efi_types.h"

/*
 * Reads a whole file from the SAME volume hype.efi itself was loaded
 * from (FW-1/ISO-1's shared prerequisite): EFI_LOADED_IMAGE_PROTOCOL
 * on our own ImageHandle gives DeviceHandle (the volume we were loaded
 * from); EFI_SIMPLE_FILE_SYSTEM_PROTOCOL on THAT handle opens its root
 * directory; ordinary EFI_FILE_PROTOCOL Open/GetInfo/Read/Close from
 * there. Confirmed neither FW-1 (loading fw/OVMF_CODE.fd/OVMF_VARS.fd)
 * nor ISO-1 (a real installer ISO) need M5's disk driver for this --
 * that's for ongoing GUEST-visible host-backed storage, a different
 * concern; this is plain UEFI Boot-Services file I/O, available before
 * ExitBootServices() the same as every other Boot Services call in
 * this project.
 *
 * Protocol GUIDs/struct layouts (EFI_LOADED_IMAGE_PROTOCOL,
 * EFI_SIMPLE_FILE_SYSTEM_PROTOCOL, EFI_FILE_PROTOCOL, EFI_FILE_INFO,
 * core/efi_types.h) are well-known, unambiguous UEFI specification
 * constants -- not project-specific values needing empirical
 * verification the way this project's AMD-specific VMCB work did.
 *
 * Split into hype_file_locate_root() (the only piece tied to "my own
 * image handle") and hype_file_get_size()/hype_file_read_into() (given
 * an already-resolved root directory) -- all three fully unit tested
 * against fake protocol structs, same dependency-injection pattern
 * already established by core/gop.c's hype_gop_locate()/core/mp.c's
 * hype_mp_locate().
 */

/*
 * Resolves the root directory of the volume `image_handle` (this
 * project's own loaded hype.efi image) was itself loaded from: two
 * chained HandleProtocol calls (EFI_LOADED_IMAGE_PROTOCOL, then
 * EFI_SIMPLE_FILE_SYSTEM_PROTOCOL on its DeviceHandle) followed by
 * OpenVolume. Fully unit tested against fake protocol structs.
 */
EFI_STATUS hype_file_locate_root(EFI_HANDLE image_handle, EFI_BOOT_SERVICES *bs,
                                  EFI_FILE_PROTOCOL **out_root);

/*
 * Gets the size, in bytes, of `path` (an absolute UEFI path, e.g.
 * L"\\EFI\\hype\\OVMF_CODE.fd") opened relative to `root`. Handles
 * EFI_FILE_INFO's own variable-length-structure GetInfo protocol: a
 * first call with a NULL/zero-size buffer to learn the required size
 * (EFI_BUFFER_TOO_SMALL), then an AllocatePool'd buffer of exactly
 * that size for the real call. Returns EFI_SUCCESS with *out_size set,
 * or a real error status otherwise.
 */
EFI_STATUS hype_file_get_size(EFI_FILE_PROTOCOL *root, EFI_BOOT_SERVICES *bs, CHAR16 *path,
                               UINT64 *out_size);

/*
 * Reads the whole content of `path` (relative to `root`) into `buffer`
 * (caller-owned, must be at least `buffer_size` bytes -- typically
 * exactly hype_file_get_size()'s own result). Returns EFI_SUCCESS only
 * if the *entire* file was read into the buffer -- a short read (e.g.
 * `buffer_size` smaller than the real file) is EFI_ABORTED, since this
 * project always wants the whole file or nothing, never a partial
 * read silently accepted.
 */
EFI_STATUS hype_file_read_into(EFI_FILE_PROTOCOL *root, CHAR16 *path, void *buffer, UINTN buffer_size);

/* GLADDER-10(a): read `len` bytes at file byte `offset` into `buffer` (one
 * chunk of a multi-GB ISO loaded into non-contiguous buffers). */
EFI_STATUS hype_file_read_range(EFI_FILE_PROTOCOL *root, CHAR16 *path, UINT64 offset, void *buffer,
                                UINTN len);

/*
 * Writes `size` bytes of `buffer` to `path` (relative to `root`),
 * replacing any existing file (a stale longer copy is Delete()d first so
 * no old tail survives). Opens CREATE|WRITE|READ, writes, Flush()es, and
 * Closes. Returns EFI_SUCCESS only if the whole buffer was written
 * (a short write is EFI_ABORTED). Used to drop hype's captured console
 * log (core/logbuf.h) onto the boot volume before ExitBootServices for
 * serial-less real-hardware debugging. Same Boot-Services file I/O as
 * the read path; unit tested against fake protocol structs.
 */
EFI_STATUS hype_file_write_new(EFI_FILE_PROTOCOL *root, CHAR16 *path, const void *buffer, UINTN size);

/*
 * Deletes `path` (relative to `root`) if it exists. Returns EFI_SUCCESS
 * on delete, or the Open error (e.g. EFI_NOT_FOUND) if it wasn't there.
 * Used once at startup to clear a stale log from a previous run before
 * the in-place overwrite flushes begin.
 */
EFI_STATUS hype_file_delete(EFI_FILE_PROTOCOL *root, CHAR16 *path);

#endif /* HYPE_CORE_FILE_IO_H */
