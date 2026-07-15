#!/usr/bin/env python3
#
# Functional test that directly boots an AX650X Ubuntu eMMC image
#
# SPDX-License-Identifier: GPL-2.0-or-later

from qemu_test import Asset, LinuxKernelTest


class AX650XUbuntuMachine(LinuxKernelTest):

    ASSET_URL = (
        'https://github.com/processmission/qemu/releases/download/'
        'ax650x-ubuntu-22.04-qemu1/'
    )
    ASSET_KERNEL = Asset(
        ASSET_URL + 'Image-5.15.73-axera',
        '9e3dc56e13cb5e786864d3456ceecfe7560d11fd25e9688531f86c0748be9bf8')
    ASSET_EMMC = Asset(
        ASSET_URL + 'ax650x-ubuntu-22.04-emmc.qcow2',
        'e362cff67d7358bc434394649f91d0b2424156eb9cd3b518438cf32fe3b9800c')

    PARTS = (
        'mmcblk0:1536K(uboot),1536K(uboot_bk),1M(env),20M(param),'
        '6M(logo),1M(dtb),64M(kernel),1M(atf),1M(optee),'
        '1M(recovery_dtb),74M(recovery),30380032K(rootfs)'
    )

    def test_aarch64_ax650x_ubuntu_quick_boot(self):
        kernel_path = self.ASSET_KERNEL.fetch()
        image_path = self.ASSET_EMMC.fetch()

        self.set_machine('ax650x-pyramid')
        self.require_accelerator('tcg')
        self.vm.set_console()

        kernel_command_line = (
            'console=ttyS0,115200n8 '
            'earlycon=uart8250,mmio32,0x2016000 '
            'root=/dev/mmcblk0p12 rootfstype=ext4 rw rootwait '
            f'blkdevparts={self.PARTS} '
            'systemd.show_status=yes systemd.log_target=console'
        )
        self.vm.add_args(
            '-accel', 'tcg,thread=multi',
            '-cpu', 'cortex-a55',
            '-smp', '8',
            '-m', '2G',
            '-kernel', kernel_path,
            '-append', kernel_command_line,
            '-drive', f'file={image_path},if=sd,format=qcow2,snapshot=on',
            '-display', 'none',
            '-monitor', 'none',
            '-no-reboot',
        )

        self.vm.launch()
        self.wait_for_console_pattern('DWMAC4/5')
        self.wait_for_console_pattern('p12(rootfs)')
        self.wait_for_console_pattern(
            'Mounted root (ext4 filesystem) on device 179:12')
        self.wait_for_console_pattern('AX650X_UBUNTU_READY')
        self.wait_for_console_pattern('AX650X_OS=Ubuntu 22.04.5 LTS')
        self.wait_for_console_pattern('AX650X_CPUS=8')
        self.wait_for_console_pattern('AX650X_ROOT=/dev/mmcblk0p12 ext4')
        self.wait_for_console_pattern('localhost login:')


if __name__ == '__main__':
    LinuxKernelTest.main()
