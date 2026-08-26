# Shared QEMU selection for the validation rigs. Source this, then use "$QEMU".
#
# Why this exists: every rig used to invoke a bare `qemu-system-x86_64`, which
# resolves to whatever the distro ships -- on this host /usr/bin, Fedora's
# 10.2.2. A newer QEMU had been built and installed under /mnt/data/dev/qemu-build
# and NOTHING pointed at it, so months of rig runs quietly used the older one.
#
# That mattered: 10.2.2 segfaults in its own ahci_commit_buf during a DMA read
# of the ESP (#730), killing whole 20-minute boots and looking from the outside
# like a firmware hang -- a 73-byte boot log with qemu at 99% CPU.
#
# Resolution order:
#   1. $QEMU, if the caller set it -- an explicit choice always wins.
#   2. the locally built QEMU, if present.
#   3. PATH, so a checkout on a machine without the local build still runs.
#
# Print which one was chosen, always. A rig that silently picks a different
# hypervisor binary than the operator assumes is how #730 stayed invisible.
: "${HYPE_QEMU_PREFIX:=/mnt/data/dev/qemu-build/install}"
if [ -z "${QEMU:-}" ]; then
    if [ -x "$HYPE_QEMU_PREFIX/bin/qemu-system-x86_64" ]; then
        QEMU="$HYPE_QEMU_PREFIX/bin/qemu-system-x86_64"
    else
        QEMU=qemu-system-x86_64
    fi
fi
export QEMU
echo "qemu: $QEMU ($("$QEMU" --version 2>/dev/null | head -1))" >&2
