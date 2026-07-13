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
    void *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif /* HYPE_EFI_TYPES_H */
