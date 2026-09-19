.. SPDX-License-Identifier: GPL-2.0-or-later

Phytium Pomelo Board (``phytium-pomelo``)
==========================================

The ``phytium-pomelo`` machine models the D2000 Pomelo board described by
upstream U-Boot. ``phytium-d2000`` is an alias for the same machine.

The reference U-Boot tree is pinned to commit
``211de43d0f954a00a490220c1aac9db298287c40``.

The model follows the upstream U-Boot ``pomelo_defconfig`` and
``phytium-pomelo.dts`` contract. It currently provides:

* eight FTC663-compatible AArch64 CPU slots in four two-core clusters;
* GICv3 with ITS and the D2000 distributor/redistributor address layout;
* the PL011 UART at ``0x28001000``;
* a generic PCIe ECAM host at ``0x40000000`` with the Pomelo PCIe windows;
* two DWMAC4-compatible Ethernet controllers at the D2000 GMAC addresses;
* low-peripheral address-map placeholders and split low/high RAM windows.

Direct Linux boot
-----------------

The machine can generate a minimal D2000 device tree when ``-dtb`` is not
provided:

.. code-block:: console

   $ qemu-system-aarch64 \
       -machine phytium-pomelo \
       -smp 8 -m 3G \
       -kernel Image \
       -initrd initrd \
       -append 'console=ttyAMA0,115200 root=/dev/vda2 rw' \
       -drive if=none,id=root,file=openeuler.img,format=raw \
       -device virtio-blk-pci,drive=root \
       -display none

An upstream Pomelo U-Boot binary may be supplied with ``-bios``. This loads
the binary at its upstream ``CONFIG_TEXT_BASE`` (``0x180000``), and provides
the minimal D2000 SMCCC initialization services used by that U-Boot port.
The full Phytium PBF/BL31 firmware chain is a separate follow-up module; the
machine does not interpret the private vendor PBF image format.

The D2000 SDCI and vendor PBF adapters are intentionally not represented by
the E2000 models. They will be added only when their upstream U-Boot/Linux
interface is available as a stable test contract.
