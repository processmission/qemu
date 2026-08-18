#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Boot the Rock 5B+ official Radxa image (headless)
# Usage: run-rock5b-plus.sh [--image PATH] [--net] [--smp N] [--mem SIZE]
#   --image PATH  image path (default <repo>/build/rock5b-plus/
#                 rock-5b-plus_bookworm_kde_r7.output_512.img)
#   --net         attach an e1000e NIC on pcie2x1l0 (-netdev user, DHCP 10.0.2.15)
#   --smp N       CPUs (default 1)
#   --mem SIZE    RAM (default 2G)
# Env: QEMU_BIN (default <repo>/build/rock5b-plus/qemu-system-aarch64)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QEMU="${QEMU_BIN:-$ROOT/build/rock5b-plus/qemu-system-aarch64}"
IMAGE="${IMAGE:-$ROOT/build/rock5b-plus/rock-5b-plus_bookworm_kde_r7.output_512.img}"
SMP=1
MEM=8G
NET=0

while [ $# -gt 0 ]; do
    case "$1" in
        --image) IMAGE="$2"; shift 2 ;;
        --net)   NET=1; shift ;;
        --smp)   SMP="$2"; shift 2 ;;
        --mem)   MEM="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

if [ ! -x "$QEMU" ]; then
    echo "QEMU binary not found: $QEMU" >&2
    echo "run scripts/rock5b-plus/build-qemu.sh first" >&2
    exit 1
fi
if [ ! -f "$IMAGE" ]; then
    echo "image not found: $IMAGE" >&2
    echo "run scripts/rock5b-plus/fetch-radxa-image.sh first" >&2
    exit 1
fi

# firmware-bootargs: disable PSCI cpuidle (TCG idle hang); skip the KDE
# graphical target (stalls without DRM)
ARGS=( \
    -machine "rock-5b-plus,firmware-bootargs=cpuidle.off=1 \
systemd.unit=multi-user.target" \
    -smp "$SMP" \
    -m "$MEM" \
    -drive "if=sd,index=0,file=$IMAGE,format=raw" \
    -snapshot \
    -serial mon:stdio \
    -display none \
)
if [ "$NET" = 1 ]; then
    ARGS+=( \
        -netdev user,id=n0 \
        -device "e1000e,bus=/pcie2x1l0/pcie/designware-pcie-root/dw-pcie,netdev=n0" \
    )
fi

echo "==> Booting $QEMU"
exec "$QEMU" "${ARGS[@]}"
