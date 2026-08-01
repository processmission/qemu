.. _MCS51-System-emulator:
.. _MCS251-System-emulator:

MCS-51 family system emulators
==============================

QEMU provides separate system targets for classic MCS-51 CPUs and their
MCS-251 extensions.  They share the MCS-51 family implementation, while the
separate executables retain each architecture's address width and instruction
mode at build time.  Build both with::

  ./configure --target-list=mcs51-softmmu,mcs251-softmmu
  ninja

The resulting executables and models are:

``qemu-system-mcs51``
  Provides the ``mcs51-cpu`` CPU and the ``stc8g1k08a`` machine.

``qemu-system-mcs251``
  Provides the ``mcs251-cpu`` CPU and the ``stc32g144k246`` machine.

Running firmware
----------------

Both machines load firmware through ``-bios``.  Use ``-nographic`` when no
graphical display is needed::

  qemu-system-mcs51 -M stc8g1k08a -bios firmware.hex -nographic
  qemu-system-mcs251 -M stc32g144k246 -bios firmware.hex -nographic

Interrupts and exceptions
-------------------------

The classic MCS-51 architecture responds to reset and vectored hardware
interrupts.  MCS-251 adds programmable four-level interrupt priorities and
nesting.  The STC32 Timer 0 mode-3 interrupt is modeled as a level above the
four programmable levels.  ``RETI`` restores the saved PC and interrupt
priority state; the MCS-251 model also restores the saved ``PSW1``.

These CPUs do not define general synchronous privilege, alignment, or illegal
instruction exceptions.  Reserved classic encodings and the MCS-251 ``TRAP``
instruction execute as documented NOPs.  TFPU arithmetic exceptions are
reported in the TFPU status register and do not vector through the CPU.

STC8G1K08A machine
------------------

The ``stc8g1k08a`` machine contains one classic MCS-51 CPU and an
STC8G1K08A MCU with 8 KiB of user Flash, 4 KiB of EEPROM, 256 bytes of
internal data RAM, and 1 KiB of extended data RAM.  The EEPROM is visible in
the Code space at ``0x2000`` through ``0x2fff``.

Booting firmware
~~~~~~~~~~~~~~~~

The machine accepts both raw and Intel HEX firmware through ``-bios``.  Raw
images are loaded at code address ``0x0000`` and may be up to 8 KiB.  A
case-insensitive ``.hex`` suffix selects the Intel HEX loader; data records
use absolute MCS-51 code addresses and must remain within the 8 KiB user
Flash.  The CPU always resets at ``0x0000``.

For example::

  qemu-system-mcs51 -M stc8g1k08a \
      -bios firmware.hex -nographic

CPU and memory
~~~~~~~~~~~~~~

The ``mcs51-cpu`` implements all 256 classic opcode encodings, including all
addressing forms, bit operations, calls, returns, and interrupts.  Opcode
``0xa5`` is the architecturally reserved one-byte NOP on this CPU.  The
architecture has overlapping Code, IDATA, XDATA, and SFR address spaces:
instruction fetches address 64 KiB of code, direct and indirect accesses
address the 256-byte internal RAM and SFR space, and ``MOVX`` addresses the
1 KiB extended RAM.  The ``MOVX @Ri`` forms use the P2 latch as address bits
15:8.  ``P_SW2.EAXFR`` selects the STC extended-register window for ``MOVX``
accesses at ``0xfa00`` through ``0xffff``.

QEMU address-space representation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The classic architecture allows Code, IDATA, and XDATA addresses to start at
zero while remaining distinct.  QEMU encodes the access type into disjoint
internal physical windows in its system address space.  The CPU's MMU index
and instruction helpers select the appropriate window:

.. list-table::
   :header-rows: 1

   * - Architectural space
     - Guest address
     - QEMU internal address
   * - Code
     - ``0x0000`` - ``0xffff``
     - ``0x00000000`` - ``0x0000ffff``
   * - IDATA
     - ``0x00`` - ``0xff``
     - ``0x00800000`` - ``0x008000ff``
   * - XDATA
     - ``0x0000`` - ``0xffff``
     - ``0x00810000`` - ``0x0081ffff``
   * - XFR
     - ``0xfa00`` - ``0xffff``
     - ``0x00c00000`` - ``0x00c005ff``
   * - SFR
     - ``0x80`` - ``0xff``
     - ``0x01000000`` - ``0x0100007f``

The high addresses reported by the monitor's ``info mtree`` command are this
QEMU-internal encoding, not addresses on an STC8 hardware bus.  Region names
such as ``stc8g.flash``, ``stc8g.idata``, and ``stc8g.xdata`` identify the
architectural space represented by each window.

Modeled peripherals
~~~~~~~~~~~~~~~~~~~

GPIO
  P3.0 through P3.3 and P5.4 through P5.5 implement the data latch, all four
  ``PnM1``/``PnM0`` modes, pull-up, Schmitt-trigger, slew-rate, drive-strength,
  and input-enable registers.  Pin reads and read-modify-write latch reads are
  distinct.  P3.2/P3.3 feed INT0/INT1, while P5.4/P5.5 feed the Timer 0/1
  counter inputs.

Timer 0 and Timer 1
  Modes 0, 1, and 2, Timer 0 mode 3, gate inputs, external falling-edge
  counting, ``x1``/``x12`` clock selection, overflow flags, and interrupts are
  implemented.

UART1
  ``SCON``, ``SBUF``, ``SADDR``, and ``SADEN`` are connected to the first QEMU
  serial chardev.  Receive enable, backpressure, ``RI``, ``TI``, and the UART
  interrupt are implemented.  Transmission completes at the byte-oriented
  chardev boundary.

System clock and interrupts
  The HIRC, external-oscillator, and IRC32K clock selections, divider, trim
  controls, and MCLKO divider are modeled.  The resulting clock drives the
  timing of the modeled Timer 0/1, PCA, ADC, SPI, I2C, and MDU devices.  The
  ``PCON.IDL`` mode stops the CPU while peripherals continue, and
  ``PCON.PD`` gates the system clock until an enabled interrupt wakes the
  CPU.  The extended interrupt controller provides the ADC, LVD, PCA, SPI,
  INT2--INT4, and I2C vectors, priority controls, and ``AUXINTIF`` W1C flags.
  INT2--INT4 requests remain latched until software acknowledges their flag.

ADC and low-voltage detector
  The 10-bit ADC implements six externally supplied input channels, the
  internal reference channel, result alignment, conversion timing, and its
  interrupt.  Conversion progress is retained across system-clock changes.
  ``RSTCFG`` implements the four low-voltage thresholds, ``PCON.LVDF``, LVD
  interrupt mode, and automatic reset mode.  The device ``vdd-millivolts``
  input supplies the modeled voltage.

PCA
  The three-channel PCA implements its internal and external clock sources,
  capture edges, compare and high-speed output modes, 6/7/8/10-bit PWM,
  shadow reload, flags, interrupts, and ``ccp-out`` signals.  Timer 0 overflow
  is connected as the PCA clock source where selected.

SPI and I2C
  SPI implements master transfers over a QEMU SSI bus, slave receive events,
  status flags, and its interrupt.  I2C implements master start, address,
  send, receive, acknowledge, and stop commands over a QEMU I2C bus, together
  with the documented slave-state flags and interrupt.  Both controllers
  retain an in-flight transfer when the system clock changes or stops.

MDU16
  The MDU16 XFR block implements the documented normalization, 16-bit
  multiply/divide, and 32-bit divide operations, including result, remainder,
  overflow, and operation timing.

Watchdog, IAP, and EEPROM
  The watchdog implements its prescaler, clear operation, timeout flag, and
  standard QEMU watchdog action.  It retains its enabled state over warm
  reset, as specified by the hardware.  The IAP controller implements the
  ``0x5a``/``0xa5`` trigger sequence, timing and validation, EEPROM read,
  program, and 512-byte erase commands.  EEPROM starts erased (``0xff``) and
  persists across warm resets.

STC8G limitations
~~~~~~~~~~~~~~~~~

The model is functional rather than cycle exact.  Pin multiplexing between the
modeled controllers and GPIO pins, MCLKO pin routing, UART bit timing,
ninth-bit transport and multiprocessor address filtering, analog GPIO and ADC
effects, power-down wake-source filtering, and unmodeled STC8G peripherals
are not yet implemented.  (The modeled 8-pin STC8G1K08A does not provide a
comparator.)  IAP does not model the factory ISP ROM selected by ``SWBS``.
Unimplemented and reserved registers read as zero and ignore writes.

STC32G144K246 machine
---------------------

Booting an Intel HEX firmware image
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The STC toolchain normally produces an Intel HEX-80 file for programming the
MCU user flash.  The machine accepts a case-insensitive ``.hex`` file through
``-bios`` and interprets its Intel HEX addresses as absolute MCS-251
addresses.  Extended segment and extended linear address records are
supported, but data records outside ``0xfc2800`` through ``0xffffff`` are
rejected.  The hardware reset vector remains ``0xff0000`` regardless of an
optional start-address record.

For example::

  qemu-system-mcs251 -M stc32g144k246 \
      -bios firmware.hex -nographic

Booting a raw flash image
~~~~~~~~~~~~~~~~~~~~~~~~~

The machine loads a raw ``-bios`` image at the beginning of the 246 KiB user
flash region, address ``0xfc2800``.  The CPU reset vector is ``0xff0000``, so
the reset-vector bytes occur at file offset ``0x2d800``.  A complete image is
therefore normally padded to preserve that address relationship and must not
exceed 246 KiB.

For example::

  qemu-system-mcs251 -M stc32g144k246 \
      -bios firmware.bin -nographic

CPU and instruction set
~~~~~~~~~~~~~~~~~~~~~~~

The CPU implements the complete documented MCS-251 instruction set and the
STC32 instruction-map behavior:

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
       ``0x7efea0`` and ``0x7efea1``, and ``TFPU_CLKDIV`` is at
       ``0x7efe93``
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

DSP32
  The complete documented DSP32 command set is implemented through
  ``DPUOP`` at SFR ``0xd8``.  It includes configuration, binary/BCD
  conversion, normalization, register exchange, arithmetic, multiplication,
  division, unary, logical, shift, compound multiply/divide, interpolation,
  and multiply-accumulate commands.  ``DPUST`` at SFR ``0x86`` exposes the
  zero, divide-by-zero, and shift-count results.

TFPU
  All documented TFPU commands are implemented.  A command starts only when
  the CPU executes ``MOV DMAIR,#immediate`` at SFR ``0xed``; other writes
  update ``DMAIR`` without starting an operation.  Basic IEEE-754 binary32
  arithmetic and integer conversion use QEMU softfloat.  Classification,
  comparison, status/control access, trigonometric commands, and system/PLL
  clock selection are also modeled.

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

This is a functional, non-cycle-exact model.  Instruction execution uses a
helper-backed TCG frontend with one architectural instruction per translation
block.  Decodetree and the disassembler cover the complete Binary and Source
opcode maps, including the Source-mode ``ESC`` prefix.  Cache and pipeline
timing, the documented one-instruction interrupt deferral, UART bit timing and
ninth-bit transport, clock outputs, power modes, and electrical GPIO
properties are not modeled.

Flash erase/program operations, DMA, USB, CAN-FD, ADC/DAC, PWM, I2S,
additional timers/UARTs, and other STC32G peripherals are outside this initial
machine.  DSP32 and TFPU commands complete synchronously and do not model
documented command latency.  The TFPU status exception bits and
control-register rounding fields follow the IEEE-754 layout inferred from the
vendor examples because their bit assignments are not published.
Trigonometric commands use the host single-precision math library.
Unimplemented and reserved registers read as zero and ignore writes.  GDB
register XML is supplied, but stock GDB builds may not recognize the MCS-251
architecture name.
