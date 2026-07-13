#!/bin/sh
# Build and run this directory's host-native unit tests with coverage,
# per AGENTS.md's 90% line/branch coverage floor on testable code.
#
# Each test_<x>.c is linked against every sibling module in ../ (not just
# ../<x>.c) since some modules call into others (e.g. console.c calls
# format.c) -- library .c files never define main, so this is safe, and
# it means adding a new core/<y>.c + core/tests/test_<y>.c needs no
# changes here.
set -e
cd "$(dirname "$0")"

rm -f test_*.profraw coverage.profdata
binaries=""

for src in test_*.c; do
    name="${src%.c}"
    clang -std=c11 -Wall -Wextra -g \
        -fprofile-instr-generate -fcoverage-mapping \
        -o "$name" "$src" ../*.c
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

# halt.c (hlt-loop CPU halt) is exempt from the coverage gate per
# AGENTS.md -- it only makes sense with a real privilege transition and
# is noreturn, so no test can call it. Still linked above; just not
# reported on here.
report_srcs=""
for f in ../*.c; do
    case "$f" in
        */halt.c) ;;
        *) report_srcs="$report_srcs $f" ;;
    esac
done

llvm-cov report "$first" $objargs -instr-profile=coverage.profdata $report_srcs
