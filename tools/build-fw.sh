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

echo "build-fw.sh: building OvmfPkg/OvmfPkgX64.dsc (RELEASE, CLANGDWARF)..."
export WORKSPACE="$EDK2_DIR"
# shellcheck disable=SC1091
. ./edksetup.sh >/dev/null
build -p OvmfPkg/OvmfPkgX64.dsc -a X64 -t CLANGDWARF -b RELEASE

OUT_DIR="$EDK2_DIR/Build/OvmfX64/RELEASE_CLANGDWARF/FV"
mkdir -p "$FW_DIR"
cp "$OUT_DIR/OVMF_CODE.fd" "$FW_DIR/OVMF_CODE.fd"
cp "$OUT_DIR/OVMF_VARS.fd" "$FW_DIR/OVMF_VARS.fd"

echo "build-fw.sh: done -- $FW_DIR/OVMF_CODE.fd and OVMF_VARS.fd updated."
