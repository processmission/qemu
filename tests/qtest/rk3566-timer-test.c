/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the Rockchip RK3566 timer.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "rk3566-test-common.h"

#define TIMER_CONTROL_MASK      0x7
#define TIMER_CONTROL_ENABLE    0x1
#define TIMER_CONTROL_IRQ_EN   0x4
#define TIMER_TEST_LOAD     240
#define TIMER_IRQ           109

static void test_timer_reset_values(void)
{
    QTestState *qts = rk3566_qtest_start();

    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                         RK3566_TIMER_LOAD), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                         RK3566_TIMER_LOAD_HI), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                         RK3566_TIMER_CUR), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                         RK3566_TIMER_CUR_HI), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                         RK3566_TIMER_CONTROL), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                         RK3566_TIMER_STATUS), ==, 0);

    /* The current value registers are read-only. */
    rk3566_mmio_write32_at(qts, RK3566_TIMER0_BASE, RK3566_TIMER_CUR,
                           0xdeadbeef);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                         RK3566_TIMER_CUR), ==, 0);

    qtest_quit(qts);
}

static void test_timer_register_access(void)
{
    QTestState *qts = rk3566_qtest_start();

    rk3566_mmio_write32_at(qts, RK3566_TIMER0_BASE, RK3566_TIMER_LOAD,
                           0x12345678);
    rk3566_mmio_write32_at(qts, RK3566_TIMER0_BASE, RK3566_TIMER_LOAD_HI,
                           0x9abcdef0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                         RK3566_TIMER_LOAD), ==, 0x12345678);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                         RK3566_TIMER_LOAD_HI), ==,
                    0x9abcdef0);

    /* The pre-write hook drops undefined control bits. */
    rk3566_mmio_write32_at(qts, RK3566_TIMER0_BASE, RK3566_TIMER_CONTROL,
                           0xffffffff);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                         RK3566_TIMER_CONTROL), ==,
                    TIMER_CONTROL_MASK);

    qtest_quit(qts);
}

static void test_timer_countdown_irq(void)
{
    QTestState *qts = rk3566_qtest_start();
    uint32_t first, second;

    qtest_irq_intercept_in(qts, RK3566_GIC_QOM);
    g_assert_cmphex(qtest_get_irq(qts, TIMER_IRQ), ==, 0);

    /* 240 ticks at 24 MHz are one 10 us period. */
    rk3566_mmio_write32_at(qts, RK3566_TIMER0_BASE, RK3566_TIMER_LOAD,
                           TIMER_TEST_LOAD);
    rk3566_mmio_write32_at(qts, RK3566_TIMER0_BASE, RK3566_TIMER_LOAD_HI, 0);
    rk3566_mmio_write32_at(qts, RK3566_TIMER0_BASE, RK3566_TIMER_CONTROL,
                           TIMER_CONTROL_ENABLE | TIMER_CONTROL_IRQ_EN);
    first = rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                 RK3566_TIMER_CUR);
    g_assert_cmpuint(first, <=, TIMER_TEST_LOAD);
    g_assert_cmpuint(first, >, 0);

    /*
     * Half a period later the counter has advanced by 120 ticks.  The ptimer
     * period is not an integer number of nanoseconds, so allow one tick of
     * quantization on each side of the exact count.
     */
    qtest_clock_step(qts, 5 * 1000);
    second = rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                   RK3566_TIMER_CUR);
    g_assert_cmpuint(first - second, >=, 119);
    g_assert_cmpuint(first - second, <=, 120);
    g_assert_cmphex(qtest_get_irq(qts, TIMER_IRQ), ==, 0);

    /* The period expires and raises the status bit and the IRQ line. */
    qtest_clock_step(qts, 5 * 1000);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                         RK3566_TIMER_STATUS), ==, 1);
    g_assert_cmphex(qtest_get_irq(qts, TIMER_IRQ), ==, 1);

    /* STATUS is write-one-to-clear and drops the interrupt. */
    rk3566_mmio_write32_at(qts, RK3566_TIMER0_BASE, RK3566_TIMER_STATUS, 1);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                         RK3566_TIMER_STATUS), ==, 0);
    g_assert_cmphex(qtest_get_irq(qts, TIMER_IRQ), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566/timer/reset-values", test_timer_reset_values);
    qtest_add_func("/rk3566/timer/register-access",
                   test_timer_register_access);
    qtest_add_func("/rk3566/timer/countdown-irq", test_timer_countdown_irq);

    return g_test_run();
}
