#!/bin/bash
# Build and install the QEMU the validation rigs expect, from source.
#
# WHY THIS EXISTS: the distro QEMU is not good enough for these rigs. Fedora's
# 10.2.2 segfaults in its own ahci_commit_buf during a DMA read of the ESP
# (#730), which kills a whole 20-minute boot and looks from the outside exactly
# like a firmware hang -- a 73-byte boot log with qemu at 99% CPU. Several
# #232 validation runs were lost to it before the cause was found.
#
# A 11.1.0 built with these flags has run the #232 and #727 rigs to completion
# with no crashes. tools/qemu-env.sh picks it up automatically once installed.
#
# EVERY machine that runs the rigs needs this, not just the one it was first
# built on -- including the Intel/VMX box. A rig silently running the distro
# QEMU is the failure this whole exercise was about.
#
# Usage:  tools/install-qemu.sh            # build + install to the default prefix
#         PREFIX=/opt/qemu tools/install-qemu.sh
#         QEMU_VER=11.1.0 tools/install-qemu.sh
set -euo pipefail

QEMU_VER="${QEMU_VER:-11.1.0}"
PREFIX="${PREFIX:-/mnt/data/dev/qemu-build/install}"
WORK="${WORK:-$(dirname "$PREFIX")}"
JOBS="${JOBS:-$(nproc)}"

echo "qemu $QEMU_VER -> $PREFIX (work: $WORK, -j$JOBS)"

# Build dependencies. Named per family rather than installed automatically:
# this may run on a machine where the operator wants to see the list first.
if command -v apt-get >/dev/null 2>&1; then
    echo "Debian/Ubuntu build deps:"
    echo "  sudo apt-get install -y git build-essential ninja-build meson pkg-config \\"
    echo "      python3-venv libglib2.0-dev libpixman-1-dev libslirp-dev libcapstone-dev \\"
    echo "      zlib1g-dev flex bison"
elif command -v dnf >/dev/null 2>&1; then
    echo "Fedora build deps:"
    echo "  sudo dnf install -y git gcc make ninja-build meson pkgconf-pkg-config \\"
    echo "      glib2-devel pixman-devel libslirp-devel capstone-devel zlib-devel flex bison"
fi

mkdir -p "$WORK"
cd "$WORK"
TARBALL="qemu-$QEMU_VER.tar.xz"
SRC="qemu-$QEMU_VER"
if [ ! -d "$SRC" ]; then
    [ -f "$TARBALL" ] || curl -fLO "https://download.qemu.org/$TARBALL"
    tar xf "$TARBALL"
fi

# The same configuration the working build used. x86_64 only -- these rigs run
# nothing else, and a full target list costs build time for no benefit.
mkdir -p "$SRC/build"
cd "$SRC/build"
if [ ! -f config.status ]; then
    ../configure \
        --target-list=x86_64-softmmu \
        --prefix="$PREFIX" \
        --enable-kvm \
        --enable-slirp \
        --enable-capstone \
        --enable-trace-backends=log \
        --disable-docs
fi
make -j"$JOBS"
make install

"$PREFIX/bin/qemu-system-x86_64" --version | head -1
echo
echo "installed. tools/qemu-env.sh finds it at the default prefix automatically;"
echo "for a different prefix, export HYPE_QEMU_PREFIX=$PREFIX in the rig's shell."
