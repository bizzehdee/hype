#ifndef HYPE_CORE_MP_H
#define HYPE_CORE_MP_H

#include "efi_types.h"

/*
 * MP (multi-processor) services (M3-2): locating
 * EFI_MP_SERVICES_PROTOCOL and picking which processor to pin a vCPU
 * to. The real AP dispatch (StartupThisAP) and the AP-side parking
 * loop are exempt hardware/UEFI-call code (mp_hw.c) -- everything here
 * is pure/mockable, same split as core/gop.c.
 */

/* Locates EFI_MP_SERVICES_PROTOCOL via bs->LocateProtocol(). Absence
 * isn't fatal to the caller -- a host with no MP services (or only one
 * CPU) just means no extra pCPU is available to pin a vCPU to. */
EFI_STATUS hype_mp_locate(EFI_BOOT_SERVICES *bs, EFI_MP_SERVICES_PROTOCOL **out_mp);

/*
 * Picks the first enabled processor that is NOT the bootstrap
 * processor (BSP), by calling mp->GetNumberOfProcessors() then
 * mp->GetProcessorInfo() for each processor number in turn. Returns
 * EFI_SUCCESS with *out_processor_number set if one was found;
 * propagates any error from the underlying calls; returns
 * EFI_NOT_FOUND if every processor is either the BSP or disabled
 * (i.e. this host has no usable extra pCPU to pin a vCPU to).
 */
EFI_STATUS hype_mp_pick_target_ap(EFI_MP_SERVICES_PROTOCOL *mp, UINTN *out_processor_number);

#endif /* HYPE_CORE_MP_H */
