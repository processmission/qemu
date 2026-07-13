.. SPDX-License-Identifier: GPL-2.0-or-later

SpacemiT K3 Pico-ITX board (``k3-pico-itx``)
================================================

The ``k3-pico-itx`` machine models the standard RISC-V platform subset
needed to boot a SpacemiT K3 SDK Linux kernel directly or through U-Boot on
the K3 Pico-ITX board.  It focuses on the eight X100 application harts and
does not expose the K3 A100 or IME harts.

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
* an ACLINT software interrupt and machine timer block with a 24 MHz timebase;
* machine- and supervisor-level APLIC and IMSIC interrupt controllers;
* the 8250-compatible UART0 at ``0xd4017000``, using interrupt source 42; and
* the K3 SDHCI controller at ``0xd4280000``, using interrupt source 99.

Boot options
------------

Both supported Linux boot paths start generic OpenSBI directly.  QEMU can
either load Linux and its initramfs, or load U-Boot proper and let U-Boot load
Linux from an SD image.  An external device tree is required because the
machine validates the exact K3 topology and address map before boot.

The following examples use artifacts built from the SpacemiT K3 Buildroot SDK
v1.0.2 and published in the ``sdk-v1.0.2-qemu2`` release of
``spacemit-k3-qemu-images``.

Direct Linux boot
~~~~~~~~~~~~~~~~~

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

Decompress ``k3-qemu-sd.raw.xz``, then start U-Boot proper as OpenSBI's next
stage:

.. code-block:: bash

   $ xz -dk k3-qemu-sd.raw.xz
   $ qemu-system-riscv64 \
       -machine k3-pico-itx \
       -bios fw_dynamic.bin \
       -kernel u-boot.bin \
       -dtb k3-pico-itx-qemu-uboot.dtb \
       -drive file=k3-qemu-sd.raw,if=sd,format=raw,snapshot=on \
       -nographic -no-reboot

U-Boot imports its deterministic environment from the SD boot partition and
loads the Linux kernel, Linux device tree, and initramfs from that partition.
The successful path prints ``K3-QEMU: Starting kernel from SD`` before Linux
prints ``K3_LINUX_MVP_PASS``.

Limitations
-----------

This machine is a Linux boot subset rather than a complete K3 hardware model.
It disables the X100 H extension because the VS interrupt files and
virtualization path are not modeled.  The A100 and IME harts, PCIe,
networking, multimedia accelerators, power management, and most board
peripherals are not implemented.

The K3 BootROM, bootinfo parser, U-Boot SPL, and LPDDR training are not
modeled.  QEMU provides initialized RAM and starts generic OpenSBI directly;
the firmware boot path therefore uses U-Boot proper rather than the vendor
SPL chain.
