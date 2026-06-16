Canaan CanMV-K230 board (``k230-canmv``)
========================================
The ``k230-canmv`` machine models the Canaan CanMV-K230 board and is
compatible with the Kendryte K230 SDK ``k230_canmv_defconfig`` output.

The K230 is a chip from the AIoT SoC series made by Kendryte ® — a part of
Canaan Inc. It uses a brand-new multi-heterogeneous unit accelerated computing
structure.

This chip has 2 RISC-V computing cores and a new-generation KPU (Knowledge
Process Unit) smart computing unit.

For more information, see <https://www.kendryte.com/en/proDetail/230>

Supported devices
-----------------
The ``k230-canmv`` machine supports the following devices:

* 1 T-Head C908 core and optional C908V big core
* Core Local Interruptor (CLINT)
* Platform-Level Interrupt Controller (PLIC)
* 2 K230 Watchdog Timer
* 5 UART
* GSDMA and UGZIP blocks for SDK U-Boot gzip image decompression
* PDMA register block
* 2 SDHCI-compatible K230 DWC MSHC controllers

Boot options
------------
The ``k230-canmv`` machine boots the K230 SDK through M-mode U-Boot. U-Boot
reads the Linux payload from an attached SD card image and then starts
OpenSBI/Linux using the normal SDK boot flow.

QEMU models the K230 SDHCI storage controller used by the CanMV removable card
slot, so no manual RAM loader setup is needed for the common Linux boot path.

Running
-------

U-Boot SD card boot
~~~~~~~~~~~~~~~~~~~

Use the SDK U-Boot binary as the machine firmware and attach a K230 SDK SD card
image with ``-drive if=sd``. The first SD drive is wired to the SDK CanMV
removable card slot, ``sdhci1@91581000``:

.. code-block:: bash

   $ SDK=k230_sdk/output/k230_canmv_defconfig
   $ SDIMG=/path/to/k230-sdk-sdcard.img
   $ qemu-system-riscv64 -machine k230-canmv \
      -bios "$SDK/little/uboot/u-boot" \
      -drive if=sd,file="$SDIMG",format=raw \
      -nographic

For an SDK image that also starts the C908V big core, enable both virtual CPUs:

.. code-block:: bash

   $ qemu-system-riscv64 -machine k230-canmv,boot-both-cores=on -smp 2 \
      -bios "$SDK/little/uboot/u-boot" \
      -drive if=sd,file="$SDIMG",format=raw \
      -nographic

Additional UARTs can be connected with extra ``-serial`` options. UART0 is used
by the U-Boot and Linux console. The C908V RT-Smart console uses UART3:

.. code-block:: bash

   $ qemu-system-riscv64 -machine k230-canmv,boot-both-cores=on -smp 2 \
      -bios "$SDK/little/uboot/u-boot" \
      -drive if=sd,file="$SDIMG",format=raw \
      -serial mon:stdio -serial null -serial null -serial pty -serial null \
      -display none
