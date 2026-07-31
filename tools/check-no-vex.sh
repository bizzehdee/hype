#!/bin/sh
# #260 guard. hype saves the guest's vector state with FXSAVE64, which covers
# x87 + MXCSR + XMM0-15 and nothing wider. That is sufficient ONLY because legacy
# SSE writes to XMMn leave YMMn[255:128] untouched, so a guest's AVX upper halves
# survive hype's own XMM use without being saved.
#
# The moment hype itself emits a VEX/EVEX instruction, that reasoning breaks: the
# upper halves would be clobbered (or zeroed) and FXSAVE would not restore them,
# silently corrupting guest AVX state again -- the exact bug #260 fixed, back but
# harder to see. Today nothing does: the build passes no -march/-mavx, so clang's
# baseline is SSE2. This check makes that a build-time guarantee rather than a
# comment someone can invalidate by adding a compiler flag.
#
# If this ever fails, the fix is NOT to relax the check -- it is to switch the
# save path to XSAVE (and deal with XCR0 being shared with the guest, see #252).
set -e
BIN=${1:-build/hype.efi}
OBJDUMP=${OBJDUMP:-llvm-objdump}

if [ ! -f "$BIN" ]; then
    echo "check-no-vex: $BIN not found -- run 'make all' first" >&2
    exit 1
fi

dis=$("$OBJDUMP" -d "$BIN" 2>/dev/null) || {
    echo "check-no-vex: could not disassemble $BIN" >&2
    exit 1
}

# Wide vector registers can only come from VEX/EVEX encodings.
wide=$(printf '%s\n' "$dis" | grep -cE '%(ymm|zmm)[0-9]' || true)
# VEX-encoded forms of vector ops that hype has no business emitting. Matched on
# the mnemonic column so operand text like "vmread" or a symbol name cannot trip it.
vex=$(printf '%s\n' "$dis" | awk '{print $NF}' | grep -cE '^(vzeroupper|vzeroall)$' || true)
vex2=$(printf '%s\n' "$dis" | grep -cE '[[:space:]]v(mov[au]p[sd]|xorp[sd]|addp[sd]|mulp[sd]|broadcast|insert|extract|perm)[a-z0-9]*[[:space:]]' || true)

fail=0
if [ "$wide" -ne 0 ]; then
    echo "check-no-vex: FAIL -- $wide instruction(s) reference %ymm/%zmm" >&2
    printf '%s\n' "$dis" | grep -E '%(ymm|zmm)[0-9]' | head -5 >&2
    fail=1
fi
if [ "$vex" -ne 0 ] || [ "$vex2" -ne 0 ]; then
    echo "check-no-vex: FAIL -- VEX-encoded vector instruction(s) present" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "check-no-vex: FXSAVE64 no longer covers everything hype clobbers." >&2
    echo "check-no-vex: see tools/check-no-vex.sh and arch/x86_64/cpu/fpu_state.h (#260)." >&2
    exit 1
fi

echo "check-no-vex: OK -- no VEX/EVEX vector instructions; FXSAVE64 covers all vector state hype touches"
