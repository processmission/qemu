/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the Rockchip RK3566 PWM.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "rk3566-test-common.h"

#define PWM_CHANNELS       4
#define PWM_CHANNEL_STRIDE 0x10
#define PWM_CTRL           0x00
#define PWM_PERIOD         0x04
#define PWM_DUTY           0x08
#define PWM_ENABLE         0x0c
#define PWM_RSVD_FIRST     0x40
#define PWM_RSVD_LAST      0xfc

#define PWM_CLOCK_HZ       24000000ULL
#define PWM_TEST_PERIOD    997
#define PWM_TEST_ORIGINAL  0x55
#define PWM_STEP_NS        1000000

static void test_pwm_reset_values(void)
{
    static const uint64_t bases[] = {
        RK3566_PWM_4_7_BASE, RK3566_PWM_8_11_BASE,
    };
    QTestState *qts = rk3566_qtest_start();

    for (unsigned b = 0; b < ARRAY_SIZE(bases); b++) {
        for (unsigned ch = 0; ch < PWM_CHANNELS; ch++) {
            uint32_t offset = ch * PWM_CHANNEL_STRIDE;

            g_assert_cmphex(rk3566_mmio_read32_at(qts, bases[b],
                                                 offset + PWM_CTRL), ==, 0);
            g_assert_cmphex(rk3566_mmio_read32_at(qts, bases[b],
                                                 offset + PWM_PERIOD), ==, 0);
            g_assert_cmphex(rk3566_mmio_read32_at(qts, bases[b],
                                                 offset + PWM_DUTY), ==, 0);
            g_assert_cmphex(rk3566_mmio_read32_at(qts, bases[b],
                                                 offset + PWM_ENABLE), ==, 0);
        }
    }

    qtest_quit(qts);
}

static void test_pwm_dual_instances(void)
{
    QTestState *qts = rk3566_qtest_start();
    g_autofree char *first = rk3566_qom_child_object_type(qts, "/machine",
                                                          "pwm4-7");
    g_autofree char *second = rk3566_qom_child_object_type(qts, "/machine",
                                                           "pwm8-11");

    g_assert_cmpstr(first, ==, "rk3566-pwm");
    g_assert_cmpstr(second, ==, "rk3566-pwm");

    /* The two instances decode the same register layout independently. */
    rk3566_mmio_write32_at(qts, RK3566_PWM_4_7_BASE, PWM_PERIOD,
                           PWM_TEST_PERIOD);
    rk3566_mmio_write32_at(qts, RK3566_PWM_8_11_BASE, PWM_PERIOD, 1000);

    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_PWM_4_7_BASE,
                                         PWM_PERIOD), ==, PWM_TEST_PERIOD);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_PWM_8_11_BASE,
                                         PWM_PERIOD), ==, 1000);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_PWM_8_11_BASE,
                                         PWM_DUTY), ==, 0);

    qtest_quit(qts);
}

static void test_pwm_counting(void)
{
    QTestState *qts = rk3566_qtest_start();
    uint32_t ticks;

    rk3566_mmio_write32_at(qts, RK3566_PWM_4_7_BASE, PWM_PERIOD,
                           PWM_TEST_PERIOD);
    rk3566_mmio_write32_at(qts, RK3566_PWM_4_7_BASE, PWM_DUTY, 300);
    rk3566_mmio_write32_at(qts, RK3566_PWM_4_7_BASE, PWM_CTRL,
                           PWM_TEST_ORIGINAL);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_PWM_4_7_BASE,
                                         PWM_CTRL), ==, PWM_TEST_ORIGINAL);

    rk3566_mmio_write32_at(qts, RK3566_PWM_4_7_BASE, PWM_ENABLE, 1);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_PWM_4_7_BASE,
                                         PWM_ENABLE), ==, 1);

    /* An enabled channel reports its position within the period. */
    qtest_clock_step(qts, PWM_STEP_NS);
    ticks = (uint32_t)(PWM_CLOCK_HZ * PWM_STEP_NS / 1000000000ULL);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_PWM_4_7_BASE,
                                         PWM_CTRL), ==,
                    ticks % PWM_TEST_PERIOD);

    /* The remaining channel registers keep their programmed value. */
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_PWM_4_7_BASE,
                                         PWM_PERIOD), ==, PWM_TEST_PERIOD);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_PWM_4_7_BASE,
                                         PWM_DUTY), ==, 300);

    /* A disabled channel returns the stored control value again. */
    rk3566_mmio_write32_at(qts, RK3566_PWM_4_7_BASE, PWM_ENABLE, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_PWM_4_7_BASE,
                                         PWM_CTRL), ==, PWM_TEST_ORIGINAL);

    qtest_quit(qts);
}

static void test_pwm_rsvd_storage(void)
{
    QTestState *qts = rk3566_qtest_start();

    /* The reserved words behave as plain storage registers. */
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_PWM_4_7_BASE,
                                         PWM_RSVD_FIRST), ==, 0);
    rk3566_mmio_write32_at(qts, RK3566_PWM_4_7_BASE, PWM_RSVD_FIRST,
                           0x12345678);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_PWM_4_7_BASE,
                                         PWM_RSVD_FIRST), ==, 0x12345678);
    rk3566_mmio_write32_at(qts, RK3566_PWM_4_7_BASE, PWM_RSVD_LAST,
                           0xdeadbeef);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_PWM_4_7_BASE,
                                         PWM_RSVD_LAST), ==, 0xdeadbeef);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566/pwm/reset-values", test_pwm_reset_values);
    qtest_add_func("/rk3566/pwm/dual-instances", test_pwm_dual_instances);
    qtest_add_func("/rk3566/pwm/counting", test_pwm_counting);
    qtest_add_func("/rk3566/pwm/rsvd-storage", test_pwm_rsvd_storage);

    return g_test_run();
}
