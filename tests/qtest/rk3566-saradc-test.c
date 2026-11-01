/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the Rockchip RK3566 SARADC.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "rk3566-test-common.h"

#define SARADC_DATA        0x00
#define SARADC_STATUS      0x04
#define SARADC_CONTROL     0x08
#define SARADC_DELAY       0x0c
#define SARADC_CONTROL_START   (1u << 3)
#define SARADC_CONTROL_IRQ_EN  (1u << 5)
#define SARADC_CONTROL_DONE    (1u << 6)
#define SARADC_IRQ         93

#define SARADC_STEP_NS     20000
#define SARADC_FULL_SCALE  1023

static void test_saradc_reset_values(void)
{
    QTestState *qts = rk3566_qtest_start();

    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SARADC_BASE,
                                         SARADC_DATA), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SARADC_BASE,
                                         SARADC_STATUS), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SARADC_BASE,
                                         SARADC_CONTROL), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SARADC_BASE,
                                         SARADC_DELAY), ==, 0);

    /* DELAY keeps six bits and CONTROL keeps the documented low bits. */
    rk3566_mmio_write32_at(qts, RK3566_SARADC_BASE, SARADC_DELAY,
                           0xffffffff);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SARADC_BASE,
                                         SARADC_DELAY), ==, 0x3f);
    rk3566_mmio_write32_at(qts, RK3566_SARADC_BASE, SARADC_CONTROL,
                           0xffffffff);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SARADC_BASE,
                                         SARADC_CONTROL), ==, 0x2f);
    rk3566_mmio_write32_at(qts, RK3566_SARADC_BASE, SARADC_CONTROL, 0);

    qtest_quit(qts);
}

static void test_saradc_conversion(void)
{
    QTestState *qts = rk3566_qtest_start();

    rk3566_mmio_write32_at(qts, RK3566_SARADC_BASE, SARADC_DELAY, 0);

    /* Channel 0 is tied to the 1.8 V reference. */
    rk3566_mmio_write32_at(qts, RK3566_SARADC_BASE, SARADC_CONTROL,
                           SARADC_CONTROL_START);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SARADC_BASE,
                                         SARADC_STATUS), ==, 1);

    qtest_clock_step(qts, SARADC_STEP_NS);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SARADC_BASE,
                                         SARADC_DATA), ==,
                    SARADC_FULL_SCALE);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SARADC_BASE,
                                         SARADC_CONTROL), ==,
                    SARADC_CONTROL_START | SARADC_CONTROL_DONE);

    /* Channel 1 is tied to 0.9 V, half of the reference. */
    rk3566_mmio_write32_at(qts, RK3566_SARADC_BASE, SARADC_CONTROL,
                           SARADC_CONTROL_START | 1);
    qtest_clock_step(qts, SARADC_STEP_NS);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SARADC_BASE,
                                         SARADC_DATA), ==,
                    SARADC_FULL_SCALE / 2);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SARADC_BASE,
                                         SARADC_CONTROL), ==,
                    (SARADC_CONTROL_START | SARADC_CONTROL_DONE) | 1);

    /* DATA is read-only. */
    rk3566_mmio_write32_at(qts, RK3566_SARADC_BASE, SARADC_DATA,
                           0xdeadbeef);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SARADC_BASE,
                                         SARADC_DATA), ==,
                    SARADC_FULL_SCALE / 2);

    qtest_quit(qts);
}

static void test_saradc_irq(void)
{
    QTestState *qts = rk3566_qtest_start();

    qtest_irq_intercept_in(qts, RK3566_GIC_QOM);
    g_assert_cmphex(qtest_get_irq(qts, SARADC_IRQ), ==, 0);

    rk3566_mmio_write32_at(qts, RK3566_SARADC_BASE, SARADC_DELAY, 0);
    rk3566_mmio_write32_at(qts, RK3566_SARADC_BASE, SARADC_CONTROL,
                           SARADC_CONTROL_START | SARADC_CONTROL_IRQ_EN);
    g_assert_cmphex(qtest_get_irq(qts, SARADC_IRQ), ==, 0);

    qtest_clock_step(qts, SARADC_STEP_NS);
    g_assert_cmphex(qtest_get_irq(qts, SARADC_IRQ), ==, 1);

    /* Disabling the engine drops the interrupt line. */
    rk3566_mmio_write32_at(qts, RK3566_SARADC_BASE, SARADC_CONTROL, 0);
    g_assert_cmphex(qtest_get_irq(qts, SARADC_IRQ), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566/saradc/reset-values",
                   test_saradc_reset_values);
    qtest_add_func("/rk3566/saradc/conversion", test_saradc_conversion);
    qtest_add_func("/rk3566/saradc/irq", test_saradc_irq);

    return g_test_run();
}
