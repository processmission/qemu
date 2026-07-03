#!/usr/bin/env bash
#
# Rebuild the RK3588S ROC PC Rockchip-packaged U-Boot fixture.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

UBOOT_URL=${UBOOT_URL:-https://github.com/u-boot/u-boot.git}
UBOOT_COMMIT=${UBOOT_COMMIT:-5ca1a73c7d3064582498a8aa96c29e714402a6d3}
RKBIN_URL=${RKBIN_URL:-https://github.com/rockchip-linux/rkbin}
RKBIN_COMMIT=${RKBIN_COMMIT:-ecb4fcbe954edf38b3ae037d5de6d9f5bccf81f4}
if [ -n "${SOURCE_DATE_EPOCH+x}" ]; then
    UBOOT_SOURCE_DATE_EPOCH=${UBOOT_SOURCE_DATE_EPOCH:-$SOURCE_DATE_EPOCH}
    FIT_SOURCE_DATE_EPOCH=${FIT_SOURCE_DATE_EPOCH:-$SOURCE_DATE_EPOCH}
else
    UBOOT_SOURCE_DATE_EPOCH=${UBOOT_SOURCE_DATE_EPOCH:-1781525104}
    FIT_SOURCE_DATE_EPOCH=${FIT_SOURCE_DATE_EPOCH:-1781525116}
fi
SOURCE_DATE_EPOCH=$UBOOT_SOURCE_DATE_EPOCH
: "${EXPECTED_UBOOT_ROCKCHIP_SHA256:=\
6cd4b3d1931a252d564c88220e071eba57009fff49620b9520f33e8c58f80fb5}"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
output_dir=${OUTPUT_DIR:-$repo_root/build/zvm-rk3588s-roc-pc-u-boot}
dockerfile=${DOCKERFILE:-$script_dir/Dockerfile}
docker_image=${DOCKER_IMAGE:-qemu-zvm-rk3588s-roc-pc-u-boot:ubuntu-24.04}
inside_container=false
native=false

usage()
{
    cat <<'EOF'
Usage:
  build-u-boot-rockchip.sh [--output-dir DIR] [--native]

Clone U-Boot and rkbin at the pinned revisions, apply the ROC-PC DTS
compatibility patch, build u-boot-rockchip.bin, and verify its SHA256.

By default this script builds the local Dockerfile and runs inside that image.
Use --native to run directly on a Linux host with the required packages.

Environment overrides:
  UBOOT_URL, UBOOT_COMMIT, RKBIN_URL, RKBIN_COMMIT,
  UBOOT_SOURCE_DATE_EPOCH, FIT_SOURCE_DATE_EPOCH, SOURCE_DATE_EPOCH,
  DOCKERFILE, DOCKER_IMAGE, EXPECTED_UBOOT_ROCKCHIP_SHA256,
  ALLOW_HASH_MISMATCH=1
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
    --output-dir)
        need_value "$@"
        output_dir=$2
        shift 2
        ;;
    --native)
        native=true
        shift
        ;;
    --inside-container)
        inside_container=true
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

mkdir -p "$output_dir"
output_dir=$(CDPATH= cd -- "$output_dir" && pwd)

if ! $inside_container && ! $native; then
    command -v docker >/dev/null 2>&1 || \
        die "missing docker; install Docker or rerun with --native on Linux"
    docker build -f "$dockerfile" -t "$docker_image" "$script_dir"
    docker run --rm \
        -e HOST_UID="$(id -u)" \
        -e HOST_GID="$(id -g)" \
        -e UBOOT_URL="$UBOOT_URL" \
        -e UBOOT_COMMIT="$UBOOT_COMMIT" \
        -e RKBIN_URL="$RKBIN_URL" \
        -e RKBIN_COMMIT="$RKBIN_COMMIT" \
        -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
        -e UBOOT_SOURCE_DATE_EPOCH="$UBOOT_SOURCE_DATE_EPOCH" \
        -e FIT_SOURCE_DATE_EPOCH="$FIT_SOURCE_DATE_EPOCH" \
        -e EXPECTED_UBOOT_ROCKCHIP_SHA256="$EXPECTED_UBOOT_ROCKCHIP_SHA256" \
        -e ALLOW_HASH_MISMATCH="${ALLOW_HASH_MISMATCH:-0}" \
        -v "$repo_root:$repo_root" \
        -v "$output_dir:$output_dir" \
        -w "$repo_root" \
        "$docker_image" \
        bash "$script_dir/build-u-boot-rockchip.sh" \
            --inside-container \
            --output-dir "$output_dir"
    exit $?
fi

if $inside_container && [ "${INSTALL_BUILD_DEPS:-0}" = 1 ]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y \
        bc \
        bison \
        build-essential \
        device-tree-compiler \
        flex \
        gcc-aarch64-linux-gnu \
        git \
        libgnutls28-dev \
        libssl-dev \
        patch \
        python3 \
        python3-dev \
        python3-pyelftools \
        python3-setuptools \
        swig
fi

command -v git >/dev/null 2>&1 || die "missing required command: git"
command -v make >/dev/null 2>&1 || die "missing required command: make"
command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || \
    die "missing required command: aarch64-linux-gnu-gcc"

clone_at_commit()
{
    repo_url=$1
    commit=$2
    dest=$3

    if [ ! -d "$dest/.git" ]; then
        rm -rf "$dest"
        mkdir -p "$dest"
        git -C "$dest" init
        git -C "$dest" remote add origin "$repo_url"
    fi

    git config --global --add safe.directory "$dest"
    git -C "$dest" remote set-url origin "$repo_url"
    if ! git -C "$dest" fetch --depth=1 origin "$commit"; then
        git -C "$dest" fetch origin
    fi
    git -C "$dest" checkout --detach "$commit"
    git -C "$dest" reset --hard "$commit"
    git -C "$dest" clean -ffdx
}

sha256_file()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

rerun_binman_with_fit_epoch()
{
    cmd_file=$build_dir/..binman_stamp.cmd
    [ -f "$cmd_file" ] || die "missing binman command file: $cmd_file"

    binman_cmd=$(sed -n 's/^cmd_\.binman_stamp := //p' "$cmd_file")
    [ -n "$binman_cmd" ] || die "could not parse binman command from $cmd_file"

    rm -f \
        "$build_dir/.binman_stamp" \
        "$build_dir/idbloader.img" \
        "$build_dir/simple-bin.fit.fit" \
        "$build_dir/simple-bin.fit.itb" \
        "$build_dir/u-boot.itb" \
        "$build_dir/u-boot-rockchip.bin"

    (
        cd "$build_dir"
        SOURCE_DATE_EPOCH=$FIT_SOURCE_DATE_EPOCH sh -c "$binman_cmd"
    )
}

restore_uboot_git()
{
    if [ -d "$saved_uboot_git" ] && [ ! -e "$uboot_src/.git" ]; then
        mv "$saved_uboot_git" "$uboot_src/.git"
    fi
}

mkdir -p "$output_dir"

uboot_src=$output_dir/u-boot-src
rkbin_src=$output_dir/rkbin
build_dir=$output_dir/u-boot-build
patch_file=$script_dir/u-boot-rk3588s-roc-pc-zvmcompat.diff
saved_uboot_git=$output_dir/u-boot-src.git-for-build

clone_at_commit "$UBOOT_URL" "$UBOOT_COMMIT" "$uboot_src"
clone_at_commit "$RKBIN_URL" "$RKBIN_COMMIT" "$rkbin_src"

git -C "$uboot_src" apply "$patch_file"

rm -rf "$build_dir"
mkdir -p "$build_dir"

export CROSS_COMPILE=aarch64-linux-gnu-
export SOURCE_DATE_EPOCH

make -C "$uboot_src" O="$build_dir" generic-rk3588_defconfig
"$uboot_src/scripts/config" --file "$build_dir/.config" \
    --enable OF_UPSTREAM \
    --set-str DEFAULT_DEVICE_TREE rockchip/rk3588s-roc-pc \
    --set-str DEFAULT_FDT_FILE rockchip/rk3588-generic.dtb \
    --set-str OF_LIST rockchip/rk3588s-roc-pc \
    --set-str SPL_OF_LIST rockchip/rk3588s-roc-pc \
    --enable CMD_CACHE
make -C "$uboot_src" O="$build_dir" olddefconfig

rm -rf "$saved_uboot_git"
mv "$uboot_src/.git" "$saved_uboot_git"
trap restore_uboot_git EXIT

BL31=$rkbin_src/bin/rk35/rk3588_bl31_v1.54.elf \
ROCKCHIP_TPL=$rkbin_src/bin/rk35/rk3588_ddr_lp4_2112MHz_lp5_2400MHz_v1.21.bin \
    make -C "$uboot_src" O="$build_dir" -j"${JOBS:-$(nproc)}"

if [ "$FIT_SOURCE_DATE_EPOCH" != "$UBOOT_SOURCE_DATE_EPOCH" ]; then
    rerun_binman_with_fit_epoch
fi

restore_uboot_git
trap - EXIT

install -m 0644 "$build_dir/u-boot-rockchip.bin" "$output_dir/u-boot-rockchip.bin"
install -m 0644 "$build_dir/u-boot.bin" "$output_dir/u-boot.bin"
install -m 0644 "$build_dir/idbloader.img" "$output_dir/idbloader.img"
install -m 0644 "$build_dir/u-boot.itb" "$output_dir/u-boot.itb"

if $inside_container && [ -n "${HOST_UID:-}" ]; then
    chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" "$output_dir"
fi

actual=$(sha256_file "$output_dir/u-boot-rockchip.bin")
if [ "$actual" != "$EXPECTED_UBOOT_ROCKCHIP_SHA256" ]; then
    echo "error: u-boot-rockchip.bin hash mismatch" >&2
    echo "expected: $EXPECTED_UBOOT_ROCKCHIP_SHA256" >&2
    echo "actual:   $actual" >&2
    [ "${ALLOW_HASH_MISMATCH:-0}" = 1 ] || exit 1
fi

echo "Done: $output_dir/u-boot-rockchip.bin"
