/*
 * QTest for the SiFli SF32LB52X ATIM/GPTIM PWM timers
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "sf32lb52-devkit-lcd"
#define ATIM_BASE 0x50004000
#define GPTIM_BASE 0x500b0000

#define R_CR1    0x00
#define R_DIER   0x0c
#define R_SR     0x10
#define R_EGR    0x14
#define R_CCMR1  0x18
#define R_CCER   0x20
#define R_CNT    0x24
#define R_PSC    0x28
#define R_ARR    0x2c
#define R_CCR1   0x34
#define R_BDTR   0x44

#define CR1_CEN   (1U << 0)
#define CR1_ARPE  (1U << 7)
#define SR_UIF    (1U << 0)
#define SR_CC1IF  (1U << 1)
#define DIER_UIE  (1U << 0)
#define IC1_PWM1  (6U << 4)
#define BDTR_MOE  (1U << 15)
#define CCER_CC1E (1U << 0)

static QTestState *pwm_start(void)
{
    QTestState *qts = qtest_init("-machine " MACHINE " -serial null");

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm[0]", "sysbus-irq");
    return qts;
}

static uint32_t pwm_rd(QTestState *qts, uint32_t base, uint32_t off)
{
    return qtest_readl(qts, base + off);
}

static void pwm_wr(QTestState *qts, uint32_t base, uint32_t off, uint32_t v)
{
    qtest_writel(qts, base + off, v);
}

static void test_pwm_reset(void)
{
    QTestState *qts = pwm_start();

    g_assert_cmphex(pwm_rd(qts, ATIM_BASE, R_CR1), ==, 0);
    g_assert_cmphex(pwm_rd(qts, ATIM_BASE, R_CNT), ==, 0);
    g_assert_cmphex(pwm_rd(qts, ATIM_BASE, R_ARR), ==, 0);
    g_assert_cmphex(pwm_rd(qts, GPTIM_BASE, R_CR1), ==, 0);

    /* ATIM AF1/AF2 reset to 1 (SVD), GPTIM has no AF registers. */
    g_assert_cmphex(pwm_rd(qts, ATIM_BASE, 0x60), ==, 1);
    g_assert_cmphex(pwm_rd(qts, ATIM_BASE, 0x64), ==, 1);

    qtest_quit(qts);
}

static void test_pwm_init_and_set_cycles(void)
{
    QTestState *qts = pwm_start();
    const uint32_t base = ATIM_BASE;

    /* pwm_sf32lb_atim init: prescaler 119 -> 1 MHz counter clock. */
    pwm_wr(qts, base, R_PSC, 119);
    pwm_wr(qts, base, R_EGR, 1);
    pwm_wr(qts, base, R_CR1, CR1_ARPE | CR1_CEN);
    pwm_wr(qts, base, R_BDTR, BDTR_MOE);
    g_assert_cmphex(pwm_rd(qts, base, R_PSC), ==, 119);
    g_assert_cmphex(pwm_rd(qts, base, R_CR1), ==, CR1_ARPE | CR1_CEN);
    g_assert_cmphex(pwm_rd(qts, base, R_BDTR), ==, BDTR_MOE);

    /* set_cycles: period 1000, pulse 250, PWM mode 1 + preload. */
    pwm_wr(qts, base, R_ARR, 999);
    pwm_wr(qts, base, R_CCR1, 250);
    pwm_wr(qts, base, R_CCMR1, IC1_PWM1 | (1U << 3));
    pwm_wr(qts, base, R_CCER, CCER_CC1E);
    g_assert_cmphex(pwm_rd(qts, base, R_ARR), ==, 999);
    g_assert_cmphex(pwm_rd(qts, base, R_CCR1), ==, 250);
    g_assert_cmphex(pwm_rd(qts, base, R_CCMR1), ==, IC1_PWM1 | (1U << 3));
    g_assert_cmphex(pwm_rd(qts, base, R_CCER), ==, CCER_CC1E);

    /* The counter advances at 1 MHz: 100 us -> about 100 counts. */
    qtest_clock_step(qts, 100 * 1000);
    g_assert_cmpuint(pwm_rd(qts, base, R_CNT), >=, 90);
    g_assert_cmpuint(pwm_rd(qts, base, R_CNT), <=, 110);

    qtest_quit(qts);
}

static void test_pwm_wrap_update_flag(void)
{
    QTestState *qts = pwm_start();
    const uint32_t base = ATIM_BASE;

    pwm_wr(qts, base, R_PSC, 119);
    pwm_wr(qts, base, R_CR1, CR1_CEN);
    pwm_wr(qts, base, R_ARR, 999);

    /* More than one period (1 ms) elapses: UIF is set. */
    qtest_clock_step(qts, 5 * 1000 * 1000);
    g_assert_cmphex(pwm_rd(qts, base, R_SR) & SR_UIF, ==, SR_UIF);
    g_assert_cmphex(pwm_rd(qts, base, R_SR) & SR_CC1IF, ==, 0);

    /* rc_w0: writing 0 clears, the counter keeps running. */
    pwm_wr(qts, base, R_SR, 0);
    g_assert_cmphex(pwm_rd(qts, base, R_SR) & SR_UIF, ==, 0);

    /* EGR.UG is write-1 self-clearing and reloads the counter. */
    pwm_wr(qts, base, R_EGR, 1);
    g_assert_cmphex(pwm_rd(qts, base, R_EGR), ==, 0);
    g_assert_cmphex(pwm_rd(qts, base, R_SR) & SR_UIF, ==, SR_UIF);

    qtest_quit(qts);
}

static void test_pwm_compare_flag_and_irq(void)
{
    QTestState *qts = pwm_start();
    const uint32_t base = ATIM_BASE;

    pwm_wr(qts, base, R_PSC, 119);
    pwm_wr(qts, base, R_ARR, 999);
    pwm_wr(qts, base, R_CCR1, 250);
    pwm_wr(qts, base, R_CCER, CCER_CC1E);
    pwm_wr(qts, base, R_DIER, DIER_UIE);
    pwm_wr(qts, base, R_CR1, CR1_CEN);

    qtest_clock_step(qts, 2 * 1000 * 1000);
    g_assert_cmphex(pwm_rd(qts, base, R_SR) & SR_CC1IF, ==, SR_CC1IF);
    g_assert_cmphex(pwm_rd(qts, base, R_SR) & SR_UIF, ==, SR_UIF);
    g_assert_true(qtest_get_irq(qts, 0));

    pwm_wr(qts, base, R_SR, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_pwm_gpt_widths(void)
{
    QTestState *qts = pwm_start();
    const uint32_t base = GPTIM_BASE;

    /* GPTIM is a 16-bit timer: wider writes are truncated. */
    pwm_wr(qts, base, R_ARR, 0x12345678);
    g_assert_cmphex(pwm_rd(qts, base, R_ARR), ==, 0x5678);
    pwm_wr(qts, base, R_CCR1, 0xffffffff);
    g_assert_cmphex(pwm_rd(qts, base, R_CCR1), ==, 0xffff);

    /* 24 MHz / 24 = 1 MHz counter clock. */
    pwm_wr(qts, base, R_PSC, 23);
    pwm_wr(qts, base, R_ARR, 0xffff);
    pwm_wr(qts, base, R_CR1, CR1_CEN);
    qtest_clock_step(qts, 100 * 1000);
    g_assert_cmpuint(pwm_rd(qts, base, R_CNT), >=, 90);
    g_assert_cmpuint(pwm_rd(qts, base, R_CNT), <=, 110);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/sf32lb52x/pwm/reset", test_pwm_reset);
    g_test_add_func("/sf32lb52x/pwm/init-set-cycles",
                    test_pwm_init_and_set_cycles);
    g_test_add_func("/sf32lb52x/pwm/wrap-update-flag",
                    test_pwm_wrap_update_flag);
    g_test_add_func("/sf32lb52x/pwm/compare-flag-irq",
                    test_pwm_compare_flag_and_irq);
    g_test_add_func("/sf32lb52x/pwm/gpt-widths", test_pwm_gpt_widths);

    return g_test_run();
}
