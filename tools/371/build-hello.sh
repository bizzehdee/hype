#!/bin/bash
# #371: build the minimal probe application, with the SAME toolchain and flags hype uses.
#
# Same clang target, same subsystem, same entry symbol -- so the PE that OVMF is asked to load
# differs from hype's only in its contents. A difference in the loader's treatment of the two would
# otherwise be a confound rather than a result.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/371
clang --target=x86_64-unknown-uefi -ffreestanding -fshort-wchar -mno-red-zone \
      -Wall -Wextra -O1 -std=c11 -c tools/371/hello.c -o build/371/hello.o
ld.lld -flavor link -subsystem:efi_application -entry:efi_main \
       -out:build/371/hello.efi build/371/hello.o
ls -l build/371/hello.efi
