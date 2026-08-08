#ifndef HYPE_CORE_PE_IDENT_H
#define HYPE_CORE_PE_IDENT_H

#include <stdint.h>

/*
 * #364: given a guest instruction pointer, which loaded module is it in?
 *
 * A guest RIP in firmware or bootloader space is an opaque number. On the Intel
 * box FreeBSD stops before its kernel runs and spins at 0x7f466526 and
 * 0x7f4493be -- addresses that mean nothing on their own, and cannot be resolved
 * against the OVMF image on disk because UEFI relocates every driver to a
 * runtime address. The usual fallback, EDK2's debug-port log of module load
 * addresses, is unavailable too: the shipped OVMF is a RELEASE build and writes
 * nothing (OVMF-DBGPORT reported writes=0).
 *
 * So do what a debugger does: walk backwards from the address to the PE image
 * that contains it, and read the module's own name out of it. UEFI loads every
 * PE/COFF image on a 4 KiB boundary starting with "MZ", so the scan is bounded
 * and cheap.
 *
 * The name comes from the CodeView (RSDS) record in the debug directory, which
 * EDK2 builds populate with the module's .pdb path -- that is where a name like
 * "DxeCore" or "loader" actually lives in a release binary.
 *
 * Pure: all memory access goes through an injected reader, so this is fully unit
 * testable and contains no MMIO, no allocation and no globals.
 */

/*
 * Reads `len` bytes at guest virtual address `va` into `dst`. Returns 0 on
 * success, non-zero if the address is not mapped or readable -- the scan relies
 * on that failure to stop rather than fault.
 */
typedef int (*hype_pe_read_fn)(void *ctx, uint64_t va, void *dst, uint64_t len);

/* UEFI images are page-aligned, so the search steps a page at a time. 4096
 * pages is 16 MiB of backward scan: comfortably more than any single EDK2
 * driver, and bounded so a wrong guess cannot spin. */
#define HYPE_PE_SCAN_STEP 4096u
#define HYPE_PE_SCAN_MAX_PAGES 4096u

/*
 * Finds the base of the PE image containing `addr`: scans back page by page for
 * an "MZ" header whose e_lfanew points at a "PE\0\0" signature, and which claims
 * a size that actually covers `addr`.
 *
 * Returns the image base, or 0 if none was found within the scan bound. The
 * size check matters -- without it the scan happily returns whatever unrelated
 * image happens to sit below, which is worse than saying nothing.
 */
uint64_t hype_pe_find_image_base(hype_pe_read_fn read, void *ctx, uint64_t addr);

/*
 * Extracts the module name for the image at `base` into `out` (NUL-terminated,
 * truncated to `out_size`). Reads the debug directory's CodeView RSDS record and
 * takes the basename of the .pdb path, minus the extension -- so
 * "d:\\build\\DxeCore.pdb" becomes "DxeCore".
 *
 * Returns 0 on success, -1 if the image has no usable debug record. A release
 * build without one is a legitimate outcome, not an error to hide: the caller
 * should report the base address alone rather than invent a name.
 */
int hype_pe_module_name(hype_pe_read_fn read, void *ctx, uint64_t base, char *out,
                        unsigned int out_size);

#endif /* HYPE_CORE_PE_IDENT_H */
