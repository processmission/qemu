/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the Rockchip RK3566 OTP controller.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "rk3566-test-common.h"

#define OTP_CMD          0x020
#define OTP_STORAGE      0x200
#define OTP_ID_WORD      0x6635

static void test_otp_reset_values(void)
{
    QTestState *qts = rk3566_qtest_start();

    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                         RK3566_OTP_STATUS), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                         RK3566_OTP_DOUT), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                         OTP_CMD), ==, 0);

    qtest_quit(qts);
}

static void test_otp_unlisted_storage(void)
{
    QTestState *qts = rk3566_qtest_start();

    /* Addresses above 0x120 are plain storage registers. */
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, OTP_STORAGE, 0x12345678);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                         OTP_STORAGE), ==, 0x12345678);
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, 0x1000, 0xfa);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE, 0x1000), ==,
                    0xfa);

    /*
     * Below 0x120 the write mask comes from the upper half-word, so a plain
     * write without mask bits leaves the register untouched.
     */
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, RK3566_OTP_CTRL, 0xffff);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                         RK3566_OTP_CTRL), ==, 0);
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, RK3566_OTP_CTRL,
                           RK3566_REG_WRITE_MASK(1));
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                         RK3566_OTP_CTRL), ==, 1);
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, RK3566_OTP_CTRL,
                           RK3566_REG_WRITE_MASK_CLEAR(1));
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                         RK3566_OTP_CTRL), ==, 0);

    qtest_quit(qts);
}

static void test_otp_command_status(void)
{
    QTestState *qts = rk3566_qtest_start();

    /* The CMD register clears its own start bit after the command. */
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, OTP_CMD,
                           RK3566_REG_WRITE_MASK(1));
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                         OTP_CMD), ==, 0);

    /*
     * With 0xfa programmed at 0x1000 the controller reports the command
     * completion flag in the status register.
     */
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, 0x1000, 0xfa);
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, OTP_CMD,
                           RK3566_REG_WRITE_MASK(1));
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                         RK3566_OTP_STATUS), ==, 0x2);

    /* STATUS is write-one-to-clear. */
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, RK3566_OTP_STATUS, 0x2);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                         RK3566_OTP_STATUS), ==, 0);

    qtest_quit(qts);
}

static void test_otp_fuse_read(void)
{
    QTestState *qts = rk3566_qtest_start();

    /*
     * Program the control, address and fuse data registers through the
     * half-word write mask, then read the fuse word at byte address 2.
     */
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, RK3566_OTP_CTRL,
                           RK3566_REG_WRITE_MASK(1));
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, RK3566_OTP_ADDR,
                           RK3566_REG_WRITE_MASK(
                               RK3566_OTP_FUSE_BYTE_ADDR / 2));
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, RK3566_OTP_TRIGGER,
                           RK3566_REG_WRITE_MASK(1));

    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                         RK3566_OTP_DOUT), ==, OTP_ID_WORD);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                         RK3566_OTP_STATUS), ==,
                    RK3566_OTP_STATUS_READ_DONE);

    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, RK3566_OTP_STATUS,
                           RK3566_OTP_STATUS_READ_DONE);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                         RK3566_OTP_STATUS), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566/otp/reset-values", test_otp_reset_values);
    qtest_add_func("/rk3566/otp/unlisted-storage",
                   test_otp_unlisted_storage);
    qtest_add_func("/rk3566/otp/command-status",
                   test_otp_command_status);
    qtest_add_func("/rk3566/otp/fuse-read", test_otp_fuse_read);

    return g_test_run();
}
