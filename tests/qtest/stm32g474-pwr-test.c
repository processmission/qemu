/*
 * QTest for the STM32G474 PWR
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define STM32G474_MACHINE "stm32g474"

#define PWR_BASE 0x40007000ULL
#define RCC_BASE 0x40021000ULL

#define PWR_CLOCK_QOM_PATH "/machine/mcu/pwr/clk"

#define PWR_CR1 0x00
#define PWR_CR3 0x08
#define PWR_SR2 0x14
#define PWR_CR5 0x80

#define PWR_CR1_RESET 0x00000200U
#define PWR_CR3_RESET 0x00008000U
#define PWR_SR2_RESET 0x00000000U
#define PWR_CR5_RESET 0x00000100U

#define PWR_CR1_VOS_MASK (3U << 9)
#define PWR_CR1_VOS_RANGE1 (1U << 9)
#define PWR_CR1_VOS_RANGE2 (2U << 9)
#define PWR_CR3_UCPD_DBDIS (1U << 14)
#define PWR_CR5_R1MODE (1U << 8)

#define RCC_APB1RSTR1 0x38
#define RCC_APB1ENR1 0x58
#define RCC_APB1RSTR1_PWRRST (1U << 28)
#define RCC_APB1ENR1_PWREN (1U << 28)

#define CLOCK_PERIOD_1SEC (1000000000ULL << 32)

static QTestState *stm32g474_qtest_start(void)
{
    return qtest_init("-machine " STM32G474_MACHINE " -serial null");
}

static uint32_t pwr_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, PWR_BASE + offset);
}

static void pwr_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, PWR_BASE + offset, value);
}

static uint32_t rcc_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, RCC_BASE + offset);
}

static void rcc_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, RCC_BASE + offset, value);
}

static void set_pwr_clock_gate(QTestState *qts, bool enabled)
{
    uint32_t value = rcc_readl(qts, RCC_APB1ENR1);

    if (enabled) {
        value |= RCC_APB1ENR1_PWREN;
    } else {
        value &= ~RCC_APB1ENR1_PWREN;
    }
    rcc_writel(qts, RCC_APB1ENR1, value);
}

static uint64_t clock_period_from_hz(uint64_t hz)
{
    return hz ? CLOCK_PERIOD_1SEC / hz : 0;
}

static uint64_t get_pwr_clock_period(QTestState *qts)
{
    QDict *response;
    uint64_t period;

    response = qtest_qmp(qts,
        "{ 'execute': 'qom-get', 'arguments': { "
        "'path': '" PWR_CLOCK_QOM_PATH "', "
        "'property': 'qtest-clock-period' } }");
    g_assert_false(qdict_haskey(response, "error"));
    period = qdict_get_int(response, "return");
    qobject_unref(response);

    return period;
}

static void assert_pwr_clock_hz(QTestState *qts, uint64_t hz)
{
    g_assert_cmphex(get_pwr_clock_period(qts), ==,
                    clock_period_from_hz(hz));
}

static void assert_power_on_reset_image(QTestState *qts)
{
    g_assert_cmphex(pwr_readl(qts, PWR_CR1), ==, PWR_CR1_RESET);
    g_assert_cmphex(pwr_readl(qts, PWR_CR3), ==, PWR_CR3_RESET);
    g_assert_cmphex(pwr_readl(qts, PWR_SR2), ==, PWR_SR2_RESET);
    g_assert_cmphex(pwr_readl(qts, PWR_CR5), ==, PWR_CR5_RESET);
}

static void dirty_pwr_registers(QTestState *qts)
{
    pwr_writel(qts, PWR_CR1, PWR_CR1_VOS_RANGE2);
    pwr_writel(qts, PWR_CR3, PWR_CR3_RESET | PWR_CR3_UCPD_DBDIS);
    pwr_writel(qts, PWR_CR5, 0);
}

static void test_pwr_mmio_reset_mask(void)
{
    QTestState *qts = stm32g474_qtest_start();
    uint32_t cr1;

    set_pwr_clock_gate(qts, true);

    cr1 = pwr_readl(qts, PWR_CR1);
    g_test_message("PWR_CR1 expected 0x%08x, observed 0x%08x",
                   PWR_CR1_RESET, cr1);
    g_assert_cmphex(cr1, ==, PWR_CR1_RESET);
    g_assert_cmphex(pwr_readl(qts, PWR_CR3), ==, PWR_CR3_RESET);
    g_assert_cmphex(pwr_readl(qts, PWR_SR2), ==, PWR_SR2_RESET);
    g_assert_cmphex(pwr_readl(qts, PWR_CR5), ==, PWR_CR5_RESET);

    pwr_writel(qts, PWR_CR1, UINT32_MAX);
    g_assert_cmphex(pwr_readl(qts, PWR_CR1), ==, 0x00004307);

    pwr_writel(qts, PWR_CR1, PWR_CR1_VOS_RANGE2);
    g_assert_cmphex(pwr_readl(qts, PWR_CR1), ==, PWR_CR1_VOS_RANGE2);

    pwr_writel(qts, PWR_CR1, 1);
    g_assert_cmphex(pwr_readl(qts, PWR_CR1), ==,
                    PWR_CR1_VOS_RANGE2 | 1);

    pwr_writel(qts, PWR_CR1, PWR_CR1_VOS_MASK | 7);
    g_assert_cmphex(pwr_readl(qts, PWR_CR1), ==,
                    PWR_CR1_VOS_RANGE2 | 7);

    pwr_writel(qts, PWR_CR1, PWR_CR1_VOS_RANGE1);
    g_assert_cmphex(pwr_readl(qts, PWR_CR1), ==, PWR_CR1_VOS_RANGE1);

    pwr_writel(qts, PWR_CR3, UINT32_MAX);
    g_assert_cmphex(pwr_readl(qts, PWR_CR3), ==, 0x0000e51f);

    pwr_writel(qts, PWR_SR2, UINT32_MAX);
    g_assert_cmphex(pwr_readl(qts, PWR_SR2), ==, PWR_SR2_RESET);

    pwr_writel(qts, PWR_CR5, UINT32_MAX);
    g_assert_cmphex(pwr_readl(qts, PWR_CR5), ==, PWR_CR5_R1MODE);

    dirty_pwr_registers(qts);
    qtest_system_reset(qts);
    assert_power_on_reset_image(qts);

    set_pwr_clock_gate(qts, true);
    dirty_pwr_registers(qts);
    qtest_system_reset(qts);
    assert_power_on_reset_image(qts);

    qtest_quit(qts);
}

static void test_pwr_ardep_startup(void)
{
    QTestState *qts = stm32g474_qtest_start();
    uint32_t value;

    set_pwr_clock_gate(qts, true);

    value = pwr_readl(qts, PWR_CR1);
    g_assert_cmphex(value & PWR_CR1_VOS_MASK, ==, PWR_CR1_VOS_RANGE1);

    value = pwr_readl(qts, PWR_CR3);
    pwr_writel(qts, PWR_CR3, value | PWR_CR3_UCPD_DBDIS);
    g_assert_cmphex(pwr_readl(qts, PWR_CR3), ==, 0x0000c000);
    g_assert_cmphex(pwr_readl(qts, PWR_SR2), ==, 0);

    value = pwr_readl(qts, PWR_CR5);
    g_assert_cmphex(value & PWR_CR5_R1MODE, ==, PWR_CR5_R1MODE);
    pwr_writel(qts, PWR_CR5, value & ~PWR_CR5_R1MODE);
    g_assert_cmphex(pwr_readl(qts, PWR_CR5), ==, 0);
    g_assert_cmphex(pwr_readl(qts, PWR_SR2), ==, 0);

    qtest_quit(qts);
}

static void test_pwr_rcc_reset(void)
{
    QTestState *qts = stm32g474_qtest_start();
    const uint32_t preserved_cr3 =
        PWR_CR3_RESET | PWR_CR3_UCPD_DBDIS;

    set_pwr_clock_gate(qts, true);
    dirty_pwr_registers(qts);

    rcc_writel(qts, RCC_APB1RSTR1, RCC_APB1RSTR1_PWRRST);
    g_assert_cmphex(pwr_readl(qts, PWR_CR1), ==, PWR_CR1_RESET);
    g_assert_cmphex(pwr_readl(qts, PWR_CR3), ==, preserved_cr3);
    g_assert_cmphex(pwr_readl(qts, PWR_SR2), ==, PWR_SR2_RESET);
    g_assert_cmphex(pwr_readl(qts, PWR_CR5), ==, 0);
    g_assert_cmphex(rcc_readl(qts, RCC_APB1ENR1) & RCC_APB1ENR1_PWREN,
                    ==, RCC_APB1ENR1_PWREN);
    assert_pwr_clock_hz(qts, 16000000);

    pwr_writel(qts, PWR_CR1, PWR_CR1_VOS_RANGE2);
    pwr_writel(qts, PWR_CR3, PWR_CR3_RESET);
    pwr_writel(qts, PWR_CR5, PWR_CR5_R1MODE);
    g_assert_cmphex(pwr_readl(qts, PWR_CR1), ==, PWR_CR1_RESET);
    g_assert_cmphex(pwr_readl(qts, PWR_CR3), ==, preserved_cr3);
    g_assert_cmphex(pwr_readl(qts, PWR_CR5), ==, 0);

    set_pwr_clock_gate(qts, false);
    g_assert_cmphex(rcc_readl(qts, RCC_APB1RSTR1) &
                    RCC_APB1RSTR1_PWRRST, ==, RCC_APB1RSTR1_PWRRST);
    assert_pwr_clock_hz(qts, 0);

    rcc_writel(qts, RCC_APB1RSTR1, 0);
    pwr_writel(qts, PWR_CR1, PWR_CR1_VOS_RANGE2);
    pwr_writel(qts, PWR_CR3, PWR_CR3_RESET);
    pwr_writel(qts, PWR_CR5, PWR_CR5_R1MODE);
    g_assert_cmphex(pwr_readl(qts, PWR_CR1), ==, PWR_CR1_VOS_RANGE2);
    g_assert_cmphex(pwr_readl(qts, PWR_CR3), ==, PWR_CR3_RESET);
    g_assert_cmphex(pwr_readl(qts, PWR_CR5), ==, PWR_CR5_R1MODE);
    assert_pwr_clock_hz(qts, 0);

    qtest_system_reset(qts);
    assert_power_on_reset_image(qts);

    qtest_quit(qts);
}

static void test_pwr_clock_gate(void)
{
    QTestState *qts = stm32g474_qtest_start();

    assert_pwr_clock_hz(qts, 0);
    set_pwr_clock_gate(qts, true);
    assert_pwr_clock_hz(qts, 16000000);
    set_pwr_clock_gate(qts, false);
    assert_pwr_clock_hz(qts, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/stm32g474/pwr/1-mmio-reset-mask",
                   test_pwr_mmio_reset_mask);
    qtest_add_func("/stm32g474/pwr/2-ardep-startup",
                   test_pwr_ardep_startup);
    qtest_add_func("/stm32g474/pwr/3-rcc-reset",
                   test_pwr_rcc_reset);
    qtest_add_func("/stm32g474/pwr/4-clock-gate",
                   test_pwr_clock_gate);

    return g_test_run();
}
