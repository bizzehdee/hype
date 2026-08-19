#!/bin/bash
# Check every checked-in section-6k input script against hype's own parser.
#
#   tools/check-input-scripts.sh [script ...]     (default: every script in the tree)
#
# A script hype refuses to arm is silent: the guest still boots and still logs, so the run looks
# fine while the driving never happened. See tools/input-check/check.c for the case that motivated
# this.
set -eu
cd "$(dirname "$0")/.."
BIN=$(mktemp /tmp/hype-input-check.XXXXXX)
trap 'rm -f "$BIN"' EXIT
clang -O0 -Wall -o "$BIN" tools/input-check/check.c core/input_script.c
if [ $# -gt 0 ]; then
    "$BIN" "$@"
else
    # shellcheck disable=SC2046
    "$BIN" $(find tools tests rig -name 'vm*.txt' -o -name '*.input' 2>/dev/null | sort)
fi
