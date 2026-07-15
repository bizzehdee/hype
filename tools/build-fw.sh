#!/bin/sh
# Builds the guest UEFI firmware blob (M4-1/M4-2, plan.md §7 /fw, /edk2)
# from the vendored edk2 submodule: BaseTools (forced to clang, since
# EDK2's own auto-detection only forces clang when `cc --version`
# already says "clang" -- Fedora's default cc is gcc), then
# OvmfPkg/OvmfPkgX64.dsc via the CLANGDWARF toolchain tag (the
# native-ELF, DWARF-debug-info Clang tag for Linux; CLANGPDB targets a
# Windows-style PE/PDB flow instead and isn't what we want here).
#
# Requires (none of this is hype.efi's own toolchain, deliberately
# separate per plan.md §8): nasm, iasl (acpica-tools), libuuid-devel,
# python3-devel, clang, git. Run from anywhere; paths are all relative
# to this script's own location.
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
EDK2_DIR="$REPO_ROOT/edk2"
FW_DIR="$REPO_ROOT/fw"

# Build target: RELEASE (default, the shipped guest firmware) or DEBUG.
# The DEBUG build compiles in every DEBUG()/ASSERT() and raises the
# print-error-level to all-on, so a driver's own trace (e.g.
# Ps2KeyboardDxe's ConIn/keyboard messages, FW-1g) actually reaches the
# serial port / debug-io port that FW-1 forwards. It is written to a
# separate *.debug.fd so it never clobbers the shipped RELEASE blob.
FW_TARGET="${FW_TARGET:-RELEASE}"
if [ "$FW_TARGET" != "RELEASE" ] && [ "$FW_TARGET" != "DEBUG" ]; then
    echo "build-fw.sh: FW_TARGET must be RELEASE or DEBUG (got '$FW_TARGET')." >&2
    exit 1
fi

if [ ! -f "$EDK2_DIR/edksetup.sh" ]; then
    echo "build-fw.sh: $EDK2_DIR doesn't look like an edk2 checkout (no edksetup.sh)." >&2
    echo "Run 'git submodule update --init --recursive' first." >&2
    exit 1
fi

for tool in nasm iasl clang python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "build-fw.sh: required tool '$tool' not found on PATH." >&2
        exit 1
    fi
done

cd "$EDK2_DIR"

# Any submodule not yet checked out (a fresh clone of the superproject
# won't have recursed into edk2's own submodules automatically).
git submodule update --init --recursive

echo "build-fw.sh: building BaseTools (clang)..."
make -C BaseTools CC=clang CXX=clang++

echo "build-fw.sh: building OvmfPkg/OvmfPkgX64.dsc ($FW_TARGET, CLANGDWARF)..."
export WORKSPACE="$EDK2_DIR"
# shellcheck disable=SC1091
. ./edksetup.sh >/dev/null
if [ "$FW_TARGET" = "DEBUG" ]; then
    # PcdDebugPrintErrorLevel=0xFFFFFFFF: enable every debug level
    # (INFO|VERBOSE|ERROR|...) so driver-level trace is emitted. DXE
    # DEBUG() output flows out the 16550 serial (BaseDebugLibSerialPort,
    # forwarded by FW-1e); SEC/PEI out the 0x402 debug-io port
    # (PlatformDebugLibIoPort, routed by FW-1g's hype-side handler).
    build -p OvmfPkg/OvmfPkgX64.dsc -a X64 -t CLANGDWARF -b DEBUG \
        --pcd gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel=0xFFFFFFFF
else
    build -p OvmfPkg/OvmfPkgX64.dsc -a X64 -t CLANGDWARF -b RELEASE
fi

OUT_DIR="$EDK2_DIR/Build/OvmfX64/${FW_TARGET}_CLANGDWARF/FV"
mkdir -p "$FW_DIR"
if [ "$FW_TARGET" = "DEBUG" ]; then
    cp "$OUT_DIR/OVMF_CODE.fd" "$FW_DIR/OVMF_CODE.debug.fd"
    cp "$OUT_DIR/OVMF_VARS.fd" "$FW_DIR/OVMF_VARS.debug.fd"
    echo "build-fw.sh: done -- $FW_DIR/OVMF_CODE.debug.fd and OVMF_VARS.debug.fd updated (DEBUG)."
else
    cp "$OUT_DIR/OVMF_CODE.fd" "$FW_DIR/OVMF_CODE.fd"
    cp "$OUT_DIR/OVMF_VARS.fd" "$FW_DIR/OVMF_VARS.fd"
    echo "build-fw.sh: done -- $FW_DIR/OVMF_CODE.fd and OVMF_VARS.fd updated (RELEASE)."
fi
