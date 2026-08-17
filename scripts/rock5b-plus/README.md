# Rock 5B+ official image: build, fetch, run

End-to-end flow for running the official Radxa OS image on the `rock-5b-plus`
machine: build QEMU, download the Radxa Bookworm KDE image, boot it.

## Scripts

- `build-qemu.sh` -- build `qemu-system-aarch64` (aarch64-softmmu, incl. rock-5b-plus)
- `fetch-radxa-image.sh` -- download the official Bookworm KDE image (SHA-512 verify + unpack)
- `run-rock5b-plus.sh` -- boot the machine headless (optional `--net` adds a NIC)

## Quick start

```bash
./scripts/rock5b-plus/build-qemu.sh
./scripts/rock5b-plus/fetch-radxa-image.sh
./scripts/rock5b-plus/run-rock5b-plus.sh
```

Login: `rock` / `rock`.

## 1. Build QEMU

### Dependencies

- Build tools: `ninja`, `meson`, `pkg-config`, GCC/Clang
- Libraries: `glib-2.0 (>= 2.56)`, `pixman`; `libslirp` is needed for
  `-netdev user` (the `--net` option)

macOS (Homebrew):

```bash
brew install ninja meson pkg-config glib pixman libslirp
```

Debian/Ubuntu:

```bash
sudo apt install ninja-build meson pkg-config libglib2.0-dev libpixman-1-dev libslirp-dev
```

### Build

```bash
./scripts/rock5b-plus/build-qemu.sh
```

Configures and builds in `<repo>/build/rock5b-plus/` (incremental; re-running
only rebuilds what changed).  The binary is `build/rock5b-plus/qemu-system-aarch64`.

Skip slirp (no `-netdev user`) with `build-qemu.sh --disable-slirp`.
Set `BUILD_DIR` to use a different build directory.

## 2. Fetch the official image

The image comes from the radxa-build GitHub release `rsdk-r7` (Bookworm KDE):

- Release page: <https://github.com/radxa-build/rock-5b-plus/releases/tag/rsdk-r7>
- Archive: `rock-5b-plus_bookworm_kde_r7.output_512.img.xz` (about 1.4 GiB;
  unpacks to about 7.7 GiB raw image)
- Checksums: `rock-5b-plus_bookworm_kde_r7.sha512sum` in the same release
  (the script verifies the `.img.xz` SHA-512)

```bash
./scripts/rock5b-plus/fetch-radxa-image.sh
```

Downloads to the current directory by default; set `IMAGE_DIR` to choose
another location.  The raw image is unpacked after verification.

## 3. Boot

```bash
# default: 1 CPU / 2G RAM / headless (multi-user target, no GUI)
./scripts/rock5b-plus/run-rock5b-plus.sh

# add an e1000e NIC on pcie2x1l0 (-netdev user, DHCP 10.0.2.15)
./scripts/rock5b-plus/run-rock5b-plus.sh --net

# image can also be given via the IMAGE env var; QEMU binary via QEMU_BIN
# custom image / resources
./scripts/rock5b-plus/run-rock5b-plus.sh --image /path/to/rock-5b-plus_bookworm_kde_r7.output_512.img --smp 4 --mem 4G
```

### Options explained

- `-machine rock-5b-plus,firmware-bootargs='cpuidle.off=1 systemd.unit=multi-user.target'`
  - `cpuidle.off=1`: disable PSCI cpuidle (under TCG the CPU can fall asleep
    and never wake up)
  - `systemd.unit=multi-user.target`: skip the KDE graphical target (it
    stalls without DRM)
- `-drive if=sd,index=0,file=<image>,format=raw`: the image is attached as an
  SD card on the eMMC controller and boots through the on-image U-Boot
- `-snapshot`: writes go to a temporary copy; the image file is untouched
- `-serial mon:stdio`: serial console (ttyFIQ0) plus QEMU monitor

### Login and verification

- User/password: `rock` / `rock`
- `dmesg` needs root: `echo rock | sudo -S dmesg` (dmesg_restrict is on)
- Checks:
  - `cat /proc/mtd` shows `mtd0: 01000000 00001000 "loader"` (16 MiB SPI NOR)
  - `lspci` shows `0002:20:00.0 PCI bridge: Synopsys DWC_usb3 / PCIe bridge`
  - with `--net`: `ip addr` shows `enP2p33s0` with DHCP address `10.0.2.15`

## 4. Troubleshooting

- **Stuck at the GUI / slow boot**: make sure firmware-bootargs contains
  `systemd.unit=multi-user.target`; first boot under TCG takes 1-2 minutes
- **`-netdev user` error**: QEMU was built without libslirp; install libslirp
  and re-run `build-qemu.sh`
- **Checksum mismatch**: delete the local `.img.xz` and re-fetch
- **virtio-net unavailable**: the Radxa kernel has no virtio PCI drivers; the
  scripts use the image-shipped e1000e driver for the network path
- **Want persistent writes**: drop `-snapshot` (not exposed by the script;
  run the QEMU command manually)

## References

- Machine docs: `docs/system/arm/rk3588.rst`
- Modeling and verification notes: `.oh-my-qemu/rock5b-plus-modeling/audit.md`
