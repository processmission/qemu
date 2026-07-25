#!/usr/bin/env python3
#
# Functional test for the RK3588 RKNPU
#
# SPDX-License-Identifier: GPL-2.0-or-later

from qemu_test import Asset, LinuxKernelTest


class RK3588RKNPU(LinuxKernelTest):

    ASSET_URL = (
        'https://github.com/Ivans-11/rk3588-rknpu-test-assets/'
        'releases/download/v1.0.1/'
    )
    ASSET_KERNEL = Asset(
        ASSET_URL + 'Image-6.1.43-rockchip-rk3588',
        '23e766cb34bc26ebc4ca4361a2ee7041ce1264ba29b43e146f8030a9bb3e324f')
    ASSET_INITRAMFS = Asset(
        ASSET_URL + 'rk3588-rknpu-yolov8n-initramfs.cpio.gz',
        '1dd5e82fd7ff9afcbb370047785f8aec0d553fe9b79590306df3f89583209cc8')

    KERNEL_COMMAND_LINE = (
        'console=ttyS2,1500000n8 '
        'earlycon=uart8250,mmio32,0xfeb50000,1500000n8 '
        'rdinit=/init loglevel=8 '
        'initcall_blacklist=rk_gmac_dwmac_driver_init'
    )

    def test_aarch64_rk3588_rknpu_yolov8n(self):
        kernel_path = self.ASSET_KERNEL.fetch()
        initramfs_path = self.ASSET_INITRAMFS.fetch()

        self.set_machine('rk3588-evb')
        self.require_accelerator('tcg')
        self.vm.set_machine('rk3588-evb,rknpu=on')
        self.vm.set_console()
        self.vm.add_args(
            '-accel', 'tcg',
            '-smp', '4',
            '-m', '2G',
            '-kernel', kernel_path,
            '-initrd', initramfs_path,
            '-append', self.KERNEL_COMMAND_LINE,
            '-display', 'none',
            '-monitor', 'none',
            '-no-reboot',
        )

        self.vm.launch()
        self.wait_for_console_pattern('Machine model: QEMU Rockchip RK3588 EVB')
        self.wait_for_console_pattern('[drm] Initialized rknpu 0.9.6')
        self.wait_for_console_pattern('model input num: 1, output num: 9')
        self.wait_for_console_pattern('inference_yolov8_model success!')
        self.wait_for_console_pattern('Detected objects: 1')
        self.wait_for_console_pattern('[1] sports ball')
        self.wait_for_console_pattern('Confidence: 70.8%')
        self.wait_for_console_pattern('Position: (479, 706, 773, 1010)')
        self.wait_for_regex_console_pattern(
            r'RK3588-RKNPU-TEST workload exit=0(?:\D|$)',
            r'RK3588-RKNPU-TEST workload '
            r'(?:exit=[1-9][0-9]*|signal=[1-9][0-9]*)',
            timeout=300)


if __name__ == '__main__':
    LinuxKernelTest.main()
