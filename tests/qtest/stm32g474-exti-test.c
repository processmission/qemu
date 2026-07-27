/*
 * QTest testcase for the STM32G474 extended interrupt controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "libqtest.h"
#include "migration/migration-qmp.h"
#include "qemu/bitops.h"
#include "qobject/qdict.h"

#define STM32G474_MACHINE "stm32g474"
#define MCU_QOM_PATH "/machine/mcu"
#define EXTI_QOM_PATH MCU_QOM_PATH "/exti"
#define EXTI_CLOCK_QOM_PATH EXTI_QOM_PATH "/clk"
#define SYSCFG_QOM_PATH MCU_QOM_PATH "/syscfg"
#define SYSCFG_CLOCK_QOM_PATH SYSCFG_QOM_PATH "/clk"

#define EXTI_BASE 0x40010400ULL
#define EXTI_SIZE 0x400

#define EXTI_IMR1 0x00
#define EXTI_EMR1 0x04
#define EXTI_RTSR1 0x08
#define EXTI_FTSR1 0x0c
#define EXTI_SWIER1 0x10
#define EXTI_PR1 0x14
#define EXTI_IMR2 0x20
#define EXTI_EMR2 0x24
#define EXTI_RTSR2 0x28
#define EXTI_FTSR2 0x2c
#define EXTI_SWIER2 0x30
#define EXTI_PR2 0x34

#define EXTI_IMR1_RESET 0x1f840000U
#define EXTI_IMR2_RESET 0x00000c3cU
#define EXTI_CONFIG1_MASK 0xe07bffffU
#define EXTI_CONFIG2_MASK 0x00000303U
#define EXTI_VALID2_MASK 0x00000f3fU
#define EXTI_LINE_COUNT 44

#define SYSCFG_BASE 0x40010000ULL
#define SYSCFG_CFGR1 0x04
#define SYSCFG_EXTICR1 0x08
#define SYSCFG_CFGR1_RESET 0x7c000000U

#define RCC_BASE 0x40021000ULL
#define RCC_APB2RSTR 0x40
#define RCC_APB2ENR 0x60
#define RCC_APB2_SYSCFG BIT(0)

#define NVIC_ISER 0xe000e100ULL
#define NVIC_ISPR 0xe000e200ULL
#define NVIC_ICPR 0xe000e280ULL

#define EXTI0_IRQ 6
#define EXTI5_9_IRQ 23
#define EXTI10_15_IRQ 40

#define CLOCK_PERIOD_1SEC (1000000000ULL << 32)

static QTestState *stm32g474_qtest_start(void)
{
    return qtest_init("-M " STM32G474_MACHINE
                      " -serial null -serial null -serial null");
}

static QTestState *stm32g474_qtest_start_incoming(void)
{
    return qtest_init("-M " STM32G474_MACHINE
                      " -serial null -serial null -serial null"
                      " -incoming defer");
}

static void assert_qom_path_exists(QTestState *qts, const char *path)
{
    QDict *response;

    response = qtest_qmp(qts,
                         "{ 'execute': 'qom-list',"
                         "  'arguments': { 'path': %s } }", path);
    if (qdict_haskey(response, "error")) {
        QDict *error = qdict_get_qdict(response, "error");

        g_error("required QOM path %s: %s", path,
                qdict_get_str(error, "desc"));
    }
    g_assert_false(qdict_haskey(response, "error"));
    qobject_unref(response);
}

static uint32_t exti_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, EXTI_BASE + offset);
}

static void exti_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, EXTI_BASE + offset, value);
}

static void exti_set_input(QTestState *qts, unsigned int line, int level)
{
    g_assert_cmpuint(line, <, EXTI_LINE_COUNT);
    qtest_set_irq_in(qts, EXTI_QOM_PATH, "line-in", line, level);
}

static uint32_t syscfg_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, SYSCFG_BASE + offset);
}

static void syscfg_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, SYSCFG_BASE + offset, value);
}

static void syscfg_set_selector(QTestState *qts, unsigned int line,
                                unsigned int port)
{
    uint32_t offset = SYSCFG_EXTICR1 +
                      line / 4 * sizeof(uint32_t);
    unsigned int shift = line % 4 * 4;
    uint32_t value = syscfg_readl(qts, offset);

    g_assert_cmpuint(line, <, 16);
    g_assert_cmpuint(port, <, 7);
    value = deposit32(value, shift, 3, port);
    syscfg_writel(qts, offset, value);
}

static void syscfg_set_gpio(QTestState *qts, unsigned int port,
                            unsigned int pin, int level)
{
    g_assert_cmpuint(port, <, 7);
    g_assert_cmpuint(pin, <, 16);
    qtest_set_irq_in(qts, MCU_QOM_PATH, "gpio-in",
                     port * 16 + pin, level);
}

static uint32_t rcc_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, RCC_BASE + offset);
}

static void rcc_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, RCC_BASE + offset, value);
}

static uint64_t clock_period(QTestState *qts, const char *path)
{
    QDict *response;
    uint64_t period;

    response = qtest_qmp(qts,
        "{ 'execute': 'qom-get', 'arguments': { 'path': %s, "
        "'property': 'qtest-clock-period' } }", path);
    g_assert_false(qdict_haskey(response, "error"));
    period = qdict_get_int(response, "return");
    qobject_unref(response);

    return period;
}

static void assert_clock_hz(QTestState *qts, const char *path, uint64_t hz)
{
    uint64_t expected = hz ? CLOCK_PERIOD_1SEC / hz : 0;

    g_assert_cmphex(clock_period(qts, path), ==, expected);
}

static uint64_t nvic_reg(uint64_t base, unsigned int irq)
{
    return base + irq / 32 * sizeof(uint32_t);
}

static uint32_t nvic_mask(unsigned int irq)
{
    return BIT(irq % 32);
}

static void nvic_enable_irq(QTestState *qts, unsigned int irq)
{
    qtest_writel(qts, nvic_reg(NVIC_ISER, irq), nvic_mask(irq));
}

static void nvic_clear_pending(QTestState *qts, unsigned int irq)
{
    qtest_writel(qts, nvic_reg(NVIC_ICPR, irq), nvic_mask(irq));
}

static bool nvic_is_pending(QTestState *qts, unsigned int irq)
{
    return qtest_readl(qts, nvic_reg(NVIC_ISPR, irq)) & nvic_mask(irq);
}

static void assert_exti_reset_image(QTestState *qts)
{
    g_assert_cmphex(exti_readl(qts, EXTI_IMR1), ==, EXTI_IMR1_RESET);
    g_assert_cmphex(exti_readl(qts, EXTI_EMR1), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_RTSR1), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_FTSR1), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_IMR2), ==, EXTI_IMR2_RESET);
    g_assert_cmphex(exti_readl(qts, EXTI_EMR2), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_RTSR2), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_FTSR2), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER2), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR2), ==, 0);
}

static void test_topology_reset_masks_access(void)
{
    QTestState *qts = stm32g474_qtest_start();

    /*
     * Keep this first: before the feature exists the test must fail because
     * the architectural EXTI child is absent, not because an MMIO read
     * happens to return an unassigned-bus value.
     */
    assert_qom_path_exists(qts, EXTI_QOM_PATH);
    assert_clock_hz(qts, EXTI_CLOCK_QOM_PATH, 0);
    assert_exti_reset_image(qts);

    /*
     * Qtest reliably exposes below-minimum access rejection. It cannot prove
     * the device saw one misaligned word transaction because address_space
     * may split that request before it reaches the MemoryRegion.
     */
    qtest_writeb(qts, EXTI_BASE + EXTI_RTSR1, UINT8_MAX);
    qtest_writew(qts, EXTI_BASE + EXTI_FTSR1, UINT16_MAX);
    g_assert_cmphex(exti_readl(qts, EXTI_RTSR1), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_FTSR1), ==, 0);

    exti_writel(qts, EXTI_IMR1, UINT32_MAX);
    exti_writel(qts, EXTI_EMR1, UINT32_MAX);
    exti_writel(qts, EXTI_RTSR1, UINT32_MAX);
    exti_writel(qts, EXTI_FTSR1, UINT32_MAX);
    g_assert_cmphex(exti_readl(qts, EXTI_IMR1), ==, UINT32_MAX);
    g_assert_cmphex(exti_readl(qts, EXTI_EMR1), ==, UINT32_MAX);
    g_assert_cmphex(exti_readl(qts, EXTI_RTSR1), ==,
                    EXTI_CONFIG1_MASK);
    g_assert_cmphex(exti_readl(qts, EXTI_FTSR1), ==,
                    EXTI_CONFIG1_MASK);

    exti_writel(qts, EXTI_IMR1, 0);
    exti_writel(qts, EXTI_SWIER1, UINT32_MAX);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==,
                    EXTI_CONFIG1_MASK);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==,
                    EXTI_CONFIG1_MASK);
    exti_writel(qts, EXTI_PR1, UINT32_MAX);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);

    exti_writel(qts, EXTI_IMR2, UINT32_MAX);
    exti_writel(qts, EXTI_EMR2, UINT32_MAX);
    exti_writel(qts, EXTI_RTSR2, UINT32_MAX);
    exti_writel(qts, EXTI_FTSR2, UINT32_MAX);
    g_assert_cmphex(exti_readl(qts, EXTI_IMR2), ==,
                    EXTI_VALID2_MASK);
    g_assert_cmphex(exti_readl(qts, EXTI_EMR2), ==,
                    EXTI_VALID2_MASK);
    g_assert_cmphex(exti_readl(qts, EXTI_RTSR2), ==,
                    EXTI_CONFIG2_MASK);
    g_assert_cmphex(exti_readl(qts, EXTI_FTSR2), ==,
                    EXTI_CONFIG2_MASK);

    exti_writel(qts, EXTI_IMR2, 0);
    exti_writel(qts, EXTI_SWIER2, UINT32_MAX);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER2), ==,
                    EXTI_CONFIG2_MASK);
    g_assert_cmphex(exti_readl(qts, EXTI_PR2), ==,
                    EXTI_CONFIG2_MASK);
    exti_writel(qts, EXTI_PR2, UINT32_MAX);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER2), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR2), ==, 0);

    exti_writel(qts, 0x18, UINT32_MAX);
    exti_writel(qts, 0x1c, UINT32_MAX);
    exti_writel(qts, 0x38, UINT32_MAX);
    exti_writel(qts, EXTI_SIZE - sizeof(uint32_t), UINT32_MAX);
    g_assert_cmphex(exti_readl(qts, 0x18), ==, 0);
    g_assert_cmphex(exti_readl(qts, 0x1c), ==, 0);
    g_assert_cmphex(exti_readl(qts, 0x38), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_SIZE - sizeof(uint32_t)), ==, 0);

    qtest_system_reset(qts);
    assert_exti_reset_image(qts);

    qtest_quit(qts);
}

static void test_external_edges_and_level_irq(void)
{
    const unsigned int line = 1;
    const unsigned int reset_line = 40;
    const uint32_t bit = BIT(line);
    const uint32_t reset_bit = BIT(reset_line - 32);
    QTestState *qts = stm32g474_qtest_start();

    qtest_irq_intercept_out_named(qts, EXTI_QOM_PATH, "sysbus-irq");
    qtest_system_reset(qts);
    exti_set_input(qts, line, 0);

    exti_writel(qts, EXTI_IMR1, bit);
    exti_writel(qts, EXTI_RTSR1, bit);
    exti_writel(qts, EXTI_FTSR1, 0);
    exti_set_input(qts, line, 1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit);
    g_assert_true(qtest_get_irq(qts, line));

    exti_writel(qts, EXTI_PR1, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit);
    g_assert_true(qtest_get_irq(qts, line));
    exti_writel(qts, EXTI_PR1, bit);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);
    g_assert_false(qtest_get_irq(qts, line));

    exti_set_input(qts, line, 1);
    exti_set_input(qts, line, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);
    g_assert_false(qtest_get_irq(qts, line));

    exti_writel(qts, EXTI_IMR1, 0);
    exti_set_input(qts, line, 1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit);
    g_assert_false(qtest_get_irq(qts, line));
    exti_writel(qts, EXTI_IMR1, bit);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit);
    g_assert_true(qtest_get_irq(qts, line));
    exti_writel(qts, EXTI_PR1, bit);
    g_assert_false(qtest_get_irq(qts, line));

    exti_writel(qts, EXTI_IMR1, 0);
    exti_set_input(qts, line, 0);
    exti_writel(qts, EXTI_RTSR1, bit);
    exti_writel(qts, EXTI_FTSR1, bit);
    exti_writel(qts, EXTI_IMR1, bit);

    exti_set_input(qts, line, 1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit);
    g_assert_true(qtest_get_irq(qts, line));
    exti_writel(qts, EXTI_PR1, bit);
    g_assert_false(qtest_get_irq(qts, line));

    exti_set_input(qts, line, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit);
    g_assert_true(qtest_get_irq(qts, line));
    exti_writel(qts, EXTI_PR1, bit);
    g_assert_false(qtest_get_irq(qts, line));

    exti_writel(qts, EXTI_RTSR1, 0);
    exti_writel(qts, EXTI_FTSR1, 0);
    exti_set_input(qts, line, 1);
    exti_set_input(qts, line, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);

    exti_writel(qts, EXTI_RTSR1, bit);
    exti_set_input(qts, line, 1);
    g_assert_true(qtest_get_irq(qts, line));
    exti_writel(qts, EXTI_IMR1, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit);
    g_assert_false(qtest_get_irq(qts, line));
    exti_writel(qts, EXTI_IMR1, bit);
    g_assert_true(qtest_get_irq(qts, line));
    exti_writel(qts, EXTI_PR1, bit);
    g_assert_false(qtest_get_irq(qts, line));

    exti_set_input(qts, reset_line, 0);
    exti_writel(qts, EXTI_IMR2, reset_bit);
    exti_writel(qts, EXTI_RTSR2, reset_bit);
    exti_set_input(qts, reset_line, 1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR2), ==, reset_bit);
    g_assert_true(qtest_get_irq(qts, reset_line));

    qtest_system_reset(qts);
    g_assert_cmphex(exti_readl(qts, EXTI_PR2), ==, 0);
    g_assert_false(qtest_get_irq(qts, reset_line));

    exti_writel(qts, EXTI_IMR2, reset_bit);
    exti_writel(qts, EXTI_RTSR2, reset_bit);
    exti_set_input(qts, reset_line, 1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR2), ==, 0);
    g_assert_false(qtest_get_irq(qts, reset_line));
    exti_set_input(qts, reset_line, 0);
    exti_set_input(qts, reset_line, 1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR2), ==, reset_bit);
    g_assert_true(qtest_get_irq(qts, reset_line));

    qtest_quit(qts);
}

static void test_software_pending(void)
{
    const uint32_t bit0 = BIT(0);
    const uint32_t bit1 = BIT(1);
    const uint32_t bit40 = BIT(40 - 32);
    QTestState *qts = stm32g474_qtest_start();
    uint64_t raises;

    qtest_irq_intercept_out_named(qts, EXTI_QOM_PATH, "sysbus-irq");
    qtest_system_reset(qts);

    exti_writel(qts, EXTI_IMR1, bit0);
    exti_writel(qts, EXTI_SWIER1, bit0);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, bit0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit0);
    g_assert_true(qtest_get_irq(qts, 0));
    raises = qtest_get_irq_raise_count(qts, 0);
    exti_writel(qts, EXTI_SWIER1, bit0);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 0), ==, raises);

    exti_writel(qts, EXTI_SWIER1, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit0);
    g_assert_true(qtest_get_irq(qts, 0));
    exti_writel(qts, EXTI_PR1, bit0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    exti_writel(qts, EXTI_IMR1, 0);
    exti_writel(qts, EXTI_SWIER1, bit0);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, bit0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit0);
    g_assert_false(qtest_get_irq(qts, 0));
    exti_writel(qts, EXTI_IMR1, bit0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit0);
    g_assert_true(qtest_get_irq(qts, 0));

    /*
     * PR is W1C even when PR itself is clear. The raw write must still clear
     * the matching stored SWIER command bit.
     */
    exti_writel(qts, EXTI_PR1, bit0);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);

    exti_writel(qts, EXTI_IMR1, bit0 | bit1);
    exti_writel(qts, EXTI_SWIER1, bit0 | bit1);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, bit0 | bit1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit0 | bit1);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    exti_writel(qts, EXTI_PR1, bit0);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, bit1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit1);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    exti_writel(qts, EXTI_PR1, bit1);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);

    exti_writel(qts, EXTI_IMR2, bit40);
    exti_writel(qts, EXTI_SWIER2, bit40);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER2), ==, bit40);
    g_assert_cmphex(exti_readl(qts, EXTI_PR2), ==, bit40);
    g_assert_true(qtest_get_irq(qts, 40));
    exti_writel(qts, EXTI_PR2, bit40);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER2), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR2), ==, 0);
    g_assert_false(qtest_get_irq(qts, 40));

    qtest_quit(qts);
}

static void test_events_direct_and_holes(void)
{
    const unsigned int event_line = 2;
    const unsigned int swier_line = 3;
    const unsigned int reset_line = 40;
    const uint32_t event_bit = BIT(event_line);
    const uint32_t swier_bit = BIT(swier_line);
    const uint32_t reset_bit = BIT(reset_line - 32);
    QTestState *qts = stm32g474_qtest_start();
    uint64_t count;
    uint64_t direct18_count;
    uint64_t hole38_count;
    uint64_t direct43_count;

    qtest_irq_intercept_out_named(qts, EXTI_QOM_PATH, "event");
    qtest_system_reset(qts);
    exti_set_input(qts, event_line, 0);

    exti_writel(qts, EXTI_IMR1, 0);
    exti_writel(qts, EXTI_EMR1, event_bit);
    exti_writel(qts, EXTI_RTSR1, event_bit);
    count = qtest_get_irq_raise_count(qts, event_line);

    exti_set_input(qts, event_line, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, event_line),
                     ==, count + 1);
    g_assert_false(qtest_get_irq(qts, event_line));
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, event_bit);
    g_assert_false(nvic_is_pending(qts, EXTI0_IRQ + event_line));
    exti_writel(qts, EXTI_PR1, event_bit);

    exti_set_input(qts, event_line, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, event_line),
                     ==, count + 1);
    exti_set_input(qts, event_line, 0);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, event_line),
                     ==, count + 1);

    exti_writel(qts, EXTI_RTSR1, 0);
    exti_writel(qts, EXTI_FTSR1, event_bit);
    exti_set_input(qts, event_line, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, event_line),
                     ==, count + 1);
    exti_set_input(qts, event_line, 0);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, event_line),
                     ==, count + 2);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, event_bit);
    exti_writel(qts, EXTI_PR1, event_bit);

    /*
     * A masked 0-to-1 SWIER write latches PR without raising an IRQ.
     * Enabling IMR later exposes that pending interrupt, while enabling EMR
     * does not retroactively pulse an event.
     */
    exti_writel(qts, EXTI_EMR1, 0);
    exti_writel(qts, EXTI_IMR1, 0);
    exti_writel(qts, EXTI_SWIER1, swier_bit);
    count = qtest_get_irq_raise_count(qts, swier_line);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, swier_bit);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, swier_bit);

    exti_writel(qts, EXTI_EMR1, swier_bit);
    exti_writel(qts, EXTI_IMR1, swier_bit);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, swier_line),
                     ==, count);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, swier_bit);
    exti_writel(qts, EXTI_PR1, swier_bit);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, 0);

    exti_writel(qts, EXTI_IMR1, 0);
    exti_writel(qts, EXTI_SWIER1, swier_bit);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, swier_line),
                     ==, count + 1);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, swier_bit);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, swier_bit);
    exti_writel(qts, EXTI_SWIER1, swier_bit);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, swier_line),
                     ==, count + 1);
    exti_writel(qts, EXTI_PR1, swier_bit);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, 0);

    exti_writel(qts, EXTI_IMR1, swier_bit);
    exti_writel(qts, EXTI_EMR1, swier_bit);
    count = qtest_get_irq_raise_count(qts, swier_line);
    exti_writel(qts, EXTI_SWIER1, swier_bit);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, swier_line),
                     ==, count + 1);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, swier_bit);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, swier_bit);
    exti_writel(qts, EXTI_PR1, swier_bit);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);

    exti_set_input(qts, reset_line, 0);
    exti_writel(qts, EXTI_IMR2, 0);
    exti_writel(qts, EXTI_EMR2, reset_bit);
    exti_writel(qts, EXTI_RTSR2, reset_bit);
    count = qtest_get_irq_raise_count(qts, reset_line);
    exti_set_input(qts, reset_line, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, reset_line),
                     ==, count + 1);
    count++;

    qtest_system_reset(qts);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, reset_line),
                     ==, count);
    g_assert_cmphex(exti_readl(qts, EXTI_PR2), ==, 0);

    exti_writel(qts, EXTI_EMR2, reset_bit);
    exti_writel(qts, EXTI_RTSR2, reset_bit);
    exti_set_input(qts, reset_line, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, reset_line),
                     ==, count);
    exti_set_input(qts, reset_line, 0);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, reset_line),
                     ==, count);
    exti_set_input(qts, reset_line, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, reset_line),
                     ==, count + 1);

    exti_writel(qts, EXTI_EMR1, BIT(18));
    exti_writel(qts, EXTI_EMR2, BIT(43 - 32));
    exti_writel(qts, EXTI_RTSR1, UINT32_MAX);
    exti_writel(qts, EXTI_FTSR1, UINT32_MAX);
    exti_writel(qts, EXTI_RTSR2, UINT32_MAX);
    exti_writel(qts, EXTI_FTSR2, UINT32_MAX);

    direct18_count = qtest_get_irq_raise_count(qts, 18);
    hole38_count = qtest_get_irq_raise_count(qts, 38);
    direct43_count = qtest_get_irq_raise_count(qts, 43);
    exti_set_input(qts, 18, 1);
    exti_set_input(qts, 18, 0);
    exti_set_input(qts, 38, 1);
    exti_set_input(qts, 38, 0);
    exti_set_input(qts, 43, 1);
    exti_set_input(qts, 43, 0);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 18), ==,
                     direct18_count);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 38), ==,
                     hole38_count);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 43), ==,
                     direct43_count);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);
    g_assert_cmphex(exti_readl(qts, EXTI_PR2), ==, reset_bit);

    qtest_quit(qts);
}

static unsigned int exti_nvic_irq(unsigned int line)
{
    g_assert_cmpuint(line, <, 16);

    if (line <= 4) {
        return EXTI0_IRQ + line;
    }
    if (line <= 9) {
        return EXTI5_9_IRQ;
    }
    return EXTI10_15_IRQ;
}

static void assert_group_holds_pending(QTestState *qts,
                                       unsigned int first_line,
                                       unsigned int second_line,
                                       unsigned int irq)
{
    uint32_t first_bit = BIT(first_line);
    uint32_t second_bit = BIT(second_line);

    exti_set_input(qts, first_line, 1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, first_bit);
    g_assert_true(nvic_is_pending(qts, irq));

    exti_set_input(qts, second_line, 1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==,
                    first_bit | second_bit);
    g_assert_true(nvic_is_pending(qts, irq));

    exti_writel(qts, EXTI_PR1, first_bit);
    nvic_clear_pending(qts, irq);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, second_bit);
    g_assert_true(nvic_is_pending(qts, irq));

    exti_writel(qts, EXTI_PR1, second_bit);
    nvic_clear_pending(qts, irq);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);
    g_assert_false(nvic_is_pending(qts, irq));

    exti_set_input(qts, first_line, 0);
    exti_set_input(qts, second_line, 0);
}

static void test_nvic_routing(void)
{
    QTestState *qts = stm32g474_qtest_start();

    qtest_system_reset(qts);
    for (unsigned int line = 0; line < 16; line++) {
        exti_set_input(qts, line, 0);
    }

    exti_writel(qts, EXTI_IMR1, 0x0000ffff);
    exti_writel(qts, EXTI_RTSR1, 0x0000ffff);
    exti_writel(qts, EXTI_FTSR1, 0);
    for (unsigned int line = 0; line <= 4; line++) {
        nvic_enable_irq(qts, exti_nvic_irq(line));
    }
    nvic_enable_irq(qts, EXTI5_9_IRQ);
    nvic_enable_irq(qts, EXTI10_15_IRQ);

    for (unsigned int line = 0; line < 16; line++) {
        unsigned int irq = exti_nvic_irq(line);
        uint32_t bit = BIT(line);

        nvic_clear_pending(qts, irq);
        g_assert_false(nvic_is_pending(qts, irq));
        exti_set_input(qts, line, 1);
        g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit);
        g_assert_true(nvic_is_pending(qts, irq));

        exti_writel(qts, EXTI_PR1, bit);
        exti_set_input(qts, line, 0);
        nvic_clear_pending(qts, irq);
        g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);
        g_assert_false(nvic_is_pending(qts, irq));
    }

    assert_group_holds_pending(qts, 5, 9, EXTI5_9_IRQ);
    assert_group_holds_pending(qts, 10, 15, EXTI10_15_IRQ);

    qtest_quit(qts);
}

static void test_syscfg_end_to_end(void)
{
    const unsigned int gpioa = 0;
    const unsigned int gpiob = 1;
    const uint32_t bit = BIT(0);
    QTestState *qts = stm32g474_qtest_start();

    qtest_system_reset(qts);
    rcc_writel(qts, RCC_APB2ENR,
               rcc_readl(qts, RCC_APB2ENR) | RCC_APB2_SYSCFG);
    syscfg_set_gpio(qts, gpioa, 0, 0);
    syscfg_set_gpio(qts, gpiob, 0, 1);

    /*
     * Zephyr selects EXTICR before programming the trigger and IMR. Routing
     * the already-high PB0 first must therefore not create pending state.
     */
    syscfg_set_selector(qts, 0, gpiob);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);
    exti_writel(qts, EXTI_RTSR1, bit);
    exti_writel(qts, EXTI_FTSR1, 0);
    exti_writel(qts, EXTI_IMR1, bit);
    nvic_enable_irq(qts, EXTI0_IRQ);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);
    g_assert_false(nvic_is_pending(qts, EXTI0_IRQ));

    syscfg_set_gpio(qts, gpiob, 0, 0);
    syscfg_set_gpio(qts, gpiob, 0, 1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit);
    g_assert_true(nvic_is_pending(qts, EXTI0_IRQ));
    exti_writel(qts, EXTI_PR1, bit);
    nvic_clear_pending(qts, EXTI0_IRQ);

    /*
     * With rising trigger and IMR already active, a low-to-high reroute is a
     * normal EXTI edge. Keep PB0 low and PA0 high before selecting GPIOA.
     */
    syscfg_set_gpio(qts, gpiob, 0, 0);
    syscfg_set_gpio(qts, gpioa, 0, 1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);
    syscfg_set_selector(qts, 0, gpioa);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, bit);
    g_assert_true(nvic_is_pending(qts, EXTI0_IRQ));
    exti_writel(qts, EXTI_PR1, bit);
    nvic_clear_pending(qts, EXTI0_IRQ);

    syscfg_set_gpio(qts, gpiob, 0, 1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, 0);
    g_assert_false(nvic_is_pending(qts, EXTI0_IRQ));

    qtest_quit(qts);
}

static void test_clock_and_reset_scope(void)
{
    const uint32_t imr1 = BIT(0);
    const uint32_t emr1 = BIT(1);
    const uint32_t rtsr1 = BIT(0) | BIT(1);
    const uint32_t ftsr1 = BIT(1);
    const uint32_t swier1 = BIT(0);
    const uint32_t imr2 = BIT(40 - 32);
    const uint32_t emr2 = BIT(41 - 32);
    const uint32_t rtsr2 = BIT(40 - 32) | BIT(41 - 32);
    QTestState *qts = stm32g474_qtest_start();

    qtest_system_reset(qts);
    assert_clock_hz(qts, SYSCFG_CLOCK_QOM_PATH, 0);
    assert_clock_hz(qts, EXTI_CLOCK_QOM_PATH, 0);

    rcc_writel(qts, RCC_APB2ENR,
               rcc_readl(qts, RCC_APB2ENR) | RCC_APB2_SYSCFG);
    assert_clock_hz(qts, SYSCFG_CLOCK_QOM_PATH, 16000000);
    assert_clock_hz(qts, EXTI_CLOCK_QOM_PATH, 16000000);

    syscfg_writel(qts, SYSCFG_CFGR1, 0x80000100);
    exti_writel(qts, EXTI_IMR1, imr1);
    exti_writel(qts, EXTI_EMR1, emr1);
    exti_writel(qts, EXTI_RTSR1, rtsr1);
    exti_writel(qts, EXTI_FTSR1, ftsr1);
    exti_writel(qts, EXTI_SWIER1, swier1);
    exti_writel(qts, EXTI_IMR2, imr2);
    exti_writel(qts, EXTI_EMR2, emr2);
    exti_writel(qts, EXTI_RTSR2, rtsr2);

    rcc_writel(qts, RCC_APB2RSTR,
               rcc_readl(qts, RCC_APB2RSTR) | RCC_APB2_SYSCFG);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR1), ==,
                    SYSCFG_CFGR1_RESET);
    g_assert_cmphex(exti_readl(qts, EXTI_IMR1), ==, imr1);
    g_assert_cmphex(exti_readl(qts, EXTI_EMR1), ==, emr1);
    g_assert_cmphex(exti_readl(qts, EXTI_RTSR1), ==, rtsr1);
    g_assert_cmphex(exti_readl(qts, EXTI_FTSR1), ==, ftsr1);
    g_assert_cmphex(exti_readl(qts, EXTI_SWIER1), ==, swier1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, swier1);
    g_assert_cmphex(exti_readl(qts, EXTI_IMR2), ==, imr2);
    g_assert_cmphex(exti_readl(qts, EXTI_EMR2), ==, emr2);
    g_assert_cmphex(exti_readl(qts, EXTI_RTSR2), ==, rtsr2);
    assert_clock_hz(qts, EXTI_CLOCK_QOM_PATH, 16000000);

    rcc_writel(qts, RCC_APB2RSTR,
               rcc_readl(qts, RCC_APB2RSTR) & ~RCC_APB2_SYSCFG);
    qtest_system_reset(qts);
    assert_exti_reset_image(qts);
    g_assert_cmphex(rcc_readl(qts, RCC_APB2RSTR) & RCC_APB2_SYSCFG,
                    ==, 0);
    g_assert_cmphex(rcc_readl(qts, RCC_APB2ENR) & RCC_APB2_SYSCFG,
                    ==, 0);
    assert_clock_hz(qts, SYSCFG_CLOCK_QOM_PATH, 0);
    assert_clock_hz(qts, EXTI_CLOCK_QOM_PATH, 0);

    qtest_quit(qts);
}

static void configure_migration_source(QTestState *qts)
{
    const uint32_t line0 = BIT(0);
    const uint32_t line40 = BIT(40 - 32);

    qtest_system_reset(qts);
    rcc_writel(qts, RCC_APB2ENR,
               rcc_readl(qts, RCC_APB2ENR) | RCC_APB2_SYSCFG);

    syscfg_set_gpio(qts, 0, 0, 0);
    syscfg_set_gpio(qts, 1, 0, 0);
    syscfg_set_selector(qts, 0, 1);
    exti_writel(qts, EXTI_IMR1, line0);
    exti_writel(qts, EXTI_RTSR1, line0);
    nvic_enable_irq(qts, EXTI0_IRQ);
    syscfg_set_gpio(qts, 1, 0, 1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR1), ==, line0);
    g_assert_true(nvic_is_pending(qts, EXTI0_IRQ));

    exti_set_input(qts, 40, 0);
    exti_writel(qts, EXTI_IMR2, 0);
    exti_writel(qts, EXTI_EMR2, line40);
    exti_writel(qts, EXTI_RTSR2, line40);
    exti_set_input(qts, 40, 1);
    g_assert_cmphex(exti_readl(qts, EXTI_PR2), ==, line40);
}

static void assert_pending_output_migration(void)
{
    const uint32_t line40 = BIT(40 - 32);
    QTestState *src;
    QTestState *dst;
    uint64_t raises;
    g_autofree char *tmpdir =
        g_dir_make_tmp("stm32g474-exti-pending-migration-XXXXXX", NULL);
    g_autofree char *sock = g_strdup_printf("%s/migration.sock", tmpdir);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);

    g_assert_nonnull(tmpdir);
    src = stm32g474_qtest_start();
    dst = stm32g474_qtest_start_incoming();
    qtest_irq_intercept_out_named(dst, EXTI_QOM_PATH, "sysbus-irq");
    raises = qtest_get_irq_raise_count(dst, 40);

    qtest_system_reset(src);
    exti_writel(src, EXTI_IMR2, line40);
    exti_writel(src, EXTI_SWIER2, line40);
    g_assert_cmphex(exti_readl(src, EXTI_SWIER2), ==, line40);
    g_assert_cmphex(exti_readl(src, EXTI_PR2), ==, line40);

    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    qtest_qmp_eventwait(dst, "RESUME");

    g_assert_cmphex(exti_readl(dst, EXTI_IMR2), ==, line40);
    g_assert_cmphex(exti_readl(dst, EXTI_SWIER2), ==, line40);
    g_assert_cmphex(exti_readl(dst, EXTI_PR2), ==, line40);
    g_assert_cmpuint(qtest_get_irq_raise_count(dst, 40), ==, raises + 1);
    g_assert_true(qtest_get_irq(dst, 40));

    exti_writel(dst, EXTI_PR2, line40);
    g_assert_cmphex(exti_readl(dst, EXTI_SWIER2), ==, 0);
    g_assert_cmphex(exti_readl(dst, EXTI_PR2), ==, 0);
    g_assert_false(qtest_get_irq(dst, 40));

    qtest_quit(src);
    qtest_quit(dst);
    g_unlink(sock);
    g_rmdir(tmpdir);
}

static void test_active_migration(void)
{
    const uint32_t line0 = BIT(0);
    const uint32_t line40 = BIT(40 - 32);
    QTestState *src;
    QTestState *dst;
    uint64_t event_count;
    g_autofree char *tmpdir =
        g_dir_make_tmp("stm32g474-exti-migration-XXXXXX", NULL);
    g_autofree char *sock = g_strdup_printf("%s/migration.sock", tmpdir);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);

    g_assert_nonnull(tmpdir);
    src = stm32g474_qtest_start();
    dst = stm32g474_qtest_start_incoming();
    qtest_irq_intercept_out_named(dst, EXTI_QOM_PATH, "event");
    event_count = qtest_get_irq_raise_count(dst, 40);

    configure_migration_source(src);
    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    qtest_qmp_eventwait(dst, "RESUME");

    assert_clock_hz(dst, EXTI_CLOCK_QOM_PATH, 16000000);
    g_assert_cmphex(exti_readl(dst, EXTI_IMR1), ==, line0);
    g_assert_cmphex(exti_readl(dst, EXTI_RTSR1), ==, line0);
    g_assert_cmphex(exti_readl(dst, EXTI_PR1), ==, line0);
    g_assert_true(nvic_is_pending(dst, EXTI0_IRQ));
    g_assert_cmphex(exti_readl(dst, EXTI_IMR2), ==, 0);
    g_assert_cmphex(exti_readl(dst, EXTI_EMR2), ==, line40);
    g_assert_cmphex(exti_readl(dst, EXTI_RTSR2), ==, line40);
    g_assert_cmphex(exti_readl(dst, EXTI_PR2), ==, line40);
    g_assert_cmpuint(qtest_get_irq_raise_count(dst, 40), ==,
                     event_count);

    exti_writel(dst, EXTI_PR1, line0);
    nvic_clear_pending(dst, EXTI0_IRQ);
    syscfg_set_gpio(dst, 1, 0, 1);
    g_assert_cmphex(exti_readl(dst, EXTI_PR1), ==, 0);
    g_assert_false(nvic_is_pending(dst, EXTI0_IRQ));
    syscfg_set_gpio(dst, 1, 0, 0);
    syscfg_set_gpio(dst, 1, 0, 1);
    g_assert_cmphex(exti_readl(dst, EXTI_PR1), ==, line0);
    g_assert_true(nvic_is_pending(dst, EXTI0_IRQ));

    exti_set_input(dst, 40, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(dst, 40), ==,
                     event_count);
    exti_set_input(dst, 40, 0);
    g_assert_cmpuint(qtest_get_irq_raise_count(dst, 40), ==,
                     event_count);
    exti_set_input(dst, 40, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(dst, 40), ==,
                     event_count + 1);

    qtest_quit(src);
    qtest_quit(dst);
    g_unlink(sock);
    g_rmdir(tmpdir);

    assert_pending_output_migration();
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/stm32g474/exti/1-topology-reset-masks-access",
                   test_topology_reset_masks_access);
    qtest_add_func("/stm32g474/exti/2-external-edges-level-irq",
                   test_external_edges_and_level_irq);
    qtest_add_func("/stm32g474/exti/3-software-pending",
                   test_software_pending);
    qtest_add_func("/stm32g474/exti/4-events-direct-holes",
                   test_events_direct_and_holes);
    qtest_add_func("/stm32g474/exti/5-nvic-routing",
                   test_nvic_routing);
    qtest_add_func("/stm32g474/exti/6-syscfg-end-to-end",
                   test_syscfg_end_to_end);
    qtest_add_func("/stm32g474/exti/7-clock-reset-scope",
                   test_clock_and_reset_scope);
    qtest_add_func("/stm32g474/exti/8-active-migration",
                   test_active_migration);

    return g_test_run();
}
