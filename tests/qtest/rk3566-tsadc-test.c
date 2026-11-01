/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the Rockchip RK3566 TSADC.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "rk3566-test-common.h"

#define TSADC_CON          0x00
#define TSADC_ENABLE       0x04
#define TSADC_INT_EN       0x08
#define TSADC_INT_STATUS   0x0c
#define TSADC_DATA0        0x20
#define TSADC_DATA1        0x24
#define TSADC_THRESH0      0x30
#define TSADC_THRESH1      0x34
#define TSADC_STORAGE0     0x40
#define TSADC_STORAGE1     0x80
#define TSADC_IRQ          115
#define TSADC_CODE_25C     2024

static void test_tsadc_reset_values(void)
{
    QTestState *qts = rk3566_qtest_start();
    static const uint32_t regs[] = {
        TSADC_CON, TSADC_ENABLE, TSADC_INT_EN, TSADC_INT_STATUS,
        TSADC_DATA0, TSADC_DATA1, TSADC_THRESH0, TSADC_THRESH1,
    };

    for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) {
        g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TSADC_BASE,
                                             regs[i]), ==, 0);
    }

    /* The data registers are read-only and stay zero while disabled. */
    rk3566_mmio_write32_at(qts, RK3566_TSADC_BASE, TSADC_DATA0, 0xdeadbeef);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TSADC_BASE,
                                         TSADC_DATA0), ==, 0);

    qtest_quit(qts);
}

static void test_tsadc_enable_and_data(void)
{
    QTestState *qts = rk3566_qtest_start();

    rk3566_mmio_write32_at(qts, RK3566_TSADC_BASE, TSADC_CON, 0x12345678);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TSADC_BASE,
                                         TSADC_CON), ==, 0x12345678);

    rk3566_mmio_write32_at(qts, RK3566_TSADC_BASE, TSADC_ENABLE, 1);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TSADC_BASE,
                                         TSADC_ENABLE), ==, 1);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TSADC_BASE,
                                         TSADC_DATA0), ==, TSADC_CODE_25C);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TSADC_BASE,
                                         TSADC_DATA1), ==, TSADC_CODE_25C);

    /* Writes to the data registers are ignored while enabled. */
    rk3566_mmio_write32_at(qts, RK3566_TSADC_BASE, TSADC_DATA0, 0xdeadbeef);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TSADC_BASE,
                                         TSADC_DATA0), ==, TSADC_CODE_25C);

    qtest_quit(qts);
}

static void test_tsadc_threshold_irq(void)
{
    QTestState *qts = rk3566_qtest_start();

    qtest_irq_intercept_in(qts, RK3566_GIC_QOM);
    g_assert_cmphex(qtest_get_irq(qts, TSADC_IRQ), ==, 0);

    rk3566_mmio_write32_at(qts, RK3566_TSADC_BASE, TSADC_ENABLE, 1);
    rk3566_mmio_write32_at(qts, RK3566_TSADC_BASE, TSADC_THRESH0, 1000);
    rk3566_mmio_write32_at(qts, RK3566_TSADC_BASE, TSADC_INT_EN, 1);

    /* The 25 C code is above the programmed threshold. */
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TSADC_BASE,
                                         TSADC_INT_STATUS), ==, 1);
    g_assert_cmphex(qtest_get_irq(qts, TSADC_IRQ), ==, 1);

    /* Masking the interrupt drops the line but keeps the status bit. */
    rk3566_mmio_write32_at(qts, RK3566_TSADC_BASE, TSADC_INT_EN, 0);
    g_assert_cmphex(qtest_get_irq(qts, TSADC_IRQ), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TSADC_BASE,
                                         TSADC_INT_STATUS), ==, 1);

    /* With the interrupt masked, INT_STATUS is write-one-to-clear. */
    rk3566_mmio_write32_at(qts, RK3566_TSADC_BASE, TSADC_INT_STATUS, 1);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TSADC_BASE,
                                         TSADC_INT_STATUS), ==, 0);

    /* A threshold above the sensor code never latches the status bit. */
    rk3566_mmio_write32_at(qts, RK3566_TSADC_BASE, TSADC_THRESH0, 4000);
    rk3566_mmio_write32_at(qts, RK3566_TSADC_BASE, TSADC_INT_EN, 1);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TSADC_BASE,
                                         TSADC_INT_STATUS), ==, 0);
    g_assert_cmphex(qtest_get_irq(qts, TSADC_IRQ), ==, 0);

    qtest_quit(qts);
}

static void test_tsadc_unlisted_storage(void)
{
    QTestState *qts = rk3566_qtest_start();

    /* Addresses outside the modeled table fall back to plain storage. */
    rk3566_mmio_write32_at(qts, RK3566_TSADC_BASE, TSADC_STORAGE0,
                           0x12345678);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TSADC_BASE,
                                         TSADC_STORAGE0), ==, 0x12345678);
    rk3566_mmio_write32_at(qts, RK3566_TSADC_BASE, TSADC_STORAGE1,
                           0xdeadbeef);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TSADC_BASE,
                                         TSADC_STORAGE1), ==, 0xdeadbeef);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566/tsadc/reset-values", test_tsadc_reset_values);
    qtest_add_func("/rk3566/tsadc/enable-data", test_tsadc_enable_and_data);
    qtest_add_func("/rk3566/tsadc/threshold-irq",
                   test_tsadc_threshold_irq);
    qtest_add_func("/rk3566/tsadc/unlisted-storage",
                   test_tsadc_unlisted_storage);

    return g_test_run();
}
