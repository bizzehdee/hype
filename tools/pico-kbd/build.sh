#!/bin/sh
# Build the #778 synthetic keyboard firmware. Produces build/hype_pico_kbd.uf2.
set -e
cd "$(dirname "$0")"

: "${PICO_SDK_PATH:=/mnt/data/dev/pico-sdk}"
export PICO_SDK_PATH
[ -f "$PICO_SDK_PATH/external/pico_sdk_import.cmake" ] || {
    echo "PICO_SDK_PATH=$PICO_SDK_PATH does not look like a pico-sdk checkout" >&2
    exit 1
}

: "${PICO_BOARD:=pico}"
export PICO_BOARD

rm -rf build
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release .. >/dev/null
make -j"$(nproc)" >/dev/null
ls -l hype_pico_kbd.uf2
