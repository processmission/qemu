#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# 编译 qemu-system-aarch64（aarch64-softmmu，含 rock-5b-plus / RK3588 机器）
# 用法: build-qemu.sh [--disable-slirp]
# 环境变量: BUILD_DIR  (默认 <仓库根>/build/rock5b-plus)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build/rock5b-plus}"
SLIRP=--enable-slirp
[ "${1:-}" = "--disable-slirp" ] && SLIRP=--disable-slirp

echo "==> 配置 (aarch64-softmmu, slirp: $SLIRP)"
mkdir -p "$BUILD"
if [ ! -f "$BUILD/build.ninja" ]; then
    ( cd "$BUILD" && "$ROOT/configure" \
        --target-list=aarch64-softmmu \
        "$SLIRP" \
        --disable-docs )
fi

echo "==> 编译 qemu-system-aarch64"
ninja -C "$BUILD" qemu-system-aarch64
echo "==> 完成: $BUILD/qemu-system-aarch64"
