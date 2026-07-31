.. SPDX-License-Identifier: GPL-2.0-or-later

.. _STC32-System-emulator:

STC32 system emulator
---------------------

QEMU provides an initial model of the STC32G family, based on the
MCS-251-compatible CPU in the STC32G144K246.  Build it with::

  ../configure --target-list=stc32-softmmu
  ninja

The resulting executable is ``qemu-system-stc32``.  The only machine and CPU
models currently provided are:

``stc32g144k246-evb``
  A minimal evaluation machine containing one STC32G144K246 SoC.

``stc32g-cpu``
  The STC32G MCS-251-compatible CPU.

Booting a raw firmware image
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The machine loads a raw ``-bios`` image at the beginning of the 246 KiB user
flash region, address ``0xfc2800``.  The CPU reset vector is ``0xff0000``, so
the reset-vector bytes occur at file offset ``0x2d800``.  A complete image is
therefore normally padded to preserve that address relationship and must not
exceed 246 KiB.

For example::

  qemu-system-stc32 -M stc32g144k246-evb \
      -bios firmware.bin -nographic

CPU and instruction set
~~~~~~~~~~~~~~~~~~~~~~~

The CPU implements the documented, non-DSP STC32/MCS-251 instruction set:

* the classic MCS-51-compatible Binary opcode map;
* the native MCS-251 Source opcode map and 8-, 16-, and 32-bit operations;
* 24-bit code and data addresses, four register banks, extended stack and
  dual data pointers;
* the ``ESC`` prefix, which selects the opposite opcode map for the following
  instruction (successive prefixes alternate the selected map);
* four interrupt priority levels, nesting, and the STC32 four-byte interrupt
  frame.

Source mode is the reset default.  The model interprets
``AUXR2.CPUMODE=1`` as Binary mode.  This polarity is an implementation
inference because the vendor documentation states the reset mode but does not
explicitly publish the bit polarity.

``DPS`` selects either 24-bit data pointer and implements automatic
increment/decrement and selection toggling for the documented instruction
forms.  The ``TA=0xaa``, ``TA=0x55`` consecutive-write sequence unlocks
independent ``DPS.AU0`` and ``DPS.AU1`` programming.

Memory map
~~~~~~~~~~

The initial SoC exposes the following memory:

.. list-table::
   :header-rows: 1

   * - Address range
     - Size
     - Function
   * - ``0x000000`` - ``0x003fff``
     - 16 KiB
     - Internal extended data RAM (``edata``), including register banks and
       stack storage
   * - ``0x010000`` - ``0x02ffff``
     - 128 KiB
     - Internal extended RAM (``xdata``)
   * - ``0x030000`` - ``0x030fff``
     - 4 KiB
     - Executable-RAM data alias when ``CKCON.RAMEXE=0``
   * - ``0x7e0000`` - ``0x7effff``
     - 64 KiB
     - XFR aperture when ``P_SW2.EAXFR=1``; Timer 0/1 prescalers are at
       ``0x7efea0`` and ``0x7efea1``
   * - ``0x7f0000`` - ``0x7fffff``
     - 64 KiB
     - External-data aperture when ``AUXR.EXTRAM=1``; no external device is
       attached by this machine
   * - ``0x800000`` - ``0x800fff``
     - 4 KiB
     - Executable-RAM code alias when ``CKCON.RAMEXE=1``
   * - ``0xfc2800`` - ``0xffffff``
     - 246 KiB
     - Read-only user flash

The two executable-RAM ranges alias one backing store.  Accesses through a
disabled aperture read as zero and ignore writes.  Direct addresses
``0x80`` - ``0xff`` select SFRs, while indirect ``@R0``/``@R1`` accesses in
that range select ``edata`` as on the hardware.

Modeled peripherals
~~~~~~~~~~~~~~~~~~~

Timer 0 and Timer 1
  Modes 0, 1, and 2, Timer 0 mode 3, internal virtual-clock operation,
  external falling-edge counting on P3.4/P3.5, P3.2/P3.3 gate inputs,
  ``x1``/``x12`` clock selection, ``TM0PS``/``TM1PS`` prescaling, overflow
  flags, and interrupts are implemented.  The default input clock is 24 MHz.
  Once armed through ``IE.ET0``, Timer 0 mode 3 behaves as the documented
  highest-priority non-maskable source.

UART1
  ``SCON`` and the separate receive/transmit sides of ``SBUF`` are connected
  to the first QEMU serial chardev.  Receive enable, ``RI``, ``TI``, and the
  UART interrupt are implemented.  Transmission completes immediately at the
  byte-oriented chardev boundary.

GPIO
  P0 through P7 data latches and ``PnM1``/``PnM0`` mode registers are
  implemented, including pin reads versus read-modify-write latch reads.
  P3.2 and P3.3 feed INT0 and INT1; P3.4 and P3.5 feed the external Timer 0
  and Timer 1 counter inputs.

Interrupt vectors
~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1

   * - Source
     - Vector
   * - INT0
     - ``0xff0003``
   * - Timer 0
     - ``0xff000b``
   * - INT1
     - ``0xff0013``
   * - Timer 1
     - ``0xff001b``
   * - UART1
     - ``0xff0023``

Limitations
~~~~~~~~~~~

This is a functional, non-cycle-exact model.  Instruction execution currently
uses a helper-backed TCG frontend with one architectural instruction per
translation block, and the disassembler recognizes only a small diagnostic
subset.  Cache and pipeline timing, the documented one-instruction interrupt
deferral, UART bit timing and ninth-bit transport, clock outputs, power modes,
and electrical GPIO properties are not modeled.

DSP32, TFPU, flash erase/program operations, DMA, USB, CAN-FD, ADC/DAC, PWM,
I2S, additional timers/UARTs, and other STC32G peripherals are outside this
initial machine.  Unimplemented and reserved registers read as zero and ignore
writes.  GDB register XML is supplied, but stock GDB builds may not recognize
the MCS-251 architecture name.
