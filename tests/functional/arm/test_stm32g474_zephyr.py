#!/usr/bin/env python3
#
# Zephyr functional tests for the STM32G474 machines
#
# SPDX-License-Identifier: GPL-2.0-or-later

from qemu_test import Asset, QemuSystemTest
from qemu_test import exec_command_and_wait_for_pattern
from qemu_test import wait_for_console_pattern


class Stm32g474Zephyr(QemuSystemTest):

    ASSET_URL = (
        'https://github.com/processmission/qemu/releases/download/'
        'stm32g474-ardep-v2-zephyr-qemu1/'
    )
    ASSET_HELLO = Asset(
        ASSET_URL + 'stm32g474-zephyr-hello.bin',
        '91fdf60e266ffb44baef6c2fece2c5c202944c7bc8d9445cb113b212da72dcd2')
    ASSET_ARDEP_UART = Asset(
        ASSET_URL + 'ardep-v2-zephyr-uart.bin',
        '7839cba2daac2451a78ece9af472f7128206db890396cbf6aef10a4a7393e07a')
    ASSET_ARDEP_MCUBOOT = Asset(
        ASSET_URL + 'ardep-v2-mcuboot-led-flash.bin',
        '785eac3636169f6f1205b6ddd8055e605a439320bb8c1c4ad08241ded2a68146')

    def launch_firmware(self, machine, asset):
        self.set_machine(machine)
        self.require_accelerator('tcg')
        self.vm.set_console()
        self.vm.add_args(
            '-accel', 'tcg',
            '-kernel', asset.fetch(),
            '-display', 'none',
            '-monitor', 'none',
        )
        self.vm.launch()

    def wait_for_zephyr(self):
        return wait_for_console_pattern(
            self, '*** Booting Zephyr OS build 684c9e8f32e4 ***')

    def wait_for_zephyr_hello(self):
        hello = b'Hello World! ardep@2.0.0/stm32g474xx'
        output = self.wait_for_zephyr()

        if hello not in output:
            wait_for_console_pattern(self, hello.decode())

    def test_stm32g474_zephyr_boot(self):
        self.launch_firmware('stm32g474', self.ASSET_HELLO)
        self.wait_for_zephyr_hello()

    def test_ardep_v2_zephyr_boot(self):
        self.launch_firmware('ardep-v2', self.ASSET_HELLO)
        self.wait_for_zephyr_hello()

    def test_ardep_v2_uart(self):
        self.launch_firmware('ardep-v2', self.ASSET_ARDEP_UART)
        self.wait_for_zephyr()
        wait_for_console_pattern(self, 'Enter a number:')
        exec_command_and_wait_for_pattern(
            self, '3', 'The square of your number: 9.000000')
        exec_command_and_wait_for_pattern(
            self, '4', 'The square of your number: 16.000000')

    def test_ardep_v2_mcuboot(self):
        self.launch_firmware('ardep-v2', self.ASSET_ARDEP_MCUBOOT)
        wait_for_console_pattern(
            self, '*** Booting MCUboot ee39e2d694bd ***')
        wait_for_console_pattern(
            self, 'I: Bootloader chainload address offset: 0x18000')
        wait_for_console_pattern(
            self, 'I: Jumping to the first image slot')
        self.wait_for_zephyr()
        wait_for_console_pattern(self, 'LED sample')


if __name__ == '__main__':
    QemuSystemTest.main()
