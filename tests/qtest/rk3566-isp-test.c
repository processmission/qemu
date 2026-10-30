/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register tests for the Rockchip RK3566 ISP model.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"
#include "rk3566-test-common.h"

#define RK3566_ISP_BASE     0xfdff0000ULL

/* Seed and shadow registers below the 0x1600 register-file boundary. */
#define ISP_SYS_RESET       0x0014
#define ISP_CTRL            0x0400
#define ISP_RAW_WIDTH       0x0410
#define ISP_RAW_HEIGHT      0x0414
#define ISP_CTRL_LATCH      (BIT(9) | BIT(10))
#define ISP_H_SIZE          0x059c
#define ISP_IMSC            0x05bc
#define ISP_RIS             0x05c0
#define ISP_MIS             0x05c4
#define ISP_ICR             0x05c8
#define ISP_ISR             0x05cc
#define ISP_FRAME_COUNT     0x0644
#define ISP_CROP_CTRL       0x0880
#define MI_CTRL             0x1400
#define MI_INIT             0x1404
#define MI_INIT_LATCH       BIT(4)
#define MI_CTRL_SHD         0x1474
#define MI_IMSC             0x14f8
#define MI_RIS              0x14fc
#define MI_MIS              0x1500
#define MI_ICR              0x1504
#define MI_ISR              0x1508
#define ISP_SHADOW_SRC0     0x1408
#define ISP_SHADOW_SRC1     0x140c
#define ISP_SHADOW_DST0     0x1478
#define ISP_SHADOW_DST1     0x147c
#define ISP_SPARSE_LOW      0x0500
#define ISP_SPARSE_HIGH     0x2000
#define ISP_SHADOW_RANGE    0x0c00

static QTestState *isp_qtest_start(void)
{
    QTestState *qts = rk3566_qtest_start();

    if (!rk3566_qtest_media_present(qts)) {
        g_test_skip("RK3566 media devices are not built "
                    "(libavcodec, libavutil, libswscale or x264 missing)");
        qtest_quit(qts);
        return NULL;
    }
    return qts;
}

static uint32_t isp_read32(QTestState *qts, uint32_t offset)
{
    return rk3566_mmio_read32_at(qts, RK3566_ISP_BASE, offset);
}

static void isp_write32(QTestState *qts, uint32_t offset, uint32_t value)
{
    rk3566_mmio_write32_at(qts, RK3566_ISP_BASE, offset, value);
}

static void test_rk3566_isp_reset_values(void)
{
    QTestState *qts = isp_qtest_start();

    if (!qts) {
        return;
    }

    g_assert_cmphex(isp_read32(qts, ISP_CTRL), ==, 0);
    g_assert_cmphex(isp_read32(qts, ISP_RIS), ==, 0);
    g_assert_cmphex(isp_read32(qts, ISP_MIS), ==, 0);
    g_assert_cmphex(isp_read32(qts, ISP_FRAME_COUNT), ==, 0);
    g_assert_cmphex(isp_read32(qts, MI_CTRL), ==, 0);
    g_assert_cmphex(isp_read32(qts, MI_RIS), ==, 0);
    g_assert_cmphex(isp_read32(qts, MI_MIS), ==, 0);
    g_assert_cmphex(isp_read32(qts, ISP_H_SIZE), ==, 0);
    g_assert_cmphex(isp_read32(qts, ISP_SPARSE_HIGH), ==, 0);

    /* Plain register-file storage stays writable and readable. */
    isp_write32(qts, ISP_RAW_WIDTH, 0x12345678);
    isp_write32(qts, ISP_RAW_HEIGHT, 0x9abcdef0);
    g_assert_cmphex(isp_read32(qts, ISP_RAW_WIDTH), ==, 0x12345678);
    g_assert_cmphex(isp_read32(qts, ISP_RAW_HEIGHT), ==, 0x9abcdef0);

    /* Status registers are read-only for the guest. */
    isp_write32(qts, ISP_FRAME_COUNT, 0xffffffff);
    isp_write32(qts, ISP_MIS, 0xffffffff);
    g_assert_cmphex(isp_read32(qts, ISP_FRAME_COUNT), ==, 0);
    g_assert_cmphex(isp_read32(qts, ISP_MIS), ==, 0);

    qtest_quit(qts);
}

static void test_rk3566_isp_shadow_latch(void)
{
    QTestState *qts = isp_qtest_start();

    if (!qts) {
        return;
    }

    isp_write32(qts, ISP_SHADOW_SRC0, 0x11223344);
    isp_write32(qts, ISP_SHADOW_SRC1, 0x55667788);
    isp_write32(qts, MI_INIT, MI_INIT_LATCH);

    g_assert_cmphex(isp_read32(qts, ISP_SHADOW_DST0), ==, 0x11223344);
    g_assert_cmphex(isp_read32(qts, ISP_SHADOW_DST1), ==, 0x55667788);
    g_assert_cmphex(isp_read32(qts, MI_INIT), ==, 0);

    /* ISP_CTRL bits 9 and 10 latch the shadow register set. */
    isp_write32(qts, ISP_SHADOW_SRC0, 0xa5a5a5a5);
    isp_write32(qts, ISP_CTRL, ISP_CTRL_LATCH);
    g_assert_cmphex(isp_read32(qts, ISP_SHADOW_DST0), ==, 0xa5a5a5a5);
    g_assert_cmphex(isp_read32(qts, ISP_CTRL), ==, 0);

    isp_write32(qts, ISP_CTRL, 0x1 | ISP_CTRL_LATCH);
    g_assert_cmphex(isp_read32(qts, ISP_CTRL), ==, 0x1);

    /* MI_CTRL latches while the capture path is idle. */
    isp_write32(qts, ISP_SHADOW_SRC0, 0x00000077);
    isp_write32(qts, MI_CTRL, 0x5);
    g_assert_cmphex(isp_read32(qts, ISP_SHADOW_DST0), ==, 0x77);
    g_assert_cmphex(isp_read32(qts, MI_CTRL_SHD), ==, 0x00050005);

    /* The shadow range below 0x1600 latches as well. */
    isp_write32(qts, ISP_SHADOW_RANGE, 0x31);
    isp_write32(qts, MI_INIT, MI_INIT_LATCH);
    g_assert_cmphex(isp_read32(qts, ISP_SHADOW_RANGE + 0x30), ==, 0x31);

    qtest_quit(qts);
}

static void test_rk3566_isp_interrupt_registers(void)
{
    QTestState *qts = isp_qtest_start();

    if (!qts) {
        return;
    }

    /* Disabling the capture path raises the start-of-frame interrupt. */
    isp_write32(qts, ISP_CTRL, 0x1);
    isp_write32(qts, ISP_CTRL, 0x0);
    g_assert_cmphex(isp_read32(qts, ISP_RIS), ==, BIT(0));

    /* ISP_ICR clears ISP_RIS, ISP_ISR sets it; both read back zero. */
    isp_write32(qts, ISP_ICR, BIT(0));
    g_assert_cmphex(isp_read32(qts, ISP_RIS), ==, 0);
    isp_write32(qts, ISP_ISR, BIT(1));
    g_assert_cmphex(isp_read32(qts, ISP_RIS), ==, BIT(1));
    g_assert_cmphex(isp_read32(qts, ISP_ISR), ==, 0);
    isp_write32(qts, ISP_ICR, BIT(1));
    g_assert_cmphex(isp_read32(qts, ISP_RIS), ==, 0);

    /* The MI interrupt bank follows the same clear/set semantics. */
    isp_write32(qts, MI_ISR, BIT(0));
    g_assert_cmphex(isp_read32(qts, MI_RIS), ==, BIT(0));
    g_assert_cmphex(isp_read32(qts, MI_ISR), ==, 0);
    isp_write32(qts, MI_ICR, BIT(0));
    g_assert_cmphex(isp_read32(qts, MI_RIS), ==, 0);

    /* Raw status registers ignore guest writes. */
    isp_write32(qts, ISP_RIS, 0xff);
    isp_write32(qts, MI_RIS, 0xff);
    g_assert_cmphex(isp_read32(qts, ISP_RIS), ==, 0);
    g_assert_cmphex(isp_read32(qts, MI_RIS), ==, 0);

    qtest_quit(qts);
}

static void test_rk3566_isp_sparse_window(void)
{
    QTestState *qts = isp_qtest_start();

    if (!qts) {
        return;
    }

    /* Addresses outside the RegisterInfo table keep plain storage. */
    isp_write32(qts, ISP_SPARSE_LOW, 0xdeadbeef);
    isp_write32(qts, ISP_SPARSE_HIGH, 0xcafebabe);
    g_assert_cmphex(isp_read32(qts, ISP_SPARSE_LOW), ==, 0xdeadbeef);
    g_assert_cmphex(isp_read32(qts, ISP_SPARSE_HIGH), ==, 0xcafebabe);

    /* Crop and channel configuration registers use the register table. */
    isp_write32(qts, ISP_CROP_CTRL, 0x3);
    isp_write32(qts, ISP_IMSC, 0x5a5a5a5a);
    isp_write32(qts, MI_IMSC, 0xa5a5a5a5);
    g_assert_cmphex(isp_read32(qts, ISP_CROP_CTRL), ==, 0x3);
    g_assert_cmphex(isp_read32(qts, ISP_IMSC), ==, 0x5a5a5a5a);
    g_assert_cmphex(isp_read32(qts, MI_IMSC), ==, 0xa5a5a5a5);

    qtest_quit(qts);
}

static void test_rk3566_isp_sys_reset(void)
{
    QTestState *qts = isp_qtest_start();

    if (!qts) {
        return;
    }

    isp_write32(qts, ISP_RAW_WIDTH, 0xbeef);
    isp_write32(qts, ISP_SPARSE_HIGH, 0xcafe);
    isp_write32(qts, ISP_SHADOW_SRC0, 0x11);

    /* A zero write leaves the register file untouched. */
    isp_write32(qts, ISP_SYS_RESET, 0);
    g_assert_cmphex(isp_read32(qts, ISP_RAW_WIDTH), ==, 0xbeef);

    isp_write32(qts, ISP_SYS_RESET, 1);
    g_assert_cmphex(isp_read32(qts, ISP_RAW_WIDTH), ==, 0);
    g_assert_cmphex(isp_read32(qts, ISP_SPARSE_HIGH), ==, 0);
    g_assert_cmphex(isp_read32(qts, ISP_SHADOW_SRC0), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566-isp/reset-values",
                   test_rk3566_isp_reset_values);
    qtest_add_func("/rk3566-isp/shadow-latch",
                   test_rk3566_isp_shadow_latch);
    qtest_add_func("/rk3566-isp/interrupt-registers",
                   test_rk3566_isp_interrupt_registers);
    qtest_add_func("/rk3566-isp/sparse-window",
                   test_rk3566_isp_sparse_window);
    qtest_add_func("/rk3566-isp/sys-reset",
                   test_rk3566_isp_sys_reset);

    return g_test_run();
}
