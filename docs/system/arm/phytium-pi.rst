.. Copyright (c) 2026 Chao Liu
.. SPDX-License-Identifier: GPL-2.0-or-later

Phytium Pi Board (``phytium-pi``)
=================================

The ``phytium-pi`` machine is a local downstream model for the Phytium Pi
board based on the Phytium E2000Q/PE2204 SoC.  It is intended for direct Arm
Linux boot, vendor SD-card firmware/U-Boot boot, and ZVM E2000 smoke testing.

This is not a complete model of the SoC.  Several register ranges are present
only to let firmware progress to the next boot stage.

Supported devices
-----------------

The machine currently models or stubs the devices needed by the verified
Linux, U-Boot, and ZVM paths:

* Up to 4 AArch64 CPU objects with the PE2204 MPIDR layout.  The generated
  device tree reports the Phytium FTC664/FTC310 compatible strings used by the
  board firmware and Linux BSP.
* Up to 4 GiB RAM, split between a low 2 GiB window at ``0x80000000`` and a
  high 2 GiB window above 32-bit physical address space.
* GICv3 distributor and redistributor regions.  The ITS and legacy CPU,
  hypervisor, and virtual CPU interface windows are exposed as unimplemented
  regions.
* Arm generic timer with a 50 MHz counter frequency.
* PL011 UART windows.  The generated direct Linux device tree exposes
  ``serial@2800d000`` as ``serial1`` and the default console.
* Two Phytium MCI controllers.  The first controller is wired to
  ``-drive if=sd,index=0`` and is the boot card for firmware and Linux tests.
* A Phytium XMAC register/MDIO/PHY shim sufficient for link-status probes.
* Firmware RAM/control windows, an SCP mailbox shim, and a DDR controller shim
  used by the vendor firmware path.
* Placeholder PCIe ECAM, RNG, USB2, PHY configuration, and miscellaneous
  firmware windows used to avoid synchronous aborts during boot probing.

Boot options
------------

Direct Linux boot
~~~~~~~~~~~~~~~~~

The direct Linux path uses QEMU's standard Arm ``-kernel`` loader and a
QEMU-generated device tree:

.. code-block:: bash

   $ APPEND="console=ttyAMA1,115200n8 earlycon=pl011,0x2800d000 rdinit=/init"
   $ SDIMG=/path/to/sd.img
   $ qemu-system-aarch64 \
       -accel tcg \
       -machine phytium-pi \
       -smp 4 \
       -m 4G \
       -kernel /path/to/Image \
       -initrd /path/to/initramfs.cpio.gz \
       -append "$APPEND" \
       -drive if=sd,index=0,file="$SDIMG",format=raw,auto-read-only=off \
       -serial mon:stdio \
       -display none

The generated device tree sets ``stdout-path`` to ``serial1:115200n8``.  Keep
``console=ttyAMA1,115200n8`` when using kernels that honor the serial alias.

Firmware SD boot
~~~~~~~~~~~~~~~~

When ``-kernel`` is not provided, the machine enters the firmware path.  The
firmware path requires at least three CPUs because the board starts the EL3
firmware entry on CPU index 2.  Attach the Phytium Pi SD image as
``if=sd,index=0``:

.. code-block:: bash

   $ SDIMG=/path/to/phytium-pi-sd.img
   $ qemu-system-aarch64 \
       -accel tcg \
       -machine phytium-pi \
       -smp 4 \
       -m 4G \
       -drive if=sd,index=0,file="$SDIMG",format=raw,auto-read-only=off \
       -serial mon:stdio \
       -display none

The boot ROM window is initialized from the first 4 MiB of the SD image.  The
model then provides the firmware RAM, DDR-status, SCP mailbox, and firmware
MMIO windows needed by the verified U-Boot path.  The local openEuler smoke
run used a complete SD genimage with a rebuilt FIT whose kernel has the
required MMC/rootfs support built in; the original package FIT can stop at
``Waiting for root device`` if its storage drivers are modules and no initrd is
present.

ZVM E2000 path
~~~~~~~~~~~~~~

The ZVM E2000 release path runs through SD firmware and U-Boot before entering
the ZVM ELF payload.  The official `ZVM-E2000 deployment guide
<https://esnl.hnu.edu.cn/zvm/document/deploy_e2000.html>`_ describes the
release flow and payload layout.  In both flows below, the SD image provides
the Phytium Pi firmware and U-Boot, and ZVM is entered with
``bootelf 0xa1000000``.

QEMU preloaded payloads
^^^^^^^^^^^^^^^^^^^^^^^

For a deterministic QEMU smoke test that does not depend on U-Boot networking,
preload the ZVM and guest payloads at the same RAM addresses used by the
official flow:

.. code-block:: bash

   $ SDIMG=/path/to/phytium-pi-sd.img
   $ ZVM=/path/to/zvm_release/e2000
   $ LINUX="$ZVM/Image_v5.16.0"
   $ LINUX_DTB="$ZVM/linux_e2000q.dtb"
   $ INITRD="$ZVM/initramfs_e2000.cpio.gz"
   $ qemu-system-aarch64 \
       -accel tcg \
       -machine phytium-pi \
       -smp 4 \
       -m 2G \
       -drive if=sd,index=0,file="$SDIMG",format=raw,auto-read-only=off \
       -device loader,file="$ZVM/zvm.elf",addr=0xa1000000,force-raw=on \
       -device loader,file="$LINUX",addr=0xa2000000,force-raw=on \
       -device loader,file="$LINUX_DTB",addr=0xf8800000,force-raw=on \
       -device loader,file="$INITRD",addr=0xf9000000,force-raw=on \
       -serial mon:stdio \
       -display none

From the U-Boot prompt, enter ZVM:

.. code-block:: text

   => dcache flush; icache flush; dcache off; icache off; bootelf 0xa1000000

U-Boot TFTP payloads
^^^^^^^^^^^^^^^^^^^^

The official E2000 flow can also load the same payloads from U-Boot over TFTP.
Serve a directory containing the ``e2000/`` release files, boot the firmware SD
image, stop at the U-Boot prompt, and run the commands below.  This uses the
same RAM layout as the QEMU preloaded path; actual TFTP traffic requires a
working XMAC data path.

.. code-block:: text

   => mw 0x80100000 0x0 0x10000000
   => mw 0x90100000 0x0 0x10000000
   => setenv ipaddr 192.168.1.128
   => setenv serverip 192.168.1.100
   => setenv bootargs ""
   => tftp 0xa1000000 e2000/zvm.elf
   => tftp 0xf8000000 e2000/zephyr_e2000_c1_m128.bin
   => tftp 0xf8200000 e2000/zephyr_e2000_c2_m128.bin
   => tftp 0xf8400000 e2000/freertos_e2000_c1_m128.bin
   => tftp 0xa2000000 e2000/Image_v5.16.0
   => tftp 0xf8800000 e2000/linux_e2000q.dtb
   => tftp 0xf9000000 e2000/initramfs_e2000.cpio.gz
   => dcache flush; icache flush; dcache off; icache off; bootelf 0xa1000000

The verified Linux guest smoke path used these ZVM commands:

.. code-block:: text

   zvm create -t linux -c 1 -m 512
   zvm run -n 0
   zvm info
   zvm look 0

The recorded run reached the ZVM host shell, created ``linux_os-0``, reported
the VM as ``running``, connected to the guest console, booted Linux 5.16, ran
``/init``, and reached a guest shell prompt.

Known limitations
-----------------

The model is a boot and smoke-test target, not a full Phytium E2000Q model.
The XMAC model does not provide a complete Ethernet data path, PCIe is not
implemented, and several firmware-visible MMIO ranges are RAM-backed or
unimplemented placeholders.  The DDR controller shim reports the status values
needed by known firmware polling loops; it does not perform DDR training or
memory diagnostics.

Running tests
-------------

Build and run the Phytium Pi qtest with:

.. code-block:: bash

   $ ninja -C build qemu-system-aarch64 tests/qtest/phytiumpi-test
   $ QTEST_QEMU_BINARY=$PWD/build/qemu-system-aarch64 \
       build/tests/qtest/phytiumpi-test --tap -k

The qtest covers machine creation, high-memory mapping, MCI register and ADMA
flows, XMAC PHY/link status, and SMP creation.
