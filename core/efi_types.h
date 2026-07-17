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
#define EFI_NOT_FOUND ((EFI_STATUS)(EFI_ERR_BIT | 14))
#define EFI_ABORTED ((EFI_STATUS)(EFI_ERR_BIT | 21))

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

/* Values match the UEFI spec's EFI_ALLOCATE_TYPE enum exactly. */
enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
};

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    UINT32 Type,
    UINT32 MemoryType,
    UINTN Pages,
    EFI_PHYSICAL_ADDRESS *Memory);

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

/* UEFI arms a watchdog timer (5-minute default) that resets the machine
 * if a Boot Services application runs that long without calling
 * ExitBootServices() -- a long-running guest loop must disable it with
 * Timeout=0, or the firmware force-reboots mid-run. */
typedef EFI_STATUS (EFIAPI *EFI_SET_WATCHDOG_TIMER)(
    UINTN Timeout,
    UINT64 WatchdogCode,
    UINTN DataSize,
    CHAR16 *WatchdogData);

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

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE Handle,
    EFI_GUID *Protocol,
    void **Interface);

typedef void (EFIAPI *EFI_EVENT_NOTIFY)(EFI_EVENT Event, void *Context);

typedef EFI_STATUS (EFIAPI *EFI_CREATE_EVENT)(
    UINT32 Type,
    UINTN NotifyTpl,
    EFI_EVENT_NOTIFY NotifyFunction,
    void *NotifyContext,
    EFI_EVENT *Event);

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
    EFI_ALLOCATE_PAGES AllocatePages;
    void *FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_POOL AllocatePool;
    EFI_FREE_POOL FreePool;
    EFI_CREATE_EVENT CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL HandleProtocol;
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
    EFI_SET_WATCHDOG_TIMER SetWatchdogTimer;
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

/* Real-hardware GOP-rendering perf fix: Blt() lets a caller push a
 * whole shadow buffer to the real framebuffer in one call (typically
 * hardware-accelerated/DMA'd by the platform's own GOP driver),
 * instead of the catastrophically slow one-uncached-store-per-pixel
 * writes a direct FrameBufferBase pointer produces on real silicon
 * (invisible under QEMU's virtual GPU, which has no such caching-
 * attribute performance cliff). Struct/enum layout and the Blt
 * function pointer's own parameter list are transcribed directly from
 * EDK2's own MdePkg/Include/Protocol/GraphicsOutput.h, not
 * reconstructed from memory -- same discipline as this project's
 * other UEFI protocol structs. */
typedef struct {
    UINT8 Blue;
    UINT8 Green;
    UINT8 Red;
    UINT8 Reserved;
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

typedef enum {
    EfiBltVideoFill,
    EfiBltVideoToBltBuffer,
    EfiBltBufferToVideo,
    EfiBltVideoToVideo,
    EfiGraphicsOutputBltOperationMax
} EFI_GRAPHICS_OUTPUT_BLT_OPERATION;

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef EFI_STATUS(EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT)(
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
    EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation, UINTN SourceX, UINTN SourceY, UINTN DestinationX,
    UINTN DestinationY, UINTN Width, UINTN Height, UINTN Delta);

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    void *QueryMode;
    void *SetMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, per the UEFI spec:
 * 9042a9de-23dc-4a38-96fb-7aded080516a. */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

/*
 * EFI_MP_SERVICES_PROTOCOL (M3-2): lets a pre-ExitBootServices client
 * enumerate and dispatch code onto additional physical CPUs (APs) --
 * firmware itself handles the real-mode INIT-SIPI-SIPI bring-up
 * sequence internally, so this project never needs to write that
 * low-level trampoline itself. Struct layout/GUID per the UEFI
 * Platform Init spec (MdePkg's Protocol/MpService.h); only the
 * functions this project actually calls are given real signatures.
 */
#define EFI_MP_SERVICES_PROTOCOL_GUID \
    { 0x3fdda605, 0xa76e, 0x4f46, { 0xad, 0x29, 0x12, 0xf4, 0x53, 0x1b, 0x3d, 0x08 } }

#define HYPE_MP_PROCESSOR_AS_BSP_BIT 0x00000001u
#define HYPE_MP_PROCESSOR_ENABLED_BIT 0x00000002u

typedef struct {
    UINT32 Package;
    UINT32 Core;
    UINT32 Thread;
} EFI_CPU_PHYSICAL_LOCATION;

typedef struct {
    UINT64 ProcessorId;
    UINT32 StatusFlag;
    EFI_CPU_PHYSICAL_LOCATION Location;
} EFI_PROCESSOR_INFORMATION;

typedef struct EFI_MP_SERVICES_PROTOCOL EFI_MP_SERVICES_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_MP_SERVICES_GET_NUMBER_OF_PROCESSORS)(
    EFI_MP_SERVICES_PROTOCOL *This,
    UINTN *NumberOfProcessors,
    UINTN *NumberOfEnabledProcessors);

typedef EFI_STATUS (EFIAPI *EFI_MP_SERVICES_GET_PROCESSOR_INFO)(
    EFI_MP_SERVICES_PROTOCOL *This,
    UINTN ProcessorNumber,
    EFI_PROCESSOR_INFORMATION *ProcessorInfoBuffer);

typedef void (EFIAPI *EFI_AP_PROCEDURE)(void *ProcedureArgument);

typedef EFI_STATUS (EFIAPI *EFI_MP_SERVICES_STARTUP_THIS_AP)(
    EFI_MP_SERVICES_PROTOCOL *This,
    EFI_AP_PROCEDURE Procedure,
    UINTN ProcessorNumber,
    EFI_EVENT WaitEvent,
    UINTN TimeoutInMicroseconds,
    void *ProcedureArgument,
    BOOLEAN *Finished);

struct EFI_MP_SERVICES_PROTOCOL {
    EFI_MP_SERVICES_GET_NUMBER_OF_PROCESSORS GetNumberOfProcessors;
    EFI_MP_SERVICES_GET_PROCESSOR_INFO GetProcessorInfo;
    void *StartupAllAPs;
    EFI_MP_SERVICES_STARTUP_THIS_AP StartupThisAP;
    void *SwitchBSP;
    void *EnableDisableAP;
    void *WhoAmI;
};

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

/*
 * EFI_LOADED_IMAGE_PROTOCOL (FW-1/ISO-1's shared file-I/O
 * prerequisite): the standard way any UEFI application discovers
 * which volume it was itself loaded from (DeviceHandle) -- fields
 * before DeviceHandle are only present so the ones after land at
 * their correct real offsets; this project never reads them.
 * Struct layout/GUID per the UEFI spec (MdePkg's Protocol/
 * LoadedImage.h).
 */
typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    UINT32 LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    UINT64 ImageSize;
    UINT32 ImageCodeType;
    UINT32 ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(
    EFI_FILE_PROTOCOL *This,
    EFI_FILE_PROTOCOL **NewHandle,
    CHAR16 *FileName,
    UINT64 OpenMode,
    UINT64 Attributes);

typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(EFI_FILE_PROTOCOL *This);

typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(
    EFI_FILE_PROTOCOL *This,
    UINTN *BufferSize,
    void *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_INFO)(
    EFI_FILE_PROTOCOL *This,
    EFI_GUID *InformationType,
    UINTN *BufferSize,
    void *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FILE_WRITE)(
    EFI_FILE_PROTOCOL *This,
    UINTN *BufferSize,
    void *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FILE_FLUSH)(EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_FILE_DELETE)(EFI_FILE_PROTOCOL *This);

/*
 * EFI_FILE_PROTOCOL: only the functions this project actually calls
 * (Open/Close/Read/GetInfo) are given real signatures, matching every
 * other protocol struct in this file -- Write/GetPosition/SetPosition/
 * SetInfo/Flush and the 2.0-revision OpenEx/ReadEx/WriteEx/FlushEx are
 * void* placeholders purely so field offsets stay correct if a later
 * milestone needs one. Struct layout per the UEFI spec (MdePkg's
 * Protocol/SimpleFileSystem.h).
 */
struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_FILE_OPEN Open;
    EFI_FILE_CLOSE Close;
    EFI_FILE_DELETE Delete;
    EFI_FILE_READ Read;
    EFI_FILE_WRITE Write;
    void *GetPosition;
    void *SetPosition;
    EFI_FILE_GET_INFO GetInfo;
    void *SetInfo;
    EFI_FILE_FLUSH Flush;
};

#define EFI_FILE_MODE_READ 0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE 0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

typedef struct {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(void *This, EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/*
 * EFI_FILE_INFO's real layout has CreateTime/LastAccessTime/
 * ModificationTime (each a 16-byte EFI_TIME) and Attribute after
 * FileSize, then a variable-length FileName[] -- none of which this
 * project needs, so only the first two fields (Size, FileSize) are
 * given real names; GetInfo's own required-buffer-size probe
 * (EFI_BUFFER_TOO_SMALL) tells the caller exactly how much space the
 * whole variable-length structure needs, so a partial definition is
 * only ever used to read FileSize back out of a buffer sized by that
 * probe, never to construct one.
 */
typedef struct {
    UINT64 Size;
    UINT64 FileSize;
} EFI_FILE_INFO_HEADER;

#define EFI_FILE_INFO_GUID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#endif /* HYPE_EFI_TYPES_H */
