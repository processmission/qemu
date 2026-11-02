#!/usr/bin/env python3
#
# Functional tests for the MicroDuck beta machine and the official
# MicroDuck firmware releases.
#
# The official release assets are full eMMC images of several gigabytes, so
# they are not downloaded by the test framework.  Set
# QEMU_TEST_MICRODUCK_FIRMWARE_DIR to a directory that contains one
# subdirectory per release with the extracted kernel, device tree and disk
# image.  The expected layout is:
#
#   $QEMU_TEST_MICRODUCK_FIRMWARE_DIR/
#       v0.1.0/{Image,rk-kernel.dtb,*.wic}
#       v1.0.0/{Image,rk-kernel.dtb,*.wic}
#       v1.0.1/{Image,rk-kernel.dtb,*.wic}
#       v1.1.0/{Image,rk-kernel.dtb,*.wic}
#
# Copyright (c) 2026 Process Mission
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import socket
import time
from pathlib import Path

from qemu_test import LinuxKernelTest, exec_command, \
    exec_command_and_wait_for_pattern
from qemu_test import skipBigDataTest, skipIfMissingEnv

FIRMWARE_ENV = "QEMU_TEST_MICRODUCK_FIRMWARE_DIR"

# Keep the console quiet: the v0.1.0 image occasionally stops making
# progress while its boot log fills the host console chardev.
KERNEL_CMDLINE = ("console=ttyFIQ0 earlycon=uart8250,mmio32,0xfe660000 "
                  "root=PARTLABEL=rootfs_a rw rootwait loglevel=4 "
                  "systemd.show_status=0 systemd.mask=bluetooth.service")


class MicroDuckFirmwareTest(LinuxKernelTest):

    def _firmware_assets(self, release):
        base = os.environ.get(FIRMWARE_ENV)
        if base is None:
            self.skipTest("set %s to the extracted firmware directory" %
                          FIRMWARE_ENV)
        release_dir = Path(base) / release
        kernel = release_dir / "Image"
        dtb = release_dir / "rk-kernel.dtb"
        images = sorted(release_dir.glob("*.wic"))
        missing = [str(path) for path in (kernel, dtb) if not path.is_file()]
        if not images:
            missing.append(str(release_dir / "*.wic"))
        if missing:
            self.skipTest("missing firmware asset(s): %s" %
                          ", ".join(missing))
        return kernel, dtb, images[0]

    def _boot(self, release):
        kernel, dtb, image = self._firmware_assets(release)
        self.set_machine("microduck-beta")
        self.require_accelerator("tcg")
        self.vm.set_console()
        self.vm.add_args("-accel", "tcg,thread=multi",
                         "-smp", "4",
                         "-m", "1024",
                         "-kernel", str(kernel),
                         "-dtb", str(dtb),
                         "-drive", "if=sd,index=0,format=raw,"
                                   "file=%s,snapshot=on" % image,
                         "-append", KERNEL_CMDLINE,
                         "-display", "none",
                         "-monitor", "none",
                         "-no-reboot")
        self.vm.launch()

    def _console_expect(self, pattern, timeout):
        console = self.vm.console_socket
        buffered = b""
        deadline = time.time() + timeout
        try:
            while time.time() < deadline:
                try:
                    console.settimeout(max(0.05, deadline - time.time()))
                    data = console.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    return False
                buffered += data
                if pattern.encode() in buffered:
                    return True
            return False
        finally:
            console.settimeout(None)

    def _wait_login(self, timeout=420):
        if self._console_expect("login:", timeout):
            return
        exec_command(self, "")
        if self._console_expect("login:", 90):
            return
        raise RuntimeError("guest did not reach a login prompt")

    def _login(self, release):
        self._wait_login()
        if release == "v0.1.0":
            exec_command_and_wait_for_pattern(self, "root", "# ")
            return
        exec_command_and_wait_for_pattern(self, "microduck", "Password:")
        exec_command_and_wait_for_pattern(self, "microduck", "$ ")
        exec_command_and_wait_for_pattern(self, "sudo -S -i", "Password:")
        exec_command_and_wait_for_pattern(self, "microduck", "# ")

    def _check_guest(self):
        exec_command_and_wait_for_pattern(self, "cat /proc/device-tree/model",
                                          "Pollen Robotics microduck beta")
        exec_command_and_wait_for_pattern(self, "ls /sys/class/video4linux",
                                          "video0")
        exec_command_and_wait_for_pattern(self,
                                          "systemctl show -p ActiveState "
                                          "--value rkaiq_3A", "active")
        exec_command_and_wait_for_pattern(self,
                                          "cat /sys/kernel/debug/rknpu/"
                                          "version", "RKNPU driver: v0.9.8")
        exec_command_and_wait_for_pattern(self,
                                          "ls -l /dev/dri", "renderD128")
        exec_command_and_wait_for_pattern(
            self,
            "timeout 60 v4l2-ctl -d /dev/video0 --stream-mmap "
            "--stream-count=1 >/dev/null 2>&1; echo CAMERA_RC=$?",
            "CAMERA_RC=0", failure_message="CAMERA_RC=1")

    # A TCG boot of the v0.1.0 image occasionally stalls before the serial
    # getty starts, so retry with a fresh machine before failing the release.
    def _reset_vm(self):
        vm = self._vms.pop("default", None)
        if vm is not None:
            try:
                vm.shutdown()
            except Exception:
                pass

    def _test_release(self, release):
        error = None
        for _ in range(3):
            try:
                self._boot(release)
                self._login(release)
                self._check_guest()
                return
            except RuntimeError as err:
                error = err
                self._reset_vm()
        self.fail("%s did not reach the guest shell: %s" % (release, error))

    @skipIfMissingEnv(FIRMWARE_ENV)
    @skipBigDataTest()
    def test_v0_1_0(self):
        self._test_release("v0.1.0")

    @skipIfMissingEnv(FIRMWARE_ENV)
    @skipBigDataTest()
    def test_v1_0_0(self):
        self._test_release("v1.0.0")

    @skipIfMissingEnv(FIRMWARE_ENV)
    @skipBigDataTest()
    def test_v1_0_1(self):
        self._test_release("v1.0.1")

    @skipIfMissingEnv(FIRMWARE_ENV)
    @skipBigDataTest()
    def test_v1_1_0(self):
        self._test_release("v1.1.0")


if __name__ == "__main__":
    LinuxKernelTest.main()
