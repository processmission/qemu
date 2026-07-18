#!/usr/bin/env python3
#
# Functional tests for the Radxa ROCK 5B+
#
# SPDX-License-Identifier: GPL-2.0-or-later

from qemu_test import Asset, LinuxKernelTest
from qemu_test import exec_command_and_wait_for_pattern


class Rock5BPlusMachine(LinuxKernelTest):

    ASSET_URL = (
        'https://github.com/processmission/qemu/releases/download/'
        'rock-5b-plus-rsdk-b2-qemu1/'
    )
    ASSET_KERNEL = Asset(
        ASSET_URL + 'Image-6.1.43-15-rk2312',
        '356a9085a1bc5144aeb19c7eb0bb21ab446ad26d24074e55be0b5c901b64510a')
    ASSET_INITRAMFS = Asset(
        ASSET_URL + 'rock-5b-plus-initramfs.cpio.gz',
        '61167f83a4dad3de82a7f91a3cd00aaf5aa31210b8e00be8ebdd2cd77138b520')
    ASSET_FIRMWARE_DISK = Asset(
        ASSET_URL + 'rock-5b-plus-rsdk-b2-linux.raw.xz',
        'f63953e48e830354f5f31e90382f1e1171b919d20aaa737ac20a1d5070759342')

    KERNEL_COMMAND_LINE = (
        'console=ttyS2,1500000n8 '
        'earlycon=uart8250,mmio32,0xfeb50000,1500000n8 '
        'keep_bootcon loglevel=8 rdinit=/init'
    )

    def setUp(self):
        super().setUp()
        self.set_machine('rock-5b-plus')
        self.require_accelerator('tcg')
        self.vm.set_console()

    def add_common_args(self):
        self.vm.add_args(
            '-accel', 'tcg',
            '-smp', '1',
            '-m', '1G',
            '-display', 'none',
            '-monitor', 'none',
            '-no-reboot',
        )

    def wait_for_linux_shell(self):
        self.wait_for_console_pattern('Linux version 6.1.43-15-rk2312')
        self.wait_for_console_pattern('Machine model: Radxa ROCK 5B+')
        self.wait_for_console_pattern('Run /init as init process')
        self.wait_for_console_pattern('=== init done, exec sh ===')
        self.wait_for_console_pattern('~ #')
        exec_command_and_wait_for_pattern(
            self, 'uname -r', '6.1.43-15-rk2312')

    def test_aarch64_rock5b_plus_direct_linux_boot(self):
        kernel_path = self.ASSET_KERNEL.fetch()
        initramfs_path = self.ASSET_INITRAMFS.fetch()

        self.add_common_args()
        self.vm.add_args(
            '-kernel', kernel_path,
            '-initrd', initramfs_path,
            '-append', self.KERNEL_COMMAND_LINE,
        )

        self.vm.launch()
        self.wait_for_linux_shell()

    def test_aarch64_rock5b_plus_firmware_linux_boot(self):
        disk_path = self.uncompress(
            self.ASSET_FIRMWARE_DISK,
            target='rock-5b-plus-rsdk-b2-linux.raw')

        self.add_common_args()
        self.vm.add_args(
            '-drive',
            f'file={disk_path},if=sd,index=0,format=raw,snapshot=on',
        )

        self.vm.launch()
        self.wait_for_console_pattern('DDR 9fffbe1e78')
        self.wait_for_console_pattern('U-Boot SPL rknext-2017.09-33')
        for component in ('atf-1', 'uboot', 'fdt', 'atf-2', 'atf-3'):
            self.wait_for_console_pattern(f'## Checking {component}')
            self.wait_for_console_pattern('+ OK')
        self.wait_for_console_pattern(
            'Jumping to U-Boot(0x00200000) via ARM Trusted Firmware')
        self.wait_for_console_pattern('U-Boot rknext-2017.09-33')
        self.wait_for_console_pattern('Found /boot/extlinux/extlinux.conf')
        self.wait_for_console_pattern('Starting kernel ...')
        self.wait_for_linux_shell()


if __name__ == '__main__':
    LinuxKernelTest.main()
