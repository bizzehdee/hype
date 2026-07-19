---
title: 'UEFI'
source_page_id: '2924'
source_revision_id: '30311'
source_timestamp: '2026-03-17T18:56:04Z'
source_format: mediawiki
---

# UEFI

(U)EFI or (Unified) Extensible Firmware Interface is a specification for x86, x86-64, ARM, and Itanium platforms that defines a software interface between the operating system and the platform firmware/BIOS. The original EFI was developed in the mid-1990s by Intel for use developing firmware/BIOS for Itanium platforms. In 2005 Intel transitioned the specification to a new working group called the Unified EFI Forum, consisting of companies such as AMD, Microsoft, Apple, and Intel itself. All modern PCs ship with UEFI firmware and UEFI is widely supported by both commercial and open source operating systems. Backwards compatibility is provided for legacy operating systems.

## UEFI basics
### Downloading UEFI images
If you choose [VirtualBox](VirtualBox) for virtualization, then UEFI is already included, no need to download the image manually. You just have to enable it in the VM's preferences by clicking "Settings" / "Systems" / "Enable EFI (special OSes only)" checkbox.

Otherwise for emulation and virtual machines, you'll need an OVMF.fd firmware image. It might be tricky to find, so here are some alternative download links too:
- [TianoCore Download](https://github.com/tianocore/tianocore.github.io/wiki/OVMF) (official link)
- [OVMF-blobs](https://github.com/BlankOn/ovmf-blobs) (unofficial precompiled 32 bit and 64 bit versions, easy to use)
- [RPM packages](https://www.kraxel.org/repos/) (official TianoCore packages)
- [Debian packages](https://packages.debian.org/sid/ovmf)
- [Gentoo packages](https://packages.gentoo.org/useflags/ovmf)
- [Arch packages](https://archlinux.org/packages/extra/any/edk2/)

Under Linux, you can also install these with your distro's package manager, for example:

**Debian / Ubuntu**
```
# apt-get install ovmf
```

**RedHat / CentOS**
```
# yum install ovmf
```

**MacOS**

Use the OVMF-blobs repo.

**Windows**

Use the OVMF-blobs repo or download the RPM version, then use *7-Zip File Manager* to extract the OVMF.fd file from the downloaded archive.

### UEFI vs. legacy BIOS
A common misconception is that UEFI and BIOS are mutually exclusive. In reality, both legacy motherboards and UEFI-based motherboards both include BIOS ROMs. The differences are in where they find the bootloader/OS, how they prepare the system before executing it, and what convenience functions they provide.

#### Platform initialization
On a legacy system, the [BIOS](BIOS) performs all the usual platform initialization (memory controller configuration, [PCI](PCI) bus configuration and BAR mapping, graphics card initialization, etc.), but then drops into a backwards-compatible [mode](real)(Real Mode) environment. The bootloader must enable the [gate](A20)(A20 Line), configure a [GDT](GDT) and an [IDT](IDT), switch to [mode](protected)(protected mode), and for x86-64 CPUs, configure paging and switch to [mode](long)(x86-64).

UEFI firmware performs those same steps, but it also enables the A20 gate and prepares either protected mode environment with flat [segmentation](segmentation) (for 32-bit x86 processors) or a long mode environment with identity-mapped paging (for x86-64 processors).

Additionally, the platform initialization procedure of UEFI firmware is standardized. This allows UEFI firmware to be extended in a vendor-neutral way.

#### Boot mechanism
A legacy BIOS loads a 512 byte flat binary blob from the [MBR](MBR) of the boot device into memory at physical address 0x7C00 and jumps to it. The bootloader can return back to the BIOS through INT 0x18. On the other hand, UEFI firmware loads an arbitrary sized UEFI application (a relocatable [PE](PE) executable file) from a [FAT](FAT) partition on a [GPT](GPT) or MBR partitioned boot device to some address selected at run-time. Then it calls that application's main entry point. The application can return control to the firmware, which will continue searching for another boot device or bring up a diagnostic menu.

#### System discovery
A legacy bootloader scans memory for structures like the [EBDA](EBDA), [SMBIOS](SMBIOS), and [ACPI](ACPI) tables. It uses PIO to talk to the root [PCI](PCI) controller and scan the PCI bus. It is possible that redundant tables may be present in memory (for example, the [MP](MP_Specification) table in the SMBIOS contains information that's also present in the ACPI [DSDT](DSDT) and [MADT](MADT)) and the bootloader can choose which to use.

When UEFI firmware calls a UEFI application's entry point function, it passes a "System Table" structure, which contains pointers to all of the system's ACPI tables, memory map, and other information relevant to an OS. Legacy tables (like MP tables) may not be present in memory.

#### Convenience functions
A legacy BIOS hooks a variety of interrupts which a bootloader can trigger to access system resources like disks and the screen. These interrupts are not standardized, except by historical convention. Each interrupt uses a different register passing convention.

UEFI firmware establishes many callable functions in memory, which are grouped into sets called "protocols" and are discoverable through the System Table. The behavior of each function in each protocol is defined by specification. UEFI applications can define their own protocols and persist them in memory for other UEFI applications to use. Functions are called with a standardized, modern calling convention supported by many C compilers.

#### Development environment
Legacy bootloaders can be developed in any environment that can generate flat binary images: NASM, GCC, etc. UEFI applications can be developed in any language that can be compiled and linked into a [PE](PE) executable file and supports the calling convention used to access functions established in memory by the UEFI firmware. In practice this means one of three development environments: EDK2, GNU-EFI or POSIX-UEFI.

[EDK2](EDK2) is a large and complex, yet feature filled environment with its own build system. It can be configured to use GCC, LLVM, MinGW, Microsoft Visual C++, etc. as a cross-compiler. Not only can it be used to compile UEFI applications, but it can also be used to compile UEFI firmware to be flashed to a BIOS ROM.

[GNU-EFI](GNU-EFI) is a set of libraries and headers for compiling UEFI applications with a system's native GCC (does not work with LLVM CLang). It can't be used to compile UEFI firmware. Since it's just a couple of libraries against which a UEFI application can be linked, it is much easier to use than TianoCore ( EDK2 ).

[POSIX-UEFI](POSIX-UEFI) is very similar to GNU-EFI, but it is distributed mainly as a source, not as a binary library, has ANSI C like names and works with GCC as well as LLVM CLang. It's shipped with a Makefile that sets up the compiler flags for you.

#### Emulation
[OVMF](OVMF), a popular open source UEFI firmware, has been ported to the QEMU (but not Bochs) emulated machine. Because it implements the UEFI specification, it behaves very similarly to commercial UEFI firmware on real machines. (OVMF itself is built with TianoCore, and its source ship with it as well, but pre-built images are available.)

### Legacy bootloader or UEFI application?
If you are targeting legacy systems for which UEFI is not available or is not reliable, you should develop a legacy bootloader. This requires intimate knowledge of 16-bit addressing and the backwards-compatibility features of an x86 or x86-64 CPU.  If you are targeting modern systems you should develop a UEFI application. Many UEFI firmwares can be configured to emulate a legacy BIOS, but there is even more variation among these emulated environments than among real legacy BIOS.

Although there is a slight learning curve to become familiar with the UEFI development environments, using the System Table, and accessing UEFI-provided protocols (functions), there are far fewer "gotchas" than trying to remain compatible with the wide variety of quickly-becoming-obsolete legacy BIOSs on real machines. UEFI is the standard for all modern PCs.

### UEFI class 0-3 and CSM
PCs are categorized as UEFI class 0, 1, 2, or 3. A class 0 machine is a legacy system with a legacy BIOS; i.e. not a UEFI system at all.

A class 1 machine is a UEFI system that runs exclusively in Compatibility Support Module (CSM) mode. CSM is a specification for how UEFI firmware can emulate a legacy BIOS. UEFI firmware in CSM mode loads legacy bootloaders. A class 1 UEFI system may not advertise UEFI support at all, since it isn't exposed to the bootloader. It's only UEFI "within" the BIOS.

A class 2 machine is a UEFI system that can launch UEFI applications but also includes the option to run in CSM mode. The majority of modern PCs are UEFI class 2 machines. Sometimes the choice to run UEFI applications vs. CSM is a one-or-the-other setting in the BIOS configuration, and other times the BIOS will decide which to use after selecting the boot device and checking whether it has a legacy bootloader or a UEFI application.

A class 3 machine is a UEFI system that does not support CSM. UEFI class 3 machines only run UEFI applications and do not implement CSM for backwards compatibility with legacy bootloaders.

### Secure Boot
Secure Boot is a digital signature scheme for UEFI applications that consists of four components:
- **PK**: Platform Key
- **KEK**: Key Exchange Keys
- **db**: Whitelist database
- **dbx**: Blacklist database

UEFI firmware that supports Secure Boot is always in one of three states:
- Setup mode, Secure Boot *off*
- User mode, Secure Boot *off*
- User mode, Secure Boot *on*

In setup mode, any UEFI application can change or delete the PK, add/remove keys from the KEK, and add/remove whitelist or blacklist entries from the db or dbx.

In user mode, regardless of whether Secure Boot is on or off:
- The PK may only be changed or deleted by a UEFI application that already has the current PK.
- Keys can only be added/removed from the KEK by a UEFI application that has the PK.
- Whitelist and blacklist entries can only be added/removed from the db and dbx by a UEFI application that has any one of the keys in the KEK.

Finally, in user mode with Secure Boot *on*, UEFI applications must meet one of the following four requirements to be launched:
- Signed, with signature in db and not in dbx
- Signed by a key in db and not in dbx
- Signed by a key in the KEK
- Unsigned, but a hash of the application is in db and not in dbx

Note that UEFI applications are not signed by the PK, unless the PK also happens to be in the KEK.

Not all UEFI firmwares support Secure Boot, although it is a requirement for Windows 8. Some UEFI firmwares support Secure Boot and there's no option to be disabled, which poses a problem for independent developers that do not have access to the PK or any of the keys in the KEK, and therefore can't install their own key or application signature or hash to the whitelist database. Independent developers should develop on systems that either do not support Secure Boot or has an option for Secure Boot to be turned off.

An easy way out to use a loader that is signed by Microsoft, and allows you to load another binary signed by a key and certificate owned by you (called MOK, Machine Owner's Key). Such a loader is [shim](https://github.com/rhboot/shim), used by RedHat, Fedora, Suse, Ubuntu, Arch and many other distros to load GRUB. The filename of the EFI executable is hardwired in shim, but if you rename your loader to GRUBX64.EFI (or GRUBIA32.EFI), you sign it with your MOK key and certificate using [sbsigntool](https://github.com/imedias/sbsigntool), then you can load any loader in Secure Boot you want.

### How to use UEFI
Traditional operating systems like Windows and Linux have an existing software architecture and a large code base to perform system configuration and device discovery. With their sophisticated layers of abstraction they don't directly benefit from UEFI. As a result, their UEFI bootloaders do little but prepare the environment for them to run.

An independent developer may find more value in using UEFI to write feature-full UEFI applications, rather than viewing UEFI as a temporary start-up environment to be jettisoned during the boot process. Unlike legacy bootloaders, which typically interact with BIOS only enough to bring up the OS, a UEFI application can implement sophisticated behavior with the help of UEFI. In other words, an independent developer shouldn't be in a rush to leave "UEFI-land".

A good starting point is writing a UEFI application that uses the System Table to fetch a memory map, and uses the "File" protocol to read files from FAT-formatted disks. The next step might be to use the System Table to locate ACPI tables.

## Developing with POSIX-UEFI
:''Main article: [POSIX-UEFI](POSIX-UEFI)

One option to compile UEFI applications on POSIX like systems is POSIX-UEFI. It provides a [libc](libc)-like API for your EFI application, and ships with a Makefile that can detect and set up the toolchain for you. It can use GCC or LLVM, and defaults to using the host compiler, but a cross compiler is still recommended.

It uses POSIX style typedefs (like *uintn_t* instead of *UINTN*), and it does not ship with the standard EFI headers. You can still get interfaces not covered by POSIX-UEFI (such as GOP) by installing the EFI headers from GNU-EFI or EDK2. Also, it compiles with the MS ABI, meaning that UEFI services can be called natively (i.e., without uefi_call_wrapper) so long as your apps are compiled with it as well.
The traditional "Hello, world" UEFI program goes like this.
```
#include <uefi.h>
 
int main (int argc, char **argv)
{
  printf("Hello, world!\n");
  return 0;
}
```
Makefile looks like this:
```
TARGET = main.efi
include uefi/Makefile
```
Run make to build it. The result of this process is a PE executable file *main.efi*.

## Developing with GNU-EFI
:''Main article: [GNU-EFI](GNU-EFI)

GNU-EFI can be used to develop both 32-bit and 64-bit UEFI applications. This section will address 64-bit UEFI applications only, and assumes that the development environment itself is running on an x86_64 system, so that no cross-compiler is needed. For a more thorough walk-through of a proper (non-gnu-efi) development environment, see [App Bare Bones](UEFI)(UEFI App Bare Bones).
        
GNU-EFI includes four things:
- **crt0-efi-x86_64.o**: A CRT0 (C runtime initialization code) that provides an entry point that UEFI firmware will call when launching the application, which will in turn call the "efi_main" function that the developer writes.
- **libgnuefi.a**: A library containing a single function (*_relocate*) that is used by the CRT0.
- **elf_x86_64_efi.lds**: A linker script used to link ELF binaries into UEFI applications.
- **efi.h** and other headers: Convenience headers that provide structures, typedefs, and constants improve readability when accessing the System Table and other UEFI resources.
- **libefi.a**: A library containing convenience functions like CRC computation, string length calculation, and easy text printing.
- **efilib.h**: Header for *libefi.a*.

At a bare minimum, a 64-bit UEFI application will need to link against *crt0-efi-x86_64.o* and *libgnuefi.a* using the *elf_x86_64_efi.lds* linker script. Most likely you will want to use the provided headers and the convenience library as well, and this section will assume that going forward.

The traditional "Hello, world" UEFI program is shown below.
```
#include <efi.h>
#include <efilib.h>

EFI_STATUS
EFIAPI
efi_main (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  InitializeLib(ImageHandle, SystemTable);
  Print(L"Hello, world!\n");
  return EFI_SUCCESS;
}
```

A few notes:
- efi.h is included so we can use types like EFI_STATUS, EFI_HANDLE, and EFI_SYSTEM_TABLE.
- When creating a 32-bit UEFI application, EFIAPI is empty; GCC will compile the "efi_main" function using the standard C calling convention. When creating a 64-bit UEFI application, EFIAPI expands to "__attribute__((ms_abi))" and GCC will compile the "efi_main" function using Microsoft's x64 calling convention, as specified by UEFI. Only functions that will be called directly from UEFI (including main, but also callbacks) need to use the UEFI calling convention.
- "InitializeLib" and "Print" are convenience functions provided by libefi.a with prototypes in efilib.h. "InitializeLib" lets libefi.a store a reference to the ImageHandle and SystemTable provided by BIOS. "Print" uses those stored references to print a string by reaching out to UEFI-provided functions in memory. (Later on we will see how to find and call UEFI-provided functions manually.)

This program is compiled and linked as below.
```
$ gcc main.c                             \
      -c                                 \
      -fno-stack-protector               \
      -fpic                              \
      -fshort-wchar                      \
      -mno-red-zone                      \
      -I /path/to/gnu-efi/headers        \
      -I /path/to/gnu-efi/headers/x86_64 \
      -DEFI_FUNCTION_WRAPPER             \
      -o main.o

$ ld main.o                         \
     /path/to/crt0-efi-x86_64.o     \
     -nostdlib                      \
     -znocombreloc                  \
     -T /path/to/elf_x86_64_efi.lds \
     -shared                        \
     -Bsymbolic                     \
     -L /path/to/libs               \
     -l:libgnuefi.a                 \
     -l:libefi.a                    \
     -o main.so

$ objcopy -j .text                       \
          -j .sdata                      \
          -j .data                       \
          -j .rodata                     \
          -j .dynamic                    \
          -j .dynsym                     \
          -j .rel                        \
          -j .rela                       \
          -j .reloc                      \
          --output-target=efi-app-x86_64 \
          main.so                        \
          main.efi
```

The result of this process is a 44 kB PE executable file *main.efi*. On a real project you'll probably want to use make or another build tool, and may need to build a cross-compiler. The way GNU-EFI works is a bit contrived: you are wrapping an ELF file built by your normal compiler into PE.

## Emulation with QEMU and OVMF
Any recent version of QEMU with a recent version of OVMF will be sufficient to run a UEFI application. QEMU binaries are available for many platforms, and a binary OVMF image (OVMF.fd) can be found on the [TianoCore](http://www.tianocore.org/ovmf/) website. QEMU (without any boot disk) can be invoked as below. (To prevent recent versions of QEMU from attempting a PXE (network) boot when no boot disk is found, use `-net none`.

The recommended way to use OVMF (for QEMU 1.6 or newer) is with a `pflash` parameter. The instructions below assume you have an OVMF image split into separate CODE and VARS sections.

```
$ qemu-system-x86_64 -cpu qemu64 \
  -drive if=pflash,format=raw,unit=0,file=path_to_OVMF_CODE.fd,readonly=on \
  -drive if=pflash,format=raw,unit=1,file=path_to_OVMF_VARS.fd \
  -net none
```

If you prefer to work on a terminal without a display, or via SSH/telnet, you will want to run QEMU without graphics support, using the `-nographic` flag.

If OVMF does not find a boot disk with a properly named UEFI application (more on this later) it will drop into a UEFI shell.

[file:OVMF_shell.png](file:OVMF_shell.png)

You can find a list of shell commands [here](https://web.archive.org/web/20170504062623/http://www.sysadminshare.com/2012/01/efi-shell-commands.html) or you can type *help* at the shell.

### Creating disk images
:''Main article: [Disk](Bootable)(Bootable Disk)

To launch a UEFI application you will need to create a disk image and present it to QEMU. UEFI firmware expects UEFI applications to be stored in a FAT12, FAT16, or FAT32 file system on a [GPT](GPT) or [MBR](MBR)-partitioned disk. Many firmwares only support FAT32, so that's what you'll want to use. Depending on your platform, there are several different ways to create a disk image containing your UEFI application, but they all start by creating a zeroed disk image file. The minimum FAT32 partition size is 33,548,800 bytes, plus you will need space for the primary and secondary GPT tables, plus some slack space so the partition can be aligned correctly. Throughout these examples we will be creating a 48,000,000 byte (93750 512-byte sectors, or 48 MB) disk image.
```
$ dd if=/dev/zero of=/path/to/uefi.img bs=512 count=93750
```

#### Virtual FAT disk images
QEMU can automatically create a virtual FAT disk image from a specified directory tree. To use this feature, simply create a directory and copy any UEFI applications you want to test into it.

```
$ mkdir esp
$ cp /path/to/main.efi esp
```

After that, you can invoke QEMU as below.

```
$ qemu-system-x86_64 -cpu qemu64 \
  -drive if=pflash,format=raw,unit=0,file=path_to_OVMF_CODE.fd,readonly=on \
  -drive if=pflash,format=raw,unit=1,file=path_to_OVMF_VARS.fd \
  -drive format=raw,file=fat:rw:esp \
  -net none
```

#### uefi-run helper application
The uefi-run application is useful for quick testing. It creates a temporary FAT image containing your EFI application and starts qemu.

```
$ uefi-run -b /path/to/OVMF.fd -q /path/to/qemu app.efi -- <extra_qemu_args>
```

uefi-run is not currently packaged for any distribution. You can install it using cargo (the Rust package manager) though ("cargo install uefi-run").

#### Linux, root required
This approach requires root privileges and uses **gdisk**, **losetup**, and **mkdosfs**. First, use gdisk to create a GPT partition table with a single [System Partition](EFI)(EFI System Partition).
```
$ gdisk /path/to/uefi.img
GPT fdisk (gdisk) version 0.8.10

Partition table scan:
  MBR: not present
  BSD: not present
  APM: not present
  GPT: not present

Creating new GPT entries.

Command (? for help): o
This option deletes all partitions and creates a new protective MBR.
Proceed? (Y/N): y

Command (? for help): n
Partition number (1-128, default 1): 1
First sector (34-93716, default = 2048) or {+-}size{KMGTP}: 2048
Last sector (2048-93716, default = 93716) or {+-}size{KMGTP}: 93716
Current type is 'Linux filesystem'
Hex code or GUID (L to show codes, Enter = 8300): ef00
Changed type of partition to 'EFI System'

Command (? for help): w

Final checks complete. About to write GPT data. THIS WILL OVERWRITE EXISTING
PARTITIONS!!

Do you want to proceed? (Y/N): y
OK; writing new GUID partition table (GPT) to uefi.img.
Warning: The kernel is still using the old partition table.
The new table will be used at the next reboot.
The operation has completed successfully.
```

Now you have disk image with [partition table](GUID)(GPT) on it and an unformatted EFI partition starting at sector 2048. Unless you deviated from the commands shown above, the disk image will use 512-byte sectors, so the EFI partition starts at byte 1,048,576 and is 46,934,528 bytes in length. Use losetup to present the partition to Linux on a loopback device.

```
losetup --offset 1048576 --sizelimit 46934528 /dev/loop0 /path/to/uefi.img
```

(If /dev/loop0 is already in use you will need to select a different loopback device.)

Format the partition for FAT32 with mkdosfs.
```
mkdosfs -F 32 /dev/loop0
```

The partition can now be mounted, so that we can copy files to it. In this example we use the "/mnt" directory, but you could also create a local directory for temporary use.
```
mount /dev/loop0 /mnt
```

Copy any UEFI applications you want to test to the file system.
```
$ cp /path/to/main.efi /mnt/
$ ...
```

Finally, unmount the partition and free the loopback device.
```
$ umount /mnt
$ losetup -d /dev/loop0
```

*uefi.img* is now a disk image containing primary and secondary GPT tables, containing a single partition of type EFI, containing a FAT32 file system, containing one or more UEFI applications.

#### Linux, root not required
This approach uses **parted**, **mformat**, and **mcopy** and can be performed with user privileges. First, use parted to create primary and secondary GPT headers, and a single EFI partition spanning the same range as the approach above.
```
$ parted /path/to/uefi.img -s -a minimal mklabel gpt
$ parted /path/to/uefi.img -s -a minimal mkpart EFI FAT16 2048s 93716s
$ parted /path/to/uefi.img -s -a minimal toggle 1 boot
```

Now create a new temporary image file that will contain the EFI partition data and use mformat to format it with FAT16.
```
dd if=/dev/zero of=/tmp/part.img bs=512 count=91669
mformat -i /tmp/part.img -h 32 -t 32 -n 64 -c 1
```

Use mcopy to copy any UEFI applications you want to test to the file system.
```
$ mcopy -i /tmp/part.img /path/to/main.efi ::
$ ...
```

Finally, write the partition image into the main disk image.
```
$ dd if=/tmp/part.img of=/path/to/uefi.img bs=512 count=91669 seek=2048 conv=notrunc
```

*uefi.img* is now a disk image containing primary and secondary GPT tables, containing a single partition of type EFI, containing a FAT16 file system, containing one or more UEFI applications.

#### FreeBSD, root required
This approach requires root privileges and uses **mdconfig**, **gpart**, **newfs_msdos**, and **mount_msdosfs**. First, create a device node that presents the zeroed disk image as a block device. This will let us work on it using standard partitioning and formatting tools.
```
$ mdconfig -f /path/to/uefi.img
md0
```

In this example the new block device is *md0*. Now create the empty primary and secondary GPT tables on the device.
```
$ gpart create -s GPT md0
md0 created
```

Now we can add a partition to the disk. We'll specify an "EFI" partition, which just means that GPT will set that partition's GUID to the special "EFI" type. Not all BIOSs require this, and the partition will still be able to be mounted and browsed normally on Linux, FreeBSD, and Windows.
```
$ gpart add -t efi md0
md0p1 added
```

Next, create a FAT16 file system on the new partition. You can specify various parameters for the file system if you'd like, but it isn't necessary. Ideally you would create a FAT32 partition for best firmware compatibility but FreeBSD seems to create FAT32 partitions that OVMF can't read.
```
$ newfs_msdos -F 16 md0p1
newfs_msdos: trim 2 sectors to adjust to a multiple of 9
/dev/md2p1: 93552 sectors in 11694 FAT16 clusters (4096 bytes/cluster)
BytesPerSec=512 SecPerClust=8 ResSectors=1 FATs=2 RootDirEnts=512 Media=0xf0 FATsecs=46 SecPerTrack=9 Heads=16 HiddenSecs=0 HugeSectors=93681
```

The partition can now be mounted, so that we can copy files to it. In this example we use the */mnt* directory, but you could also create a local directory for temporary use.
```
$ mount_msdosfs /dev/md0p1 /mnt
```

Copy any UEFI applications you want to test to the file system.
```
$ cp /path/to/main.efi /mnt/
$ ...
```

Finally, unmount the partition and free the block device.
```
$ umount /mnt
$ mdconfig -d -u md0
```

*uefi.img* is now a disk image containing primary and secondary GPT tables, containing a single partition of type EFI, containing a FAT16 file system, containing one or more UEFI applications.

#### Mac OS (root not required)
Mac OS has a single tool (hdiutil) that creates the disk image and copy files at same time.

Let's say that you're creating a UEFI boot for x86_64. By definition the file name should be BOOTX64.EFI and this file should be located in the /EFI/BOOT folder.

First, let's create a temporary folder that will contains all files and folders required for booting UEFI.
  
```
$ mkdir -p diskImage/EFI/BOOT
```

Secondly, let's copy the boot application to the required location:

```
$ cp bootx64.efi diskImage/EFI/BOOT/BOOTX64.EFI
```

Finally, let's create a disk image partitioned with GPT, formatted with fat32 (-fs fat32), overriding destination file if needed (-ov), define disk size (-size 48m), define volume name (-volname NEWOS), the file format which the disk will be encoded (-format UDTO - the same used for DVDs/CDs) and the source folder containing the files that will be copied to the new disk:

```
$ hdiutil create -fs fat32 -ov -size 48m -volname NEWOS -format UDTO -srcfolder diskImage uefi.cdr
```

uefi.cdr should be ready to be used by QEMU.

### Launching UEFI applications
Once your disk image is ready, you can invoke QEMU as below.
```
$ qemu-system-x86_64 -cpu qemu64 -bios /path/to/OVMF.fd -drive file=uefi.disk,if=ide
```

When OVMF drops into the UEFI shell, you will see an additional entry in the "Mapping table", labeled "FS0". This indicates that the firmware detected the disk, discovered the partition, and was able to mount the file system. You can explore the file system by switching to it using the DOS-style syntax "FS0:", as illustrated below.

[File:OVMF_browse_fs.png](File:OVMF_browse_fs.png)

You can launch a UEFI application by entering its name.

[File:OVMF_run_app.png](File:OVMF_run_app.png)

Notice that the UEFI shell resumed once the application terminated. Of course if this was a proper bootloader it would never resume, but rather launch an OS.

Some commercial UEFI firmware provides UEFI shells or the capability to launch user-selected UEFI applications, such as the firmware that ships with the HP EliteBook line of laptops. Most, however, do not expose this functionality to the end-user.

### Debugging
OVMF can be built in debug mode, and it will output logging messages to IO port 0x402.
You can use some flags like the ones below to capture the output.

`-debugcon file:uefi_debug.log -global isa-debugcon.iobase=0x402`

Note that release builds will not output debug messages, or will have reduced output.

See [UEFI applications with GDB](Debugging)(Debugging UEFI applications with GDB).

## Running on real hardware
### NVRAM variables
- Main article:* [NVRAM](UEFI)(UEFI NVRAM)
A UEFI firmware will present most of its configuration options through a text or graphical configuration menu, just like a legacy BIOS. Selections made in these menus are stored in the NVRAM chip between reboots. Unlike legacy BIOS, however, the firmware developer has the option to expose some or all of these "NVRAM variables" to the OS and end-user via convenience functions made resident in RAM by the firmware at boot.

The Linux **efivarfs** kernel module will use these functions to list NVRAM variables in the */sys/firmware/efi/efivars* file. NVRAM variables can also be dumped from within the UEFI shell itself with the **dmpstore** command. Device boot order is always accessible via NVRAM variables. The Linux program **efibootmgr** works specifically with the boot order NVRAM variables. The UEFI shell offers the **bcfg** command for the same purpose.

### Bootable UEFI applications
The boot order NVRAM variables determine where firmware will look for UEFI applications to be launched at boot. Although this can be changed (for example, an OS installer might customize the boot entry for the hard drive to which it was installed) firmware typically looks for a UEFI application named "BOOTIA32.efi" (for 32-bit applications) or "BOOTX64.efi" (for 64-bit applications) stored in the "/EFI/BOOT" path in the boot device's file system. This is the default path and name for OVMF.

Unlike a UEFI application launched from the shell, if a bootable UEFI application returns BIOS will continue searching for other boot devices.

### Exposed functionality
Real PCs vary in the amount of UEFI capability they expose to the user. For example, even a class 3 machine may not make any mention of UEFI in its BIOS configuration and may not offer a UEFI shell. Additionally, some BIOS vendors make their UEFI firmware configuration screens look identical to their legacy BIOS configuration screens. Class 2 machines may present somewhat confusing boot menus and configuration options. For example, one laptop manufacturer includes a configuration option to enable/disable UEFI (i.e. switch between UEFI and CSM behavior), named "OS: Windows 8". Another laptop, if it fails to find a bootable UEFI application on the selected boot device (or if that application returns a status other than EFI_SUCCESS) will fall back to CSM behavior and then complain that the drive has a corrupted MBR. With time, and with the emergence of class 3 machines, clarity of UEFI boot behavior will improve.

To make testing on real hardware easier, you can install a bootable UEFI application to the internal hard drive of the system that provides a boot menu, such as [rEFInd](http://www.rodsbooks.com/refind/). This may also be convenient for multi-boot scenarios.

### PC firmware developers
On x86 and x86-64 platforms, the following BIOS developers offer UEFI firmware:
- AMI (Aptio).
- Phoenix (SecureCore, TrustedCore, AwardCore).
- Insyde (InsydeH20).

## UEFI applications in detail
### Binary Format
UEFI executables are regular PE32 / PE32+ (Windows x32 / x64) images, with a specific **subsystem**.
Every UEFI application is basically a windows EXE (or DLL) without symbol tables.

{| class="wikitable"
|+ Types of UEFI images
|-
! Type
! Description
! Subsystem
|-
| Applications || OS loaders and other utility programs. || 10
|-
| Boot service driver || Drivers used by the firmware when booting (e.g. disk drivers, network drivers). || 11
|-
| Runtime driver || Drivers which may stay loaded even after the OS loads and exits the boot services. || 12
|}

UEFI images must also specify the type of machine code they contain. A UEFI loader will refuse to boot an incompatible image.

{| class="wikitable"
|+ Types of machines
|-
! Name / arch
! Value
|-
|x86||0x014c
|-
|x86_64||0x8664
|-
|Itanium x64||0x0200
|-
|UEFI Byte Code||0x0EBC
|-
|ARM<sup>1</sup>||0x01C2
|-
|AArch (ARM x64)||0xAA64
|-
|RISC-V x32||0x5032
|-
|RISC-V x64||0x5064
|-
|RISC-V x128||0x5128
|}

[1] ARM means you can use Thumb / Thumb 2 instructions, but UEFI interfaces are in ARM mode.

#### Initialization
Applications must either load an OS and exit boot services, or return from the main function (in which case the boot loader will look for the next app to load).

Drivers must initialize and then return 0 on success, or an error code. A computer might fail to boot if a required driver fails to load.

#### Memory
The memory map returned by UEFI will mark the memory areas which drivers use.

Once your OS loader finished, your kernel is allowed to reuse the memory where the boot loader was loaded.

The memory types are `Efi{Loader/BootServices/RuntimeServices}{Code/Data}`.

After exiting the boot services, you may reuse whatever non-read-only memory the boot drivers used.

However, memory used by the runtime drivers must **never** be touched - the runtime drivers stay active and loaded for as long as the computer runs.

One way to see a breakdown of a PE file containing a UEFI application is by```$ objdump --all-headers /path/to/main.efi```
Its output is quite long. Among other things, it shows the **subsystem**, that is the type of the UEFI image, mentioned earlier.

### Calling Conventions
UEFI specifies the following calling conventions:
- **cdecl** for x86 UEFI functions
- **Microsoft's 64-bit calling convention** for x86-64 UEFI functions
- **SMC** for ARM UEFI functions

This has two impacts on UEFI application developers:
- A UEFI application's main entry point must expect to be called with the corresponding calling convention.
- Any UEFI-provided functions that a UEFI application calls must be called with the corresponding calling convention.
Note that functions strictly internal to the application can use whatever calling convention the developer chooses.

#### POSIX-UEFI, GNU-EFI and GCC
- Main article:* [GNU-EFI](GNU-EFI)
cdecl is the standard calling convention used by GCC, so no special attributes or modifiers are needed for writing the main entry point or calling UEFI functions in an x86 UEFI application developed with GNU-EFI. For x86-64, however, the entry point function must be declared with the "___attribute___((ms_abi))" modifier and all calls to UEFI-provided functions must be made through the "uefi_call_wrapper" thunk. This thunk is called with cdecl, but then translates to the Microsoft x86-64 calling convention before calling the requested UEFI function. This is necessary because older releases of GCC do not support specifying calling conventions for function pointers.

For [POSIX-UEFI](POSIX-UEFI), which also uses GCC, your entry point looks like the standard main(), and no special ABI is required. Also the build environment takes care of the compiler flags for you, so you can simply call UEFI functions without "uefi_call_wrapper", no matter if you're using the host gcc or a cross-compiler.

For developer convenience, both POSIX-UEFI and GNU-EFI provides the "EFIAPI" macro, which expands to "cdecl" when targeting x86 and "__attribute__(ms_abi))" when targeting x86-64. Additionally, the "uefi_call_wrapper" thunk will simply pass the call through on x86. This allows the same source code to target x86 and x86-64. For example, the following main function will compile with the correct calling convention on both x86 and x86-64 and the call through the "uefi_call_wrapper" thunk will select the correct calling convention to use when calling the UEFI function (in this case, printing a string).
```
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS status = uefi_call_wrapper(SystemTable->ConOut->OutputString,
                                        2,
                                        SystemTable->ConOut,
                                        L"Hello, World!\n");
  return status;
}
```

### Language binding
UEFI applications are typically written in C, although bindings could be written for any other language that compiles to machine code. Assembly is also an option; a [uefi.inc](uefi.inc) is available for [FASM](FASM) that allows UEFI applications to be written as below.
```
format pe64 dll efi
entry main

section '.text' code executable readable

include 'uefi.inc'

main:
    ; initialize UEFI library
    InitializeLib
    jc @f

    ; call uefi function to print to screen
    uefi_call_wrapper ConOut, OutputString, ConOut, _hello

@@: mov eax, EFI_SUCCESS
    retn

section '.data' data readable writeable

_hello                                  du 'Hello World',13,10,0

section '.reloc' fixups data discardable
```

As a UEFI application contains normal x86 or x86-64 machine code, inline assembly is also an option in compilers that support it.

### EFI Byte Code
UEFI also includes a virtual machine specification based on a byte code format called EFI Byte Code (EBC), which can be used to write platform-independent device drivers, but not UEFI applications. As of 2015 there has been limited use of EBC.

## Common Problems
### My UEFI application hangs/resets after about 5 minutes
When control is handed to your UEFI application by firmware, it sets a watchdog timer for 5 minutes, after which the firmware is reinvoked as it assumes your application has hung.  The firmware in this case will normally try to reset the system (although the OVMF firmware in VirtualBox simply causes the screen to go black and hang).  To counteract this, you are required to refresh the watchdog timer before it times out.  Alternatively, you can disable it completely with code like ```SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);```Obviously this is not a problem for most bootloaders, but can cause an issue if you have an interactive loader which waits for user input.  Also note that you are required to disable the watchdog timer if you exit back to the firmware.

### My bootloader hangs if I use user defined EFI_MEMORY_TYPE values
For the memory management functions in EFI, an OS is meant to be able to use "memory type" values above 0x80000000 for its own purposes. In the OVMF EFI firmware release "r11337" (for Qemu, etc) there is a bug where the firmware assumes the memory type is within the range of values defined for EFI's own use, and uses the memory type as an array index. The end result is an "array index out of bounds" bug; where the higher memory type values (e.g. perfectly legal values above 0x80000000) cause the 64-bit version of the firmware to crash (page fault), and cause incorrect "attribute" values to be reported by the 32-bit version of the firmware. This same bug is also present in whatever version of the EFI firmware VirtualBox uses (which looks like an older version of OVMF); and I suspect (but don't know) that the bug may be present in a wide variety of firmware that was derived from the tianocore project (not just OVMF).

## See also
### Articles
- [Bare Bones](UEFI)(UEFI Bare Bones)
- [System Partition](EFI)(EFI System Partition)
- [PE](PE) file format
- [EDK2](EDK2)
- [POSIX-UEFI](POSIX-UEFI)
- [GNU-EFI](GNU-EFI)
- [Uefi Barebones MSVC/Clang/Visual Studio](https://github.com/nebulaeonline/nebulae/tree/UefiBarebones)

### External Links
- [UEFI specifications et al.](https://uefi.org/specifications)
- [Intel TianoCore EDK2](http://www.tianocore.org/)
- [OVMF firmware images](https://github.com/tianocore/tianocore.github.io/wiki/OVMF) for use with [QEMU](QEMU)
- [Phoenix UEFI Wiki](https://web.archive.org/web/20181028201454/http://wiki.phoenix.com/wiki/index.php/Main_Page)
- [Several articles about UEFI](http://x86asm.net/articles/others/index.html)
- [PE specification covering the (U)EFI binary format](https://www.microsoft.com/en-us/download/details.aspx?id=19509)
- [Blog about UEFI, with bits about UEFI development](https://uefi.blogspot.com/)
- [Presentation guiding through simple UEFI application setup](https://web.archive.org/web/20160316192235/http://internshipatdell.wikispaces.com/file/view/How+to+build+an+UEFI+application.pptx)
- [Presentation giving an overview of windows uefi booting](https://uefi.org/sites/default/files/resources/UEFI-Plugfest-WindowsBootEnvironment.pdf)
- [POSIX-UEFI](https://gitlab.com/bztsrc/posix-uefi) documentation and source
- [Article on EFI](Wikipedia)(Wikipedia:Extensible_Firmware_Interface)

[Category:x86](Category:x86)
[Category:x86-64](Category:x86-64)
[Category:ARM](Category:ARM)
[Category:Firmware](Category:Firmware)
[Category:UEFI](Category:UEFI)
[de:Unified_Extensible_Firmware_Interface](de:Unified_Extensible_Firmware_Interface)
