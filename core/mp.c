#include "mp.h"

EFI_STATUS hype_mp_locate(EFI_BOOT_SERVICES *bs, EFI_MP_SERVICES_PROTOCOL **out_mp) {
    EFI_GUID guid = EFI_MP_SERVICES_PROTOCOL_GUID;
    void *interface = 0;
    EFI_STATUS status;

    status = bs->LocateProtocol(&guid, 0, &interface);
    if (status != EFI_SUCCESS) {
        return status;
    }

    *out_mp = (EFI_MP_SERVICES_PROTOCOL *)interface;
    return EFI_SUCCESS;
}

EFI_STATUS hype_mp_pick_target_ap(EFI_MP_SERVICES_PROTOCOL *mp, UINTN *out_processor_number) {
    UINTN count = 0, enabled_count = 0;
    EFI_STATUS status;
    UINTN i;

    status = mp->GetNumberOfProcessors(mp, &count, &enabled_count);
    if (status != EFI_SUCCESS) {
        return status;
    }

    for (i = 0; i < count; i++) {
        EFI_PROCESSOR_INFORMATION info;

        status = mp->GetProcessorInfo(mp, i, &info);
        if (status != EFI_SUCCESS) {
            return status;
        }

        int is_bsp = (info.StatusFlag & HYPE_MP_PROCESSOR_AS_BSP_BIT) != 0;
        int is_enabled = (info.StatusFlag & HYPE_MP_PROCESSOR_ENABLED_BIT) != 0;

        if (is_enabled && !is_bsp) {
            *out_processor_number = i;
            return EFI_SUCCESS;
        }
    }

    return EFI_NOT_FOUND;
}
