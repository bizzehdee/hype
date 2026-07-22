#!/bin/sh
# Build and run this directory's host-native unit tests with coverage,
# per AGENTS.md's 90% line/branch coverage floor on testable code.
#
# Each test_<x>.c is linked against every module in LIB_DIRS (not just
# its own matching <x>.c) since modules call into each other across
# directories (e.g. arch/x86_64/cpu/gdt.c, console.c calls format.c) --
# library .c files never define main, so this is safe, and it means
# adding a new module + core/tests/test_<y>.c needs no changes here.
set -e
cd "$(dirname "$0")"

LIB_DIRS="../ ../../arch/x86_64/cpu/ ../../arch/x86_64/svm/ ../../arch/x86_64/vmx/ ../../devices/"

# Files that only make sense with a real CPU privilege transition (hlt,
# lgdt+segment reload, ...) are exempt from the coverage gate per
# AGENTS.md. Still linked below; just not reported on.
is_exempt() {
    case "$1" in
        */halt.c|*/gdt_load.c|*/idt_load.c|*/paging_load.c|*/serial_hw.c|*/cpu/pic.c|*/pit_hw.c|*/timer_isr.c|*/cpu_features_hw.c|*/svm_enable_hw.c|*/vmx_enable_hw.c|*/vmcs_hw.c|*/svm_vcpu.c|*/ps2_host_hw.c|*/host_pci_hw.c|*/ahci_host_hw.c|*/ap_boot.c) return 0 ;;
        *) return 1 ;;
    esac
}

lib_srcs=""
report_srcs=""
for d in $LIB_DIRS; do
    for f in "$d"*.c; do
        [ -e "$f" ] || continue
        # ap_boot.c is pure glue that references symbols defined only in
        # ap_trampoline.S (16-bit asm, not built for the host test target) and
        # is referenced by no test -- linking it into every test binary just
        # produces unresolved-symbol errors, so skip it from the link entirely.
        case "$f" in */ap_boot.c) continue ;; esac
        lib_srcs="$lib_srcs $f"
        if ! is_exempt "$f"; then
            report_srcs="$report_srcs $f"
        fi
    done
done

rm -f test_*.profraw coverage.profdata
binaries=""

for src in test_*.c; do
    name="${src%.c}"
    clang -std=c11 -Wall -Wextra -g \
        -fprofile-instr-generate -fcoverage-mapping \
        -o "$name" "$src" $lib_srcs
    LLVM_PROFILE_FILE="$name.profraw" "./$name"
    binaries="$binaries $name"
done

llvm-profdata merge -sparse test_*.profraw -o coverage.profdata

set -- $binaries
first="$1"
shift
objargs=""
for b in "$@"; do
    objargs="$objargs -object $b"
done

llvm-cov report "$first" $objargs -instr-profile=coverage.profdata $report_srcs
