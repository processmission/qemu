#!/usr/bin/env python3
#
# Functional tests for the STM32G474 machines
#
# SPDX-License-Identifier: GPL-2.0-or-later

import struct
from pathlib import Path

from qemu_test import QemuSystemTest, wait_for_console_pattern


FLASH_BASE = 0x08000000
SRAM_END = 0x20020000
RCC_APB1ENR1 = 0x40021058
RCC_APB1ENR1_UART4EN = 1 << 19
UART4_BASE = 0x40004C00


def make_uart_firmware(message):
    code = []
    literal_loads = []

    def emit(word):
        code.append(word)

    def emit_ldr_literal(reg, value):
        literal_loads.append((len(code), reg, value))
        emit(0)

    def emit_movs(reg, value):
        emit(0x2000 | (reg << 8) | value)

    def emit_str(reg, base, offset):
        assert offset % 4 == 0
        emit(0x6000 | ((offset // 4) << 6) | (base << 3) | reg)

    emit_ldr_literal(0, RCC_APB1ENR1)
    emit_ldr_literal(1, RCC_APB1ENR1_UART4EN)
    emit_str(1, 0, 0)
    emit_ldr_literal(0, UART4_BASE)
    emit_movs(1, 139)
    emit_str(1, 0, 0x0C)
    emit_movs(1, 9)
    emit_str(1, 0, 0)

    for byte in message.encode("ascii"):
        emit_movs(1, byte)
        emit_str(1, 0, 0x28)

    emit(0xE7FE)

    code_offset = 8
    if (code_offset + len(code) * 2) % 4:
        emit(0xBF00)
    literal_offset = code_offset + len(code) * 2

    for literal_index, (word_index, reg, _) in enumerate(literal_loads):
        instruction_offset = code_offset + word_index * 2
        pc = (instruction_offset + 4) & ~3
        target = literal_offset + literal_index * 4
        displacement = target - pc
        assert displacement % 4 == 0
        assert 0 <= displacement // 4 <= 0xFF
        code[word_index] = 0x4800 | (reg << 8) | (displacement // 4)

    image = bytearray(struct.pack("<II", SRAM_END, FLASH_BASE + 9))
    image.extend(struct.pack(f"<{len(code)}H", *code))
    image.extend(struct.pack(
        f"<{len(literal_loads)}I",
        *(value for _, _, value in literal_loads),
    ))
    return image


class Stm32g474Machine(QemuSystemTest):

    def boot_and_reset(self, machine, marker):
        firmware = self.scratch_file(f"{machine}.bin")
        Path(firmware).write_bytes(make_uart_firmware(marker + "\n"))

        self.set_machine(machine)
        self.vm.set_console()
        self.vm.add_args("-kernel", firmware)
        self.vm.launch()

        wait_for_console_pattern(self, marker)
        self.vm.qmp("system_reset")
        wait_for_console_pattern(self, marker)

    def test_stm32g474_raw_boot(self):
        self.boot_and_reset("stm32g474", "STM32G474 functional boot")

    def test_ardep_v2_raw_boot(self):
        self.boot_and_reset("ardep-v2", "ARDEP V2 functional boot")


if __name__ == "__main__":
    QemuSystemTest.main()
