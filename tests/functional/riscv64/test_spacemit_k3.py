#!/usr/bin/env python3
#
# Functional test for the SpacemiT K3 Pico-ITX machine
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Boot the pinned SpacemiT K3 SDK Linux image on k3-pico-itx.
"""

from qemu_test import Asset, QemuSystemTest
from qemu_test import wait_for_console_pattern


class SpacemitK3Test(QemuSystemTest):
    """Test the Linux-first K3 Pico-ITX boot path."""

    RELEASE_URL = (
        'https://github.com/zevorn/spacemit-k3-qemu-images/releases/'
        'download/sdk-v1.0.2-qemu2/'
    )

    ASSET_KERNEL = Asset(
        RELEASE_URL + 'Image',
        'cc7359e16ba6569fee49be475d561ab42f90e0aa0e9fb7fc1ead4ab1be066efd')
    ASSET_FIRMWARE = Asset(
        RELEASE_URL + 'fw_dynamic.bin',
        'e62c515fa5aa5fadee169a316c30f99f365b09ae2eff72c783d936b0b337e0a1')
    ASSET_INITRAMFS = Asset(
        RELEASE_URL + 'k3-qemu-initramfs.cpio.gz',
        '42670a19d756fc239376774577cc267b26fdec872827eed3a02139e8e5ce11e3')
    ASSET_DTB = Asset(
        RELEASE_URL + 'k3-pico-itx-qemu.dtb',
        '35e2fc80a64e41963d091cb23b142ed6dfc1e56f9e808061c6e241d7da4bf56e')

    def _wait_for_linux_boot(self):
        panic = 'Kernel panic - not syncing'
        expected = (
            'Linux version 6.18.3-g0ffac20d9ef9',
            ('Machine model: SpacemiT K3 Pico-ITX '
             '(QEMU Linux-first subset)'),
            'SBI specification v2.0 detected',
            'SBI implementation ID=0x1 Version=0x10006',
            ('riscv-timer: Timer interrupt in S-mode is available '
             'via sstc extension'),
            'smp: Brought up 1 node, 8 CPUs',
            ('riscv-imsic: interrupt-controller@e0400000: '
             'per-CPU IDs 511'),
            ('riscv-aplic e0804000.interrupt-controller: '
             '512 interrupts forwarded'),
            'd4017000.serial: ttyS0 at MMIO 0xd4017000',
            ('K3-QEMU: model=SpacemiT K3 Pico-ITX '
             '(QEMU Linux-first subset)'),
            'K3-QEMU: cpus=8',
            'K3-QEMU: Linux boot successful',
            'K3_LINUX_MVP_PASS',
        )

        for pattern in expected:
            wait_for_console_pattern(self, pattern, panic)

    def test_linux_boot(self):
        self.set_machine('k3-pico-itx')

        kernel_path = self.ASSET_KERNEL.fetch()
        firmware_path = self.ASSET_FIRMWARE.fetch()
        initramfs_path = self.ASSET_INITRAMFS.fetch()
        dtb_path = self.ASSET_DTB.fetch()

        kernel_command_line = (
            'earlycon=uart8250,mmio32,0xd4017000,115200 '
            'console=ttyS0,115200 rdinit=/init'
        )
        self.vm.add_args('-bios', firmware_path,
                         '-kernel', kernel_path,
                         '-initrd', initramfs_path,
                         '-dtb', dtb_path,
                         '-append', kernel_command_line,
                         '-no-reboot')
        self.vm.set_console()
        self.vm.launch()
        self._wait_for_linux_boot()


if __name__ == '__main__':
    QemuSystemTest.main()
