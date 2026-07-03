#!/usr/bin/env bash
#
# Rebuild the minimal ZVM RK3588S ROC PC functional-test fixture.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

kernel_dir=
u_boot_rockchip=
output=
keep_raw=false

usage()
{
    cat <<'EOF'
Usage:
  build-image.sh \
      --kernel-dir DIR \
      --uboot-rockchip FILE \
      --output FILE.raw.zst [--keep-raw]

Builds the no-filesystem RK3588S ROC PC ZVM raw image with the repository
packaging helper, then compresses it with zstd for functional tests.

Required:
  --kernel-dir DIR          ZVM RK3588 kernel_images directory.
  --uboot-rockchip FILE     Rockchip u-boot-rockchip.bin.
  --output FILE.raw.zst     Compressed output fixture.

Options:
  --keep-raw                Keep the intermediate raw image next to output.
  -h, --help                Show this help.
EOF
}

die()
{
    echo "error: $*" >&2
    exit 1
}

need_value()
{
    [ "$#" -ge 2 ] || die "$1 requires an argument"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --kernel-dir)
        need_value "$@"
        kernel_dir=$2
        shift 2
        ;;
    --uboot-rockchip)
        need_value "$@"
        u_boot_rockchip=$2
        shift 2
        ;;
    --output)
        need_value "$@"
        output=$2
        shift 2
        ;;
    --keep-raw)
        keep_raw=true
        shift
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        die "unknown argument: $1"
        ;;
    esac
done

[ -n "$kernel_dir" ] || die "--kernel-dir is required"
[ -n "$u_boot_rockchip" ] || die "--uboot-rockchip is required"
[ -n "$output" ] || die "--output is required"
command -v zstd >/dev/null 2>&1 || die "missing required command: zstd"

case "$output" in
*.zst)
    raw_output=${output%.zst}
    ;;
*)
    raw_output=$output.raw
    ;;
esac

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
helper=$repo_root/scripts/zvm/rk3588s-roc-pc-make-zvm-image.sh

"$helper" \
    --kernel-dir "$kernel_dir" \
    --uboot-rockchip "$u_boot_rockchip" \
    --output "$raw_output" \
    --no-filesystems \
    --force

zstd -T0 -15 -f "$raw_output" -o "$output"

if ! $keep_raw; then
    rm -f "$raw_output"
fi

echo "Done: $output"
