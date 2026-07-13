#ifndef HYPE_EFI_TYPES_H
#define HYPE_EFI_TYPES_H

/*
 * Minimal hand-rolled EFI type/struct subset (SETUP-6). Swap this header
 * for gnu-efi's <efi.h>/<efilib.h> once that package is available in the
 * build environment (plan.md §8 allows either clang/lld direct or
 * GNU-EFI) -- this file exists only because gnu-efi wasn't installed
 * when this code was written, not because it's the preferred long-term
 * source of these definitions.
 *
 * Struct layouts below follow the UEFI Specification's field order and
 * natural alignment exactly, up through the fields this code actually
 * uses (EFI_SYSTEM_TABLE.ConOut, EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL.Reset/
 * OutputString). Trailing fields are declared as opaque pointers/reserved
 * slots purely to keep any *earlier* field's offset correct -- do not
 * read/write through them until they're given real types.
 */

typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef long long INT64;
typedef int INT32;
typedef UINT64 UINTN;
typedef UINT16 CHAR16;
typedef UINT8 BOOLEAN;
typedef UINT64 EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;

#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS ((EFI_STATUS)0)
#define EFI_ERR_BIT 0x8000000000000000ULL
#define EFI_BUFFER_TOO_SMALL ((EFI_STATUS)(EFI_ERR_BIT | 5))

typedef UINT64 EFI_PHYSICAL_ADDRESS;
typedef UINT64 EFI_VIRTUAL_ADDRESS;

/* Values match the UEFI spec's EFI_MEMORY_TYPE enum exactly. */
enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiUnacceptedMemoryType,
    EfiMaxMemoryType
};

typedef struct {
    UINT32 Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    BOOLEAN ExtendedVerification);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    CHAR16 *String);

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET Reset;
    EFI_TEXT_STRING OutputString;
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    void *ClearScreen;
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
};

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey,
    UINTN *DescriptorSize,
    UINT32 *DescriptorVersion);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    UINT32 PoolType,
    UINTN Size,
    void **Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(void *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle,
    UINTN MapKey);

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID *Protocol,
    void *Registration,
    void **Interface);

/*
 * Full 44-function-pointer layout per the UEFI spec's
 * EFI_BOOT_SERVICES_REVISION table, so that fields past DescriptorVersion
 * (needed later, e.g. ExitBootServices for M1-4) land at correct offsets.
 * Only the handful this code actually calls are given real signatures;
 * the rest are void* placeholders -- same rule as the rest of this file.
 */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    void *AllocatePages;
    void *FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_POOL AllocatePool;
    EFI_FREE_POOL FreePool;
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    void *HandleProtocol;
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
    void *GetNextMonotonicCount;
    void *Stall;
    void *SetWatchdogTimer;
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL LocateProtocol;
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

/* Values match the UEFI spec's EFI_GRAPHICS_PIXEL_FORMAT enum exactly. */
enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
};

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    void *QueryMode;
    void *SetMode;
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, per the UEFI spec:
 * 9042a9de-23dc-4a38-96fb-7aded080516a. */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif /* HYPE_EFI_TYPES_H */
