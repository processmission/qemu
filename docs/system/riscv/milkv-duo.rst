Milk-V Duo board (``milkv-duo``)
================================

The ``milkv-duo`` machine models the Milk-V Duo board with a Sophgo CV1800B
SoC. It is intended for early firmware, kernel, and device-model development
for the CV1800B platform.

Supported devices
-----------------

The ``milkv-duo`` machine supports the following devices:

* Up to 2 T-Head C906 RISC-V harts
* Core Local Interruptor (ACLINT)
* Platform-Level Interrupt Controller (PLIC)
* DesignWare 8250-compatible UART0
* SDHCI-compatible SD0 controller
* CV1800B clock controller

Several CV1800B MMIO regions are present as unimplemented devices so firmware
or guest software can probe the expected address map while the corresponding
device models are still incomplete.

Boot options
------------

The machine supports direct payload loading with ``-kernel`` and firmware
entry with ``-bios``. The model does not generate a device tree, so direct
Linux boot requires an external CV1800B or Milk-V Duo DTB passed with
``-dtb``.

Direct Linux boot
~~~~~~~~~~~~~~~~~

Use ``-kernel`` to load a kernel image, ``-dtb`` to provide the board device
tree, and optionally ``-initrd`` and ``-append`` for the root filesystem and
kernel command line:

.. code-block:: bash

   $ qemu-system-riscv64 -machine milkv-duo \
      -smp 1 -m 64M \
      -kernel /path/to/Image \
      -dtb /path/to/milkv-duo.dtb \
      -initrd /path/to/initramfs.cpio.gz \
      -append "console=ttyS0" \
      -serial mon:stdio \
      -display none

Firmware boot
~~~~~~~~~~~~~

Use ``-bios`` to enter a CV1800B firmware image. The firmware image is loaded
at the start of DRAM and entered from the modelled boot ROM reset vector:

.. code-block:: bash

   $ qemu-system-riscv64 -machine milkv-duo \
      -smp 1 -m 64M \
      -bios /path/to/firmware.elf \
      -serial mon:stdio \
      -display none

Known limitations
-----------------

The current model focuses on the CPU, interrupt controller, UART, SDHCI
controller, and clock-controller paths needed by early software bring-up.
Other CV1800B peripheral blocks are represented by unimplemented MMIO regions.

Running tests
-------------

The Milk-V Duo qtest covers the UART component ID registers and basic
read/write behavior of the CV1800B clock controller:

.. code-block:: bash

   $ QTEST_QEMU_BINARY=./build/qemu-system-riscv64 \
      ./build/tests/qtest/milkv-duo-test --tap -k
