---
title: 'EDK2'
source_page_id: '4624'
source_revision_id: '30304'
source_timestamp: '2026-03-16T20:35:59Z'
source_format: mediawiki
---

# EDK2

> This page or section is a work in progress and may be incomplete.
EDK2 is the official development environment for [UEFI](UEFI) applications. It is developed by the open-source Tianocore project, of which Intel, HP, and Microsoft are the primary contributors. Although it may be bigger than [GNU-EFI](GNU-EFI), it has more features, hence, some OS developers may prefer it over GNU-EFI.

## What is EDK2?
EDK2 is full on implementation of the [UEFI](UEFI) spec. It contains the Open Virtual Machine Firmware (OVMF) UEFI firmware package, which is primarily targeted at [QEMU](QEMU), and is used by many OS developers to test out their UEFI applications. It also has [ARM](ARM) and [AArch64](AArch64) firmware packages for QEMU and various boards, and [RISC-V](RISC-V) firmware packages for various [HiFive](HiFive) boards. It also has tools for UEFI driver developers, but that is outside the scope of this wiki.

Of most interest the OS developers is the UEFI library and the EDK2 build system. This article will show you the structure of EDK2, how to build firmware for QEMU and real machines, and how to build UEFI applications.

## Structure of EDK2
EDK2 is made up of multiple repos, all of which can be found at [https://github.com/tianocore](https://github.com/tianocore). The main ones are:

- edk2, which contains the build system, library, OVMF, and ArmVirt firmware

- edk2-platforms, which contains firmware for various real hardware platforms, such as the SiFive U450, Raspberry Pi, and quite a few others

- edk2-non-osi, which contains proprietary binary blobs for various platforms.

EDK2 itself is written in C with some Python, and is licensed under the BSD 2 Clause + Patent license. The blobs are subject to their own licenses.
### Directory structure
EDK2 is split up into multiple subdirectories, where each one normally contains a "Package". The OvmfPkg subdirectory contains OVMF, the MdePkg subdirectory contains the UEFI library, and so on. Each one of these contains a file ending in .dsc. This file describes the package inside of the folder, and contains things such as dependencies, different components, and so on. Each component is described its own file that contains sources, application type, and so on.
TODO: Describe this in more detail
## Building EDK2's base
No matter what you are trying to build from EDK2, these steps must be completed. First, you must decide if you want to only build stuff inside of the main EDK2 repo, or if you also want stuff from edk2-platforms. Note that these guides assume you are on a Linux system (Windows Subsystem for Linux is OK)
### Building without edk2-platforms
If you only want the UEFI library or virtual machine firmware, then this guide will suffice. Make sure you have all the dependencies installed first:
```
sudo apt install build-essential git python2 uuid-dev nasm acpica-tools        # Or whatever your package manager is
```
First, we must clone EDK2
```
git clone https://github.com/tianocore/edk2.git
```
Note that this is the latest non-stable source. If you want the stable source then run:
```
git clone https://github.com/tianocore/edk2.git -b"stable/202011"
```
or whatever is appropriate at the time you are reading this. After that, we now must clone the submodules
```
cd edk2
git submodule update --init
```
Then, we need to setup the environment for EDK2
```
export EDK_TOOLS_PATH=$PWD/BaseTools
# If you are compiling for ARM, then this is needed
sudo apt install gcc-aarch64-linux-gnu        # Or whatever your package manager is
export GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
# This is needed on RISC-V
sudo apt install gcc-riscv64-linux-gnu        # Or whatever your package manager is
export GCC5_RISCV64_PREFIX=riscv64-linux-gnu-
# Run this no matter what you are compiling for
source edksetup.sh
```
The last step generates various configuration files and sets up some variables. Note that edksetup.sh must be sourced, as its variables must persist.
Now we need to build the base tools
```
make -C BaseTools
```
And lastly, we must actually build EDK2 using its Python build script. 
```
build -a YOUR_ARCH -t GCC5 -p YOUR_PACKAGE
```
YOUR_ARCH can be IA32, X64, AARCH64, ARM, or RISCV64. Note that not all packages compile for all architectures. YOUR_PACKAGE must point to a package in the source tree. Further guides in this article will tell you what -p option to pass to EDK2's build script.

#### EDK2 since stable release 202602
If you decide to compile from the latest Git source tree (as of 2026-02-27), the build command changes slightly, as the `GCC5` compiler option has been deprecated. Instead, simply remove the '5', so that the compiler option simply becomes `GCC`. This also applies when building OVMF and ArmVirt.

### Building with edk2-platforms
If you actually want firmware that runs on real hardware, then you must perform the following steps instead of the one above

First, we need to create our workspace folder
```
mkdir edk2 && cd edk2
export WORKSPACE=$PWD
```
Now we need to clone the source to all repos. edk2-non-osi is optional depending on the platform you are building for, but I would recommend downloading it anyway
```
git clone https://github.com/tianocore/edk2.git -b"stable/202011"
git clone https://github.com/tianocore/edk2-platforms.git
git clone https://github.com/tianocore/edk2-non-osi.git
cd edk2 && git submodule update --init
cd ../edk2-platforms && git submodule update --init && cd ..
```
We need to let EDK2's build system know where these folders are. We'll also setup a couple other variables as well
```
export EDK_TOOLS_PATH="$PWD/BaseTools"
export PACKAGES_PATH="$PWD/edk2:$PWD/edk2-platforms:$PWD/edk2-non-osi"
```
We now must setup the environment now
```
. edk2/edksetup.sh
make -C edk2/BaseTools
```
After that, building is exactly like before. To build for a certain platform in edk2-platforms, simply find the relevant .dsc file.
## Building an EDK2 UEFI application
Once you have EDK2's base built, building UEFI applications is super easy. As OS developers, the main package will be working with is MdeModulePkg, a complete UEFI wrapper library.

First, we must create a directory that contains our application. We'll create it inside of the main EDK2 directory, like this
```
cd edk2
mkdir MyEfiApp
```
Now, create a file named MyEfiApp.inf in the MyEfiApp directory. Give it the following contents
```
[Defines]
  INF_VERSION = 1.25
  BASE_NAME = MyEfiApp
  FILE_GUID = # Get a GUID from guidgen.com
  MODULE_TYPE = UEFI_APPLICATION
  VERSION_STRING = 1.0
  ENTRY_POINT = UefiEntry

[Sources]
  UefiMain.c

[Packages]
  MdePkg/MdePkg.dec

[LibraryClasses]
  UefiApplicationEntryPoint
  UefiLib

[Guids]

[Ppis]

[Protocols]

[FeaturePcd]

[Pcd]

```
Create a file called UefiMain.c in the same directory MyEfiApp.inf, and give it the following contents
```
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_STATUS EFIAPI UefiEntry(IN EFI_HANDLE imgHandle, IN EFI_SYSTEM_TABLE* sysTable)
{
    gST = sysTable;
    gBS = sysTable->BootServices;
    gImageHandle = imgHandle;
    // UEFI apps automatically exit after 5 minutes. Stop that here
    gBS->SetWatchdogTimer(0, 0, 0, NULL);
    Print(L"Hello, world!\r\n");
    // Allocate a string
    CHAR16* str = NULL;
    gBS->AllocatePool(EfiLoaderData, 36, (VOID**)&str);
    // Copy over a string
    CHAR16* str2 = L"Allocated string\r\n";
    gBS->CopyMem((VOID*)str, (VOID*)str2, 36);
    Print(str);
    gBS->FreePool(str);
    return EFI_SUCCESS;
}
```

Now run edksetup.sh if need be. After that, find a "Components" section MdeModulePkg/MdeModulePkg.dsc, and add this to it
```
[Components]
  ...
  MyEfiApp/MyEfiApp.inf
```
After that, call EDK2's build system like this to build MyEfiApp
```
build -a YOUR_ARCH -t GCC5 -p MdeModulePkg/MdeModulePkg.dsc
```
Finally, in the folder Build/MdeModule/DEBUG_GCC5/YOUR_ARCH, there should be a file named MyEfiApp.efi.

## Building OVMF and ArmVirt
Building OVMF and ArmVirtPkg is easy, after setting up EDK2, run the following for OVMF:
```
build -a YOUR_ARCH -t GCC5 -p OvmfPkg/OvmfPkgYOUR_ARCH.dsc
```
YOUR_ARCH can only be IA32 or X64 for OVMF. For ArmVirtPkg, run this instead:
```
build -a YOUR_ARCH -t GCC5 -p ArmVirtPkg/ArmVirtQemu.dsc
```
YOUR_ARCH can only be ARM or AARCH64. After this,, the firmware will be output in for OVMF Build/OvmfYOUR_ARCH/DEBUG_GCC5/FV, or for ArmVirtPkg Build/ArmVirtQemu-YOUR_ARCH/DEBUG_GCC5/FV

[Category:UEFI](Category:UEFI)
[Category:Firmware](Category:Firmware)
