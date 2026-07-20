.. SPDX-License-Identifier: GPL-2.0-or-later

SpacemiT K3 Pico-ITX board (``k3-pico-itx``)
================================================

The ``k3-pico-itx`` machine models the standard RISC-V platform subset
needed to boot a SpacemiT K3 SDK Linux kernel on the K3 Pico-ITX board.  It
focuses on the eight X100 application harts and does not expose the K3 A100 or
IME harts.

The machine has a fixed CPU topology and memory layout matching the Linux
view of the board.  The default configuration must be used without overriding
the CPU type, RAM size, or SMP topology.

Supported devices
-----------------

The ``k3-pico-itx`` machine supports:

* eight SpacemiT X100 harts implementing RVA23S64 in two clusters of four;
* 256-bit RISC-V vector registers, Sstc, and the Smaia/Ssaia extensions;
* 2 GiB of RAM starting at ``0x102000000``;
* a 32 MiB firmware window starting at ``0x100000000``;
* 512 KiB of on-chip SRAM starting at ``0xc0800000``;
* an ACLINT software interrupt and machine timer block with a 24 MHz timebase;
* machine- and supervisor-level APLIC and IMSIC interrupt controllers;
* the 8250-compatible UART0 at ``0xd4017000``, using interrupt source 42;
* the SDHCI0 controller at ``0xd4280000``, using interrupt source 99;
* the SD clock/reset and boot-mode registers used by U-Boot; and
* the standard RISC-V register interface of the T100 IOMMU at ``0xc0f00000``,
  using interrupt source 234.

Boot options
------------

The machine supports both direct Linux boot and a firmware path through
U-Boot proper and SD.  Both paths use generic OpenSBI firmware and require an
external device tree because the machine validates the exact K3 Linux topology
and address map before boot.

Direct Linux boot
~~~~~~~~~~~~~~~~~

The following example uses artifacts built from the SpacemiT K3 Buildroot SDK
v1.0.2:

.. code-block:: bash

   $ qemu-system-riscv64 \
       -machine k3-pico-itx \
       -bios fw_dynamic.bin \
       -kernel Image \
       -initrd k3-qemu-initramfs.cpio.gz \
       -dtb k3-pico-itx-qemu.dtb \
       -append "earlycon=uart8250,mmio32,0xd4017000,115200 \
                console=ttyS0,115200 rdinit=/init" \
       -nographic -no-reboot

The machine supplies the fixed 2 GiB RAM size and 8-hart topology, so no
``-m`` or ``-smp`` options are needed.  Direct kernel boot requires
OpenSBI; ``-bios none`` is rejected when ``-kernel`` is present.
The machine uses the fixed ``spacemit-x100`` CPU model.

U-Boot and SD boot
~~~~~~~~~~~~~~~~~~

The K3 SDK U-Boot proper can run as the supervisor-mode next stage of OpenSBI.
Attach a raw SD image as the first SD drive; it is wired to SDHCI0 and appears
as MMC device 0 in U-Boot:

.. code-block:: bash

   $ qemu-system-riscv64 \
       -machine k3-pico-itx \
       -bios fw_dynamic.bin \
       -kernel u-boot.bin \
       -dtb k3-pico-itx-qemu-uboot.dtb \
       -drive file=k3-qemu-sd.raw,if=sd,format=raw,snapshot=on \
       -nographic -no-reboot

QEMU reports SD boot through the K3 CIU boot flag.  U-Boot then initializes
SDHCI0, finds the GPT partition named ``bootfs``, imports ``env_k3.txt``, and
loads the Linux ``Image``, device tree, and optional initramfs from that FAT
partition.  Using ``snapshot=on`` prevents firmware or the guest from changing
the source image.

The controller advertises a 52 MHz base clock, four-bit 3.3 V operation,
SD High Speed (up to 50 MHz), and ADMA2.  It does not advertise 1.8 V
signaling or UHS modes.

Limitations
-----------

This machine is a boot-path subset rather than a complete K3 hardware model.
It disables the X100 H extension because the VS interrupt files and
virtualization path are not modeled.  The K3 BootROM, bootinfo parsing,
U-Boot SPL, and LPDDR initialization and training are also not modeled; QEMU
provides initialized RAM and starts OpenSBI directly.

The A100 and IME harts, eMMC, UFS, SPI flash, PCIe, networking, multimedia
accelerators, system power management, and most board peripherals are not
implemented.  SDHCI0 implements the register and DMA behavior required by the
documented U-Boot-to-Linux path, not every vendor PHY tuning mode.

The IOMMU exposes the standard 4 KiB register window and its single wired
interrupt.  The machine does not modify the external device tree; the SDK DTS
currently marks this node disabled, so a guest DTB must enable it before Linux
can probe it.  No requester is attached yet: K3 maps PCIe0 through PCIe2 to the
IOMMU, but those host bridges are not modeled, while SDHCI0 is not an IOMMU
client in the K3 DTS.

The T100 distributed IOATCs, vendor performance events and filters, hybrid
WSI/MSI interrupt behavior, and PCIe ATS/PRI paths are also not modeled.  The
reported 56-bit physical address size is a QEMU platform-model choice rather
than a measured K3 capability value.
