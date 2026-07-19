---
title: 'EFI System Partition'
source_page_id: '4394'
source_revision_id: '27953'
source_timestamp: '2023-07-09T16:07:02Z'
source_format: mediawiki
---

# EFI System Partition

The EFI System Partition (ESP) is a dedicated partition on [GPT](GPT). Usually a small one (32-256MiB), located at the beginning of the disk and its partition record at the beginning of the GPT.

## Identify
Unlike normal bootable partitions, which have the attribute flag bit 2 set, ESP is identified by its GUID, which is

 C12A7328-F81F-11D2-BA4B-00A0C93EC93B

## Format
In theory ESP can be formatted to any [FAT](FAT) file system, [FAT12](FAT12)/[16](FAT16)/[32](FAT32). In reality most firmware only support FAT32, and some platforms with limited resources may support [FAT16](FAT16) too. [FAT12](FAT12) is very rarely implemented, and it's safe to assume it won't work on a real hardware.

There's one notable exception, Apple machines (like iMac and MacBooks) use a special firmware which only accepts [HFS+](HFS+) file system on the EFI System Partition.

## Access
In the EFI Shell, the ESP is the very first file system, denoting as *FS0:*. [GRUB](GRUB) and [ELILO](http://elilo.sourceforge.net) recognize ESP, and they install their files there by default. Other boot loaders, like [rEFInd](http://www.rodsbooks.com/refind/) can only work if ESP exists.
From your application, you can use the [EFI_SIMPLE_FILESYSTEM_PROTOCOL](Loading files under UEFI) to access files on the ESP programmatically.

## Important Files on ESP
- FS0:\STARTUP.NSH - an EFI Shell script, similar to MS-DOS autoexec.bat
- FS0:\BOOTMGR.EFI - the EFI boot manager
- FS0:\BOOT\BOOTX86.EFI - the default x86_32 boot loader
- FS0:\BOOT\BOOTX64.EFI - the default x86_64 boot loader

To avoid filename clashes, you should not create files in the root directory of ESP. The specification mandates that root directory is for vendor directories, like FS0:\APPLE, FS0:\MICROSOFT etc. Some applications (like GRUB) just creates a one level directory without vendor parent directory. Feel free to create a directory for your own OS, and put your files there.

## See Also
### Articles
- [UEFI](UEFI)
- [Disk](Bootable)(Bootable Disk) - tutorial on how to create GPT

### External Links
- [Intel EFI specifications et al.](http://www.intel.com/technology/efi/)
- [EFI System Partition](Wikipedia:)(wikipedia:EFI_System_Partition)

[Category:Firmware](Category:Firmware)
[Category:UEFI](Category:UEFI)
[Category:Disks](Category:Disks)
