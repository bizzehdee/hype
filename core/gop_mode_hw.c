#include "gop_mode.h"
#include "efi_types.h"

/*
 * TERM-7 (#443): the hardware half -- calls the live GOP protocol's own
 * QueryMode/SetMode. Coverage-exempt (core/tests/run.sh) for the same reason
 * every other `_hw.c` is: a real UEFI protocol call, not logic to unit-test.
 */

unsigned int hype_gop_mode_enumerate(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, hype_gop_mode_t *out,
                                     unsigned int cap) {
    unsigned int n = 0;
    uint32_t mode_number;
    uint32_t max_mode;

    if (gop == 0 || gop->Mode == 0 || gop->QueryMode == 0) {
        return 0;
    }
    max_mode = gop->Mode->MaxMode;
    for (mode_number = 0; mode_number < max_mode && n < cap; mode_number++) {
        UINTN size_of_info = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
        if (gop->QueryMode(gop, mode_number, &size_of_info, &info) != EFI_SUCCESS || info == 0) {
            continue; /* a mode this firmware refuses to describe is simply not offered */
        }
        out[n].mode_number = mode_number;
        out[n].width = info->HorizontalResolution;
        out[n].height = info->VerticalResolution;
        n++;
    }
    return n;
}

int hype_gop_mode_set(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, uint32_t mode_number) {
    if (gop == 0 || gop->SetMode == 0) {
        return -1;
    }
    return (gop->SetMode(gop, mode_number) == EFI_SUCCESS) ? 0 : -1;
}
