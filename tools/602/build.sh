#!/bin/sh
# #602: builds every fuzz_*.c harness in this directory as a libFuzzer binary linked
# against the REAL device-model tree (the same set of .c files core/tests/run.sh links
# into every unit test binary), with ASan+UBSan on and hard failure (not just a report)
# on any sanitizer finding -- "a silent wild write is the failure mode that matters".
#
# Usage: ./build.sh [fuzz_name ...]   (default: build every fuzz_*.c in this directory)
set -e
cd "$(dirname "$0")"
ROOT="$(cd ../.. && pwd)"

LIB_DIRS="$ROOT/core/ $ROOT/arch/x86_64/cpu/ $ROOT/arch/x86_64/svm/ $ROOT/arch/x86_64/vmx/ $ROOT/devices/"

is_exempt_from_link() {
    # Same exclusion core/tests/run.sh makes: ap_boot.c references symbols that live
    # only in 16-bit real-mode assembly never built for the host target, and nothing
    # here calls it.
    case "$1" in */ap_boot.c) return 0 ;; *) return 1 ;; esac
}

lib_srcs=""
for d in $LIB_DIRS; do
    for f in "$d"*.c; do
        [ -e "$f" ] || continue
        is_exempt_from_link "$f" && continue
        lib_srcs="$lib_srcs $f"
    done
done

# vmx_run.S / svm_run.S: real VMX/SVM entry trampolines, never called on the host
# (their only caller is exempt hardware-shim code), assembled purely so the whole tree
# links -- identical reasoning to core/tests/run.sh's own asm_srcs.
asm_srcs="$ROOT/arch/x86_64/vmx/vmx_run.S $ROOT/arch/x86_64/svm/svm_run.S"

CFLAGS="-std=c11 -Wall -Wextra -g -O1 \
    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all"

if [ "$#" -gt 0 ]; then
    targets="$*"
else
    targets=""
    for f in fuzz_*.c; do
        targets="$targets ${f%.c}"
    done
fi

for t in $targets; do
    echo "building $t ..."
    clang $CFLAGS -o "$t" "$t.c" $lib_srcs $asm_srcs
done
echo "build.sh: done: $targets"
