#!/usr/bin/env python3
#
# MCS-251 ISA integration test
#
# Copyright (c) 2026 Process Mission
#
# SPDX-License-Identifier: GPL-2.0-or-later
#

from pathlib import Path

from qemu_test import QemuSystemTest, wait_for_console_pattern


class SourceProgram:
    def __init__(self):
        self.code = bytearray()
        self.labels = {}
        self.relative_patches = []
        self.address_patches = []
        self.near_address_patches = []
        self.absolute11_patches = []
        self.dword_address_patches = []

    def emit(self, *values):
        self.code.extend(values)

    def label(self, name):
        self.labels[name] = len(self.code)

    def branch(self, opcode, label):
        self.emit(opcode, 0)
        self.relative_patches.append((len(self.code) - 1, label))

    def bit_branch(self, operation, bit, direct, label):
        self.emit(0xa9, operation | bit, direct, 0)
        self.relative_patches.append((len(self.code) - 1, label))

    def classic_bit_branch(self, opcode, bit, label):
        self.emit(opcode, bit, 0)
        self.relative_patches.append((len(self.code) - 1, label))

    def ecall(self, label):
        self.emit(0x9a, 0, 0, 0)
        self.address_patches.append((len(self.code) - 3, label))

    def lcall(self, label):
        self.emit(0x12, 0, 0)
        self.near_address_patches.append((len(self.code) - 2, label))

    def ljmp(self, label):
        self.emit(0x02, 0, 0)
        self.near_address_patches.append((len(self.code) - 2, label))

    def acall(self, label):
        self.emit(0x11, 0)
        self.absolute11_patches.append((len(self.code) - 2, label))

    def ajmp(self, label):
        self.emit(0x01, 0)
        self.absolute11_patches.append((len(self.code) - 2, label))

    def jump(self, label):
        self.emit(0x8a, 0, 0, 0)
        self.address_patches.append((len(self.code) - 3, label))

    def mov_dptr(self, label):
        self.emit(0x90, 0, 0)
        self.near_address_patches.append((len(self.code) - 2, label))

    def mov_dword_label(self, register, label):
        self.emit(0x7e, register << 4 | 0x08, 0, 0)
        low_offset = len(self.code) - 2
        self.emit(0x7a, register << 4 | 0x0c, 0, 0)
        high_offset = len(self.code) - 2
        self.dword_address_patches.append((low_offset, high_offset, label))

    def mov_word_label(self, register, label):
        self.emit(0x7e, register << 4 | 0x04, 0, 0)
        self.near_address_patches.append((len(self.code) - 2, label))

    def cjne_a(self, value, label):
        self.emit(0xb4, value, 0)
        self.relative_patches.append((len(self.code) - 1, label))

    def djnz_direct(self, direct, label):
        self.emit(0xd5, direct, 0)
        self.relative_patches.append((len(self.code) - 1, label))

    def fail_if(self, opcode):
        inverse = {
            0x50: 0x40,
            0x78: 0x68,
        }[opcode]
        self.emit(inverse, 4)
        self.jump('fail')

    def binary_fail_if_not_equal(self):
        self.emit(0xa5, 0x68, 5)
        self.emit(0xa5, 0x8a, 0, 0, 0)
        self.address_patches.append((len(self.code) - 3, 'fail'))

    def finish(self, base):
        for offset, label in self.relative_patches:
            displacement = self.labels[label] - (offset + 1)
            if not -128 <= displacement <= 127:
                raise ValueError(f'relative branch to {label} is too far')
            self.code[offset] = displacement & 0xff
        for offset, label in self.address_patches:
            address = base + self.labels[label]
            self.code[offset:offset + 3] = address.to_bytes(3, 'big')
        for offset, label in self.near_address_patches:
            address = base + self.labels[label]
            self.code[offset:offset + 2] = (address & 0xffff).to_bytes(
                2, 'big')
        for offset, label in self.absolute11_patches:
            address = base + self.labels[label]
            next_address = base + offset + 2
            if address & ~0x7ff != next_address & ~0x7ff:
                raise ValueError(f'absolute jump to {label} crosses 2 KiB')
            self.code[offset] |= address >> 3 & 0xe0
            self.code[offset + 1] = address & 0xff
        for low_offset, high_offset, label in self.dword_address_patches:
            address = base + self.labels[label]
            self.code[low_offset:low_offset + 2] = (
                address & 0xffff).to_bytes(2, 'big')
            self.code[high_offset:high_offset + 2] = (
                address >> 16).to_bytes(2, 'big')
        return self.code


class MCS251ISATest(QemuSystemTest):
    FLASH_BASE = 0xfc2800
    FLASH_SIZE = 246 * 1024
    RESET_PC = 0xff0000

    @staticmethod
    def ihex_record(address, record_type, data):
        record = bytearray((
            len(data),
            address >> 8 & 0xff,
            address & 0xff,
            record_type,
        ))
        record.extend(data)
        record.append(-sum(record) & 0xff)
        return ':' + ''.join(f'{value:02X}' for value in record)

    def convert_to_ihex(self, raw_firmware):
        image = raw_firmware.read_bytes()
        reset_offset = self.RESET_PC - self.FLASH_BASE
        payload = image[reset_offset:].rstrip(b'\x00')
        address = self.RESET_PC
        upper_address = None
        records = []

        while payload:
            current_upper = address >> 16
            if current_upper != upper_address:
                records.append(self.ihex_record(
                    0, 4, current_upper.to_bytes(2, 'big')))
                upper_address = current_upper

            record_size = min(16, len(payload),
                              0x10000 - (address & 0xffff))
            records.append(self.ihex_record(
                address & 0xffff, 0, payload[:record_size]))
            payload = payload[record_size:]
            address += record_size

        records.append(self.ihex_record(0, 1, b''))
        path = Path(self.scratch_file('mcs251-isa.hex'))
        path.write_text('\n'.join(records) + '\n', encoding='ascii')
        return path

    @staticmethod
    def mov_imm(program, register, value):
        program.emit(0x7e, register << 4, value)

    @staticmethod
    def cmp_imm(program, register, value):
        program.emit(0xbe, register << 4, value)

    @staticmethod
    def mov_dword_imm(program, register, value):
        program.emit(0x7e, register << 4 | 0x08,
                     value >> 8 & 0xff, value & 0xff)
        program.emit(0x7a, register << 4 | 0x0c,
                     value >> 24, value >> 16 & 0xff)

    def assert_direct(self, program, direct, value):
        program.emit(0xe5, direct)
        self.cmp_imm(program, 11, value)
        program.fail_if(0x78)

    def tfpu_write32(self, program, base_register, value):
        for offset in range(4):
            shift = (3 - offset) * 8

            self.mov_imm(program, base_register + offset,
                         value >> shift & 0xff)

    def tfpu_assert32(self, program, base_register, value):
        for offset in range(4):
            shift = (3 - offset) * 8

            self.cmp_imm(program, base_register + offset,
                         value >> shift & 0xff)
            program.fail_if(0x78)

    @staticmethod
    def tfpu_command(program, command):
        program.emit(0x75, 0xed, command)

    @staticmethod
    def classic_assert_acc(program, value):
        program.emit(0xb4, value, 2)
        program.emit(0x80, 3)
        program.ljmp('fail')

    def classic_assert_direct(self, program, direct, value):
        program.emit(0xe5, direct)
        self.classic_assert_acc(program, value)

    def classic_assert_nz(self, program, value):
        program.emit(0xe5, 0xd1)
        program.emit(0x54, 0x22)
        self.classic_assert_acc(program, value)

    @staticmethod
    def assert_branch(program, opcode, taken, name, extended):
        target = f'{name}_target'
        done = f'{name}_done'
        failure_jump = program.jump if extended else program.ljmp

        program.branch(opcode, target)
        if taken:
            failure_jump('fail')
            program.label(target)
        else:
            program.branch(0x80, done)
            program.label(target)
            failure_jump('fail')
            program.label(done)

    @staticmethod
    def assert_classic_bit_branch(program, opcode, bit, taken, name):
        target = f'{name}_target'
        done = f'{name}_done'

        program.classic_bit_branch(opcode, bit, target)
        if taken:
            program.ljmp('fail')
            program.label(target)
        else:
            program.branch(0x80, done)
            program.label(target)
            program.ljmp('fail')
            program.label(done)

    @staticmethod
    def assert_native_bit_branch(program, operation, bit, direct,
                                 taken, name):
        target = f'{name}_target'
        done = f'{name}_done'

        program.bit_branch(operation, bit, direct, target)
        if taken:
            program.jump('fail')
            program.label(target)
        else:
            program.branch(0x80, done)
            program.label(target)
            program.jump('fail')
            program.label(done)

    @staticmethod
    def snapshot_flags(program):
        program.emit(0x85, 0xd0, 0x30)
        program.emit(0x85, 0xd1, 0x31)

    @staticmethod
    def emit_text(program, text):
        for value in text:
            program.emit(0x75, 0x99, value)

    def build_firmware(self):
        p = SourceProgram()

        self.mov_imm(p, 0, 0x12)
        self.mov_imm(p, 1, 0x34)
        p.emit(0x2c, 0x01)
        self.cmp_imm(p, 0, 0x46)
        p.fail_if(0x78)
        self.emit_text(p, b'1\n')

        for bank, value in enumerate((0x11, 0x22, 0x33, 0x44)):
            p.emit(0x75, 0xd0, bank << 3)
            self.mov_imm(p, 0, value)
        for bank, value in enumerate((0x11, 0x22, 0x33, 0x44)):
            p.emit(0x75, 0xd0, bank << 3)
            self.cmp_imm(p, 0, value)
            p.fail_if(0x78)
            self.assert_direct(p, bank << 3, value)
        p.emit(0x75, 0xd0, 0x00)
        self.mov_imm(p, 10, 0x66)
        self.assert_direct(p, 0xf0, 0x66)
        p.emit(0x75, 0xe0, 0x77)
        self.cmp_imm(p, 11, 0x77)
        p.fail_if(0x78)
        self.emit_text(p, b'BANK\n')

        p.emit(0x75, 0x91, 0x00)
        p.emit(0x75, 0x92, 0xff)
        p.emit(0x75, 0x90, 0xaa)
        p.emit(0xa5, 0x78, 0x90)
        p.emit(0xa5, 0x76, 0x55)
        p.emit(0xa5, 0xe6)
        self.cmp_imm(p, 11, 0x55)
        p.fail_if(0x78)
        self.assert_direct(p, 0x90, 0xaa)
        self.emit_text(p, b'IDATA\n')

        p.emit(0x7e, 0x14, 0x12, 0x34)
        p.emit(0x7e, 0x24, 0x00, 0x01)
        p.emit(0x2d, 0x12)
        p.emit(0xbe, 0x14, 0x12, 0x35)
        p.fail_if(0x78)
        self.emit_text(p, b'2\n')

        p.emit(0x7e, 0x18, 0xff, 0xff)
        p.emit(0x7a, 0x1c, 0x12, 0x34)
        p.emit(0xbe, 0x24, 0x12, 0x34)
        p.fail_if(0x78)
        p.emit(0xbe, 0x34, 0xff, 0xff)
        p.fail_if(0x78)
        self.emit_text(p, b'2a\n')
        p.emit(0x7a, 0x1f, 0x00, 0x40)
        p.emit(0x7e, 0x2f, 0x00, 0x40)
        p.emit(0xbe, 0x44, 0x12, 0x34)
        p.fail_if(0x78)
        p.emit(0xbe, 0x54, 0xff, 0xff)
        p.fail_if(0x78)
        self.emit_text(p, b'2b\n')
        p.emit(0xbf, 0x21)
        p.fail_if(0x78)
        self.emit_text(p, b'3\n')

        self.mov_imm(p, 3, 0x80)
        p.emit(0x1a, 0x43)
        p.emit(0xbe, 0x44, 0xff, 0x80)
        p.fail_if(0x78)
        p.emit(0x0a, 0x53)
        p.emit(0xbe, 0x54, 0x00, 0x80)
        p.fail_if(0x78)
        self.emit_text(p, b'4\n')

        self.mov_imm(p, 4, 0x81)
        p.emit(0x0e, 0x40)
        self.cmp_imm(p, 4, 0xc0)
        p.fail_if(0x78)
        self.emit_text(p, b'5\n')

        self.mov_imm(p, 5, 1)
        p.emit(0x0b, 0x52)
        self.cmp_imm(p, 5, 5)
        p.fail_if(0x78)
        self.emit_text(p, b'6\n')

        p.emit(0xa9, 0xd3, 0x20)
        p.emit(0xa9, 0xa3, 0x20)
        p.fail_if(0x50)
        p.emit(0xa9, 0xc3, 0x20)
        p.bit_branch(0x30, 3, 0x20, 'bit_ok')
        p.jump('fail')
        p.label('bit_ok')
        self.emit_text(p, b'7\n')

        self.mov_imm(p, 6, 0x5a)
        p.emit(0xca, 0x68)
        self.mov_imm(p, 6, 0)
        p.emit(0xda, 0x68)
        self.cmp_imm(p, 6, 0x5a)
        p.fail_if(0x78)

        p.emit(0x7e, 0x14, 0x12, 0x34)
        p.emit(0xca, 0x19)
        p.emit(0x7e, 0x14, 0x00, 0x00)
        p.emit(0xda, 0x19)
        p.emit(0xbe, 0x14, 0x12, 0x34)
        p.fail_if(0x78)

        self.mov_dword_imm(p, 1, 0x12345678)
        p.emit(0xca, 0x1b)
        self.mov_dword_imm(p, 1, 0)
        p.emit(0xda, 0x1b)
        p.emit(0xbe, 0x24, 0x12, 0x34)
        p.fail_if(0x78)
        p.emit(0xbe, 0x34, 0x56, 0x78)
        p.fail_if(0x78)
        self.assert_direct(p, 0x81, 0x07)
        self.assert_direct(p, 0x85, 0x00)
        self.emit_text(p, b'8\n')

        self.mov_imm(p, 7, 0)
        p.ecall('subroutine')
        self.cmp_imm(p, 7, 1)
        p.fail_if(0x78)
        p.lcall('near_subroutine')
        self.cmp_imm(p, 7, 2)
        p.fail_if(0x78)
        self.emit_text(p, b'9\n')

        p.emit(0xa5, 0x74, 0x50)
        p.emit(0xa5, 0x04)
        self.cmp_imm(p, 11, 0x51)
        p.fail_if(0x78)
        self.emit_text(p, b'A\n')

        p.emit(0xa5, 0xa5, 0x7e, 0x80, 0x5c)
        self.cmp_imm(p, 8, 0x5c)
        p.fail_if(0x78)
        self.emit_text(p, b'A2\n')

        p.emit(0x74, 0x04)
        p.emit(0x83)
        p.jump('movc_done')
        p.emit(0x5d)
        p.label('movc_done')
        self.cmp_imm(p, 11, 0x5d)
        p.fail_if(0x78)
        self.emit_text(p, b'MOVC\n')

        p.emit(0x75, 0xe3, 0x00)
        p.emit(0x90, 0x12, 0x34)
        p.emit(0x75, 0xe3, 0x01)
        p.emit(0x90, 0x56, 0x78)
        p.emit(0x75, 0xe3, 0x00)
        self.assert_direct(p, 0x82, 0x34)
        self.assert_direct(p, 0x83, 0x12)
        p.emit(0x75, 0xe3, 0x01)
        self.assert_direct(p, 0x82, 0x78)
        self.assert_direct(p, 0x83, 0x56)

        p.emit(0x75, 0xe3, 0x20)
        p.emit(0x90, 0x9a, 0xbc)
        self.assert_direct(p, 0xe3, 0x21)
        self.assert_direct(p, 0x82, 0x78)
        p.emit(0x90, 0xde, 0xf0)
        self.assert_direct(p, 0xe3, 0x20)
        self.assert_direct(p, 0x82, 0xbc)

        p.emit(0x0b, 0xec)
        self.assert_direct(p, 0xe3, 0x21)
        self.assert_direct(p, 0x82, 0xf0)
        p.emit(0x75, 0xe3, 0x20)
        self.assert_direct(p, 0x82, 0xbd)
        p.emit(0x75, 0xe3, 0x21)
        p.emit(0x1b, 0xec)
        self.assert_direct(p, 0xe3, 0x20)
        self.assert_direct(p, 0x82, 0xbd)
        p.emit(0x75, 0xe3, 0x21)
        self.assert_direct(p, 0x82, 0xef)

        p.emit(0x75, 0xe3, 0x00)
        p.emit(0x90, 0x00, 0x40)
        p.emit(0x74, 0x5a)
        p.emit(0x75, 0xe3, 0x08)
        self.assert_direct(p, 0xe3, 0x00)
        p.emit(0x74, 0x5a)
        p.emit(0x75, 0xae, 0xaa)
        p.emit(0x75, 0xae, 0x55)
        p.emit(0x75, 0xe3, 0x08)
        p.emit(0xf0)
        self.assert_direct(p, 0x82, 0x41)
        p.emit(0x75, 0xe3, 0x00)
        p.emit(0x90, 0x00, 0x40)
        p.emit(0xe0)
        self.cmp_imm(p, 11, 0x5a)
        p.fail_if(0x78)

        p.emit(0x90, 0x00, 0x41)
        p.emit(0x75, 0xae, 0xaa)
        p.emit(0x75, 0xae, 0x55)
        p.emit(0x75, 0xe3, 0x48)
        p.emit(0xe0)
        self.assert_direct(p, 0x82, 0x40)
        p.emit(0x75, 0xe3, 0x20)
        p.emit(0xbf, 0xe1)
        self.assert_direct(p, 0xe3, 0x20)
        self.emit_text(p, b'DPTR\n')

        self.mov_dword_imm(p, 1, 0x007efea0)
        self.mov_imm(p, 0, 0x12)
        p.emit(0x7a, 0x1b, 0x00)
        p.emit(0x7e, 0x1b, 0x10)
        self.cmp_imm(p, 1, 0x00)
        p.fail_if(0x78)
        p.emit(0x75, 0xba, 0x80)
        p.emit(0x7a, 0x1b, 0x00)
        p.emit(0x7e, 0x1b, 0x10)
        self.cmp_imm(p, 1, 0x12)
        p.fail_if(0x78)
        p.emit(0x75, 0xba, 0x00)
        p.emit(0x7e, 0x1b, 0x10)
        self.cmp_imm(p, 1, 0x00)
        p.fail_if(0x78)

        self.mov_dword_imm(p, 1, 0x00030000)
        self.mov_imm(p, 0, 0x6b)
        p.emit(0x7a, 0x1b, 0x00)
        p.emit(0x7e, 0x1b, 0x10)
        self.cmp_imm(p, 1, 0x6b)
        p.fail_if(0x78)
        p.emit(0x75, 0xea, 0x87)
        self.mov_dword_imm(p, 1, 0x00800000)
        p.emit(0x7e, 0x1b, 0x10)
        self.cmp_imm(p, 1, 0x6b)
        p.fail_if(0x78)
        self.mov_imm(p, 0, 0x99)
        p.emit(0x7a, 0x1b, 0x00)
        p.emit(0x7e, 0x1b, 0x10)
        self.cmp_imm(p, 1, 0x6b)
        p.fail_if(0x78)
        p.emit(0x75, 0xea, 0x07)
        self.mov_dword_imm(p, 1, 0x00030000)
        p.emit(0x7e, 0x1b, 0x10)
        self.cmp_imm(p, 1, 0x6b)
        p.fail_if(0x78)

        for address, value in ((0x00030000, 0x0b),
                               (0x00030001, 0x70),
                               (0x00030002, 0xaa)):
            self.mov_dword_imm(p, 1, address)
            self.mov_imm(p, 0, value)
            p.emit(0x7a, 0x1b, 0x00)
        p.emit(0x75, 0xea, 0x87)
        p.emit(0x9a, 0x80, 0x00, 0x00)
        self.cmp_imm(p, 7, 0x03)
        p.fail_if(0x78)
        p.emit(0x75, 0xea, 0x07)
        self.emit_text(p, b'MAP\n')

        p.emit(0x75, 0x97, 0x40)
        p.emit(0x74, 0x42)
        p.emit(0xa5, 0x7e, 0x80, 0x42)
        p.emit(0xa5, 0xbe, 0x80, 0x42)
        p.binary_fail_if_not_equal()
        p.emit(0x75, 0x97, 0x00)

        p.emit(0x06)
        self.emit_text(p, b'B\n')
        self.emit_text(p, b'PASS\n')
        p.branch(0x80, 'done')

        p.label('fail')
        self.emit_text(p, b'FAIL\n')
        p.label('done')
        p.branch(0x80, 'done')

        p.label('subroutine')
        p.emit(0x0b, 0x70)
        p.emit(0xaa)

        p.label('near_subroutine')
        p.emit(0x0b, 0x70)
        p.emit(0x22)

        code = p.finish(self.RESET_PC)
        image = bytearray(self.FLASH_SIZE)
        image[self.RESET_PC - self.FLASH_BASE:
              self.RESET_PC - self.FLASH_BASE + len(code)] = code
        path = Path(self.scratch_file('mcs251-isa.bin'))
        path.write_bytes(image)
        return path

    def build_classic_firmware(self):
        p = SourceProgram()

        p.emit(0x75, 0x97, 0x40)
        p.emit(0x00)

        p.emit(0x75, 0xd0, 0x00)
        p.emit(0x74, 0x7f)
        p.emit(0x24, 0x01)
        self.snapshot_flags(p)
        self.classic_assert_acc(p, 0x80)
        self.classic_assert_direct(p, 0x30, 0x45)
        self.classic_assert_direct(p, 0x31, 0x64)

        p.emit(0x75, 0xd0, 0x00)
        p.emit(0xd3)
        p.emit(0x74, 0xff)
        p.emit(0x34, 0x00)
        self.snapshot_flags(p)
        self.classic_assert_acc(p, 0x00)
        self.classic_assert_direct(p, 0x30, 0xc0)
        self.classic_assert_direct(p, 0x31, 0xc2)

        p.emit(0x75, 0xd0, 0x00)
        p.emit(0xc3)
        p.emit(0x74, 0x00)
        p.emit(0x94, 0x01)
        self.snapshot_flags(p)
        self.classic_assert_acc(p, 0xff)
        self.classic_assert_direct(p, 0x30, 0xc0)
        self.classic_assert_direct(p, 0x31, 0xe0)

        p.emit(0x75, 0xd0, 0x00)
        p.emit(0x74, 0x10)
        p.emit(0x75, 0xf0, 0x10)
        p.emit(0xa4)
        self.snapshot_flags(p)
        self.classic_assert_acc(p, 0x00)
        self.classic_assert_direct(p, 0xf0, 0x01)
        p.emit(0xe5, 0x30)
        p.emit(0x54, 0x84)
        self.classic_assert_acc(p, 0x04)

        p.emit(0x74, 0xfb)
        p.emit(0x75, 0xf0, 0x12)
        p.emit(0x84)
        self.classic_assert_acc(p, 0x0d)
        self.classic_assert_direct(p, 0xf0, 0x11)

        p.emit(0x74, 0x55)
        p.emit(0x75, 0xf0, 0x00)
        p.emit(0x84)
        self.snapshot_flags(p)
        self.classic_assert_acc(p, 0x55)
        self.classic_assert_direct(p, 0xf0, 0x00)
        p.emit(0xe5, 0x30)
        p.emit(0x54, 0x04)
        self.classic_assert_acc(p, 0x04)

        p.emit(0x75, 0xd0, 0x00)
        p.emit(0x74, 0x9a)
        p.emit(0xd4)
        self.snapshot_flags(p)
        self.classic_assert_acc(p, 0x00)
        p.emit(0xe5, 0x30)
        p.emit(0x54, 0x80)
        self.classic_assert_acc(p, 0x80)
        self.emit_text(p, b'C-ARITH\n')

        p.emit(0x78, 0x7f)
        p.emit(0x08)
        self.classic_assert_nz(p, 0x20)
        p.emit(0xe8)
        self.classic_assert_acc(p, 0x80)
        p.emit(0x18)
        p.emit(0xe8)
        self.classic_assert_acc(p, 0x7f)

        p.emit(0x74, 0xff)
        p.emit(0x04)
        p.emit(0xf5, 0x32)
        self.classic_assert_nz(p, 0x02)
        self.classic_assert_direct(p, 0x32, 0x00)

        p.emit(0x78, 0x22)
        p.emit(0x76, 0xfe)
        p.emit(0x06)
        self.classic_assert_nz(p, 0x20)
        p.emit(0x16)
        p.emit(0xe6)
        self.classic_assert_acc(p, 0xfe)
        p.emit(0x75, 0x23, 0xff)
        p.emit(0x05, 0x23)
        self.classic_assert_nz(p, 0x02)
        self.classic_assert_direct(p, 0x23, 0x00)
        p.emit(0x15, 0x23)
        self.classic_assert_direct(p, 0x23, 0xff)

        p.emit(0x74, 0x55)
        p.emit(0x44, 0x0a)
        p.emit(0x54, 0xf0)
        p.emit(0x64, 0xff)
        self.classic_assert_acc(p, 0xaf)
        p.emit(0x75, 0x24, 0x0f)
        p.emit(0x42, 0x24)
        p.emit(0x53, 0x24, 0xf0)
        p.emit(0x62, 0x24)
        self.classic_assert_direct(p, 0x24, 0x0f)

        p.emit(0x74, 0x81)
        p.emit(0x03)
        self.classic_assert_acc(p, 0xc0)
        p.emit(0x74, 0x81)
        p.emit(0x23)
        self.classic_assert_acc(p, 0x03)
        p.emit(0xc3)
        p.emit(0x74, 0x81)
        p.emit(0x13)
        self.snapshot_flags(p)
        self.classic_assert_acc(p, 0x40)
        p.emit(0xe5, 0x30)
        p.emit(0x54, 0x80)
        self.classic_assert_acc(p, 0x80)
        p.emit(0xd3)
        p.emit(0x74, 0x81)
        p.emit(0x33)
        self.classic_assert_acc(p, 0x03)
        p.emit(0x74, 0xab)
        p.emit(0xc4)
        self.classic_assert_acc(p, 0xba)
        p.emit(0xe4)
        self.classic_assert_acc(p, 0x00)
        p.emit(0xf4)
        self.classic_assert_acc(p, 0xff)
        self.emit_text(p, b'C-LOGIC\n')

        p.emit(0x75, 0x91, 0xff)
        p.emit(0x75, 0x92, 0x00)
        p.emit(0x75, 0x90, 0xaa)
        p.emit(0x05, 0x90)
        self.classic_assert_direct(p, 0x90, 0xff)
        p.emit(0x75, 0x91, 0x00)
        p.emit(0x75, 0x92, 0xff)
        self.classic_assert_direct(p, 0x90, 0xab)
        self.emit_text(p, b'C-RMW\n')

        p.emit(0x7a, 0x5a)
        p.emit(0xea)
        self.classic_assert_acc(p, 0x5a)
        p.emit(0x8a, 0x25)
        p.emit(0xab, 0x25)
        p.emit(0xeb)
        self.classic_assert_acc(p, 0x5a)

        p.emit(0x78, 0x26)
        p.emit(0x74, 0xa5)
        p.emit(0xf6)
        p.emit(0xe4)
        p.emit(0xe6)
        self.classic_assert_acc(p, 0xa5)
        p.emit(0x75, 0x27, 0x12)
        p.emit(0x74, 0x34)
        p.emit(0xc5, 0x27)
        self.classic_assert_acc(p, 0x12)
        self.classic_assert_direct(p, 0x27, 0x34)
        p.emit(0x74, 0xa5)
        p.emit(0x75, 0x26, 0x3c)
        p.emit(0xd6)
        self.classic_assert_acc(p, 0xac)
        self.classic_assert_direct(p, 0x26, 0x35)

        p.emit(0x75, 0x28, 0x77)
        p.emit(0xc0, 0x28)
        p.emit(0x75, 0x28, 0x00)
        p.emit(0xd0, 0x29)
        self.classic_assert_direct(p, 0x29, 0x77)
        self.classic_assert_direct(p, 0x81, 0x07)

        p.emit(0x90, 0x00, 0x40)
        p.emit(0x74, 0x66)
        p.emit(0xf0)
        p.emit(0xe4)
        p.emit(0xe0)
        self.classic_assert_acc(p, 0x66)
        p.emit(0x75, 0xeb, 0x01)
        p.emit(0x90, 0x00, 0x41)
        p.emit(0x74, 0x99)
        p.emit(0xf0)
        p.emit(0x75, 0xa0, 0x00)
        p.emit(0x78, 0x41)
        p.emit(0xe4)
        p.emit(0xe2)
        self.classic_assert_acc(p, 0x99)
        p.emit(0x74, 0x5a)
        p.emit(0xf2)
        p.emit(0x90, 0x00, 0x41)
        p.emit(0xe4)
        p.emit(0xe0)
        self.classic_assert_acc(p, 0x5a)

        p.mov_dptr('classic_movc_data')
        p.emit(0x74, 0x00)
        p.emit(0x93)
        p.ajmp('classic_movc_done')
        p.label('classic_movc_data')
        p.emit(0x6e)
        p.label('classic_movc_done')
        self.classic_assert_acc(p, 0x6e)
        self.emit_text(p, b'C-MOVE\n')

        p.emit(0x75, 0x20, 0x00)
        p.emit(0xd2, 0x00)
        self.assert_classic_bit_branch(
            p, 0x20, 0x00, True, 'classic_jb_set')
        self.assert_classic_bit_branch(
            p, 0x30, 0x00, False, 'classic_jnb_set')
        p.emit(0xa2, 0x00)
        self.assert_branch(p, 0x40, True, 'classic_jc_set', False)
        self.assert_branch(p, 0x50, False, 'classic_jnc_set', False)
        p.emit(0x92, 0x01)
        p.emit(0xb2, 0x00)
        self.assert_classic_bit_branch(
            p, 0x20, 0x00, False, 'classic_jb_clear')
        self.assert_classic_bit_branch(
            p, 0x30, 0x00, True, 'classic_jnb_clear')
        p.emit(0xd2, 0x02)
        self.assert_classic_bit_branch(
            p, 0x10, 0x02, True, 'classic_jbc_set')
        self.assert_classic_bit_branch(
            p, 0x10, 0x02, False, 'classic_jbc_clear')
        self.assert_classic_bit_branch(
            p, 0x30, 0x02, True, 'classic_jnb_after_jbc')
        p.emit(0xc3)
        self.assert_branch(p, 0x40, False, 'classic_jc_clear', False)
        self.assert_branch(p, 0x50, True, 'classic_jnc_clear', False)
        p.emit(0xa0, 0x00)
        self.assert_branch(p, 0x40, True, 'classic_or_not_bit', False)
        self.assert_branch(p, 0x50, False,
                           'classic_or_not_bit_inverse', False)
        p.emit(0xc3, 0xd2, 0x00, 0x72, 0x00)
        self.assert_branch(p, 0x40, True, 'classic_or_set_bit', False)
        p.emit(0xd2, 0x00)
        p.emit(0xb0, 0x00)
        self.assert_branch(p, 0x50, True, 'classic_and_not_bit', False)
        self.assert_branch(p, 0x40, False,
                           'classic_and_not_bit_inverse', False)

        p.emit(0x74, 0x00)
        self.assert_branch(p, 0x60, True, 'classic_jz_zero', False)
        self.assert_branch(p, 0x70, False, 'classic_jnz_zero', False)
        p.emit(0x74, 0x01)
        self.assert_branch(p, 0x60, False, 'classic_jz_nonzero', False)
        self.assert_branch(p, 0x70, True, 'classic_jnz_nonzero', False)

        p.emit(0x74, 0x12)
        p.cjne_a(0x34, 'classic_cjne_unequal')
        p.ljmp('fail')
        p.label('classic_cjne_unequal')
        self.snapshot_flags(p)
        p.emit(0xe5, 0x30)
        p.emit(0x54, 0x80)
        self.classic_assert_acc(p, 0x80)

        p.emit(0x7a, 0x02)
        p.label('classic_djnz_r')
        p.branch(0xda, 'classic_djnz_r')
        p.emit(0xea)
        self.classic_assert_acc(p, 0x00)
        p.emit(0x75, 0x21, 0x02)
        p.label('classic_djnz_direct')
        p.djnz_direct(0x21, 'classic_djnz_direct')
        self.classic_assert_direct(p, 0x21, 0x00)

        p.emit(0x7f, 0x00)
        p.acall('classic_subroutine')
        p.lcall('classic_subroutine')
        p.emit(0xef)
        self.classic_assert_acc(p, 0x02)
        p.mov_dptr('classic_indirect_jump')
        p.emit(0x74, 0x00)
        p.emit(0x73)
        p.ljmp('fail')
        p.label('classic_indirect_jump')
        self.emit_text(p, b'C-BRANCH\n')

        self.emit_text(p, b'CLASSIC-PASS\n')
        p.branch(0x80, 'done')

        p.label('fail')
        self.emit_text(p, b'CLASSIC-FAIL\n')
        p.label('done')
        p.branch(0x80, 'done')

        p.label('classic_subroutine')
        p.emit(0x0f)
        p.emit(0x22)

        code = p.finish(self.RESET_PC)
        image = bytearray(self.FLASH_SIZE)
        image[self.RESET_PC - self.FLASH_BASE:
              self.RESET_PC - self.FLASH_BASE + len(code)] = code
        path = Path(self.scratch_file('mcs251-classic-isa.bin'))
        path.write_bytes(image)
        return path

    def build_native_firmware(self):
        p = SourceProgram()

        self.mov_imm(p, 0, 0xf0)
        self.mov_imm(p, 1, 0x0f)
        p.emit(0x4c, 0x01)
        self.cmp_imm(p, 0, 0xff)
        p.fail_if(0x78)
        self.mov_imm(p, 2, 0x0f)
        p.emit(0x5c, 0x02)
        self.cmp_imm(p, 0, 0x0f)
        p.fail_if(0x78)
        p.emit(0x6c, 0x01)
        self.cmp_imm(p, 0, 0x00)
        p.fail_if(0x78)
        self.mov_imm(p, 0, 0x10)
        self.mov_imm(p, 1, 0x01)
        p.emit(0x9c, 0x01)
        self.cmp_imm(p, 0, 0x0f)
        p.fail_if(0x78)

        p.emit(0x7e, 0x14, 0xf0, 0xf0)
        p.emit(0x7e, 0x24, 0x0f, 0x0f)
        p.emit(0x4d, 0x12)
        p.emit(0xbe, 0x14, 0xff, 0xff)
        p.fail_if(0x78)
        p.emit(0x5d, 0x12)
        p.emit(0xbe, 0x14, 0x0f, 0x0f)
        p.fail_if(0x78)
        p.emit(0x6d, 0x12)
        p.emit(0xbe, 0x14, 0x00, 0x00)
        p.fail_if(0x78)

        self.mov_dword_imm(p, 1, 0x00010000)
        self.mov_dword_imm(p, 2, 0x00000002)
        p.emit(0x2f, 0x12)
        self.mov_dword_imm(p, 3, 0x00010002)
        p.emit(0xbf, 0x13)
        p.fail_if(0x78)
        p.emit(0x9f, 0x12)
        self.mov_dword_imm(p, 3, 0x00010000)
        p.emit(0xbf, 0x13)
        p.fail_if(0x78)
        self.emit_text(p, b'N-REG\n')

        self.mov_imm(p, 0, 5)
        self.mov_imm(p, 1, 3)
        p.emit(0x2c, 0x01)
        self.mov_imm(p, 2, 0x0f)
        p.emit(0x6c, 0x02)
        self.mov_imm(p, 3, 1)
        p.emit(0x9c, 0x03)
        self.cmp_imm(p, 0, 6)
        p.fail_if(0x78)
        self.emit_text(p, b'N-COMPOSE\n')

        p.emit(0x75, 0x40, 0x05)
        self.mov_imm(p, 0, 0x03)
        p.emit(0x2e, 0x01, 0x40)
        self.cmp_imm(p, 0, 0x08)
        p.fail_if(0x78)
        p.emit(0x75, 0x41, 0x33)
        self.mov_imm(p, 1, 0x00)
        p.emit(0x7e, 0x11, 0x41)
        self.cmp_imm(p, 1, 0x33)
        p.fail_if(0x78)
        p.emit(0x7e, 0x15, 0x40)
        p.emit(0xbe, 0x14, 0x05, 0x33)
        p.fail_if(0x78)
        p.emit(0x7e, 0x2c, 0xff, 0xfe)
        self.mov_dword_imm(p, 3, 0xfffffffe)
        p.emit(0xbf, 0x23)
        p.fail_if(0x78)
        p.emit(0xbe, 0x3c, 0xff, 0xfe)
        p.fail_if(0x78)
        p.emit(0x7e, 0x3c, 0x00, 0x01)
        p.emit(0xbe, 0x38, 0x00, 0x01)
        p.fail_if(0x78)
        self.mov_dword_imm(p, 3, 0x00000001)
        p.emit(0xbe, 0x3c, 0x00, 0x01)
        p.fail_if(0x78)
        self.emit_text(p, b'N-DIRECT\n')

        p.emit(0x7e, 0x04, 0x00, 0x60)
        self.mov_imm(p, 2, 0xaa)
        p.emit(0x19, 0x20, 0x00, 0x02)
        self.mov_imm(p, 3, 0x00)
        p.emit(0x09, 0x30, 0x00, 0x02)
        self.cmp_imm(p, 3, 0xaa)
        p.fail_if(0x78)
        p.emit(0x7e, 0x04, 0x00, 0x62)
        p.emit(0x7e, 0x09, 0x40)
        self.cmp_imm(p, 4, 0xaa)
        p.fail_if(0x78)
        p.emit(0x7e, 0x04, 0x00, 0x60)
        self.mov_imm(p, 2, 0xbb)
        p.emit(0x19, 0x20, 0xff, 0xfe)
        self.mov_imm(p, 3, 0x00)
        p.emit(0x09, 0x30, 0xff, 0xfe)
        self.cmp_imm(p, 3, 0xbb)
        p.fail_if(0x78)
        self.emit_text(p, b'N-DISP8\n')

        self.mov_dword_imm(p, 2, 0x00010060)
        p.emit(0x7e, 0x14, 0x12, 0x34)
        p.emit(0x79, 0x12, 0x00, 0x02)
        p.emit(0x7e, 0x34, 0x00, 0x00)
        p.emit(0x69, 0x32, 0x00, 0x02)
        p.emit(0xbe, 0x34, 0x12, 0x34)
        p.fail_if(0x78)
        self.mov_dword_imm(p, 2, 0x00010062)
        p.emit(0x7e, 0x2b, 0x50)
        self.cmp_imm(p, 5, 0x12)
        p.fail_if(0x78)
        self.mov_dword_imm(p, 15, 0x000100fe)
        self.mov_imm(p, 0, 0x3c)
        p.emit(0x39, 0x0f, 0x00, 0x00)
        self.mov_dword_imm(p, 15, 0x00010100)
        self.mov_imm(p, 1, 0x00)
        p.emit(0x29, 0x1f, 0xff, 0xfe)
        self.cmp_imm(p, 1, 0x3c)
        p.fail_if(0x78)
        self.emit_text(p, b'N-DISP16\n')

        p.emit(0x75, 0xe3, 0x00)
        self.mov_dword_imm(p, 14, 0x00010080)
        p.emit(0x75, 0xe3, 0x01)
        self.mov_dword_imm(p, 14, 0x00010090)
        p.emit(0x75, 0xe3, 0x38)
        self.mov_imm(p, 0, 0x5a)
        p.emit(0x39, 0x0e, 0x00, 0x02)
        self.assert_direct(p, 0xe3, 0x39)
        self.assert_direct(p, 0x82, 0x90)
        p.emit(0x75, 0xe3, 0x00)
        self.assert_direct(p, 0x82, 0x81)
        self.mov_dword_imm(p, 2, 0x00010082)
        p.emit(0x29, 0x12, 0x00, 0x00)
        self.cmp_imm(p, 1, 0x5a)
        p.fail_if(0x78)

        self.mov_dword_imm(p, 2, 0x000100a2)
        p.emit(0x7e, 0x24, 0xa5, 0x5a)
        p.emit(0x79, 0x22, 0x00, 0x00)
        p.emit(0x75, 0xe3, 0x00)
        self.mov_dword_imm(p, 14, 0x000100a0)
        p.emit(0x75, 0xe3, 0x01)
        self.mov_dword_imm(p, 14, 0x000100b0)
        p.emit(0x75, 0xe3, 0x38)
        p.emit(0x69, 0x3e, 0x00, 0x02)
        p.emit(0xbe, 0x34, 0xa5, 0x5a)
        p.fail_if(0x78)
        self.assert_direct(p, 0xe3, 0x39)
        self.assert_direct(p, 0x82, 0xb0)
        p.emit(0x75, 0xe3, 0x00)
        self.assert_direct(p, 0x82, 0xa1)
        self.emit_text(p, b'N-DPTR-DISP16\n')

        self.mov_dword_imm(p, 1, 0x00010070)
        p.emit(0x7e, 0x44, 0x56, 0x78)
        p.emit(0x1b, 0x1a, 0x40)
        p.emit(0x7e, 0x54, 0x00, 0x00)
        p.emit(0x0b, 0x1a, 0x50)
        p.emit(0xbe, 0x54, 0x56, 0x78)
        p.fail_if(0x78)
        self.emit_text(p, b'N-WORDIND\n')
        self.emit_text(p, b'N-MEM\n')

        p.emit(0x7e, 0x44, 0x10, 0x00)
        p.emit(0x0b, 0x46)
        p.emit(0x1b, 0x45)
        p.emit(0xbe, 0x44, 0x10, 0x02)
        p.fail_if(0x78)
        self.mov_dword_imm(p, 3, 0x00010000)
        p.emit(0x0b, 0x3e)
        p.emit(0x1b, 0x3c)
        self.mov_dword_imm(p, 4, 0x00010003)
        p.emit(0xbf, 0x34)
        p.fail_if(0x78)

        self.mov_imm(p, 4, 0x81)
        p.emit(0x3e, 0x40)
        p.emit(0x85, 0xd0, 0x42)
        self.cmp_imm(p, 4, 0x02)
        p.fail_if(0x78)
        p.emit(0xe5, 0x42)
        p.emit(0x54, 0x80)
        self.cmp_imm(p, 11, 0x80)
        p.fail_if(0x78)
        p.emit(0x7e, 0x34, 0x80, 0x01)
        p.emit(0x1e, 0x34)
        p.emit(0xbe, 0x34, 0x40, 0x00)
        p.fail_if(0x78)
        p.emit(0x7e, 0x44, 0x80, 0x00)
        p.emit(0x0e, 0x44)
        p.emit(0xbe, 0x44, 0xc0, 0x00)
        p.fail_if(0x78)
        self.emit_text(p, b'N-SHIFT\n')

        self.mov_imm(p, 2, 0x10)
        self.mov_imm(p, 3, 0x10)
        p.emit(0xac, 0x23)
        self.cmp_imm(p, 2, 0x01)
        p.fail_if(0x78)
        self.cmp_imm(p, 3, 0x00)
        p.fail_if(0x78)
        self.mov_imm(p, 5, 0xfb)
        self.mov_imm(p, 6, 0x12)
        p.emit(0x8c, 0x56)
        self.cmp_imm(p, 4, 0x11)
        p.fail_if(0x78)
        self.cmp_imm(p, 5, 0x0d)
        p.fail_if(0x78)

        p.emit(0x7e, 0x24, 0x01, 0x00)
        p.emit(0x7e, 0x44, 0x00, 0x10)
        p.emit(0xad, 0x24)
        p.emit(0xbe, 0x24, 0x00, 0x00)
        p.fail_if(0x78)
        p.emit(0xbe, 0x34, 0x10, 0x00)
        p.fail_if(0x78)
        p.emit(0x7e, 0x34, 0x12, 0x34)
        p.emit(0x7e, 0x44, 0x00, 0x10)
        p.emit(0x8d, 0x34)
        p.emit(0xbe, 0x24, 0x00, 0x04)
        p.fail_if(0x78)
        p.emit(0xbe, 0x34, 0x01, 0x23)
        p.fail_if(0x78)

        self.mov_imm(p, 5, 0xfb)
        self.mov_imm(p, 6, 0x00)
        p.emit(0x8c, 0x56)
        self.snapshot_flags(p)
        self.cmp_imm(p, 5, 0xfb)
        p.fail_if(0x78)
        p.emit(0xe5, 0x30)
        p.emit(0x54, 0x04)
        self.cmp_imm(p, 11, 0x04)
        p.fail_if(0x78)
        self.emit_text(p, b'N-MULDIV\n')

        p.emit(0x75, 0x2a, 0x00)
        p.emit(0xa9, 0xd3, 0x2a)
        self.assert_native_bit_branch(
            p, 0x20, 3, 0x2a, True, 'native_jb_set')
        self.assert_native_bit_branch(
            p, 0x30, 3, 0x2a, False, 'native_jnb_set')
        p.emit(0xa9, 0xa3, 0x2a)
        p.fail_if(0x50)
        p.emit(0xa9, 0xb3, 0x2a)
        self.assert_native_bit_branch(
            p, 0x20, 3, 0x2a, False, 'native_jb_clear')
        self.assert_native_bit_branch(
            p, 0x30, 3, 0x2a, True, 'native_jnb_clear')
        p.emit(0xc3)
        p.emit(0xa9, 0xe0, 0x2a)
        p.fail_if(0x50)
        p.emit(0xa9, 0xd0, 0x2a)
        p.emit(0xa9, 0xf0, 0x2a)
        p.branch(0x50, 'native_not_bit')
        p.jump('fail')
        p.label('native_not_bit')
        p.emit(0xa9, 0xd1, 0x2a)
        self.assert_native_bit_branch(
            p, 0x10, 1, 0x2a, True, 'native_jbc_set')
        self.assert_native_bit_branch(
            p, 0x10, 1, 0x2a, False, 'native_jbc_clear')
        self.assert_native_bit_branch(
            p, 0x30, 1, 0x2a, True, 'native_jnb_after_jbc')
        self.assert_native_bit_branch(
            p, 0x20, 1, 0x2a, False, 'native_jb_after_jbc')

        p.emit(0xca, 0x02, 0x5a)
        p.emit(0xda, 0x88)
        self.cmp_imm(p, 8, 0x5a)
        p.fail_if(0x78)
        p.emit(0xca, 0x06, 0x12, 0x34)
        p.emit(0xda, 0x29)
        p.emit(0xbe, 0x24, 0x12, 0x34)
        p.fail_if(0x78)
        self.emit_text(p, b'N-BITSTACK\n')

        self.mov_imm(p, 9, 0x00)
        p.mov_dword_label(1, 'native_extended_subroutine')
        p.emit(0x99, 0x18)
        self.cmp_imm(p, 9, 0x01)
        p.fail_if(0x78)
        p.mov_word_label(0, 'native_near_subroutine')
        p.emit(0x99, 0x04)
        self.cmp_imm(p, 9, 0x02)
        p.fail_if(0x78)
        p.mov_dword_label(2, 'native_indirect_jump')
        p.emit(0x89, 0x28)
        p.jump('fail')
        p.label('native_indirect_jump')

        branch_vectors = (
            (0x01, 0x02, (
                (0x08, True), (0x18, False),
                (0x28, True), (0x38, False),
                (0x48, True), (0x58, False),
                (0x68, False), (0x78, True),
            )),
            (0x02, 0x01, (
                (0x08, False), (0x18, True),
                (0x28, False), (0x38, True),
                (0x48, False), (0x58, True),
                (0x68, False), (0x78, True),
            )),
            (0x02, 0x02, (
                (0x68, True), (0x78, False),
            )),
        )
        for vector, (lhs, rhs, outcomes) in enumerate(branch_vectors):
            self.mov_imm(p, 0, lhs)
            self.cmp_imm(p, 0, rhs)
            for opcode, taken in outcomes:
                self.assert_branch(
                    p, opcode, taken,
                    f'native_branch_{vector}_{opcode:02x}', True)

        self.mov_imm(p, 0, 0x5a)
        p.emit(0x0b, 0x03)
        p.emit(0x2e, 0x02)
        p.emit(0xb9)
        p.emit(0x06)
        self.cmp_imm(p, 0, 0x5a)
        p.fail_if(0x78)
        self.emit_text(p, b'N-CONTROL\n')

        self.emit_text(p, b'NATIVE-PASS\n')
        p.branch(0x80, 'done')

        p.label('fail')
        self.emit_text(p, b'NATIVE-FAIL\n')
        p.label('done')
        p.branch(0x80, 'done')

        p.label('native_extended_subroutine')
        p.emit(0x0b, 0x90)
        p.emit(0xaa)
        p.label('native_near_subroutine')
        p.emit(0x0b, 0x90)
        p.emit(0x22)

        code = p.finish(self.RESET_PC)
        image = bytearray(self.FLASH_SIZE)
        image[self.RESET_PC - self.FLASH_BASE:
              self.RESET_PC - self.FLASH_BASE + len(code)] = code
        path = Path(self.scratch_file('mcs251-native-isa.bin'))
        path.write_bytes(image)
        return path

    def build_tfpu_firmware(self):
        p = SourceProgram()

        self.tfpu_write32(p, 4, 0x11223344)
        p.emit(0x74, 0x1c)
        p.emit(0xf5, 0xed)
        self.tfpu_assert32(p, 4, 0x11223344)

        arithmetic_vectors = (
            (0x1c, 0x3f800000, 0x40000000, 0x40400000),
            (0x1d, 0x40400000, 0x3f800000, 0x40000000),
            (0x1e, 0x40000000, 0x40400000, 0x40c00000),
            (0x1f, 0x40c00000, 0x40000000, 0x40400000),
        )
        for command, ar, br, expected in arithmetic_vectors:
            self.tfpu_write32(p, 4, ar)
            self.tfpu_write32(p, 0, br)
            self.tfpu_command(p, command)
            self.tfpu_assert32(p, 4, expected)

        self.tfpu_write32(p, 4, 0x3fc00000)
        self.tfpu_write32(p, 0, 0xbf000000)
        self.tfpu_command(p, 0x1c)
        self.tfpu_write32(p, 0, 0x40800000)
        self.tfpu_command(p, 0x1e)
        self.tfpu_assert32(p, 4, 0x40800000)

        self.tfpu_write32(p, 4, 0x40800000)
        self.tfpu_command(p, 0x20)
        self.tfpu_assert32(p, 4, 0x40000000)

        self.tfpu_command(p, 0x32)
        self.tfpu_write32(p, 4, 0x3f800000)
        self.tfpu_write32(p, 0, 0)
        self.tfpu_command(p, 0x1f)
        self.tfpu_assert32(p, 4, 0x7f800000)
        self.tfpu_command(p, 0x33)
        self.cmp_imm(p, 7, 2)
        p.fail_if(0x78)
        self.tfpu_command(p, 0x32)

        self.tfpu_write32(p, 4, 0xbf800000)
        self.tfpu_command(p, 0x20)
        self.tfpu_assert32(p, 4, 0x7fc00000)
        self.tfpu_command(p, 0x33)
        self.cmp_imm(p, 7, 1)
        p.fail_if(0x78)
        self.tfpu_command(p, 0x32)

        compare_vectors = (
            (0x40400000, 0x40000000, 0x00),
            (0x40000000, 0x40000000, 0x08),
            (0x3f800000, 0x40000000, 0x01),
        )
        for ar, br, expected in compare_vectors:
            self.tfpu_write32(p, 4, ar)
            self.tfpu_write32(p, 0, br)
            self.tfpu_command(p, 0x21)
            self.cmp_imm(p, 7, expected)
            p.fail_if(0x78)

        self.tfpu_write32(p, 4, 0x7fc000aa)
        self.tfpu_write32(p, 0, 0x3f800000)
        self.tfpu_command(p, 0x21)
        self.cmp_imm(p, 7, 0xaa)
        p.fail_if(0x78)

        classify_vectors = (
            (0x7fc00000, 0x00),
            (0xffc00000, 0x03),
            (0x3f800000, 0x04),
            (0xbf800000, 0x06),
            (0x7f800000, 0x05),
            (0xff800000, 0x07),
            (0x00000000, 0x08),
            (0x80000000, 0x0a),
            (0x00000001, 0x0c),
            (0x80000001, 0x0e),
        )
        for value, expected in classify_vectors:
            self.tfpu_write32(p, 4, value)
            self.tfpu_command(p, 0x22)
            self.cmp_imm(p, 7, expected)
            p.fail_if(0x78)

        self.tfpu_write32(p, 4, 0x42280000)
        self.tfpu_command(p, 0x23)
        self.cmp_imm(p, 7, 42)
        p.fail_if(0x78)

        self.tfpu_write32(p, 4, 0x42280000)
        self.tfpu_command(p, 0x24)
        self.cmp_imm(p, 6, 0)
        p.fail_if(0x78)
        self.cmp_imm(p, 7, 42)
        p.fail_if(0x78)

        self.tfpu_write32(p, 4, 0x47800000)
        self.tfpu_command(p, 0x25)
        self.tfpu_assert32(p, 4, 0x00010000)

        self.tfpu_write32(p, 4, 0xc0000000)
        self.tfpu_command(p, 0x23)
        self.cmp_imm(p, 7, 0xfe)
        p.fail_if(0x78)

        self.tfpu_write32(p, 4, 0xc0000000)
        self.tfpu_command(p, 0x24)
        self.cmp_imm(p, 6, 0xff)
        p.fail_if(0x78)
        self.cmp_imm(p, 7, 0xfe)
        p.fail_if(0x78)

        self.tfpu_write32(p, 4, 0xc0000000)
        self.tfpu_command(p, 0x25)
        self.tfpu_assert32(p, 4, 0xfffffffe)

        int8_boundary_vectors = (
            (0x42fe0000, 0x7f, 0),
            (0xc3000000, 0x80, 0),
            (0x43000000, 0x7f, 1),
            (0xc3010000, 0x80, 1),
        )
        for value, expected, expected_status in int8_boundary_vectors:
            self.tfpu_command(p, 0x32)
            self.tfpu_write32(p, 4, value)
            self.tfpu_command(p, 0x23)
            self.cmp_imm(p, 7, expected)
            p.fail_if(0x78)
            self.tfpu_command(p, 0x33)
            self.cmp_imm(p, 7, expected_status)
            p.fail_if(0x78)

        rounding_vectors = (
            (0, 2),
            (1, 1),
            (2, 1),
            (3, 2),
        )
        for control, expected in rounding_vectors:
            self.mov_imm(p, 7, control)
            self.tfpu_command(p, 0x36)
            self.tfpu_write32(p, 4, 0x3fc00000)
            self.tfpu_command(p, 0x23)
            self.cmp_imm(p, 7, expected)
            p.fail_if(0x78)

        self.tfpu_write32(p, 4, 0x000000fe)
        self.tfpu_command(p, 0x27)
        self.tfpu_assert32(p, 4, 0xc0000000)

        self.tfpu_write32(p, 4, 0x0000fffe)
        self.tfpu_command(p, 0x28)
        self.tfpu_assert32(p, 4, 0xc0000000)

        self.tfpu_write32(p, 4, 0xfffffffe)
        self.tfpu_command(p, 0x29)
        self.tfpu_assert32(p, 4, 0xc0000000)

        trigonometric_vectors = (
            (0x2d, 0x00000000),
            (0x2e, 0x3f800000),
            (0x2f, 0x00000000),
            (0x30, 0x00000000),
        )
        for command, expected in trigonometric_vectors:
            self.tfpu_write32(p, 4, 0)
            self.tfpu_command(p, command)
            self.tfpu_assert32(p, 4, expected)

        negative_zero_vectors = (
            (0x2d, 0x80000000),
            (0x2e, 0x3f800000),
            (0x2f, 0x80000000),
            (0x30, 0x80000000),
        )
        for command, expected in negative_zero_vectors:
            self.tfpu_write32(p, 4, 0x80000000)
            self.tfpu_command(p, command)
            self.tfpu_assert32(p, 4, expected)

        self.tfpu_command(p, 0x31)
        self.tfpu_command(p, 0x33)
        self.cmp_imm(p, 7, 1)
        p.fail_if(0x78)
        self.tfpu_command(p, 0x32)
        self.tfpu_command(p, 0x33)
        self.cmp_imm(p, 7, 0)
        p.fail_if(0x78)

        self.mov_imm(p, 7, 0x15)
        self.tfpu_command(p, 0x34)
        self.mov_imm(p, 7, 0)
        self.tfpu_command(p, 0x33)
        self.cmp_imm(p, 7, 0x15)
        p.fail_if(0x78)

        self.mov_imm(p, 7, 3)
        self.tfpu_command(p, 0x36)
        self.mov_imm(p, 7, 0)
        self.tfpu_command(p, 0x35)
        self.cmp_imm(p, 7, 3)
        p.fail_if(0x78)

        self.tfpu_command(p, 0x3e)
        self.tfpu_command(p, 0x3f)
        self.assert_direct(p, 0xed, 0x3f)

        self.emit_text(p, b'TFPU-PASS\n')
        p.branch(0x80, 'done')

        p.label('fail')
        self.emit_text(p, b'TFPU-FAIL\n')
        p.label('done')
        p.branch(0x80, 'done')

        code = p.finish(self.RESET_PC)
        image = bytearray(self.FLASH_SIZE)
        image[self.RESET_PC - self.FLASH_BASE:
              self.RESET_PC - self.FLASH_BASE + len(code)] = code
        path = Path(self.scratch_file('stc32-tfpu.bin'))
        path.write_bytes(image)
        return path

    def build_interrupt_firmware(self, mode3=False):
        image = bytearray(self.FLASH_SIZE)

        def write(address, data):
            start = address - self.FLASH_BASE
            image[start:start + len(data)] = data

        write(self.RESET_PC, bytes((0x02, 0x01, 0x00)))
        write(self.RESET_PC + 0x0b, bytes((0x02, 0x02, 0x00)))

        main = SourceProgram()
        main.emit(0x75, 0x20, 0x00)
        main.emit(0x75, 0x89, 0x03 if mode3 else 0x01)
        main.emit(0x75, 0x8c, 0xff)
        main.emit(0x75, 0x8a, 0xfe)
        if mode3:
            main.emit(0x75, 0xa8, 0x02)
            main.emit(0x75, 0xa8, 0x00)
        else:
            main.emit(0x75, 0xa8, 0x82)
        main.emit(0x75, 0x88, 0x10)
        main.label('poll')
        main.emit(0xe5, 0x20)
        main.branch(0x60, 'poll')
        self.emit_text(main, b'IRQ-PASS\n')
        main.label('done')
        main.branch(0x80, 'done')
        write(self.RESET_PC + 0x100,
              main.finish(self.RESET_PC + 0x100))

        handler = bytes((
            0x75, 0x88, 0x00,
            0x75, 0x20, 0x01,
            0x32,
        ))
        write(self.RESET_PC + 0x200, handler)

        filename = ('stc32-mode3-interrupt.bin' if mode3
                    else 'stc32-interrupt.bin')
        path = Path(self.scratch_file(filename))
        path.write_bytes(image)
        return path

    def test_source_binary_and_escape_maps(self):
        firmware = self.build_firmware()

        self.set_machine('stc32g144k246')
        self.vm.add_args('-bios', str(firmware))
        self.vm.set_console()
        self.vm.launch()

        wait_for_console_pattern(self, 'PASS', failure_message='FAIL')

    def test_intel_hex_firmware(self):
        firmware = self.convert_to_ihex(self.build_firmware())

        self.set_machine('stc32g144k246')
        self.vm.add_args('-bios', str(firmware))
        self.vm.set_console()
        self.vm.launch()

        wait_for_console_pattern(self, 'PASS', failure_message='FAIL')

    def test_classic_instruction_set(self):
        firmware = self.build_classic_firmware()

        self.set_machine('stc32g144k246')
        self.vm.add_args('-bios', str(firmware))
        self.vm.set_console()
        self.vm.launch()

        wait_for_console_pattern(self, 'CLASSIC-PASS',
                                 failure_message='CLASSIC-FAIL')

    def test_native_instruction_set(self):
        firmware = self.build_native_firmware()

        self.set_machine('stc32g144k246')
        self.vm.add_args('-bios', str(firmware))
        self.vm.set_console()
        self.vm.launch()

        wait_for_console_pattern(self, 'NATIVE-PASS',
                                 failure_message='NATIVE-FAIL')

    def test_tfpu_command_set(self):
        firmware = self.build_tfpu_firmware()

        self.set_machine('stc32g144k246')
        self.vm.add_args('-bios', str(firmware))
        self.vm.set_console()
        self.vm.launch()

        wait_for_console_pattern(self, 'TFPU-PASS',
                                 failure_message='TFPU-FAIL')

    def test_timer_interrupt_entry_and_reti(self):
        firmware = self.build_interrupt_firmware()
        log = Path(self.scratch_file('mcs251-interrupt.log'))

        self.set_machine('stc32g144k246')
        self.vm.add_args('-bios', str(firmware),
                         '-d', 'int,cpu_reset,trace:mcs51_*',
                         '-D', str(log))
        self.vm.set_console()
        self.vm.launch()

        wait_for_console_pattern(self, 'IRQ-PASS')
        self.vm.shutdown()

        contents = log.read_text()
        self.assertIn('mcs251-cpu: CPU 0 reset', contents)
        self.assertIn('mcs251-cpu: CPU 0 taking IRQ 1', contents)
        self.assertIn('mcs251-cpu: CPU 0 RETI', contents)
        self.assertIn('mcs51_cpu_reset', contents)
        self.assertIn('mcs51_irq_set', contents)
        self.assertIn('mcs51_irq_take', contents)
        self.assertIn('mcs51_irq_return', contents)

    def test_timer0_mode3_interrupt_is_latched_nmi(self):
        firmware = self.build_interrupt_firmware(mode3=True)

        self.set_machine('stc32g144k246')
        self.vm.add_args('-bios', str(firmware))
        self.vm.set_console()
        self.vm.launch()

        wait_for_console_pattern(self, 'IRQ-PASS')


if __name__ == '__main__':
    QemuSystemTest.main()
