#!/usr/bin/env python3
#
# Functional test that boots the ZVM RK3588S ROC PC release fixture.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
from pathlib import Path
import stat
import subprocess

from qemu_test import QemuSystemTest, skipIfMissingCommands
from qemu_test import wait_for_console_pattern


class RK3588ZVM(QemuSystemTest):

    ZVM_IMAGE = ('roms/zvm-rk3588s-roc-pc/'
                 'rk3588s-zvm-no-fs.raw.zst')
    BOOT_ATTEMPTS = 8

    def extract_sparse_raw(self, attempt):
        source_root = Path(__file__).resolve().parents[3]
        compressed = source_root / self.ZVM_IMAGE
        raw = self.scratch_file(f'rk3588s-zvm-no-fs-{attempt}.raw')

        subprocess.run(['zstd', '--sparse', '-f', '-d', str(compressed),
                        '-o', raw],
                       check=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
        os.chmod(raw, stat.S_IRUSR | stat.S_IWUSR)
        return raw

    def boot_zvm_release(self, attempt):
        image_path = self.extract_sparse_raw(attempt)
        vm = self.get_vm(name=f'zvm-boot-{attempt}')

        vm.set_console()
        vm.add_args('-accel', 'tcg',
                    '-icount', 'shift=0,sleep=off',
                    '-smp', '8',
                    '-m', '1G',
                    '-drive',
                    f'if=sd,index=2,file={image_path},format=raw',
                    '-nic', 'user,model=gmac0',
                    '-display', 'none')
        vm.launch()

        failure = 'SPL: failed to boot from all boot devices'
        wait_for_console_pattern(self, 'U-Boot SPL 2026.07-rc4', failure,
                                 vm=vm)
        wait_for_console_pattern(self, 'loading ZVM img ...', failure, vm=vm)
        wait_for_console_pattern(self,
                                 'Init virt syscon for ZVM successful.',
                                 failure, vm=vm)
        wait_for_console_pattern(self, 'Start VM Successful!', failure, vm=vm)
        wait_for_console_pattern(self,
                                 'zephyr VM 0 created and running',
                                 failure, vm=vm)

    @skipIfMissingCommands('zstd')
    def test_aarch64_rk3588s_roc_pc_zvm_release(self):
        self.require_accelerator('tcg')
        self.set_machine('rk3588s-roc-pc')

        failure = 'SPL: failed to boot from all boot devices'
        for attempt in range(1, self.BOOT_ATTEMPTS + 1):
            try:
                self.boot_zvm_release(attempt)
                return
            except AssertionError as exc:
                if failure not in str(exc) or attempt == self.BOOT_ATTEMPTS:
                    raise
                self.log.warning('ZVM boot attempt %d/%d hit SPL MMC read '
                                 'failure; retrying with a fresh raw image',
                                 attempt, self.BOOT_ATTEMPTS)
                self.get_vm(name=f'zvm-boot-{attempt}').shutdown()


if __name__ == '__main__':
    QemuSystemTest.main()
