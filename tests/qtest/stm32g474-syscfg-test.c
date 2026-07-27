/*
 * QTest testcase for the STM32G474 system configuration controller
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
#define SYSCFG_QOM_PATH MCU_QOM_PATH "/syscfg"
#define SYSCFG_CLOCK_QOM_PATH SYSCFG_QOM_PATH "/clk"

#define SYSCFG_BASE 0x40010000ULL
#define SYSCFG_SIZE 0x30

#define SYSCFG_MEMRMP 0x00
#define SYSCFG_CFGR1 0x04
#define SYSCFG_EXTICR1 0x08
#define SYSCFG_EXTICR2 0x0c
#define SYSCFG_EXTICR3 0x10
#define SYSCFG_EXTICR4 0x14
#define SYSCFG_SCSR 0x18
#define SYSCFG_CFGR2 0x1c
#define SYSCFG_SWPR 0x20
#define SYSCFG_SKR 0x24

#define SYSCFG_CFGR1_RESET 0x7c000000U
#define SYSCFG_CFGR1_MASK 0xfcff0300U
#define SYSCFG_SCSR_CCMER (1U << 0)
#define SYSCFG_SCSR_CCMBSY (1U << 1)
#define SYSCFG_CFGR2_LOCK_MASK 0x0000000fU
#define SYSCFG_CFGR2_SPF (1U << 8)

#define SYSCFG_GPIO_PORT_COUNT 7
#define SYSCFG_GPIO_PIN_COUNT 16
#define SYSCFG_GPIOG_INDEX 6

#define RCC_BASE 0x40021000ULL
#define RCC_APB2RSTR 0x40
#define RCC_AHB2ENR 0x4c
#define RCC_APB2ENR 0x60
#define RCC_APB2_SYSCFG (1U << 0)
#define RCC_AHB2_GPIOBEN (1U << 1)
#define RCC_AHB2_GPIODEN (1U << 3)
#define RCC_AHB2_GPIOGEN (1U << 6)

#define GPIOB_BASE 0x48000400ULL
#define GPIOD_BASE 0x48000c00ULL
#define GPIOG_BASE 0x48001800ULL
#define GPIO_MODER 0x00
#define GPIO_ODR 0x14
#define GPIO_MODE_OUTPUT 1U

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

static uint32_t syscfg_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, SYSCFG_BASE + offset);
}

static void syscfg_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, SYSCFG_BASE + offset, value);
}

static uint32_t rcc_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, RCC_BASE + offset);
}

static void rcc_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, RCC_BASE + offset, value);
}

static uint64_t clock_period_from_hz(uint64_t hz)
{
    return hz ? CLOCK_PERIOD_1SEC / hz : 0;
}

static uint64_t syscfg_clock_period(QTestState *qts)
{
    QDict *response;
    uint64_t period;

    response = qtest_qmp(qts,
        "{ 'execute': 'qom-get', 'arguments': { 'path': %s, "
        "'property': 'qtest-clock-period' } }", SYSCFG_CLOCK_QOM_PATH);
    g_assert_false(qdict_haskey(response, "error"));
    period = qdict_get_int(response, "return");
    qobject_unref(response);

    return period;
}

static void assert_syscfg_clock_hz(QTestState *qts, uint64_t hz)
{
    g_assert_cmphex(syscfg_clock_period(qts), ==, clock_period_from_hz(hz));
}

static uint32_t exticr_offset(unsigned int line)
{
    g_assert_cmpuint(line, <, SYSCFG_GPIO_PIN_COUNT);

    return SYSCFG_EXTICR1 + (line / 4) * sizeof(uint32_t);
}

static unsigned int exticr_shift(unsigned int line)
{
    return (line % 4) * 4;
}

static uint32_t syscfg_get_selector(QTestState *qts, unsigned int line)
{
    return extract32(syscfg_readl(qts, exticr_offset(line)),
                     exticr_shift(line), 3);
}

static void syscfg_set_selector(QTestState *qts, unsigned int line,
                                unsigned int port)
{
    uint32_t offset = exticr_offset(line);
    unsigned int shift = exticr_shift(line);
    uint32_t value = syscfg_readl(qts, offset);

    value = deposit32(value, shift, 3, port);
    syscfg_writel(qts, offset, value);
}

static void syscfg_set_raw_selector(QTestState *qts, unsigned int line,
                                    unsigned int selector)
{
    uint32_t offset = exticr_offset(line);
    unsigned int shift = exticr_shift(line);
    uint32_t value = syscfg_readl(qts, offset);

    value = deposit32(value, shift, 4, selector);
    syscfg_writel(qts, offset, value);
}

static void syscfg_set_gpio(QTestState *qts, unsigned int port,
                            unsigned int pin, int level)
{
    g_assert_cmpuint(port, <, SYSCFG_GPIO_PORT_COUNT);
    g_assert_cmpuint(pin, <, SYSCFG_GPIO_PIN_COUNT);
    qtest_set_irq_in(qts, MCU_QOM_PATH, "gpio-in",
                     port * SYSCFG_GPIO_PIN_COUNT + pin, level);
}

static void syscfg_clear_gpio_line(QTestState *qts, unsigned int line)
{
    for (unsigned int port = 0; port < SYSCFG_GPIO_PORT_COUNT; port++) {
        syscfg_set_gpio(qts, port, line, 0);
    }
}

static void syscfg_set_parity_error(QTestState *qts, int level)
{
    qtest_set_irq_in(qts, SYSCFG_QOM_PATH, "parity-error", 0, level);
}

static void gpio_drive_output(QTestState *qts, uint64_t base,
                              unsigned int pin, bool level)
{
    uint32_t moder = qtest_readl(qts, base + GPIO_MODER);
    uint32_t odr = qtest_readl(qts, base + GPIO_ODR);

    moder = deposit32(moder, pin * 2, 2, GPIO_MODE_OUTPUT);
    qtest_writel(qts, base + GPIO_MODER, moder);
    qtest_writel(qts, base + GPIO_ODR,
                 level ? odr | BIT(pin) : odr & ~BIT(pin));
}

static void assert_syscfg_reset_image(QTestState *qts)
{
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_MEMRMP), ==, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR1), ==,
                    SYSCFG_CFGR1_RESET);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_EXTICR1), ==, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_EXTICR2), ==, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_EXTICR3), ==, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_EXTICR4), ==, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SCSR), ==, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SWPR), ==, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SKR), ==, 0);
}

static void test_topology_reset_masks_access(void)
{
    QTestState *qts = stm32g474_qtest_start();

    /*
     * Keep this first: before the feature exists the test must fail because
     * the architectural SYSCFG child is absent, not because an MMIO read
     * happens to return an unassigned-bus value.
     */
    assert_qom_path_exists(qts, SYSCFG_QOM_PATH);
    assert_syscfg_clock_hz(qts, 0);
    assert_syscfg_reset_image(qts);

    qtest_writeb(qts, SYSCFG_BASE + SYSCFG_CFGR1 + 3, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR1), ==,
                    SYSCFG_CFGR1_RESET);
    qtest_writew(qts, SYSCFG_BASE + SYSCFG_CFGR1 + 2, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR1), ==,
                    SYSCFG_CFGR1_RESET);

    syscfg_writel(qts, SYSCFG_MEMRMP, UINT32_MAX);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_MEMRMP), ==, 0);

    syscfg_writel(qts, SYSCFG_CFGR1, UINT32_MAX);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR1), ==,
                    SYSCFG_CFGR1_MASK);
    syscfg_writel(qts, SYSCFG_CFGR1, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR1), ==, 0);

    for (uint32_t offset = SYSCFG_EXTICR1;
         offset <= SYSCFG_EXTICR4; offset += sizeof(uint32_t)) {
        syscfg_writel(qts, offset, 0x00005432);
        g_assert_cmphex(syscfg_readl(qts, offset), ==, 0x00005432);
        syscfg_writel(qts, offset, 0x00008888);
        g_assert_cmphex(syscfg_readl(qts, offset), ==, 0x00005432);
    }

    syscfg_writel(qts, SYSCFG_SCSR, UINT32_MAX);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SCSR), ==, 0);

    syscfg_writel(qts, SYSCFG_CFGR2, UINT32_MAX);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==,
                    SYSCFG_CFGR2_LOCK_MASK);

    syscfg_writel(qts, SYSCFG_SWPR, 0xa5a55a5a);
    syscfg_writel(qts, SYSCFG_SWPR, 0x0000a5a5);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SWPR), ==, 0xa5a5ffff);

    syscfg_writel(qts, SYSCFG_SKR, UINT32_MAX);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SKR), ==, 0);

    for (uint32_t offset = SYSCFG_SKR + sizeof(uint32_t);
         offset < SYSCFG_SIZE; offset += sizeof(uint32_t)) {
        syscfg_writel(qts, offset, UINT32_MAX);
        g_assert_cmphex(syscfg_readl(qts, offset), ==, 0);
    }

    qtest_system_reset(qts);
    assert_syscfg_reset_image(qts);
    qtest_quit(qts);
}

static void test_gpio_selector_matrix(void)
{
    QTestState *qts = stm32g474_qtest_start();

    qtest_irq_intercept_out_named(qts, SYSCFG_QOM_PATH, "exti-out");
    qtest_system_reset(qts);

    for (unsigned int line = 0; line < SYSCFG_GPIO_PIN_COUNT; line++) {
        for (unsigned int port = 0; port < SYSCFG_GPIOG_INDEX; port++) {
            unsigned int other = (port + 1) % SYSCFG_GPIOG_INDEX;

            syscfg_clear_gpio_line(qts, line);
            syscfg_set_selector(qts, line, port);
            g_assert_cmpuint(syscfg_get_selector(qts, line), ==, port);

            syscfg_set_gpio(qts, port, line, 1);
            g_assert_true(qtest_get_irq(qts, line));
            syscfg_set_gpio(qts, other, line, 1);
            syscfg_set_gpio(qts, port, line, 0);
            g_assert_false(qtest_get_irq(qts, line));
        }

        if (line <= 10) {
            syscfg_clear_gpio_line(qts, line);
            syscfg_set_selector(qts, line, SYSCFG_GPIOG_INDEX);
            g_assert_cmpuint(syscfg_get_selector(qts, line), ==,
                             SYSCFG_GPIOG_INDEX);
            syscfg_set_gpio(qts, SYSCFG_GPIOG_INDEX, line, 1);
            g_assert_true(qtest_get_irq(qts, line));
            syscfg_set_gpio(qts, SYSCFG_GPIOG_INDEX, line, 0);
            g_assert_false(qtest_get_irq(qts, line));
        }
    }

    for (unsigned int line = 11; line < SYSCFG_GPIO_PIN_COUNT; line++) {
        syscfg_clear_gpio_line(qts, line);
        syscfg_set_selector(qts, line, 5);
        syscfg_set_gpio(qts, 5, line, 1);
        syscfg_set_gpio(qts, SYSCFG_GPIOG_INDEX, line, 1);
        g_assert_true(qtest_get_irq(qts, line));

        syscfg_set_selector(qts, line, SYSCFG_GPIOG_INDEX);
        g_assert_cmpuint(syscfg_get_selector(qts, line), ==, 5);
        syscfg_set_gpio(qts, 5, line, 0);
        g_assert_false(qtest_get_irq(qts, line));
    }

    for (unsigned int line = 0; line < SYSCFG_GPIO_PIN_COUNT; line++) {
        syscfg_clear_gpio_line(qts, line);
        syscfg_set_selector(qts, line, 1);
        syscfg_set_gpio(qts, 1, line, 1);
        g_assert_true(qtest_get_irq(qts, line));

        syscfg_set_selector(qts, line, 7);
        g_assert_cmpuint(syscfg_get_selector(qts, line), ==, 1);
        syscfg_set_gpio(qts, 1, line, 0);
        g_assert_false(qtest_get_irq(qts, line));
    }

    for (unsigned int line = 0; line < SYSCFG_GPIO_PIN_COUNT; line++) {
        for (unsigned int selector = 8; selector <= 15; selector++) {
            syscfg_set_selector(qts, line, 2);
            syscfg_set_raw_selector(qts, line, selector);
            g_assert_cmpuint(extract32(
                                 syscfg_readl(qts, exticr_offset(line)),
                                 exticr_shift(line), 4), ==, 2);
        }
    }

    syscfg_writel(qts, SYSCFG_EXTICR1, 0x00004321);
    syscfg_writel(qts, SYSCFG_EXTICR1, 0x00000f58);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_EXTICR1), ==, 0x00000351);

    qtest_quit(qts);
}

static void test_gpio_wiring_and_immediate_reroute(void)
{
    QTestState *qts = stm32g474_qtest_start();

    qtest_irq_intercept_out_named(qts, SYSCFG_QOM_PATH, "exti-out");
    qtest_system_reset(qts);

    rcc_writel(qts, RCC_AHB2ENR,
               rcc_readl(qts, RCC_AHB2ENR) | RCC_AHB2_GPIODEN);
    syscfg_set_selector(qts, 3, 3);
    gpio_drive_output(qts, GPIOD_BASE, 3, true);
    g_assert_true(qtest_get_irq(qts, 3));
    gpio_drive_output(qts, GPIOD_BASE, 3, false);
    g_assert_false(qtest_get_irq(qts, 3));

    syscfg_clear_gpio_line(qts, 2);
    syscfg_set_gpio(qts, 0, 2, 1);
    syscfg_set_selector(qts, 2, 0);
    g_assert_true(qtest_get_irq(qts, 2));

    syscfg_set_selector(qts, 2, 1);
    g_assert_false(qtest_get_irq(qts, 2));
    syscfg_set_gpio(qts, 0, 2, 0);
    syscfg_set_gpio(qts, 0, 2, 1);
    g_assert_false(qtest_get_irq(qts, 2));
    syscfg_set_gpio(qts, 1, 2, 1);
    g_assert_true(qtest_get_irq(qts, 2));

    qtest_quit(qts);
}

static void test_parity_set_only_and_key(void)
{
    QTestState *qts = stm32g474_qtest_start();

    syscfg_set_parity_error(qts, 1);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==,
                    SYSCFG_CFGR2_SPF);
    syscfg_writel(qts, SYSCFG_CFGR2, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==,
                    SYSCFG_CFGR2_SPF);
    syscfg_writel(qts, SYSCFG_CFGR2, SYSCFG_CFGR2_SPF);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==, 0);
    syscfg_set_parity_error(qts, 0);
    syscfg_set_parity_error(qts, 1);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==,
                    SYSCFG_CFGR2_SPF);

    syscfg_writel(qts, SYSCFG_CFGR2, 0x5);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==, 0x105);
    syscfg_writel(qts, SYSCFG_CFGR2, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==, 0x105);
    syscfg_writel(qts, SYSCFG_CFGR2, 0x10a);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==, 0x00f);

    syscfg_writel(qts, SYSCFG_SWPR, 0x80000001);
    syscfg_writel(qts, SYSCFG_SWPR, 0);
    syscfg_writel(qts, SYSCFG_SWPR, 0x00010002);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SWPR), ==, 0x80010003);

    syscfg_writel(qts, SYSCFG_SCSR, 1);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SCSR), ==, 0);

    syscfg_writel(qts, SYSCFG_SKR, 0xca);
    syscfg_writel(qts, SYSCFG_SKR, 0x00);
    syscfg_writel(qts, SYSCFG_SKR, 0x53);
    syscfg_writel(qts, SYSCFG_SCSR, 1);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SCSR), ==, 0);

    syscfg_writel(qts, SYSCFG_SKR, 0xca);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SKR), ==, 0);
    syscfg_writel(qts, SYSCFG_SKR, 0x53);
    syscfg_writel(qts, SYSCFG_SCSR, 1);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SCSR), ==,
                    SYSCFG_SCSR_CCMER);
    syscfg_writel(qts, SYSCFG_SCSR, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SCSR), ==,
                    SYSCFG_SCSR_CCMER);
    syscfg_writel(qts, SYSCFG_SCSR, SYSCFG_SCSR_CCMBSY);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SCSR), ==,
                    SYSCFG_SCSR_CCMER);

    qtest_system_reset(qts);
    assert_syscfg_reset_image(qts);
    qtest_quit(qts);
}

static void test_rcc_clock_and_reset(void)
{
    const uint32_t cfgr1_value = 0x84030300;
    const uint32_t swpr_value = 0x80000001;
    QTestState *qts = stm32g474_qtest_start();

    qtest_irq_intercept_out_named(qts, SYSCFG_QOM_PATH, "exti-out");
    qtest_system_reset(qts);
    assert_syscfg_clock_hz(qts, 0);

    rcc_writel(qts, RCC_APB2ENR,
               rcc_readl(qts, RCC_APB2ENR) | RCC_APB2_SYSCFG);
    assert_syscfg_clock_hz(qts, 16000000);

    syscfg_clear_gpio_line(qts, 0);
    syscfg_set_gpio(qts, 0, 0, 1);
    syscfg_set_selector(qts, 0, 1);
    g_assert_false(qtest_get_irq(qts, 0));

    syscfg_writel(qts, SYSCFG_CFGR1, cfgr1_value);
    syscfg_set_parity_error(qts, 1);
    syscfg_writel(qts, SYSCFG_CFGR2, 0x5);
    syscfg_writel(qts, SYSCFG_SWPR, swpr_value);
    syscfg_writel(qts, SYSCFG_SKR, 0xca);
    syscfg_writel(qts, SYSCFG_SKR, 0x53);
    syscfg_writel(qts, SYSCFG_SCSR, 1);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SCSR), ==, 1);
    syscfg_writel(qts, SYSCFG_SKR, 0xca);

    rcc_writel(qts, RCC_APB2RSTR,
               rcc_readl(qts, RCC_APB2RSTR) | RCC_APB2_SYSCFG);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR1), ==,
                    SYSCFG_CFGR1_RESET);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_EXTICR1), ==, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SCSR), ==, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==, 0x5);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SWPR), ==, swpr_value);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SKR), ==, 0);
    g_assert_true(qtest_get_irq(qts, 0));
    assert_syscfg_clock_hz(qts, 16000000);

    syscfg_writel(qts, SYSCFG_CFGR1, cfgr1_value);
    syscfg_set_selector(qts, 0, 1);
    syscfg_set_parity_error(qts, 0);
    syscfg_set_parity_error(qts, 1);
    syscfg_writel(qts, SYSCFG_CFGR2, 0xa);
    syscfg_writel(qts, SYSCFG_SWPR, 0x00010002);
    syscfg_writel(qts, SYSCFG_SKR, 0xca);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR1), ==,
                    SYSCFG_CFGR1_RESET);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_EXTICR1), ==, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==, 0x5);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SWPR), ==, swpr_value);
    g_assert_true(qtest_get_irq(qts, 0));

    rcc_writel(qts, RCC_APB2ENR,
               rcc_readl(qts, RCC_APB2ENR) & ~RCC_APB2_SYSCFG);
    assert_syscfg_clock_hz(qts, 0);
    rcc_writel(qts, RCC_APB2RSTR,
               rcc_readl(qts, RCC_APB2RSTR) & ~RCC_APB2_SYSCFG);

    syscfg_writel(qts, SYSCFG_CFGR1, cfgr1_value);
    syscfg_set_selector(qts, 0, 1);
    syscfg_writel(qts, SYSCFG_CFGR2, 0xa);
    syscfg_writel(qts, SYSCFG_SWPR, 0x00010002);
    syscfg_writel(qts, SYSCFG_SKR, 0x53);
    syscfg_writel(qts, SYSCFG_SCSR, 1);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR1), ==, cfgr1_value);
    g_assert_cmphex(syscfg_get_selector(qts, 0), ==, 1);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==, 0xf);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SWPR), ==, 0x80010003);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SCSR), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_system_reset(qts);
    assert_syscfg_reset_image(qts);
    g_assert_cmphex(rcc_readl(qts, RCC_APB2RSTR) & RCC_APB2_SYSCFG, ==, 0);
    g_assert_cmphex(rcc_readl(qts, RCC_APB2ENR) & RCC_APB2_SYSCFG, ==, 0);
    assert_syscfg_clock_hz(qts, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_cold_reset(void)
{
    const unsigned int reset_high_line = 13;
    QTestState *qts = stm32g474_qtest_start();

    qtest_irq_intercept_out_named(qts, SYSCFG_QOM_PATH, "exti-out");
    qtest_system_reset(qts);

    /*
     * PA13 resolves high from its reset pull-up. Use that architectural
     * source so GPIO and SYSCFG reset replay agree on the selected level.
     */
    syscfg_clear_gpio_line(qts, reset_high_line);
    syscfg_set_gpio(qts, 0, reset_high_line, 1);
    syscfg_set_selector(qts, reset_high_line, 1);
    syscfg_writel(qts, SYSCFG_CFGR1, SYSCFG_CFGR1_MASK);
    syscfg_writel(qts, SYSCFG_CFGR2, SYSCFG_CFGR2_LOCK_MASK);
    syscfg_writel(qts, SYSCFG_SWPR, UINT32_MAX);
    syscfg_writel(qts, SYSCFG_SKR, 0xca);
    g_assert_false(qtest_get_irq(qts, reset_high_line));

    for (unsigned int reset = 0; reset < 2; reset++) {
        qtest_system_reset(qts);
        assert_syscfg_reset_image(qts);
        g_assert_true(qtest_get_irq(qts, reset_high_line));

        syscfg_writel(qts, SYSCFG_SKR, 0x53);
        syscfg_writel(qts, SYSCFG_SCSR, 1);
        g_assert_cmphex(syscfg_readl(qts, SYSCFG_SCSR), ==, 0);
        syscfg_writel(qts, SYSCFG_CFGR1, 0x80000100);
        syscfg_set_selector(qts, reset_high_line, 1);
        g_assert_false(qtest_get_irq(qts, reset_high_line));
    }

    qtest_quit(qts);
}

static void configure_active_migration_source(QTestState *qts)
{
    rcc_writel(qts, RCC_APB2ENR,
               rcc_readl(qts, RCC_APB2ENR) | RCC_APB2_SYSCFG);
    rcc_writel(qts, RCC_AHB2ENR,
               rcc_readl(qts, RCC_AHB2ENR) |
               RCC_AHB2_GPIOBEN | RCC_AHB2_GPIOGEN);
    gpio_drive_output(qts, GPIOB_BASE, 0, true);
    gpio_drive_output(qts, GPIOG_BASE, 10, true);
    syscfg_set_selector(qts, 0, 1);
    syscfg_set_selector(qts, 10, SYSCFG_GPIOG_INDEX);
    syscfg_writel(qts, SYSCFG_CFGR1, 0x84030300);
    syscfg_set_parity_error(qts, 1);
    syscfg_writel(qts, SYSCFG_CFGR2, 0x5);
    syscfg_writel(qts, SYSCFG_SWPR, 0x80010003);
    syscfg_writel(qts, SYSCFG_SKR, 0xca);
}

static void assert_active_migration_state(QTestState *qts)
{
    assert_syscfg_clock_hz(qts, 16000000);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR1), ==, 0x84030300);
    g_assert_cmpuint(syscfg_get_selector(qts, 0), ==, 1);
    g_assert_cmpuint(syscfg_get_selector(qts, 10), ==,
                     SYSCFG_GPIOG_INDEX);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==, 0x105);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SWPR), ==, 0x80010003);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 10));

    syscfg_set_selector(qts, 0, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    syscfg_set_selector(qts, 0, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    syscfg_writel(qts, SYSCFG_CFGR2, SYSCFG_CFGR2_SPF);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==, 0x5);
    syscfg_writel(qts, SYSCFG_CFGR2, 0xa);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_CFGR2), ==, 0xf);
    syscfg_writel(qts, SYSCFG_SWPR, 0);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SWPR), ==, 0x80010003);

    /*
     * Finish the key sequence across migration. CCM erasure is deliberately
     * unimplemented, so CCMER remains set and CCMBSY remains clear.
     */
    syscfg_writel(qts, SYSCFG_SKR, 0x53);
    syscfg_writel(qts, SYSCFG_SCSR, 1);
    g_assert_cmphex(syscfg_readl(qts, SYSCFG_SCSR), ==, 1);
}

static void test_active_migration(void)
{
    QTestState *src;
    QTestState *dst;
    g_autofree char *tmpdir =
        g_dir_make_tmp("stm32g474-syscfg-migration-XXXXXX", NULL);
    g_autofree char *sock = g_strdup_printf("%s/migration.sock", tmpdir);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);

    g_assert_nonnull(tmpdir);
    src = stm32g474_qtest_start();
    dst = stm32g474_qtest_start_incoming();
    qtest_irq_intercept_out_named(dst, SYSCFG_QOM_PATH, "exti-out");

    configure_active_migration_source(src);
    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    qtest_qmp_eventwait(dst, "RESUME");

    assert_active_migration_state(dst);

    qtest_quit(src);
    qtest_quit(dst);
    g_unlink(sock);
    g_rmdir(tmpdir);
}

static void test_latched_ccmer_migration(void)
{
    QTestState *src;
    QTestState *dst;
    g_autofree char *tmpdir =
        g_dir_make_tmp("stm32g474-syscfg-ccmer-migration-XXXXXX", NULL);
    g_autofree char *sock = g_strdup_printf("%s/migration.sock", tmpdir);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);

    g_assert_nonnull(tmpdir);
    src = stm32g474_qtest_start();
    dst = stm32g474_qtest_start_incoming();

    syscfg_writel(src, SYSCFG_SKR, 0xca);
    syscfg_writel(src, SYSCFG_SKR, 0x53);
    syscfg_writel(src, SYSCFG_SCSR, SYSCFG_SCSR_CCMER);
    g_assert_cmphex(syscfg_readl(src, SYSCFG_SCSR), ==,
                    SYSCFG_SCSR_CCMER);

    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    qtest_qmp_eventwait(dst, "RESUME");

    g_assert_cmphex(syscfg_readl(dst, SYSCFG_SCSR), ==,
                    SYSCFG_SCSR_CCMER);
    syscfg_writel(dst, SYSCFG_SCSR, 0);
    g_assert_cmphex(syscfg_readl(dst, SYSCFG_SCSR), ==,
                    SYSCFG_SCSR_CCMER);

    qtest_quit(src);
    qtest_quit(dst);
    g_unlink(sock);
    g_rmdir(tmpdir);
}

static void test_held_reset_migration(void)
{
    QTestState *src;
    QTestState *dst;
    g_autofree char *tmpdir =
        g_dir_make_tmp("stm32g474-syscfg-reset-migration-XXXXXX", NULL);
    g_autofree char *sock = g_strdup_printf("%s/migration.sock", tmpdir);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);

    g_assert_nonnull(tmpdir);
    src = stm32g474_qtest_start();
    dst = stm32g474_qtest_start_incoming();

    syscfg_writel(src, SYSCFG_CFGR2, 0x5);
    syscfg_writel(src, SYSCFG_SWPR, 0x80000001);
    syscfg_writel(src, SYSCFG_SKR, 0xca);
    rcc_writel(src, RCC_APB2RSTR,
               rcc_readl(src, RCC_APB2RSTR) | RCC_APB2_SYSCFG);

    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    qtest_qmp_eventwait(dst, "RESUME");

    g_assert_cmphex(rcc_readl(dst, RCC_APB2RSTR) & RCC_APB2_SYSCFG, ==,
                    RCC_APB2_SYSCFG);
    g_assert_cmphex(syscfg_readl(dst, SYSCFG_CFGR1), ==,
                    SYSCFG_CFGR1_RESET);
    g_assert_cmphex(syscfg_readl(dst, SYSCFG_CFGR2), ==, 0x5);
    g_assert_cmphex(syscfg_readl(dst, SYSCFG_SWPR), ==, 0x80000001);

    syscfg_writel(dst, SYSCFG_CFGR1, 0x80000100);
    syscfg_writel(dst, SYSCFG_CFGR2, 0xa);
    syscfg_writel(dst, SYSCFG_SWPR, 0x00010002);
    g_assert_cmphex(syscfg_readl(dst, SYSCFG_CFGR1), ==,
                    SYSCFG_CFGR1_RESET);
    g_assert_cmphex(syscfg_readl(dst, SYSCFG_CFGR2), ==, 0x5);
    g_assert_cmphex(syscfg_readl(dst, SYSCFG_SWPR), ==, 0x80000001);

    rcc_writel(dst, RCC_APB2RSTR,
               rcc_readl(dst, RCC_APB2RSTR) & ~RCC_APB2_SYSCFG);
    syscfg_writel(dst, SYSCFG_CFGR1, 0x80000100);
    syscfg_writel(dst, SYSCFG_CFGR2, 0xa);
    syscfg_writel(dst, SYSCFG_SWPR, 0x00010002);
    syscfg_writel(dst, SYSCFG_SKR, 0x53);
    syscfg_writel(dst, SYSCFG_SCSR, 1);
    g_assert_cmphex(syscfg_readl(dst, SYSCFG_CFGR1), ==, 0x80000100);
    g_assert_cmphex(syscfg_readl(dst, SYSCFG_CFGR2), ==, 0xf);
    g_assert_cmphex(syscfg_readl(dst, SYSCFG_SWPR), ==, 0x80010003);
    g_assert_cmphex(syscfg_readl(dst, SYSCFG_SCSR), ==, 0);

    qtest_quit(src);
    qtest_quit(dst);
    g_unlink(sock);
    g_rmdir(tmpdir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/stm32g474/syscfg/1-topology-reset-masks-access",
                   test_topology_reset_masks_access);
    qtest_add_func("/stm32g474/syscfg/2-gpio-selector-matrix",
                   test_gpio_selector_matrix);
    qtest_add_func("/stm32g474/syscfg/3-gpio-wiring-immediate-reroute",
                   test_gpio_wiring_and_immediate_reroute);
    qtest_add_func("/stm32g474/syscfg/4-parity-set-only-key",
                   test_parity_set_only_and_key);
    qtest_add_func("/stm32g474/syscfg/5-rcc-clock-reset",
                   test_rcc_clock_and_reset);
    qtest_add_func("/stm32g474/syscfg/6-cold-reset",
                   test_cold_reset);
    qtest_add_func("/stm32g474/syscfg/7-active-migration",
                   test_active_migration);
    qtest_add_func("/stm32g474/syscfg/8-latched-ccmer-migration",
                   test_latched_ccmer_migration);
    qtest_add_func("/stm32g474/syscfg/9-held-reset-migration",
                   test_held_reset_migration);

    return g_test_run();
}
