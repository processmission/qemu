# Process Mission QEMU

Process Mission maintains this downstream QEMU branch for board and SoC model
development that is not yet carried by upstream QEMU. It tracks new machine
models, boot flows, and test coverage as they are added.

This branch is regularly rebased on top of upstream QEMU. Please file an issue
for new machine requests or bug reports.

## Machine Models

| arch | machine | direct | firmware | src |
| --- | --- | --- | --- | --- |
| RISC-V | [`k230-canmv`](docs/system/riscv/k230-canmv.rst) | ✅ | ✅ | PM |
| RISC-V | [`k3-pico-itx`](docs/system/riscv/spacemit-k3.rst) | ✅ | ✅ | PM |
| RISC-V | [`milkv-duo`](docs/system/riscv/milkv-duo.rst) | ✅ | ✅ | UP |
| RISC-V | [`riscv-server-ref`](docs/system/riscv/riscv-server-ref.rst) | ✅ | ✅ | UP |
| ARM | [`ax650x-pyramid`](docs/system/arm/ax650x-pyramid.rst) | ✅ | ❌ | PM |
| ARM | [`phytium-pi`](docs/system/arm/phytium-pi.rst) | ✅ | ✅ | PM |
| ARM | [`rk3588-evb`](docs/system/arm/rk3588.rst) | ✅ | ✅ | PM |
| ARM | [`rk3588s-roc-pc`](docs/system/arm/rk3588.rst) | ✅ | ✅ | PM |
| ARM | [`s32k566-cvb-r52`](docs/system/arm/s32k5.rst) | ✅ | ❌ | PM |

Source legend:

- `PM`: Process Mission downstream-maintained model.
- `UP`: imported from upstream QEMU/qemu-devel.
- `OSS`: imported from other open-source repositories.
- `VND`: imported from vendor sources.

## Development Workflow

The machine models in this branch are developed with the `oh-my-qemu` workflow.
It provides agent skills for planning, register extraction, peripheral modeling,
board modeling, qtest, build, debugging, and verification.

- Skill repository: <https://github.com/processmission/oh-my-qemu>

## AX650X Pyramid quick start

The `ax650x-pyramid` machine directly boots Linux on the M5Stack AI Pyramid /
AXERA AX650X platform. Detailed machine documentation is available in
[docs/system/arm/ax650x-pyramid.rst](docs/system/arm/ax650x-pyramid.rst).

### Boot Ubuntu 22.04 from eMMC

The Ubuntu image has no MBR or GPT. Linux creates its twelve partitions from
the fixed `blkdevparts` command line. The command below uses `snapshot=on`, so
guest writes are discarded when QEMU exits.

```sh
QEMU=${QEMU:-build/qemu-system-aarch64}
ASSET_DIR=${ASSET_DIR:-assets/ax650x}
KERNEL=${KERNEL:-$ASSET_DIR/Image-5.15.73-axera}
EMMC=${EMMC:-$ASSET_DIR/ax650x-ubuntu-22.04-emmc.raw}

PARTS='mmcblk0:1536K(uboot),1536K(uboot_bk),1M(env),20M(param)'
PARTS="$PARTS,6M(logo),1M(dtb),64M(kernel),1M(atf),1M(optee)"
PARTS="$PARTS,1M(recovery_dtb),74M(recovery),30380032K(rootfs)"
CMDLINE='console=ttyS0,115200n8 earlycon=uart8250,mmio32,0x2016000'
CMDLINE="$CMDLINE root=/dev/mmcblk0p12 rootfstype=ext4 rw rootwait"
CMDLINE="$CMDLINE blkdevparts=$PARTS"
CMDLINE="$CMDLINE systemd.show_status=yes systemd.log_target=console"

for input in "$QEMU" "$KERNEL" "$EMMC"; do
    if [ ! -r "$input" ]; then
        echo "missing input: $input" >&2
        exit 1
    fi
done

exec "$QEMU" \
    -machine ax650x-pyramid \
    -accel tcg,thread=multi \
    -cpu cortex-a55 \
    -smp 8 \
    -m 2G \
    -kernel "$KERNEL" \
    -append "$CMDLINE" \
    -drive "file=$EMMC,if=sd,format=raw,snapshot=on" \
    -chardev stdio,id=serial0,signal=off \
    -serial chardev:serial0 \
    -display none \
    -monitor none \
    -no-reboot
```

Override `QEMU`, `KERNEL`, or `EMMC` in the environment when the artifacts
live elsewhere. Remove `snapshot=on` only when persistent image writes are
intentional.

### Run the Ubuntu quick-boot functional test

The functional test uses the same direct-boot contract but lets the test
harness own the serial chardev. It is in the `thorough` suite because the
kernel and compressed eMMC image are downloaded assets.

```sh
meson test -C build \
    --suite thorough \
    func-aarch64-ax650x_ubuntu \
    --print-errorlogs
```

The pinned kernel and qcow2 image are published at:

<https://github.com/processmission/qemu/releases/tag/ax650x-ubuntu-22.04-qemu1>

The harness verifies both SHA-256 digests. The test uses disposable eMMC
writes and waits for DWMAC probe, partition 12, the mounted ext4 root
filesystem, Ubuntu readiness markers, and the serial login prompt.
