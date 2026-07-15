.. SPDX-License-Identifier: GPL-2.0-or-later

M5Stack AI Pyramid (``ax650x-pyramid``)
========================================

Overview
--------

The ``ax650x-pyramid`` machine models the Linux boot subset of the M5Stack
AI Pyramid, whose runtime device tree identifies the SoC as
``axera,ax650x``.  The model is intended for direct kernel boot with an
AX650X-enabled Arm64 Linux kernel.  It does not model the product's AXERA
accelerators or firmware boot chain.

The machine has a fixed topology matching the target system: eight Cortex-A55
CPUs and 2 GiB of RAM starting at physical address ``0x100000000``.  Other CPU
counts and memory sizes are rejected.

Supported devices
-----------------

The machine provides the following devices and architectural services:

* eight Cortex-A55 CPUs, with MPIDRs ``0x000`` through ``0x700``;
* GIC-400 compatible GICv2 interrupt controller with virtualization
  extensions;
* Arm generic timers running at 24 MHz;
* PSCI 1.0 using the SMC conduit;
* the AXERA UART0 console at ``0x02016000``, including the extension
  registers needed by the AXERA 8250 driver;
* an AX650X SDHCI/eMMC controller at ``0x28000000``, including the vendor PHY
  and eMMC registers used during Linux probe;
* two Synopsys DWMAC 4.10a compatible Ethernet controllers, including the
  AX650X clock/reset glue, MDIO PHY identity, DMA and interrupt paths;
* the DesignWare APB GPIO blocks used for PHY reset; and
* the AX650X hardware spinlock block used by the GPIO driver.

QEMU generates a minimal device tree containing only those implemented
devices.  An eMMC backend supplied with ``if=sd`` is attached to the AX650X
controller.

Direct Linux boot
-----------------

The model requires an uncompressed Arm64 ``Image`` containing the AXERA UART
and SDHCI drivers.  The following example boots an ext4 root filesystem from
partition 12 of an eMMC image that uses the target's fixed partition layout.
The image has no MBR or GPT, so the complete ``blkdevparts`` argument is
required:

.. code-block:: shell

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

``snapshot=on`` keeps the reusable eMMC image unchanged.  Remove it only when
persistent guest writes are intentional.

The downstream model has been exercised with an AXERA Linux 5.15.73 kernel
and Ubuntu 22.04.5 LTS.  Linux enumerates all eight CPUs, probes the eMMC using
64-bit ADMA, creates the twelve fixed partitions, and mounts
``/dev/mmcblk0p12`` as the writable root filesystem.

Known limitations
-----------------

* Only QEMU direct kernel boot is implemented.  BootROM, SPL, U-Boot, Arm
  Trusted Firmware and OP-TEE images are not loaded or executed.
* NPU, VDSP, RISC-V auxiliary cores, video, ISP, display, audio, USB, PCIe,
  SATA and the full clock, reset and power-management trees are not modeled.
  GPIO behavior is limited to the DesignWare APB subset needed for PHY reset.
* DWMAC TSO, PTP/TSN, multi-queue performance fidelity and analog PHY timing
  are not modeled.
* The UART extension window implements the probe-time subset.  The AXERA
  driver can report a harmless capability mismatch because the UART component
  version register intentionally selects its compatible fallback path.
* QEMU direct boot starts CPUs at EL2.  The target firmware starts Linux at
  EL1, so the vendor kernel can print a non-fatal GICv2 CPU-interface range
  warning under QEMU.
* The SDHCI vendor bank implements the registers required for probe, reset and
  data transfer.  HS200/HS400 timing accuracy is not claimed.

Running tests
-------------

The board qtest covers CPU topology, RAM, GICv2, UART MMIO and IRQ behavior,
SDHCI vendor registers, reset, block I/O, eMMC IRQ routing, 64-bit ADMA,
Auto CMD23, DWMAC synthesis registers, MDIO, GPIO and reset glue, hardware
spinlocks, 40-bit DMA and Ethernet IRQ routing::

  $ meson test -C build qemu:qtest-aarch64/ax650x-pyramid-test \
      --print-errorlogs

The Ubuntu quick-boot functional test downloads a pinned kernel and compressed
qcow2 image from the `AX650X Ubuntu 22.04 QEMU assets release
<https://github.com/processmission/qemu/releases/tag/ax650x-ubuntu-22.04-qemu1>`__.
The functional asset layer verifies both SHA-256 digests.  The test uses
``snapshot=on`` and waits for DWMAC probe, the eMMC partition map, the mounted
root filesystem, Ubuntu readiness markers and the serial login prompt:

.. code-block:: shell

  $ meson test -C build \
        --suite thorough \
        func-aarch64-ax650x_ubuntu \
        --print-errorlogs

The test is registered in the ``thorough`` functional suite because the
kernel and Ubuntu image are external assets.  A cached copy is reused only
after its declared content hash has been checked.
