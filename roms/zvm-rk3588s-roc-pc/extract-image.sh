#!/usr/bin/env bash
#
# Extract a ZVM RK3588S ROC PC compressed raw image with sparse output.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 INPUT.raw.zst OUTPUT.raw" >&2
    exit 1
fi

input=$1
output=$2

command -v zstd >/dev/null 2>&1 || {
    echo "error: missing required command: zstd" >&2
    exit 1
}

zstd --sparse -f -d "$input" -o "$output"
