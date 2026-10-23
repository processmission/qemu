#!/usr/bin/env python3
#
# Functional test for RK3588 RKNPU with YOLOv8s
#
# SPDX-License-Identifier: GPL-2.0-or-later

from qemu_test import Asset, LinuxKernelTest


class RK3588RKNPUYOLOv8s(LinuxKernelTest):

    ASSET_URL = (
        'https://github.com/processmission/qemu/releases/download/'
        'rk3588-rknpu-yolov8s-v1/'
    )
    ASSET_KERNEL = Asset(
        ASSET_URL + 'Image',
        '23e766cb34bc26ebc4ca4361a2ee7041ce1264ba29b43e146f8030a9bb3e324f')
    ASSET_ROOTFS = Asset(
        ASSET_URL + 'rootfs.cpio.gz',
        'd19f8e6efd11a0eae3881163d77141f35f202a1c9dae5ffe8012bb2ee2872725')

    KERNEL_COMMAND_LINE = (
        'console=ttyS2,1500000n8 '
        'earlycon=uart8250,mmio32,0xfeb50000,1500000n8 '
        'rdinit=/init'
    )

    def test_aarch64_rk3588_rknpu_yolov8s(self):
        kernel_path = self.ASSET_KERNEL.fetch()
        rootfs_path = self.ASSET_ROOTFS.fetch()

        self.set_machine('rock-5b-plus')
        self.require_accelerator('tcg')
        self.vm.set_machine('rock-5b-plus,rknpu=on')
        self.vm.set_console()
        self.vm.add_args(
            '-accel', 'tcg',
            '-smp', '8',
            '-m', '2G',
            '-kernel', kernel_path,
            '-initrd', rootfs_path,
            '-append', self.KERNEL_COMMAND_LINE,
            '-display', 'none',
            '-monitor', 'none',
            '-no-reboot',
        )

        self.vm.launch()
        self.wait_for_console_pattern('RK3588-RKNPU-YOLOV8S-TEST starting')
        self.wait_for_console_pattern('model input num: 1, output num: 9')
        self.wait_for_console_pattern('inference_yolov8_model success!')
        self.wait_for_console_pattern('Detected objects: 1')
        self.wait_for_console_pattern('[1] sports ball')
        self.wait_for_regex_console_pattern(
            r'Confidence: [0-9]+\.[0-9]+%',
            r'RK3588-RKNPU-YOLOV8S-TEST exit=[1-9]',
            timeout=300)
        self.wait_for_console_pattern('RK3588-RKNPU-YOLOV8S-TEST exit=0')


if __name__ == '__main__':
    LinuxKernelTest.main()
