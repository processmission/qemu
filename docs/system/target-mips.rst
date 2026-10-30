.. _MIPS-System-emulator:

MIPS System emulator
--------------------

Four executables cover simulation of 32 and 64-bit MIPS systems in both
endian options, ``qemu-system-mips``, ``qemu-system-mipsel``
``qemu-system-mips64`` and ``qemu-system-mips64el``. Five different
machine types are emulated:

-  The MIPS Malta prototype board \"malta\"

-  An ACER Pica \"pica61\". This machine needs the 64-bit emulator.

-  A MIPS Magnum R4000 machine \"magnum\". This machine needs the
   64-bit emulator.

-  A Sony PlayStation 2 \"ps2\". This machine needs the 64-bit little
   endian emulator, ``qemu-system-mips64el``.

The Malta emulation supports the following devices:

-  Core board with MIPS 24Kf CPU and Galileo system controller

-  PIIX4 PCI/USB/SMbus controller

-  The Multi-I/O chip's serial device

-  PCI network cards (PCnet32 and others)

-  Malta FPGA serial device

-  Cirrus (default) or any other PCI VGA graphics card

The Boston board emulation supports the following devices:

-  Xilinx FPGA, which includes a PCIe root port and an UART

-  Intel EG20T PCH connects the I/O peripherals, but only the SATA bus
   is emulated

The ACER Pica emulation supports:

-  MIPS R4000 CPU

-  PC-style IRQ and DMA controllers

-  PC Keyboard

-  IDE controller

The MIPS Magnum R4000 emulation supports:

-  MIPS R4000 CPU

-  PC-style IRQ controller

-  PC Keyboard

-  SCSI controller

-  G364 framebuffer

The Fuloong 2E emulation supports:

-  Loongson 2E CPU

-  Bonito64 system controller as North Bridge

-  VT82C686 chipset as South Bridge

-  RTL8139D as a network card chipset

The Loongson-3 virtual platform emulation supports:

-  Loongson 3A CPU

-  LIOINTC as interrupt controller

-  GPEX and virtio as peripheral devices

Sony PlayStation 2 (``ps2``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``ps2`` machine models the PlayStation 2 as far as the Linux kernel
for it needs, such as the one built by the meta-playstation2 layer
(``linux-ps2``, ``ps2_defconfig``), with its OpenGL library and the
games that draw through it.

There is no BIOS.  QEMU builds a ROM with a ROM directory, a ROMVER
file and empty IOP modules, whose reset code jumps to the ELF kernel
given with ``-kernel``.  The I/O processor (IOP) does not run code
either: the SIF answers the remote procedure calls that the kernel makes
to the IOP modules, as PCSX2 does when it emulates the IOP BIOS.

The PlayStation 2 emulation supports:

-  Emotion Engine with an R5900 CPU and 32 MiB of RAM

-  Interrupt and DMA controllers, timers and the serial port (``ttyS0``)

-  Vector unit 1 and its interface VIF1, which load and run VU1
   microprograms that pass their GIF packets to the GS (``XGKICK``)

-  Graphics Synthesizer and GIF, with a graphic console showing the
   frame buffer.  Drawing covers points, lines, triangles and sprites,
   with textures, colour lookup tables, fog, alpha and depth tests and
   alpha blending, and the transfers between host and local memory in
   both directions.  There is no antialiasing, mipmapping or bilinear
   filtering.

-  CDVD S-commands for the real time clock and the console model

-  SIF with the RPC servers of the IOP modules that Linux loads its own
   IOP modules through

-  USB OHCI host controller of the IOP, with two ports

-  Expansion bay with the SPEED chip and an ATA hard disk, given with
   ``-drive if=ide``

The kernel must be a 32-bit little endian ELF file.  As there is no way to
pass a command line or an initial RAM disk to it, build them into the
kernel with ``CONFIG_CMDLINE`` and ``CONFIG_INITRAMFS_SOURCE``.  For both
the frame buffer and the serial port as consoles, with the shell on the
serial port, use ``console=tty0 console=ttyS0``.

Example::

   qemu-system-mips64el -M ps2 -kernel vmlinux -serial stdio \
       -drive file=hdd.img,if=ide,format=raw \
       -device usb-kbd -device usb-storage,drive=usbd \
       -drive file=usb.img,if=none,id=usbd,format=raw

.. include:: cpu-models-mips.rst.inc

.. _nanoMIPS-System-emulator:

nanoMIPS System emulator
~~~~~~~~~~~~~~~~~~~~~~~~

Executable ``qemu-system-mipsel`` also covers simulation of 32-bit
nanoMIPS system in little endian mode:

-  nanoMIPS I7200 CPU

Example of ``qemu-system-mipsel`` usage for nanoMIPS is shown below:

Download ``<disk_image_file>`` from
https://mipsdistros.mips.com/LinuxDistro/nanomips/buildroot/index.html.

Download ``<kernel_image_file>`` from
https://mipsdistros.mips.com/LinuxDistro/nanomips/kernels/v4.15.18-432-gb2eb9a8b07a1-20180627102142/index.html.

Start system emulation of Malta board with nanoMIPS I7200 CPU::

   qemu-system-mipsel -cpu I7200 -kernel <kernel_image_file> \
       -M malta -serial stdio -m <memory_size> -drive file=<disk_image_file>,format=raw \
       -append "mem=256m@0x0 rw console=ttyS0 vga=cirrus vesa=0x111 root=/dev/sda"
