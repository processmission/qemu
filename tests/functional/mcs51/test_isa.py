#!/usr/bin/env python3
#
# MCS-51 ISA integration tests
#
# Copyright (c) 2026 Process Mission
#
# SPDX-License-Identifier: GPL-2.0-or-later
#

from pathlib import Path

from qemu_test import QemuSystemTest, wait_for_console_pattern


class Program:
    def __init__(self):
        self.code = bytearray()
        self.labels = {}
        self.relative_patches = []
        self.address_patches = []
        self.absolute11_patches = []

    def emit(self, *values):
        self.code.extend(values)

    def label(self, name):
        self.labels[name] = len(self.code)

    def branch(self, opcode, label):
        self.emit(opcode, 0)
        self.relative_patches.append((len(self.code) - 1, label))

    def bit_branch(self, opcode, bit, label):
        self.emit(opcode, bit, 0)
        self.relative_patches.append((len(self.code) - 1, label))

    def lcall(self, label):
        self.emit(0x12, 0, 0)
        self.address_patches.append((len(self.code) - 2, label))

    def ljmp(self, label):
        self.emit(0x02, 0, 0)
        self.address_patches.append((len(self.code) - 2, label))

    def acall(self, label):
        self.emit(0x11, 0)
        self.absolute11_patches.append((len(self.code) - 2, label))

    def ajmp(self, label):
        self.emit(0x01, 0)
        self.absolute11_patches.append((len(self.code) - 2, label))

    def mov_dptr(self, label):
        self.emit(0x90, 0, 0)
        self.address_patches.append((len(self.code) - 2, label))

    def cjne(self, opcode, value, label):
        self.emit(opcode, value, 0)
        self.relative_patches.append((len(self.code) - 1, label))

    def djnz_direct(self, direct, label):
        self.emit(0xd5, direct, 0)
        self.relative_patches.append((len(self.code) - 1, label))

    def finish(self, base=0):
        for offset, label in self.relative_patches:
            displacement = self.labels[label] - (offset + 1)
            if not -128 <= displacement <= 127:
                raise ValueError(f'relative branch to {label} is too far')
            self.code[offset] = displacement & 0xff
        for offset, label in self.address_patches:
            address = base + self.labels[label]
            self.code[offset:offset + 2] = address.to_bytes(2, 'big')
        for offset, label in self.absolute11_patches:
            address = base + self.labels[label]
            next_address = base + offset + 2
            if address & ~0x7ff != next_address & ~0x7ff:
                raise ValueError(f'absolute jump to {label} crosses 2 KiB')
            self.code[offset] |= address >> 3 & 0xe0
            self.code[offset + 1] = address & 0xff
        return self.code


class MCS51ISATest(QemuSystemTest):
    FLASH_SIZE = 8 * 1024

    @staticmethod
    def assert_acc(program, value):
        program.emit(0xb4, value, 2)
        program.emit(0x80, 3)
        program.ljmp('fail')

    def assert_direct(self, program, direct, value):
        program.emit(0xe5, direct)
        self.assert_acc(program, value)

    @staticmethod
    def assert_branch(program, opcode, taken, name):
        target = f'{name}_target'
        done = f'{name}_done'

        program.branch(opcode, target)
        if taken:
            program.ljmp('fail')
            program.label(target)
        else:
            program.branch(0x80, done)
            program.label(target)
            program.ljmp('fail')
            program.label(done)

    @staticmethod
    def assert_bit_branch(program, opcode, bit, taken, name):
        target = f'{name}_target'
        done = f'{name}_done'

        program.bit_branch(opcode, bit, target)
        if taken:
            program.ljmp('fail')
            program.label(target)
        else:
            program.branch(0x80, done)
            program.label(target)
            program.ljmp('fail')
            program.label(done)

    @staticmethod
    def emit_text(program, text):
        for value in text:
            program.emit(0x75, 0x99, value)

    @staticmethod
    def snapshot_psw(program):
        program.emit(0x85, 0xd0, 0x30)

    def build_isa_firmware(self):
        p = Program()

        p.emit(0x00, 0xa5)
        p.emit(0x75, 0xd0, 0x00)
        p.emit(0x74, 0x7f, 0x24, 0x01)
        self.snapshot_psw(p)
        self.assert_acc(p, 0x80)
        self.assert_direct(p, 0x30, 0x45)

        p.emit(0x75, 0xd0, 0x00, 0xd3)
        p.emit(0x74, 0xff, 0x34, 0x00)
        self.snapshot_psw(p)
        self.assert_acc(p, 0x00)
        self.assert_direct(p, 0x30, 0xc0)

        p.emit(0x75, 0xd0, 0x00, 0xc3)
        p.emit(0x74, 0x00, 0x94, 0x01)
        self.snapshot_psw(p)
        self.assert_acc(p, 0xff)
        self.assert_direct(p, 0x30, 0xc0)

        p.emit(0x74, 0x10, 0x75, 0xf0, 0x10, 0xa4)
        self.snapshot_psw(p)
        self.assert_acc(p, 0x00)
        self.assert_direct(p, 0xf0, 0x01)
        p.emit(0xe5, 0x30, 0x54, 0x84)
        self.assert_acc(p, 0x04)

        p.emit(0x74, 0xfb, 0x75, 0xf0, 0x12, 0x84)
        self.assert_acc(p, 0x0d)
        self.assert_direct(p, 0xf0, 0x11)
        p.emit(0x74, 0x55, 0x75, 0xf0, 0x00, 0x84)
        self.snapshot_psw(p)
        self.assert_acc(p, 0x55)
        p.emit(0xe5, 0x30, 0x54, 0x04)
        self.assert_acc(p, 0x04)

        p.emit(0x75, 0xd0, 0x00, 0x74, 0x9a, 0xd4)
        self.snapshot_psw(p)
        self.assert_acc(p, 0x00)
        p.emit(0xe5, 0x30, 0x54, 0x80)
        self.assert_acc(p, 0x80)

        # ADD, ADDC, and SUBB cover immediate, direct, indirect, and Rn.
        p.emit(0x75, 0x20, 0x02, 0x78, 0x21, 0x76, 0x03)
        p.emit(0x79, 0x22, 0x77, 0x04, 0x7a, 0x05)
        for insn, expected in (((0x25, 0x20), 0x03), ((0x26,), 0x04),
                               ((0x27,), 0x05), ((0x2a,), 0x06)):
            p.emit(0x74, 0x01, *insn)
            self.assert_acc(p, expected)
        for insn, expected in (((0x35, 0x20), 0x04), ((0x36,), 0x05),
                               ((0x37,), 0x06), ((0x3a,), 0x07)):
            p.emit(0xd3, 0x74, 0x01, *insn)
            self.assert_acc(p, expected)
        for insn, expected in (((0x95, 0x20), 0x08), ((0x96,), 0x07),
                               ((0x97,), 0x06), ((0x9a,), 0x05)):
            p.emit(0xc3, 0x74, 0x0a, *insn)
            self.assert_acc(p, expected)

        p.emit(0x78, 0x7f, 0x08, 0xe8)
        self.assert_acc(p, 0x80)
        p.emit(0x18, 0xe8)
        self.assert_acc(p, 0x7f)
        p.emit(0x78, 0x22, 0x76, 0xfe, 0x06, 0x16, 0xe6)
        self.assert_acc(p, 0xfe)
        p.emit(0x75, 0x23, 0xff, 0x05, 0x23)
        self.assert_direct(p, 0x23, 0x00)
        p.emit(0x15, 0x23)
        self.assert_direct(p, 0x23, 0xff)

        # Logical forms for A and direct destinations.
        p.emit(0x75, 0x20, 0x0f, 0x78, 0x21, 0x76, 0x33,
               0x7b, 0x55)
        for insn, expected in (((0x45, 0x20), 0x5f), ((0x46,), 0x73),
                               ((0x4b,), 0x55)):
            p.emit(0x74, 0x50, *insn)
            self.assert_acc(p, expected)
        for insn, expected in (((0x55, 0x20), 0x00), ((0x56,), 0x10),
                               ((0x5b,), 0x50)):
            p.emit(0x74, 0x50, *insn)
            self.assert_acc(p, expected)
        for insn, expected in (((0x65, 0x20), 0x5f), ((0x66,), 0x63),
                               ((0x6b,), 0x05)):
            p.emit(0x74, 0x50, *insn)
            self.assert_acc(p, expected)
        p.emit(0x74, 0x55, 0x44, 0x0a, 0x54, 0xf0, 0x64, 0xff)
        self.assert_acc(p, 0xaf)
        p.emit(0x75, 0x24, 0x0f, 0x42, 0x24,
               0x53, 0x24, 0xf0, 0x62, 0x24)
        self.assert_direct(p, 0x24, 0x0f)
        p.emit(0x43, 0x24, 0xf0, 0x53, 0x24, 0x3c,
               0x63, 0x24, 0x0f)
        self.assert_direct(p, 0x24, 0x33)

        p.emit(0x74, 0x81, 0x03)
        self.assert_acc(p, 0xc0)
        p.emit(0x74, 0x81, 0x23)
        self.assert_acc(p, 0x03)
        p.emit(0xc3, 0x74, 0x81, 0x13)
        self.snapshot_psw(p)
        self.assert_acc(p, 0x40)
        p.emit(0xe5, 0x30, 0x54, 0x80)
        self.assert_acc(p, 0x80)
        p.emit(0xd3, 0x74, 0x81, 0x33)
        self.assert_acc(p, 0x03)
        p.emit(0x74, 0xab, 0xc4)
        self.assert_acc(p, 0xba)
        p.emit(0xe4)
        self.assert_acc(p, 0x00)
        p.emit(0xf4)
        self.assert_acc(p, 0xff)

        # GPIO read-modify-write uses the latch, not the sampled pin.
        p.emit(0x75, 0xb1, 0x0f, 0x75, 0xb2, 0x00,
               0x75, 0xb0, 0x0a, 0x05, 0xb0)
        self.assert_direct(p, 0xb0, 0xff)
        p.emit(0x75, 0xb1, 0x00, 0x75, 0xb2, 0x0f)
        self.assert_direct(p, 0xb0, 0xfb)

        # MOV covers every addressing class.
        p.emit(0x7a, 0x5a, 0xea)
        self.assert_acc(p, 0x5a)
        p.emit(0x8a, 0x25, 0xab, 0x25, 0xeb)
        self.assert_acc(p, 0x5a)
        p.emit(0x78, 0x26, 0x76, 0xa5, 0xe6)
        self.assert_acc(p, 0xa5)
        p.emit(0x86, 0x27, 0xa7, 0x27, 0xe7)
        self.assert_acc(p, 0xa5)
        p.emit(0x75, 0x28, 0x3c, 0xa6, 0x28, 0xe6)
        self.assert_acc(p, 0x3c)
        p.emit(0x85, 0x28, 0x29)
        self.assert_direct(p, 0x29, 0x3c)

        # XCH and XCHD cover direct, indirect, and register forms.
        p.emit(0x75, 0x27, 0x12, 0x74, 0x34, 0xc5, 0x27)
        self.assert_acc(p, 0x12)
        self.assert_direct(p, 0x27, 0x34)
        p.emit(0x78, 0x26, 0x76, 0x56, 0x74, 0x78, 0xc6)
        self.assert_acc(p, 0x56)
        p.emit(0x7c, 0x9a, 0xcc)
        self.assert_acc(p, 0x9a)
        p.emit(0x74, 0xa5, 0x75, 0x26, 0x3c, 0xd6)
        self.assert_acc(p, 0xac)
        self.assert_direct(p, 0x26, 0x35)

        p.emit(0x75, 0x28, 0x77, 0xc0, 0x28,
               0x75, 0x28, 0x00, 0xd0, 0x29)
        self.assert_direct(p, 0x29, 0x77)
        self.assert_direct(p, 0x81, 0x07)

        # MOVX through DPTR and both Ri encodings.
        p.emit(0x90, 0x00, 0x40, 0x74, 0x66, 0xf0, 0xe4, 0xe0)
        self.assert_acc(p, 0x66)
        p.emit(0x75, 0xa0, 0x00,
               0x78, 0x41, 0x74, 0x99, 0xf2, 0xe4, 0xe2)
        self.assert_acc(p, 0x99)
        p.emit(0x79, 0x42, 0x74, 0x5a, 0xf3, 0xe4, 0xe3)
        self.assert_acc(p, 0x5a)

        # The Ri forms use P2 as the high byte of the XDATA address.
        p.emit(0x78, 0x43,
               0x75, 0xa0, 0x00, 0x74, 0x11, 0xf2,
               0x75, 0xa0, 0x01, 0x74, 0x22, 0xf2,
               0xe4, 0xe2)
        self.assert_acc(p, 0x22)
        p.emit(0x75, 0xa0, 0x00, 0xe4, 0xe2)
        self.assert_acc(p, 0x11)

        # EXTRAM disconnects internal XDATA without modifying it.
        p.emit(0x90, 0x00, 0x40, 0x75, 0x8e, 0x03,
               0x74, 0x12, 0xf0, 0xe0)
        self.assert_acc(p, 0x00)
        p.emit(0x75, 0x8e, 0x01, 0xe0)
        self.assert_acc(p, 0x66)

        # EAXFR exposes the STC extension register window to MOVX.
        p.emit(0x75, 0xba, 0x80, 0x90, 0xfe, 0x13,
               0x74, 0x05, 0xf0, 0xe4, 0xe0)
        self.assert_acc(p, 0x05)
        p.emit(0x75, 0xba, 0x00)

        # MOVC uses both PC-relative and DPTR-relative code addressing.
        p.mov_dptr('movc_dptr_data')
        p.emit(0x74, 0x00, 0x93)
        p.ajmp('movc_dptr_done')
        p.label('movc_dptr_data')
        p.emit(0x6e)
        p.label('movc_dptr_done')
        self.assert_acc(p, 0x6e)
        p.emit(0x74, 0x03, 0x83)
        p.ljmp('movc_pc_done')
        p.emit(0x7d)
        p.label('movc_pc_done')
        self.assert_acc(p, 0x7d)

        # Bit operations and all conditional branch mnemonics.
        p.emit(0x75, 0x20, 0x00, 0xd2, 0x00)
        self.assert_bit_branch(p, 0x20, 0x00, True, 'jb_set')
        self.assert_bit_branch(p, 0x30, 0x00, False, 'jnb_set')
        p.emit(0xa2, 0x00)
        self.assert_branch(p, 0x40, True, 'jc_set')
        self.assert_branch(p, 0x50, False, 'jnc_set')
        p.emit(0x92, 0x01, 0xb2, 0x00)
        self.assert_bit_branch(p, 0x20, 0x00, False, 'jb_clear')
        self.assert_bit_branch(p, 0x30, 0x00, True, 'jnb_clear')
        p.emit(0xd2, 0x02)
        self.assert_bit_branch(p, 0x10, 0x02, True, 'jbc_set')
        self.assert_bit_branch(p, 0x10, 0x02, False, 'jbc_clear')

        p.emit(0xc3, 0xa0, 0x00)
        self.assert_branch(p, 0x40, True, 'orl_not_bit')
        p.emit(0xd2, 0x00, 0xb0, 0x00)
        self.assert_branch(p, 0x50, True, 'anl_not_bit')
        p.emit(0xc2, 0x00, 0xd3, 0x72, 0x00)
        self.assert_branch(p, 0x40, True, 'orl_bit')
        p.emit(0xc3, 0xd2, 0x00, 0x72, 0x00)
        self.assert_branch(p, 0x40, True, 'orl_set_bit')
        p.emit(0xd2, 0x00, 0x82, 0x00)
        self.assert_branch(p, 0x40, True, 'anl_bit')
        p.emit(0xb3)
        self.assert_branch(p, 0x50, True, 'cpl_c')
        p.emit(0xc3)

        p.emit(0x74, 0x00)
        self.assert_branch(p, 0x60, True, 'jz_zero')
        self.assert_branch(p, 0x70, False, 'jnz_zero')
        p.emit(0x74, 0x01)
        self.assert_branch(p, 0x60, False, 'jz_nonzero')
        self.assert_branch(p, 0x70, True, 'jnz_nonzero')

        # All CJNE operand classes and both DJNZ classes.
        p.emit(0x74, 0x12)
        p.cjne(0xb4, 0x34, 'cjne_a_imm')
        p.ljmp('fail')
        p.label('cjne_a_imm')
        p.emit(0x75, 0x21, 0x34)
        p.cjne(0xb5, 0x21, 'cjne_a_direct')
        p.ljmp('fail')
        p.label('cjne_a_direct')
        p.emit(0x78, 0x22, 0x76, 0x11)
        p.cjne(0xb6, 0x22, 'cjne_indirect')
        p.ljmp('fail')
        p.label('cjne_indirect')
        p.emit(0x7a, 0x01)
        p.cjne(0xba, 0x02, 'cjne_register')
        p.ljmp('fail')
        p.label('cjne_register')
        self.snapshot_psw(p)
        p.emit(0xe5, 0x30, 0x54, 0x80)
        self.assert_acc(p, 0x80)

        p.emit(0x7a, 0x02)
        p.label('djnz_r')
        p.branch(0xda, 'djnz_r')
        p.emit(0xea)
        self.assert_acc(p, 0x00)
        p.emit(0x75, 0x21, 0x02)
        p.label('djnz_direct')
        p.djnz_direct(0x21, 'djnz_direct')
        self.assert_direct(p, 0x21, 0x00)

        # Register banks and indirect-only upper IDATA remain distinct.
        for bank, value in enumerate((0x11, 0x22, 0x33, 0x44)):
            p.emit(0x75, 0xd0, bank << 3, 0x78, value)
        for bank, value in enumerate((0x11, 0x22, 0x33, 0x44)):
            p.emit(0x75, 0xd0, bank << 3, 0xe8)
            self.assert_acc(p, value)
        p.emit(0x75, 0xd0, 0x00, 0x78, 0x80,
               0x74, 0xa6, 0xf6, 0xe4, 0xe6)
        self.assert_acc(p, 0xa6)

        # The 8-bit stack pointer wraps at 0xff.
        p.emit(0x75, 0x81, 0xfe, 0x75, 0x20, 0x12, 0xc0, 0x20,
               0x75, 0x20, 0x34, 0xc0, 0x20, 0xd0, 0x21, 0xd0, 0x22)
        self.assert_direct(p, 0x21, 0x34)
        self.assert_direct(p, 0x22, 0x12)
        self.assert_direct(p, 0x81, 0xfe)
        p.emit(0x75, 0x81, 0x07)

        # AJMP/SJMP/LJMP, calls, returns, and JMP @A+DPTR.
        p.ajmp('after_ajmp')
        p.ljmp('fail')
        p.label('after_ajmp')
        p.branch(0x80, 'after_sjmp')
        p.ljmp('fail')
        p.label('after_sjmp')
        p.emit(0x7f, 0x00)
        p.acall('subroutine')
        p.lcall('subroutine')
        p.emit(0xef)
        self.assert_acc(p, 0x02)
        p.mov_dptr('indirect_jump')
        p.emit(0x74, 0x00, 0x73)
        p.ljmp('fail')
        p.label('indirect_jump')

        self.emit_text(p, b'MCS51-PASS\n')
        p.branch(0x80, 'done')

        p.label('fail')
        self.emit_text(p, b'MCS51-FAIL\n')
        p.label('done')
        p.branch(0x80, 'done')

        p.label('subroutine')
        p.emit(0x0f, 0x22)

        code = p.finish()
        if len(code) > self.FLASH_SIZE:
            raise ValueError('MCS-51 ISA firmware exceeds Flash')
        image = bytearray(self.FLASH_SIZE)
        image[:len(code)] = code
        path = Path(self.scratch_file('mcs51-isa.bin'))
        path.write_bytes(image)
        return path

    def build_interrupt_firmware(self, mode3=False):
        image = bytearray(self.FLASH_SIZE)

        def write(address, data):
            image[address:address + len(data)] = data

        write(0x0000, bytes((0x02, 0x01, 0x00)))
        write(0x000b, bytes((0x02, 0x02, 0x00)))

        main = Program()
        main.emit(0x75, 0x20, 0x00)
        main.emit(0x75, 0x89, 0x03 if mode3 else 0x01)
        main.emit(0x75, 0x8c, 0xff, 0x75, 0x8a, 0xfe)
        main.emit(0x75, 0xa8, 0x02 if mode3 else 0x82)
        main.emit(0x75, 0x88, 0x10)
        main.label('poll')
        main.emit(0xe5, 0x20)
        main.branch(0x60, 'poll')
        main.emit(0xe5, 0x81)
        main.emit(0xb4, 0x07, 0x03)
        main.ljmp('pass')
        self.emit_text(main, b'IRQ-FAIL\n')
        main.label('stop')
        main.branch(0x80, 'stop')
        main.label('pass')
        self.emit_text(main, b'IRQ-PASS\n')
        main.branch(0x80, 'stop')
        write(0x0100, main.finish(0x0100))

        handler = bytes((
            0x75, 0x88, 0x00,
            0x75, 0x20, 0x01,
            0x32,
        ))
        write(0x0200, handler)

        name = 'mcs51-mode3-irq.bin' if mode3 else 'mcs51-irq.bin'
        path = Path(self.scratch_file(name))
        path.write_bytes(image)
        return path

    def build_idle_firmware(self):
        image = bytearray(self.FLASH_SIZE)

        def write(address, data):
            image[address:address + len(data)] = data

        write(0x0000, bytes((0x02, 0x01, 0x00)))
        write(0x000b, bytes((0x02, 0x02, 0x00)))

        main = Program()
        main.emit(0x75, 0x20, 0x00)
        main.emit(0x75, 0x89, 0x01)
        main.emit(0x75, 0x8c, 0x00, 0x75, 0x8a, 0x00)
        main.emit(0x75, 0xa8, 0x82, 0x75, 0x88, 0x10)
        main.emit(0x75, 0x87, 0x31)
        self.assert_direct(main, 0x20, 0x01)
        self.assert_direct(main, 0x87, 0x30)
        self.emit_text(main, b'IDLE-PASS\n')
        main.label('done')
        main.branch(0x80, 'done')
        main.label('fail')
        self.emit_text(main, b'IDLE-FAIL\n')
        main.branch(0x80, 'done')
        write(0x0100, main.finish(0x0100))

        handler = bytes((
            0x75, 0x88, 0x00,
            0x75, 0x20, 0x01,
            0x32,
        ))
        write(0x0200, handler)

        path = Path(self.scratch_file('mcs51-idle.bin'))
        path.write_bytes(image)
        return path

    def build_powerdown_firmware(self):
        image = bytearray(self.FLASH_SIZE)

        def write(address, data):
            image[address:address + len(data)] = data

        write(0x0000, bytes((0x02, 0x01, 0x00)))
        write(0x0023, bytes((0x02, 0x02, 0x00)))

        main = Program()
        main.emit(0x75, 0x20, 0x00)
        main.emit(0x75, 0x98, 0x10)
        self.emit_text(main, b'PD-READY\n')
        main.emit(0x75, 0x98, 0x10)
        main.emit(0x75, 0xa8, 0x90)
        main.emit(0x75, 0x87, 0x32)
        self.assert_direct(main, 0x20, 0x01)
        self.assert_direct(main, 0x87, 0x30)
        self.emit_text(main, b'PD-PASS\n')
        main.label('done')
        main.branch(0x80, 'done')
        main.label('fail')
        self.emit_text(main, b'PD-FAIL\n')
        main.branch(0x80, 'done')
        write(0x0100, main.finish(0x0100))

        handler = bytes((
            0x75, 0x98, 0x10,
            0x75, 0x20, 0x01,
            0x32,
        ))
        write(0x0200, handler)

        path = Path(self.scratch_file('mcs51-powerdown.bin'))
        path.write_bytes(image)
        return path

    def run_firmware(self, firmware, success, failure=None):
        self.set_machine('stc8g1k08a')
        self.vm.add_args('-bios', str(firmware))
        self.vm.set_console()
        self.vm.launch()
        wait_for_console_pattern(self, success, failure_message=failure)

    def test_instruction_set(self):
        self.run_firmware(self.build_isa_firmware(),
                          'MCS51-PASS', 'MCS51-FAIL')

    def test_timer_interrupt_entry_and_reti(self):
        firmware = self.build_interrupt_firmware()
        log = Path(self.scratch_file('mcs51-interrupt.log'))

        self.set_machine('stc8g1k08a')
        self.vm.add_args('-bios', str(firmware),
                         '-d', 'int,cpu_reset,trace:mcs51_*',
                         '-D', str(log))
        self.vm.set_console()
        self.vm.launch()
        wait_for_console_pattern(self, 'IRQ-PASS',
                                 failure_message='IRQ-FAIL')
        self.vm.shutdown()

        contents = log.read_text()
        self.assertIn('mcs51-cpu: CPU 0 reset', contents)
        self.assertIn('mcs51-cpu: CPU 0 taking IRQ 1', contents)
        self.assertIn('mcs51-cpu: CPU 0 RETI', contents)
        self.assertIn('mcs51_cpu_reset', contents)
        self.assertIn('mcs51_irq_set', contents)
        self.assertIn('mcs51_irq_take', contents)
        self.assertIn('mcs51_irq_return', contents)

    def test_timer0_mode3_is_nmi(self):
        self.run_firmware(self.build_interrupt_firmware(mode3=True),
                          'IRQ-PASS', 'IRQ-FAIL')

    def test_idle_mode_wakes_on_timer_interrupt(self):
        self.run_firmware(self.build_idle_firmware(), 'IDLE-PASS',
                          'IDLE-FAIL')

    def test_powerdown_mode_wakes_on_uart_receive(self):
        self.set_machine('stc8g1k08a')
        self.vm.add_args('-bios', str(self.build_powerdown_firmware()))
        self.vm.set_console()
        self.vm.launch()
        wait_for_console_pattern(self, 'PD-READY')
        self.vm.console_socket.sendall(b'W')
        wait_for_console_pattern(self, 'PD-PASS', failure_message='PD-FAIL')


if __name__ == '__main__':
    QemuSystemTest.main()
