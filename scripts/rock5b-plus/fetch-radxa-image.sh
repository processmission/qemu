#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Fetch the official Radxa Rock 5B+ Bookworm KDE r7 image (SHA-512 verify + unpack)
# Env: IMAGE_DIR (default: current directory)
set -euo pipefail

TAG=rsdk-r7
NAME=rock-5b-plus_bookworm_kde_r7.output_512.img
BASE="https://github.com/radxa-build/rock-5b-plus/releases/download/${TAG}"
OUT="${IMAGE_DIR:-$(pwd)}"

echo "==> Downloading SHA-512 checksum file"
curl -fL --retry 3 -o "$OUT/$NAME.sha512sum" "$BASE/$NAME.sha512sum"
EXPECT_XZ=$(awk -v n="./$NAME.xz" '$2 == n { print $1 }' "$OUT/$NAME.sha512sum")
if [ -z "$EXPECT_XZ" ]; then
    echo "error: no $NAME.xz entry in checksum file" >&2
    exit 1
fi

echo "==> Downloading image archive (about 1.4 GiB, resumable)"
curl -fL --retry 3 --continue-at - -o "$OUT/$NAME.xz" "$BASE/$NAME.xz"

echo "==> Verifying SHA-512"
GOT_XZ=$(shasum -a 512 "$OUT/$NAME.xz" | awk '{print $1}')
if [ "$GOT_XZ" != "$EXPECT_XZ" ]; then
    echo "checksum mismatch:" >&2
    echo "  expected $EXPECT_XZ" >&2
    echo "  got      $GOT_XZ" >&2
    rm -f "$OUT/$NAME.xz"
    exit 1
fi

if [ ! -f "$OUT/$NAME" ]; then
    echo "==> Unpacking raw image (about 7.7 GiB)"
    xz -dk "$OUT/$NAME.xz"
fi
echo "==> Image ready: $OUT/$NAME"
