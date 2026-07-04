#!/usr/bin/env python3
#
# Functional test that assembles the full ZVM RK3588S ROC PC release image
# from remote assets and boots the bundled Linux guest.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import hashlib
import logging
import os
from pathlib import Path
import platform
import struct
import subprocess
import zlib

from qemu_test import Asset, QemuSystemTest, get_qemu_img, which


class RK3588ZVMLinuxGuest(QemuSystemTest):

    ASSET_KERNEL_IMAGES = Asset(
        'https://esnl.hnu.edu.cn/zvm/resource/kernel_images.zip',
        '05aa22b772808abef7082d3dfc513ea75b0813f3e0d4f028ad595e2c86fe3ec7')
    ASSET_FILESYSTEM_PART1 = Asset(
        'https://esnl.hnu.edu.cn/zvm/resource/file_system_images_part1.zip',
        '9a1840d8d486b2de8e8bedc1647cd3c5aa2f622111b45bb9f0d8e1d26b07c5f0')
    ASSET_FILESYSTEM_PART2 = Asset(
        'https://esnl.hnu.edu.cn/zvm/resource/file_system_images_part2.zip',
        '17b048b0a473dcbd4e5b7f1c5d4f50547ffefe3e040c0b8835e55cba7f45b823')

    SOURCE_ROOT = Path(__file__).resolve().parents[3]
    ZVM_NO_FS_IMAGE = (SOURCE_ROOT / 'roms' / 'zvm-rk3588s-roc-pc' /
                       'rk3588s-zvm-no-fs.raw.zst')
    MAKE_IMAGE_SCRIPT = (SOURCE_ROOT / 'scripts' / 'zvm' /
                         'rk3588s-roc-pc-make-zvm-image.sh')

    UBOOT_ROCKCHIP_OFFSET = 64 * 512
    UBOOT_ROCKCHIP_SIZE = 9456128
    UBOOT_ROCKCHIP_SHA256 = \
        '6cd4b3d1931a252d564c88220e071eba57009fff49620b9520f33e8c58f80fb5'
    BOOT_SCRIPT_SHA256 = \
        '5402d2ea0e5a9a65e577799283ff46fd596914351769c3015d30d6285c787171'
    BOOT_SCRIPT_TIMESTAMP = 0x6a2fe158

    KERNEL_HASHES = {
        'boot.scr':
            '3f002128aef0fd5d9e5792e2643697b027dda1767efa43dd718439b13d4a2a63',
        'boot.txt':
            'c183dd5e0676a714c7a03781d0788961d797856a529e857e8eeed6ef18bda557',
        'nrtos_images.bin':
            '0f40c4e0d07aad5b5bef2b7528dbf3c395f95d0129a0f0a91d81ca9bbfb79a26',
        'raw_kernel_images/Image':
            '0eca25c10fb39e1200ccd097f6de2cfc767dcd0abd0360645fe816d17869f2e9',
        'zvm_release_rk3588v2.bin':
            'e8851ef0a8f413d98554218248411162024d5e9ba47f90f4aa0908710d193380',
    }
    FILESYSTEM_HASHES = {
        'android_vda.img':
            '8c1bc5c3568fcbc74c04f8862dea1687baf93e6dcec0c0b18aef494bf2d00c10',
        'oh_vda.bin':
            '7e50e7ac8cd5b90d087f09b0c4ebfacb13b9b4aeb7651e191d376b595bf6d9e2',
        'linux_diskimg.bin':
            '2a59a5503a3e43184adccdb6288a9c9b26599882eacbe387b5db3d408f85891f',
    }

    BOOT_ATTEMPTS = 8
    SPL_FAILURE = 'SPL: failed to boot from all boot devices'
    BOOT_FAILURES = (
        SPL_FAILURE,
        'load_linux_image_form_disk error',
        'load linux images error',
        'Failed to run linux VM 1',
        'Timeout waiting for vm: linux_os-1 start',
    )

    def local_release_dirs(self):
        release_root = os.getenv('ZVM_RK3588_RELEASE_DIR')
        kernel_dir = os.getenv('ZVM_RK3588_KERNEL_DIR')
        filesystem_dir = os.getenv('ZVM_RK3588_FILESYSTEM_DIR')

        if release_root:
            root = Path(release_root)
            if not kernel_dir:
                kernel_dir = str(root / 'kernel_images')
            if not filesystem_dir:
                for candidate in (
                        root / 'file_system_images_official' / 'image',
                        root / 'file_system_images' / 'image',
                        root / 'image'):
                    if candidate.is_dir():
                        filesystem_dir = str(candidate)
                        break

        if not kernel_dir or not filesystem_dir:
            return None

        kernel_path = Path(kernel_dir)
        filesystem_path = Path(filesystem_dir)
        if not kernel_path.is_dir() or not filesystem_path.is_dir():
            return None
        return kernel_path, filesystem_path

    def assets_available(self):
        if self.local_release_dirs():
            return True
        return super().assets_available()

    @staticmethod
    def sha256_file(path):
        digest = hashlib.sha256()
        with open(path, 'rb') as stream:
            for chunk in iter(lambda: stream.read(1 << 20), b''):
                digest.update(chunk)
        return digest.hexdigest()

    def require_commands(self, commands):
        missing = [command for command in commands if not which(command)]
        if missing:
            self.skipTest('required command(s) not installed: ' +
                          ', '.join(missing))

    def require_image_tools(self):
        self.require_commands(('dd', 'python3', 'truncate', 'zstd'))

        host_os = platform.system()
        if host_os == 'Linux':
            self.require_commands(('mkfs.vfat', 'mcopy'))
        elif host_os == 'Darwin':
            self.require_commands(('hdiutil', 'newfs_msdos'))
        else:
            self.skipTest(f'unsupported host OS for image assembly: {host_os}')

    def sevenzip(self):
        for command in ('7z', '7zz'):
            path = which(command)
            if path:
                return path
        self.skipTest('required command(s) not installed: 7z or 7zz')

    def validate_release_inputs(self, kernel_dir, filesystem_dir):
        for name, expected in self.KERNEL_HASHES.items():
            path = kernel_dir / name
            self.assertTrue(path.is_file(), f'missing {path}')
            self.assertEqual(expected, self.sha256_file(path),
                             f'unexpected hash for {path}')

        for name, expected in self.FILESYSTEM_HASHES.items():
            path = filesystem_dir / name
            self.assertTrue(path.is_file(), f'missing {path}')
            self.assertEqual(expected, self.sha256_file(path),
                             f'unexpected hash for {path}')

    def prepare_release_dirs(self):
        local_dirs = self.local_release_dirs()
        if local_dirs:
            kernel_dir, filesystem_dir = local_dirs
            self.validate_release_inputs(kernel_dir, filesystem_dir)
            return kernel_dir, filesystem_dir

        release_dir = Path(self.scratch_file('zvm-release'))
        filesystem_split_dir = Path(self.scratch_file('zvm-filesystems'))
        release_dir.mkdir(parents=True, exist_ok=True)
        filesystem_split_dir.mkdir(parents=True, exist_ok=True)

        self.archive_extract(self.ASSET_KERNEL_IMAGES, sub_dir='zvm-release',
                             format='zip')
        self.archive_extract(self.ASSET_FILESYSTEM_PART1,
                             sub_dir='zvm-filesystems', format='zip')
        self.archive_extract(self.ASSET_FILESYSTEM_PART2,
                             sub_dir='zvm-filesystems', format='zip')

        subprocess.run([self.sevenzip(), 'x', '-y', 'image.7z.001'],
                       cwd=filesystem_split_dir, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True)

        kernel_dir = release_dir / 'kernel_images'
        filesystem_dir = filesystem_split_dir / 'image'
        self.validate_release_inputs(kernel_dir, filesystem_dir)
        return kernel_dir, filesystem_dir

    def extract_uboot_rockchip(self):
        raw_path = self.scratch_file('rk3588s-zvm-no-fs.raw')
        subprocess.run(['zstd', '--sparse', '-f', '-d',
                        str(self.ZVM_NO_FS_IMAGE), '-o', raw_path],
                       check=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)

        uboot_path = Path(self.scratch_file('u-boot-rockchip.bin'))
        with open(raw_path, 'rb') as raw, uboot_path.open('wb') as uboot:
            raw.seek(self.UBOOT_ROCKCHIP_OFFSET)
            data = raw.read(self.UBOOT_ROCKCHIP_SIZE)
            self.assertEqual(self.UBOOT_ROCKCHIP_SIZE, len(data),
                             'short u-boot-rockchip.bin extract')
            uboot.write(data)

        self.assertEqual(self.UBOOT_ROCKCHIP_SHA256,
                         self.sha256_file(uboot_path),
                         f'unexpected hash for {uboot_path}')
        return uboot_path

    def generate_boot_script(self, kernel_dir):
        boot_txt = kernel_dir / 'boot.txt'
        script = boot_txt.read_bytes()
        data = struct.pack('>II', len(script), 0) + script
        name = b'boot (Auto Generated)'
        name = name + b'\0' * (32 - len(name))
        data_crc = zlib.crc32(data) & 0xffffffff

        header = struct.pack('>IIIIIIIBBBB32s',
                             0x27051956, 0, self.BOOT_SCRIPT_TIMESTAMP,
                             len(data), 0, 0, data_crc,
                             5, 2, 6, 0, name)
        header_crc = zlib.crc32(header) & 0xffffffff
        header = struct.pack('>IIIIIIIBBBB32s',
                             0x27051956, header_crc,
                             self.BOOT_SCRIPT_TIMESTAMP,
                             len(data), 0, 0, data_crc,
                             5, 2, 6, 0, name)

        boot_script = Path(self.scratch_file('boot.scr'))
        boot_script.write_bytes(header + data)
        self.assertEqual(self.BOOT_SCRIPT_SHA256,
                         self.sha256_file(boot_script),
                         f'unexpected hash for {boot_script}')
        return boot_script

    def build_card_image(self):
        self.require_image_tools()
        kernel_dir, filesystem_dir = self.prepare_release_dirs()
        uboot_path = self.extract_uboot_rockchip()
        boot_script = self.generate_boot_script(kernel_dir)
        image_path = self.scratch_file('rk3588s-zvm-linux-guest.raw')

        subprocess.run([str(self.MAKE_IMAGE_SCRIPT),
                        '--kernel-dir', str(kernel_dir),
                        '--filesystem-dir', str(filesystem_dir),
                        '--uboot-rockchip', str(uboot_path),
                        '--boot-script', str(boot_script),
                        '--output', image_path,
                        '--force'],
                       check=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
        return image_path

    def image_for_attempt(self, base_image_path, attempt):
        image_path = self.scratch_file(f'rk3588s-zvm-linux-guest-{attempt}.raw')
        subprocess.run([get_qemu_img(self), 'convert',
                        '-f', 'raw',
                        '-O', 'raw',
                        '-S', '4k',
                        base_image_path,
                        image_path],
                       check=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
        return image_path

    def wait_for_console_patterns(self, vm, success, failures):
        success_bytes = success.encode()
        failure_bytes = [(failure, failure.encode()) for failure in failures]
        console_logger = logging.getLogger('console')
        line = b''

        while True:
            char = vm.console_socket.recv(1)
            if not char:
                self.fail(f"EOF in console, expected '{success}'")
            line += char

            if success_bytes in line:
                console_logger.debug(line.decode(errors='replace').strip())
                return

            for failure, failure_b in failure_bytes:
                if failure_b in line:
                    console_logger.debug(line.decode(errors='replace').strip())
                    vm.console_socket.close()
                    self.fail(f"'{failure}' found in console, "
                              f"expected '{success}'")

            if char == b'\n':
                console_logger.debug(line.decode(errors='replace').strip())
                line = b''

    def boot_zvm_linux_guest(self, base_image_path, attempt):
        image_path = self.image_for_attempt(base_image_path, attempt)
        vm = self.get_vm(name=f'zvm-linux-guest-{attempt}')

        vm.set_console()
        vm.add_args('-accel', 'tcg',
                    '-icount', 'shift=0,sleep=off',
                    '-smp', '8',
                    '-m', '1G',
                    '-drive',
                    f'if=sd,index=2,file={image_path},format=raw',
                    '-nic', 'user,model=gmac0',
                    '-nic', 'user,model=gmac1',
                    '-display', 'none')
        vm.launch()

        for pattern in (
                'U-Boot SPL 2026.07-rc4',
                'loading ZVM img ...',
                'Init virt syscon for ZVM successful.',
                'Start VM Successful!',
                'zephyr VM 0 created and running',
                '[CREATE] linux VM 1 created and running'):
            self.wait_for_console_patterns(vm, pattern, self.BOOT_FAILURES)

    def test_aarch64_rk3588s_roc_pc_zvm_linux_guest(self):
        self.require_accelerator('tcg')
        self.set_machine('rk3588s-roc-pc')

        base_image_path = self.build_card_image()
        for attempt in range(1, self.BOOT_ATTEMPTS + 1):
            try:
                self.boot_zvm_linux_guest(base_image_path, attempt)
                return
            except AssertionError as exc:
                if (self.SPL_FAILURE not in str(exc) or
                        attempt == self.BOOT_ATTEMPTS):
                    raise
                self.log.warning('ZVM Linux guest boot attempt %d/%d hit '
                                 'SPL MMC read failure; retrying',
                                 attempt, self.BOOT_ATTEMPTS)
                self.get_vm(name=f'zvm-linux-guest-{attempt}').shutdown()


if __name__ == '__main__':
    QemuSystemTest.main()
