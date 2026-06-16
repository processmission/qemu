#!/usr/bin/env bash
#
# Build a QEMU raw TF-card image for the ZVM RK3588S ROC PC release package.
#
# Copyright (c) 2026 Process Mission
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The generated image uses the layout expected by the upstream RK3588 ZVM
# deployment package:
#   - Rockchip RKNS firmware at LBA 64
#   - partition 1: FAT32 boot partition with zvm_release_rk3588v2.bin
#                  and boot.scr/boot.scr.uimg
#   - partition 2: FAT32 payload partition with nrtos_images.bin at
#                  partition offset 1024 MiB, and optional filesystem images
#                  at the release package offsets.

set -euo pipefail

total_sectors=$((32 * 1024 * 1024 * 1024 / 512))
p1_start=$((0x8000))
p2_start=$((0x1005000))
p1_size=$((p2_start - p1_start))
p2_size=$((total_sectors - p2_start))
p1_start_mib=$((p1_start / 2048))
p2_start_mib=$((p2_start / 2048))

kernel_dir=
filesystem_dir=
u_boot_rockchip=
output=
boot_script=
regen=false
write_filesystems=true
force=false

usage()
{
    cat <<'EOF'
Usage:
  rk3588s-roc-pc-make-zvm-image.sh \
      --kernel-dir DIR \
      --filesystem-dir DIR \
      --uboot-rockchip FILE \
      --output FILE [options]

Required:
  --kernel-dir DIR          ZVM kernel_images directory.
  --filesystem-dir DIR      Directory containing android_vda.img, oh_vda.bin,
                            and linux_diskimg.bin.
  --uboot-rockchip FILE     Rockchip u-boot-rockchip.bin for the image header.
  --output FILE             Output raw TF-card image.

Options:
  --boot-script FILE        U-Boot script image to copy as boot.scr and
                            boot.scr.uimg. Defaults to DIR/boot.scr.
  --regen                   Run "printf '3\n' | ./auto_py.sh" in --kernel-dir
                            before packing the card.
  --no-filesystems          Skip Android/OpenHarmony/Linux filesystem slots.
  --force                   Overwrite an existing output image.
  -h, --help                Show this help.
EOF
}

die()
{
    echo "error: $*" >&2
    exit 1
}

need_cmd()
{
    command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
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
    --filesystem-dir)
        need_value "$@"
        filesystem_dir=$2
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
    --boot-script)
        need_value "$@"
        boot_script=$2
        shift 2
        ;;
    --regen)
        regen=true
        shift
        ;;
    --no-filesystems)
        write_filesystems=false
        shift
        ;;
    --force)
        force=true
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

if $write_filesystems && [ -z "$filesystem_dir" ]; then
    die "--filesystem-dir is required unless --no-filesystems is used"
fi

need_cmd dd
need_cmd python3
need_cmd truncate

host_os=$(uname -s)
case "$host_os" in
Linux)
    need_cmd mkfs.vfat
    need_cmd mcopy
    ;;
Darwin)
    need_cmd hdiutil
    need_cmd newfs_msdos
    ;;
*)
    die "unsupported host OS: $host_os"
    ;;
esac

[ -d "$kernel_dir" ] || die "kernel directory not found: $kernel_dir"
[ -f "$u_boot_rockchip" ] || die "u-boot-rockchip file not found: $u_boot_rockchip"

if [ -z "$boot_script" ]; then
    boot_script="$kernel_dir/boot.scr"
fi

if $regen; then
    [ -x "$kernel_dir/auto_py.sh" ] || die "cannot execute $kernel_dir/auto_py.sh"
    (cd "$kernel_dir" && printf '3\n' | ./auto_py.sh)
fi

zvm_release="$kernel_dir/zvm_release_rk3588v2.bin"
nrtos_images="$kernel_dir/nrtos_images.bin"

[ -f "$zvm_release" ] || die "missing $zvm_release"
[ -f "$nrtos_images" ] || die "missing $nrtos_images"
[ -f "$boot_script" ] || die "missing boot script image: $boot_script"

if $write_filesystems; then
    [ -d "$filesystem_dir" ] || die "filesystem directory not found: $filesystem_dir"
    [ -f "$filesystem_dir/android_vda.img" ] || die "missing android_vda.img"
    [ -f "$filesystem_dir/oh_vda.bin" ] || die "missing oh_vda.bin"
    [ -f "$filesystem_dir/linux_diskimg.bin" ] || die "missing linux_diskimg.bin"
fi

if [ -e "$output" ] && ! $force; then
    die "output exists, use --force to overwrite: $output"
fi

mkdir -p "$(dirname "$output")"
workdir=$(mktemp -d "${TMPDIR:-/tmp}/rk3588s-zvm.XXXXXX")
attached_targets=()
darwin_target=

cleanup()
{
    local target

    for target in ${attached_targets[@]+"${attached_targets[@]}"}; do
        hdiutil detach -quiet "$target" >/dev/null 2>&1 || true
    done
    rm -rf "$workdir"
}

darwin_attach()
{
    local image=$1
    local mode=$2
    local mountpoint=${3:-}
    local target

    if [ "$mode" = "nomount" ]; then
        target=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage \
                 -nomount "$image" | awk 'NR == 1 { print $1 }')
        [ -n "$target" ] || die "failed to attach raw image: $image"
    else
        hdiutil attach -quiet -imagekey diskimage-class=CRawDiskImage \
            -nobrowse -mountpoint "$mountpoint" "$image"
        target=$mountpoint
    fi

    darwin_target=$target
    attached_targets+=("$target")
}

darwin_detach()
{
    local target=$1

    hdiutil detach -quiet "$target"
}

trap cleanup EXIT

p1_img="$workdir/rk3588boot.fat"
p2_img="$workdir/zvmfs.fat"
p1_mount="$workdir/rk3588boot.mnt"

echo "Creating sparse raw image: $output"
rm -f "$output"
truncate -s $((total_sectors * 512)) "$output"

python3 - "$output" "$p1_start" "$p1_size" "$p2_start" "$p2_size" <<'PY'
import struct
import sys

path, p1_start, p1_size, p2_start, p2_size = sys.argv[1:]

def entry(active, ptype, start, size):
    # CHS values are ignored by modern firmware; expose LBA fields only.
    return struct.pack("<B3sB3sII", active, b"\xff\xff\xff", ptype,
                       b"\xff\xff\xff", int(start), int(size))

with open(path, "r+b") as f:
    f.seek(446)
    f.write(entry(0x80, 0x0c, p1_start, p1_size))
    f.write(entry(0x00, 0x0b, p2_start, p2_size))
    f.write(b"\x00" * 32)
    f.seek(510)
    f.write(b"\x55\xaa")
PY

echo "Formatting FAT partition images"
case "$host_os" in
Linux)
    mkfs.vfat -F 32 -n RK3588BOOT -C "$p1_img" $((p1_size / 2)) >/dev/null
    mkfs.vfat -F 32 -n ZVMFS -C "$p2_img" $((p2_size / 2)) >/dev/null
    ;;
Darwin)
    truncate -s $((p1_size * 512)) "$p1_img"
    darwin_attach "$p1_img" nomount
    p1_dev=$darwin_target
    if ! newfs_msdos -F 32 -v RK3588BOOT "$p1_dev" >/dev/null; then
        darwin_detach "$p1_dev" || true
        exit 1
    fi
    darwin_detach "$p1_dev"

    truncate -s $((p2_size * 512)) "$p2_img"
    darwin_attach "$p2_img" nomount
    p2_dev=$darwin_target
    if ! newfs_msdos -F 32 -v ZVMFS "$p2_dev" >/dev/null; then
        darwin_detach "$p2_dev" || true
        exit 1
    fi
    darwin_detach "$p2_dev"
    ;;
esac

echo "Populating boot partition"
case "$host_os" in
Linux)
    mcopy -o -i "$p1_img" "$zvm_release" ::/zvm_release_rk3588v2.bin
    mcopy -o -i "$p1_img" "$boot_script" ::/boot.scr
    mcopy -o -i "$p1_img" "$boot_script" ::/boot.scr.uimg
    ;;
Darwin)
    mkdir -p "$p1_mount"
    darwin_attach "$p1_img" mount "$p1_mount" >/dev/null
    cp -f "$zvm_release" "$p1_mount/zvm_release_rk3588v2.bin"
    cp -f "$boot_script" "$p1_mount/boot.scr"
    cp -f "$boot_script" "$p1_mount/boot.scr.uimg"
    darwin_detach "$p1_mount"
    ;;
esac

echo "Writing nrtos_images.bin at partition 2 offset 1024 MiB"
dd if="$nrtos_images" of="$p2_img" bs=1M seek=1024 conv=notrunc status=progress

if $write_filesystems; then
    echo "Writing release filesystem images"
    dd if="$filesystem_dir/android_vda.img" of="$p2_img" bs=1M seek=3072 \
        conv=notrunc,sparse status=progress
    dd if="$filesystem_dir/oh_vda.bin" of="$p2_img" bs=1M seek=6144 \
        conv=notrunc,sparse status=progress
    for seek in 9216 12288 15360 19432; do
        dd if="$filesystem_dir/linux_diskimg.bin" of="$p2_img" bs=1M \
            seek="$seek" conv=notrunc,sparse status=progress
    done
fi

echo "Installing partitions into raw image"
dd if="$p1_img" of="$output" bs=1M seek="$p1_start_mib" \
    conv=notrunc,sparse status=progress
dd if="$p2_img" of="$output" bs=1M seek="$p2_start_mib" \
    conv=notrunc,sparse status=progress

echo "Installing Rockchip RKNS firmware at LBA 64"
dd if="$u_boot_rockchip" of="$output" bs=512 seek=64 conv=notrunc status=none

echo "Done: $output"
