/*
 * QTest for the STM32G474 RCC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define STM32G474_MACHINE "stm32g474"

#define RCC_BASE 0x40021000ULL
#define RCC_QOM_PATH "/machine/mcu/rcc"

#define RCC_CR          0x00
#define RCC_ICSCR       0x04
#define RCC_CFGR        0x08
#define RCC_PLLCFGR     0x0c
#define RCC_CIER        0x18
#define RCC_CIFR        0x1c
#define RCC_CICR        0x20
#define RCC_AHB1RSTR    0x28
#define RCC_AHB2RSTR    0x2c
#define RCC_AHB3RSTR    0x30
#define RCC_APB1RSTR1   0x38
#define RCC_APB1RSTR2   0x3c
#define RCC_APB2RSTR    0x40
#define RCC_AHB1ENR     0x48
#define RCC_AHB2ENR     0x4c
#define RCC_AHB3ENR     0x50
#define RCC_APB1ENR1    0x58
#define RCC_APB1ENR2    0x5c
#define RCC_APB2ENR     0x60
#define RCC_AHB1SMENR   0x68
#define RCC_AHB2SMENR   0x6c
#define RCC_AHB3SMENR   0x70
#define RCC_APB1SMENR1  0x78
#define RCC_APB1SMENR2  0x7c
#define RCC_APB2SMENR   0x80
#define RCC_CCIPR       0x88
#define RCC_BDCR        0x90
#define RCC_CSR         0x94
#define RCC_CRRCR       0x98
#define RCC_CCIPR2      0x9c

#define CR_HSION        (1U << 8)
#define CR_HSIKERON     (1U << 9)
#define CR_HSIRDY       (1U << 10)
#define CR_HSEON        (1U << 16)
#define CR_HSERDY       (1U << 17)
#define CR_HSEBYP       (1U << 18)
#define CR_CSSON        (1U << 19)
#define CR_PLLON        (1U << 24)
#define CR_PLLRDY       (1U << 25)

#define ICSCR_MODEL_RESET 0x40400000U

#define CFGR_SW_MASK    (3U << 0)
#define CFGR_SW_HSI16   (1U << 0)
#define CFGR_SW_HSE     (2U << 0)
#define CFGR_SW_PLL     (3U << 0)
#define CFGR_SWS_MASK   (3U << 2)
#define CFGR_SWS_HSI16  (1U << 2)
#define CFGR_SWS_PLL    (3U << 2)
#define CFGR_HPRE_MASK  (0xfU << 4)
#define CFGR_HPRE_DIV2  (8U << 4)
#define CFGR_PPRE1_MASK (7U << 8)
#define CFGR_PPRE1_DIV4 (5U << 8)
#define CFGR_PPRE2_MASK (7U << 11)
#define CFGR_PPRE2_DIV8 (6U << 11)

#define PLLCFGR_HSI16_170MHZ 0x01105532U
#define PLLCFGR_PROTECTED_MASK (0xff627ff3U)
#define PLLCFGR_PLLPEN       (1U << 16)
#define PLLCFGR_PLLREN       (1U << 24)
#define PLLCFGR_PLLSRC_HSI16 2U

#define CIER_LSIRDY       (1U << 0)
#define CIER_HSI48RDY     (1U << 10)
#define CIFR_LSIRDY       (1U << 0)
#define CIFR_HSI48RDY     (1U << 10)

#define AHB1ENR_FLASHEN   (1U << 8)
#define AHB1ENR_DMA1EN    (1U << 0)
#define AHB2ENR_GPIO_MASK 0x0000007fU
#define APB1ENR1_USART2EN (1U << 17)
#define APB1ENR1_UART4EN  (1U << 19)
#define APB1ENR1_USBEN    (1U << 23)
#define APB1ENR1_FDCANEN  (1U << 25)
#define APB1ENR1_PWREN    (1U << 28)
#define APB2ENR_SYSCFGEN  (1U << 0)
#define APB2ENR_USART1EN  (1U << 14)

#define APB1RSTR1_USART2RST (1U << 17)
#define APB1RSTR1_UART4RST  (1U << 19)
#define APB1RSTR1_USBRST    (1U << 23)
#define APB1RSTR1_FDCANRST  (1U << 25)
#define APB1RSTR1_PWRRST    (1U << 28)
#define APB2RSTR_SYSCFGRST  (1U << 0)
#define APB2RSTR_USART1RST  (1U << 14)
#define AHB1RSTR_FLASHRST   (1U << 8)
#define AHB2RSTR_GPIOARST   (1U << 0)
#define AHB2RSTR_GPIOBRST   (1U << 1)
#define AHB2RSTR_GPIOCRST   (1U << 2)
#define AHB2RSTR_GPIODRST   (1U << 3)
#define AHB2RSTR_GPIOERST   (1U << 4)
#define AHB2RSTR_GPIOFRST   (1U << 5)
#define AHB2RSTR_GPIOGRST   (1U << 6)
#define AHB2RSTR_GPIO_MASK  0x0000007fU

#define CCIPR_USART1_SHIFT 0
#define CCIPR_USART2_SHIFT 2
#define CCIPR_UART4_SHIFT  6
#define CCIPR_FDCAN_SHIFT  24
#define CCIPR_CLK48_SHIFT  26
#define CCIPR_MUX_MASK(shift) (3U << (shift))
#define CCIPR_MUX(value, shift) ((uint32_t)(value) << (shift))

#define CSR_LSION        (1U << 0)
#define CSR_LSIRDY       (1U << 1)
#define CRRCR_HSI48ON    (1U << 0)
#define CRRCR_HSI48RDY   (1U << 1)
#define CRRCR_MODEL_RESET 0x00000000U

#define BDCR_READY_RO_MASK ((1U << 6) | (1U << 1))
#define BDCR_WRITABLE_MASK (0x0301837fU & ~BDCR_READY_RO_MASK)

#define NVIC_ISER0 0xe000e100ULL
#define NVIC_ISPR0 0xe000e200ULL
#define NVIC_ICPR0 0xe000e280ULL
#define RCC_IRQ_BIT (1U << 5)

#define CLOCK_PERIOD_1SEC (1000000000ULL << 32)

enum {
    RCC_RESET_USART1,
    RCC_RESET_USART2,
    RCC_RESET_UART4,
    RCC_RESET_PWR,
    RCC_RESET_FLASH,
    RCC_RESET_GPIOA,
    RCC_RESET_GPIOB,
    RCC_RESET_GPIOC,
    RCC_RESET_GPIOD,
    RCC_RESET_GPIOE,
    RCC_RESET_GPIOF,
    RCC_RESET_GPIOG,
    RCC_RESET_SYSCFG,
    RCC_RESET_FDCAN,
    RCC_RESET_USB,
    RCC_RESET_COUNT,
};

G_STATIC_ASSERT(RCC_RESET_USART1 == 0);
G_STATIC_ASSERT(RCC_RESET_USART2 == 1);
G_STATIC_ASSERT(RCC_RESET_UART4 == 2);
G_STATIC_ASSERT(RCC_RESET_PWR == 3);
G_STATIC_ASSERT(RCC_RESET_FLASH == 4);
G_STATIC_ASSERT(RCC_RESET_GPIOA == 5);
G_STATIC_ASSERT(RCC_RESET_GPIOB == 6);
G_STATIC_ASSERT(RCC_RESET_GPIOC == 7);
G_STATIC_ASSERT(RCC_RESET_GPIOD == 8);
G_STATIC_ASSERT(RCC_RESET_GPIOE == 9);
G_STATIC_ASSERT(RCC_RESET_GPIOF == 10);
G_STATIC_ASSERT(RCC_RESET_GPIOG == 11);
G_STATIC_ASSERT(RCC_RESET_SYSCFG == 12);
G_STATIC_ASSERT(RCC_RESET_FDCAN == 13);
G_STATIC_ASSERT(RCC_RESET_USB == 14);
G_STATIC_ASSERT(RCC_RESET_COUNT == 15);

typedef struct RccRegister {
    const char *name;
    uint32_t offset;
    uint32_t reset;
    uint32_t implemented;
} RccRegister;

typedef struct RccResetOutput {
    const char *name;
    uint32_t offset;
    uint32_t bit;
    unsigned int index;
} RccResetOutput;

static const RccRegister rcc_registers[] = {
    { "CR",         RCC_CR,         0x00000500, 0x030f0700 },
    { "ICSCR",      RCC_ICSCR,      ICSCR_MODEL_RESET, 0x7fff0000 },
    { "CFGR",       RCC_CFGR,       0x00000005, 0x7f003fff },
    { "PLLCFGR",    RCC_PLLCFGR,    0x00001000, 0xff737ff3 },
    { "CIER",       RCC_CIER,       0x00000000, 0x0000063b },
    { "CIFR",       RCC_CIFR,       0x00000000, 0x0000073b },
    { "CICR",       RCC_CICR,       0x00000000, 0x0000073b },
    { "AHB1RSTR",   RCC_AHB1RSTR,   0x00000000, 0x0000111f },
    { "AHB2RSTR",   RCC_AHB2RSTR,   0x00000000, 0x040f607f },
    { "AHB3RSTR",   RCC_AHB3RSTR,   0x00000000, 0x00000101 },
    { "APB1RSTR1",  RCC_APB1RSTR1,  0x00000000, 0xd2fec13f },
    { "APB1RSTR2",  RCC_APB1RSTR2,  0x00000000, 0x00000103 },
    { "APB2RSTR",   RCC_APB2RSTR,   0x00000000, 0x0437f801 },
    { "AHB1ENR",    RCC_AHB1ENR,    0x00000100, 0x0000111f },
    { "AHB2ENR",    RCC_AHB2ENR,    0x00000000, 0x040f607f },
    { "AHB3ENR",    RCC_AHB3ENR,    0x00000000, 0x00000101 },
    { "APB1ENR1",   RCC_APB1ENR1,   0x00000400, 0xd2fecd3f },
    { "APB1ENR2",   RCC_APB1ENR2,   0x00000000, 0x00000103 },
    { "APB2ENR",    RCC_APB2ENR,    0x00000000, 0x0437f801 },
    { "AHB1SMENR",  RCC_AHB1SMENR,  0x0000131f, 0x0000131f },
    { "AHB2SMENR",  RCC_AHB2SMENR,  0x040f667f, 0x040f667f },
    { "AHB3SMENR",  RCC_AHB3SMENR,  0x00000101, 0x00000101 },
    { "APB1SMENR1", RCC_APB1SMENR1, 0xd2fecd3f, 0xd2fecd3f },
    { "APB1SMENR2", RCC_APB1SMENR2, 0x00000103, 0x00000103 },
    { "APB2SMENR",  RCC_APB2SMENR,  0x0437f801, 0x0437f801 },
    { "CCIPR",      RCC_CCIPR,      0x00000000, 0xffffffff },
    { "BDCR",       RCC_BDCR,       0x00000000, 0x0301837f },
    { "CSR",        RCC_CSR,        0x0c000000, 0xfe800003 },
    { "CRRCR",      RCC_CRRCR,      CRRCR_MODEL_RESET, 0x0000ff83 },
    { "CCIPR2",     RCC_CCIPR2,     0x00000000, 0x00300003 },
};

static const RccRegister all_ones_registers[] = {
    { "AHB1RSTR",   RCC_AHB1RSTR,   0, 0x0000111f },
    { "AHB2RSTR",   RCC_AHB2RSTR,   0, 0x040f607f },
    { "AHB3RSTR",   RCC_AHB3RSTR,   0, 0x00000101 },
    { "APB1RSTR1",  RCC_APB1RSTR1,  0, 0xd2fec13f },
    { "APB1RSTR2",  RCC_APB1RSTR2,  0, 0x00000103 },
    { "APB2RSTR",   RCC_APB2RSTR,   0, 0x0437f801 },
    { "AHB1ENR",    RCC_AHB1ENR,    0, 0x0000111f },
    { "AHB2ENR",    RCC_AHB2ENR,    0, 0x040f607f },
    { "AHB3ENR",    RCC_AHB3ENR,    0, 0x00000101 },
    { "APB1ENR1",   RCC_APB1ENR1,   0, 0xd2fecd3f },
    { "APB1ENR2",   RCC_APB1ENR2,   0, 0x00000103 },
    { "APB2ENR",    RCC_APB2ENR,    0, 0x0437f801 },
    { "AHB1SMENR",  RCC_AHB1SMENR,  0, 0x0000131f },
    { "AHB2SMENR",  RCC_AHB2SMENR,  0, 0x040f667f },
    { "AHB3SMENR",  RCC_AHB3SMENR,  0, 0x00000101 },
    { "APB1SMENR1", RCC_APB1SMENR1, 0, 0xd2fecd3f },
    { "APB1SMENR2", RCC_APB1SMENR2, 0, 0x00000103 },
    { "APB2SMENR",  RCC_APB2SMENR,  0, 0x0437f801 },
    { "CCIPR",      RCC_CCIPR,      0, 0xffffffff },
    { "CCIPR2",     RCC_CCIPR2,     0, 0x00300003 },
};

static const char *const core_clocks[] = {
    "sysclk", "hclk", "pclk1", "pclk2",
};

static const char *const gpio_clocks[] = {
    "gpioa", "gpiob", "gpioc", "gpiod", "gpioe", "gpiof", "gpiog",
};

static const RccResetOutput reset_outputs[] = {
    { "USART1", RCC_APB2RSTR,  APB2RSTR_USART1RST,  RCC_RESET_USART1 },
    { "USART2", RCC_APB1RSTR1, APB1RSTR1_USART2RST, RCC_RESET_USART2 },
    { "UART4",  RCC_APB1RSTR1, APB1RSTR1_UART4RST,  RCC_RESET_UART4 },
    { "PWR",    RCC_APB1RSTR1, APB1RSTR1_PWRRST,    RCC_RESET_PWR },
    { "FLASH",  RCC_AHB1RSTR,  AHB1RSTR_FLASHRST,   RCC_RESET_FLASH },
    { "GPIOA",  RCC_AHB2RSTR,  AHB2RSTR_GPIOARST,   RCC_RESET_GPIOA },
    { "GPIOB",  RCC_AHB2RSTR,  AHB2RSTR_GPIOBRST,   RCC_RESET_GPIOB },
    { "GPIOC",  RCC_AHB2RSTR,  AHB2RSTR_GPIOCRST,   RCC_RESET_GPIOC },
    { "GPIOD",  RCC_AHB2RSTR,  AHB2RSTR_GPIODRST,   RCC_RESET_GPIOD },
    { "GPIOE",  RCC_AHB2RSTR,  AHB2RSTR_GPIOERST,   RCC_RESET_GPIOE },
    { "GPIOF",  RCC_AHB2RSTR,  AHB2RSTR_GPIOFRST,   RCC_RESET_GPIOF },
    { "GPIOG",  RCC_AHB2RSTR,  AHB2RSTR_GPIOGRST,   RCC_RESET_GPIOG },
    { "SYSCFG", RCC_APB2RSTR,  APB2RSTR_SYSCFGRST,  RCC_RESET_SYSCFG },
    { "FDCAN",  RCC_APB1RSTR1, APB1RSTR1_FDCANRST,  RCC_RESET_FDCAN },
    { "USB",    RCC_APB1RSTR1, APB1RSTR1_USBRST,    RCC_RESET_USB },
};

G_STATIC_ASSERT(ARRAY_SIZE(reset_outputs) == RCC_RESET_COUNT);

static QTestState *stm32g474_qtest_start(void)
{
    return qtest_init("-machine " STM32G474_MACHINE " -serial null");
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

static uint64_t get_clock_period(QTestState *qts, const char *clock)
{
    g_autofree char *path = g_strdup_printf("%s/%s", RCC_QOM_PATH, clock);
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

static void assert_clock_hz(QTestState *qts, const char *clock, uint64_t hz)
{
    g_test_message("%s clock", clock);
    g_assert_cmphex(get_clock_period(qts, clock), ==,
                    clock_period_from_hz(hz));
}

static void assert_core_clocks(QTestState *qts, uint64_t sysclk,
                               uint64_t hclk, uint64_t pclk1,
                               uint64_t pclk2, uint64_t refclk)
{
    const uint64_t rates[] = { sysclk, hclk, pclk1, pclk2 };

    for (unsigned int i = 0; i < ARRAY_SIZE(core_clocks); i++) {
        assert_clock_hz(qts, core_clocks[i], rates[i]);
    }
    assert_clock_hz(qts, "cortex-refclk", refclk);
}

static void assert_reset_clocks(QTestState *qts)
{
    static const char *const stopped_clocks[] = {
        "pll-p", "pll-q", "pll-r", "dma1", "pwr", "usart1", "usart2",
        "uart4", "usb", "fdcan", "syscfg",
    };

    assert_core_clocks(qts, 16000000, 16000000, 16000000, 16000000,
                       2000000);
    assert_clock_hz(qts, "flash", 16000000);
    for (unsigned int i = 0; i < ARRAY_SIZE(stopped_clocks); i++) {
        assert_clock_hz(qts, stopped_clocks[i], 0);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(gpio_clocks); i++) {
        assert_clock_hz(qts, gpio_clocks[i], 0);
    }
}

static void assert_reset_image(QTestState *qts, bool hsebyp)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(rcc_registers); i++) {
        uint32_t expected = rcc_registers[i].reset;

        if (rcc_registers[i].offset == RCC_CR && hsebyp) {
            expected |= CR_HSEBYP;
        }
        g_test_message("%s model reset value", rcc_registers[i].name);
        g_assert_cmphex(expected & ~rcc_registers[i].implemented, ==, 0);
        g_assert_cmphex(rcc_readl(qts, rcc_registers[i].offset), ==,
                        expected);
    }
}

static void enable_hsi16_pll_170mhz(QTestState *qts)
{
    rcc_writel(qts, RCC_PLLCFGR, PLLCFGR_HSI16_170MHZ);
    rcc_writel(qts, RCC_CR, rcc_readl(qts, RCC_CR) | CR_PLLON);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) & (CR_PLLON | CR_PLLRDY), ==,
                    CR_PLLON | CR_PLLRDY);
    assert_clock_hz(qts, "pll-r", 170000000);
    assert_clock_hz(qts, "pll-q", 170000000);
}

static uint32_t hsi16_pllcfgr(unsigned int m, unsigned int n,
                              unsigned int r)
{
    g_assert_cmpuint(m, >=, 1);
    g_assert_cmpuint(m, <=, 16);
    g_assert_cmpuint(n, <=, 127);
    g_assert_cmpuint(r, >=, 2);
    g_assert_cmpuint(r, <=, 8);
    g_assert_cmpuint(r % 2, ==, 0);

    return PLLCFGR_PLLSRC_HSI16 | ((m - 1) << 4) | (n << 8) |
           PLLCFGR_PLLREN | ((r / 2 - 1) << 25);
}

static void assert_pll_config(QTestState *qts, unsigned int m,
                              unsigned int n, unsigned int r,
                              bool ready, uint64_t r_rate)
{
    rcc_writel(qts, RCC_CR, rcc_readl(qts, RCC_CR) & ~CR_PLLON);
    rcc_writel(qts, RCC_PLLCFGR, hsi16_pllcfgr(m, n, r));
    rcc_writel(qts, RCC_CR, rcc_readl(qts, RCC_CR) | CR_PLLON);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) & CR_PLLRDY, ==,
                    ready ? CR_PLLRDY : 0);
    assert_clock_hz(qts, "pll-r", r_rate);
}

static uint32_t ccipr_set_mux(uint32_t value, unsigned int shift,
                              unsigned int selection)
{
    return (value & ~CCIPR_MUX_MASK(shift)) |
           CCIPR_MUX(selection, shift);
}

static bool nvic_irq_pending(QTestState *qts)
{
    return (qtest_readl(qts, NVIC_ISPR0) & RCC_IRQ_BIT) != 0;
}

static void nvic_clear_irq(QTestState *qts)
{
    qtest_writel(qts, NVIC_ICPR0, RCC_IRQ_BIT);
}

static void test_rcc_mmio_reset(void)
{
    QTestState *qts = stm32g474_qtest_start();
    uint32_t cr;

    g_test_message("CR model reset value");
    g_assert_cmphex(rcc_readl(qts, RCC_CR), ==, 0x00000500);

    for (unsigned int i = 1; i < ARRAY_SIZE(rcc_registers); i++) {
        g_test_message("%s model reset value", rcc_registers[i].name);
        g_assert_cmphex(rcc_readl(qts, rcc_registers[i].offset), ==,
                        rcc_registers[i].reset);
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(all_ones_registers); i++) {
        rcc_writel(qts, all_ones_registers[i].offset, UINT32_MAX);
        g_test_message("%s implemented-bit mask",
                       all_ones_registers[i].name);
        g_assert_cmphex(rcc_readl(qts, all_ones_registers[i].offset), ==,
                        all_ones_registers[i].implemented);
    }

    rcc_writel(qts, RCC_CIER, UINT32_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_CIER), ==, 0x0000063b);
    rcc_writel(qts, RCC_CIFR, UINT32_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_CIFR), ==, 0);
    rcc_writel(qts, RCC_CICR, UINT32_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_CICR), ==, 0);

    qtest_system_reset(qts);
    rcc_writel(qts, RCC_ICSCR, UINT32_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_ICSCR), ==, 0x7f7f0000);

    qtest_system_reset(qts);
    rcc_writel(qts, RCC_PLLCFGR, UINT32_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_PLLCFGR), ==, 0xff737ff3);

    qtest_system_reset(qts);
    rcc_writel(qts, RCC_CFGR, UINT32_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_CFGR), ==, 0x7f003ff7);

    qtest_system_reset(qts);
    rcc_writel(qts, RCC_CR, UINT32_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_CR), ==, 0x010d0700);
    rcc_writel(qts, RCC_CR, rcc_readl(qts, RCC_CR) & ~CR_CSSON);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) & CR_CSSON, ==, CR_CSSON);
    rcc_writel(qts, RCC_CRRCR, UINT32_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_CRRCR), ==,
                    CRRCR_HSI48ON | CRRCR_HSI48RDY);
    rcc_writel(qts, RCC_CSR, UINT32_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_CSR), ==,
                    CSR_LSION | CSR_LSIRDY);

    qtest_system_reset(qts);
    rcc_writel(qts, RCC_BDCR, UINT32_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_BDCR), ==, BDCR_WRITABLE_MASK);

    qtest_system_reset(qts);
    qtest_writeb(qts, RCC_BASE + RCC_CCIPR + 1, 0xa5);
    g_assert_cmphex(rcc_readl(qts, RCC_CCIPR), ==, 0x0000a500);
    g_assert_cmphex(qtest_readb(qts, RCC_BASE + RCC_CCIPR + 1), ==, 0xa5);
    qtest_writew(qts, RCC_BASE + RCC_CCIPR + 2, 0x5aa5);
    g_assert_cmphex(rcc_readl(qts, RCC_CCIPR), ==, 0x5aa5a500);
    g_assert_cmphex(qtest_readw(qts, RCC_BASE + RCC_CCIPR + 2), ==, 0x5aa5);
    rcc_writel(qts, RCC_CCIPR, 0x12345678);
    g_assert_cmphex(rcc_readl(qts, RCC_CCIPR), ==, 0x12345678);

    qtest_writeb(qts, RCC_BASE + RCC_CR + 1, 0);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) & 0x0000ff00, ==, 0x00000500);
    qtest_writeb(qts, RCC_BASE + RCC_CR + 1, UINT8_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) & 0x0000ff00, ==, 0x00000700);
    g_assert_cmphex(qtest_readb(qts, RCC_BASE + RCC_CR + 1), ==, 0x07);
    qtest_writew(qts, RCC_BASE + RCC_CR, 0);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) & UINT16_MAX, ==, 0x0500);
    qtest_writew(qts, RCC_BASE + RCC_CR, UINT16_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) & UINT16_MAX, ==, 0x0700);
    qtest_writeb(qts, RCC_BASE + RCC_CFGR, UINT8_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_CFGR) & UINT8_MAX, ==, 0xf7);
    qtest_writew(qts, RCC_BASE + RCC_CFGR, UINT16_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_CFGR) & UINT16_MAX, ==, 0x3ff7);
    qtest_writeb(qts, RCC_BASE + RCC_AHB2ENR, UINT8_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_AHB2ENR), ==, AHB2ENR_GPIO_MASK);
    qtest_writew(qts, RCC_BASE + RCC_AHB2ENR, UINT16_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_AHB2ENR), ==, 0x0000607f);
    qtest_writeb(qts, RCC_BASE + RCC_AHB2RSTR, UINT8_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_AHB2RSTR), ==, AHB2ENR_GPIO_MASK);
    qtest_writew(qts, RCC_BASE + RCC_AHB2RSTR, UINT16_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_AHB2RSTR), ==, 0x0000607f);

    qtest_system_reset(qts);
    rcc_writel(qts, RCC_PLLCFGR,
               PLLCFGR_HSI16_170MHZ | PLLCFGR_PLLPEN);
    cr = rcc_readl(qts, RCC_CR) | CR_HSEON | CR_PLLON;
    rcc_writel(qts, RCC_CR, cr);
    rcc_writel(qts, RCC_CFGR, CFGR_SW_PLL);
    rcc_writel(qts, RCC_CIER, CIER_LSIRDY | CIER_HSI48RDY);
    rcc_writel(qts, RCC_CSR, CSR_LSION);
    rcc_writel(qts, RCC_CRRCR, CRRCR_HSI48ON);
    rcc_writel(qts, RCC_AHB2ENR, AHB2ENR_GPIO_MASK);
    rcc_writel(qts, RCC_AHB1ENR, AHB1ENR_DMA1EN);
    rcc_writel(qts, RCC_APB1ENR1, UINT32_MAX);
    rcc_writel(qts, RCC_APB2RSTR, APB2RSTR_USART1RST);
    rcc_writel(qts, RCC_AHB1SMENR, 0);
    rcc_writel(qts, RCC_AHB2SMENR, 0);
    rcc_writel(qts, RCC_AHB3SMENR, 0);
    rcc_writel(qts, RCC_APB1SMENR1, 0);
    rcc_writel(qts, RCC_APB1SMENR2, 0);
    rcc_writel(qts, RCC_APB2SMENR, 0);
    assert_clock_hz(qts, "hclk", 170000000);
    assert_clock_hz(qts, "pll-p", 48571428);
    assert_clock_hz(qts, "pll-q", 170000000);
    assert_clock_hz(qts, "pll-r", 170000000);

    for (unsigned int reset = 0; reset < 2; reset++) {
        qtest_system_reset(qts);
        assert_reset_image(qts, true);
        assert_reset_clocks(qts);
    }

    qtest_quit(qts);
}

static void test_rcc_core_clocks(void)
{
    QTestState *qts = stm32g474_qtest_start();
    uint32_t cfgr;
    uint32_t pllcfgr;

    assert_core_clocks(qts, 16000000, 16000000, 16000000, 16000000,
                       2000000);

    rcc_writel(qts, RCC_CR, rcc_readl(qts, RCC_CR) | CR_HSEBYP);
    rcc_writel(qts, RCC_CR, rcc_readl(qts, RCC_CR) | CR_HSEON);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) &
                    (CR_HSEBYP | CR_HSEON | CR_HSERDY), ==,
                    CR_HSEBYP | CR_HSEON);
    rcc_writel(qts, RCC_CR, rcc_readl(qts, RCC_CR) & ~CR_HSEBYP);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) &
                    (CR_HSEBYP | CR_HSEON | CR_HSERDY), ==,
                    CR_HSEBYP | CR_HSEON);

    cfgr = (rcc_readl(qts, RCC_CFGR) & ~CFGR_SW_MASK) | CFGR_SW_HSE;
    rcc_writel(qts, RCC_CFGR, cfgr);
    g_assert_cmphex(rcc_readl(qts, RCC_CFGR) &
                    (CFGR_SW_MASK | CFGR_SWS_MASK), ==,
                    CFGR_SW_HSE | CFGR_SWS_HSI16);
    assert_core_clocks(qts, 16000000, 16000000, 16000000, 16000000,
                       2000000);

    enable_hsi16_pll_170mhz(qts);

    cfgr = rcc_readl(qts, RCC_CFGR);
    cfgr &= ~(CFGR_SW_MASK | CFGR_HPRE_MASK);
    rcc_writel(qts, RCC_CFGR, cfgr | CFGR_SW_PLL | CFGR_HPRE_DIV2);
    g_assert_cmphex(rcc_readl(qts, RCC_CFGR) & CFGR_SWS_MASK, ==,
                    CFGR_SWS_PLL);
    assert_core_clocks(qts, 170000000, 85000000, 85000000, 85000000,
                       10625000);

    cfgr = rcc_readl(qts, RCC_CFGR) & ~CFGR_HPRE_MASK;
    rcc_writel(qts, RCC_CFGR, cfgr);
    assert_core_clocks(qts, 170000000, 170000000, 170000000, 170000000,
                       21250000);

    cfgr = rcc_readl(qts, RCC_CFGR);
    cfgr &= ~(CFGR_PPRE1_MASK | CFGR_PPRE2_MASK);
    rcc_writel(qts, RCC_CFGR,
               cfgr | CFGR_PPRE1_DIV4 | CFGR_PPRE2_DIV8);
    assert_core_clocks(qts, 170000000, 170000000, 42500000, 21250000,
                       21250000);

    pllcfgr = rcc_readl(qts, RCC_PLLCFGR);
    rcc_writel(qts, RCC_PLLCFGR, UINT32_MAX);
    g_assert_cmphex(rcc_readl(qts, RCC_PLLCFGR) &
                    PLLCFGR_PROTECTED_MASK, ==,
                    pllcfgr & PLLCFGR_PROTECTED_MASK);

    rcc_writel(qts, RCC_PLLCFGR, 0);
    g_assert_cmphex(rcc_readl(qts, RCC_PLLCFGR) &
                    PLLCFGR_PROTECTED_MASK, ==,
                    pllcfgr & PLLCFGR_PROTECTED_MASK);

    rcc_writel(qts, RCC_CR, rcc_readl(qts, RCC_CR) & ~CR_PLLON);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) & (CR_PLLON | CR_PLLRDY), ==,
                    CR_PLLON | CR_PLLRDY);
    rcc_writel(qts, RCC_CR, rcc_readl(qts, RCC_CR) & ~CR_HSION);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) & (CR_HSION | CR_HSIRDY), ==,
                    CR_HSION | CR_HSIRDY);

    cfgr = (rcc_readl(qts, RCC_CFGR) & ~CFGR_SW_MASK) | CFGR_SW_HSE;
    rcc_writel(qts, RCC_CFGR, cfgr);
    g_assert_cmphex(rcc_readl(qts, RCC_CFGR) &
                    (CFGR_SW_MASK | CFGR_SWS_MASK), ==,
                    CFGR_SW_HSE | CFGR_SWS_PLL);
    assert_core_clocks(qts, 170000000, 170000000, 42500000, 21250000,
                       21250000);

    qtest_system_reset(qts);
    g_assert_cmphex(rcc_readl(qts, RCC_CR), ==,
                    0x00000500 | CR_HSEBYP);
    assert_core_clocks(qts, 16000000, 16000000, 16000000, 16000000,
                       2000000);

    qtest_quit(qts);
}

static void test_rcc_pll_ranges(void)
{
    QTestState *qts = stm32g474_qtest_start();

    /* VCO is 96 MHz, but the 16 MHz / 7 input is below 2.66 MHz. */
    assert_pll_config(qts, 7, 42, 2, false, 0);

    /* The closest divided HSI16 input above the lower bound is valid. */
    assert_pll_config(qts, 6, 36, 2, true, 48000000);

    /* The VCO range is inclusive from 96 through 344 MHz. */
    assert_pll_config(qts, 4, 23, 2, false, 0);
    assert_pll_config(qts, 4, 24, 2, true, 48000000);
    assert_pll_config(qts, 4, 86, 4, true, 86000000);
    assert_pll_config(qts, 1, 22, 4, false, 0);

    /* An undivided HSI16 input exercises the 16 MHz upper boundary. */
    assert_pll_config(qts, 1, 8, 2, true, 64000000);

    qtest_quit(qts);
}

static void test_rcc_ready_interrupts(void)
{
    QTestState *qts = stm32g474_qtest_start();

    qtest_writel(qts, NVIC_ISER0, RCC_IRQ_BIT);
    nvic_clear_irq(qts);
    g_assert_false(nvic_irq_pending(qts));

    rcc_writel(qts, RCC_CIER, CIER_LSIRDY);
    rcc_writel(qts, RCC_CSR, CSR_LSION);
    g_assert_cmphex(rcc_readl(qts, RCC_CIFR), ==, CIFR_LSIRDY);
    g_assert_true(nvic_irq_pending(qts));

    rcc_writel(qts, RCC_CIER, 0);
    g_assert_cmphex(rcc_readl(qts, RCC_CIFR), ==, CIFR_LSIRDY);
    nvic_clear_irq(qts);
    g_assert_false(nvic_irq_pending(qts));
    rcc_writel(qts, RCC_CIER, CIER_LSIRDY);
    g_assert_true(nvic_irq_pending(qts));

    qtest_writeb(qts, RCC_BASE + RCC_CICR, CIFR_LSIRDY);
    g_assert_cmphex(rcc_readl(qts, RCC_CICR), ==, 0);
    g_assert_cmphex(rcc_readl(qts, RCC_CIFR), ==, 0);
    nvic_clear_irq(qts);
    g_assert_false(nvic_irq_pending(qts));

    rcc_writel(qts, RCC_CRRCR, CRRCR_HSI48ON);
    g_assert_cmphex(rcc_readl(qts, RCC_CRRCR) &
                    (CRRCR_HSI48ON | CRRCR_HSI48RDY), ==,
                    CRRCR_HSI48ON | CRRCR_HSI48RDY);
    g_assert_cmphex(rcc_readl(qts, RCC_CIFR), ==, 0);
    rcc_writel(qts, RCC_CIER, CIER_HSI48RDY);
    g_assert_cmphex(rcc_readl(qts, RCC_CIFR), ==, 0);
    g_assert_false(nvic_irq_pending(qts));

    rcc_writel(qts, RCC_CSR, 0);
    rcc_writel(qts, RCC_CRRCR, 0);
    rcc_writel(qts, RCC_CIER, CIER_LSIRDY | CIER_HSI48RDY);
    rcc_writel(qts, RCC_CSR, CSR_LSION);
    rcc_writel(qts, RCC_CRRCR, CRRCR_HSI48ON);
    g_assert_cmphex(rcc_readl(qts, RCC_CIFR), ==,
                    CIFR_LSIRDY | CIFR_HSI48RDY);
    g_assert_true(nvic_irq_pending(qts));

    qtest_writew(qts, RCC_BASE + RCC_CICR, CIFR_LSIRDY);
    g_assert_cmphex(rcc_readl(qts, RCC_CIFR), ==, CIFR_HSI48RDY);
    nvic_clear_irq(qts);
    g_assert_true(nvic_irq_pending(qts));

    rcc_writel(qts, RCC_CICR, CIFR_HSI48RDY);
    nvic_clear_irq(qts);
    g_assert_cmphex(rcc_readl(qts, RCC_CIFR), ==, 0);
    g_assert_false(nvic_irq_pending(qts));

    rcc_writel(qts, RCC_CSR, 0);
    rcc_writel(qts, RCC_CRRCR, 0);
    rcc_writel(qts, RCC_CSR, CSR_LSION);
    g_assert_true(nvic_irq_pending(qts));
    qtest_system_reset(qts);
    g_assert_cmphex(rcc_readl(qts, RCC_CIFR), ==, 0);
    g_assert_false(nvic_irq_pending(qts));

    qtest_quit(qts);
}

static void test_rcc_gates_muxes_resets(void)
{
    QTestState *qts = stm32g474_qtest_start();
    uint32_t ccipr;
    uint32_t cfgr;
    uint32_t apb1enr1;

    assert_clock_hz(qts, "syscfg", 0);
    assert_clock_hz(qts, "flash", 16000000);
    assert_clock_hz(qts, "dma1", 0);
    assert_clock_hz(qts, "pwr", 0);
    assert_clock_hz(qts, "usart1", 0);
    assert_clock_hz(qts, "usart2", 0);
    assert_clock_hz(qts, "uart4", 0);
    assert_clock_hz(qts, "usb", 0);
    assert_clock_hz(qts, "fdcan", 0);
    for (unsigned int i = 0; i < ARRAY_SIZE(gpio_clocks); i++) {
        assert_clock_hz(qts, gpio_clocks[i], 0);
    }

    for (unsigned int enabled = 0; enabled < ARRAY_SIZE(gpio_clocks);
         enabled++) {
        rcc_writel(qts, RCC_AHB2ENR, 1U << enabled);
        for (unsigned int i = 0; i < ARRAY_SIZE(gpio_clocks); i++) {
            assert_clock_hz(qts, gpio_clocks[i],
                            i == enabled ? 16000000 : 0);
        }
    }
    rcc_writel(qts, RCC_AHB2ENR, 0x3);
    assert_clock_hz(qts, "gpioa", 16000000);
    assert_clock_hz(qts, "gpiob", 16000000);
    rcc_writel(qts, RCC_AHB2ENR, 0x2);
    assert_clock_hz(qts, "gpioa", 0);
    assert_clock_hz(qts, "gpiob", 16000000);
    rcc_writel(qts, RCC_AHB2ENR, 0);

    rcc_writel(qts, RCC_APB2ENR, APB2ENR_SYSCFGEN);
    assert_clock_hz(qts, "syscfg", 16000000);
    assert_clock_hz(qts, "usart1", 0);

    enable_hsi16_pll_170mhz(qts);
    cfgr = rcc_readl(qts, RCC_CFGR);
    cfgr &= ~(CFGR_SW_MASK | CFGR_PPRE1_MASK | CFGR_PPRE2_MASK);
    rcc_writel(qts, RCC_CFGR, cfgr | CFGR_SW_PLL |
               CFGR_PPRE1_DIV4 | CFGR_PPRE2_DIV8);
    assert_core_clocks(qts, 170000000, 170000000, 42500000, 21250000,
                       21250000);
    assert_clock_hz(qts, "syscfg", 21250000);
    rcc_writel(qts, RCC_AHB1ENR, AHB1ENR_FLASHEN | AHB1ENR_DMA1EN);
    assert_clock_hz(qts, "flash", 170000000);
    assert_clock_hz(qts, "dma1", 170000000);
    rcc_writel(qts, RCC_AHB1ENR, AHB1ENR_FLASHEN);
    assert_clock_hz(qts, "flash", 170000000);
    assert_clock_hz(qts, "dma1", 0);

    apb1enr1 = APB1ENR1_PWREN | APB1ENR1_USART2EN |
               APB1ENR1_UART4EN | APB1ENR1_USBEN |
               APB1ENR1_FDCANEN;
    rcc_writel(qts, RCC_APB1ENR1, apb1enr1);
    rcc_writel(qts, RCC_APB2ENR, APB2ENR_USART1EN);
    assert_clock_hz(qts, "syscfg", 0);
    assert_clock_hz(qts, "usart1", 21250000);
    rcc_writel(qts, RCC_APB2ENR, APB2ENR_SYSCFGEN);
    assert_clock_hz(qts, "syscfg", 21250000);
    assert_clock_hz(qts, "usart1", 0);
    rcc_writel(qts, RCC_APB2ENR,
               APB2ENR_SYSCFGEN | APB2ENR_USART1EN);
    assert_clock_hz(qts, "pwr", 42500000);
    assert_clock_hz(qts, "syscfg", 21250000);
    assert_clock_hz(qts, "usart1", 21250000);
    assert_clock_hz(qts, "usart2", 42500000);
    assert_clock_hz(qts, "uart4", 42500000);

    ccipr = rcc_readl(qts, RCC_CCIPR);
    ccipr = ccipr_set_mux(ccipr, CCIPR_USART1_SHIFT, 1);
    ccipr = ccipr_set_mux(ccipr, CCIPR_USART2_SHIFT, 1);
    ccipr = ccipr_set_mux(ccipr, CCIPR_UART4_SHIFT, 1);
    rcc_writel(qts, RCC_CCIPR, ccipr);
    assert_clock_hz(qts, "usart1", 170000000);
    assert_clock_hz(qts, "usart2", 170000000);
    assert_clock_hz(qts, "uart4", 170000000);

    ccipr = ccipr_set_mux(ccipr, CCIPR_USART1_SHIFT, 2);
    ccipr = ccipr_set_mux(ccipr, CCIPR_USART2_SHIFT, 2);
    ccipr = ccipr_set_mux(ccipr, CCIPR_UART4_SHIFT, 2);
    rcc_writel(qts, RCC_CCIPR, ccipr);
    assert_clock_hz(qts, "usart1", 16000000);
    assert_clock_hz(qts, "usart2", 16000000);
    assert_clock_hz(qts, "uart4", 16000000);

    ccipr = ccipr_set_mux(ccipr, CCIPR_USART1_SHIFT, 3);
    ccipr = ccipr_set_mux(ccipr, CCIPR_USART2_SHIFT, 3);
    ccipr = ccipr_set_mux(ccipr, CCIPR_UART4_SHIFT, 3);
    rcc_writel(qts, RCC_CCIPR, ccipr);
    assert_clock_hz(qts, "usart1", 0);
    assert_clock_hz(qts, "usart2", 0);
    assert_clock_hz(qts, "uart4", 0);

    rcc_writel(qts, RCC_CRRCR, CRRCR_HSI48ON);
    ccipr = ccipr_set_mux(ccipr, CCIPR_CLK48_SHIFT, 0);
    rcc_writel(qts, RCC_CCIPR, ccipr);
    assert_clock_hz(qts, "usb", 48000000);
    ccipr = ccipr_set_mux(ccipr, CCIPR_CLK48_SHIFT, 2);
    rcc_writel(qts, RCC_CCIPR, ccipr);
    assert_clock_hz(qts, "usb", 170000000);
    ccipr = ccipr_set_mux(ccipr, CCIPR_CLK48_SHIFT, 1);
    rcc_writel(qts, RCC_CCIPR, ccipr);
    assert_clock_hz(qts, "usb", 0);
    ccipr = ccipr_set_mux(ccipr, CCIPR_CLK48_SHIFT, 0);
    rcc_writel(qts, RCC_CCIPR, ccipr);
    assert_clock_hz(qts, "usb", 48000000);

    ccipr = ccipr_set_mux(ccipr, CCIPR_FDCAN_SHIFT, 0);
    rcc_writel(qts, RCC_CCIPR, ccipr);
    assert_clock_hz(qts, "fdcan", 0);
    ccipr = ccipr_set_mux(ccipr, CCIPR_FDCAN_SHIFT, 1);
    rcc_writel(qts, RCC_CCIPR, ccipr);
    assert_clock_hz(qts, "fdcan", 170000000);
    ccipr = ccipr_set_mux(ccipr, CCIPR_FDCAN_SHIFT, 2);
    rcc_writel(qts, RCC_CCIPR, ccipr);
    assert_clock_hz(qts, "fdcan", 42500000);
    ccipr = ccipr_set_mux(ccipr, CCIPR_FDCAN_SHIFT, 3);
    rcc_writel(qts, RCC_CCIPR, ccipr);
    assert_clock_hz(qts, "fdcan", 0);

    ccipr = ccipr_set_mux(ccipr, CCIPR_USART1_SHIFT, 0);
    ccipr = ccipr_set_mux(ccipr, CCIPR_USART2_SHIFT, 0);
    ccipr = ccipr_set_mux(ccipr, CCIPR_UART4_SHIFT, 0);
    ccipr = ccipr_set_mux(ccipr, CCIPR_FDCAN_SHIFT, 2);
    rcc_writel(qts, RCC_CCIPR, ccipr);
    assert_clock_hz(qts, "usart1", 21250000);
    assert_clock_hz(qts, "usart2", 42500000);
    assert_clock_hz(qts, "uart4", 42500000);
    assert_clock_hz(qts, "usb", 48000000);
    assert_clock_hz(qts, "fdcan", 42500000);
    assert_clock_hz(qts, "syscfg", 21250000);

    rcc_writel(qts, RCC_APB1ENR1, apb1enr1 & ~APB1ENR1_FDCANEN);
    assert_clock_hz(qts, "fdcan", 0);
    assert_clock_hz(qts, "usb", 48000000);
    assert_clock_hz(qts, "usart2", 42500000);
    assert_clock_hz(qts, "uart4", 42500000);
    rcc_writel(qts, RCC_APB1ENR1, apb1enr1);

    qtest_irq_intercept_out_named(qts, RCC_QOM_PATH, "peripheral-reset");
    for (unsigned int selected = 0; selected < ARRAY_SIZE(reset_outputs);
         selected++) {
        g_test_message("%s reset output", reset_outputs[selected].name);
        rcc_writel(qts, reset_outputs[selected].offset,
                   reset_outputs[selected].bit);
        g_assert_cmphex(rcc_readl(qts, reset_outputs[selected].offset), ==,
                        reset_outputs[selected].bit);
        for (unsigned int i = 0; i < RCC_RESET_COUNT; i++) {
            g_assert_cmpint(qtest_get_irq(qts, i), ==,
                            i == reset_outputs[selected].index);
        }
        assert_clock_hz(qts, "usart1", 21250000);
        assert_clock_hz(qts, "usart2", 42500000);
        assert_clock_hz(qts, "uart4", 42500000);
        assert_clock_hz(qts, "pwr", 42500000);
        assert_clock_hz(qts, "flash", 170000000);
        assert_clock_hz(qts, "syscfg", 21250000);
        rcc_writel(qts, reset_outputs[selected].offset, 0);
        for (unsigned int i = 0; i < RCC_RESET_COUNT; i++) {
            g_assert_false(qtest_get_irq(qts, i));
        }
    }

    rcc_writel(qts, RCC_AHB2RSTR, AHB2RSTR_GPIO_MASK);
    for (unsigned int selected = RCC_RESET_GPIOA;
         selected <= RCC_RESET_GPIOG; selected++) {
        uint32_t selected_bit = 1U << (selected - RCC_RESET_GPIOA);

        g_test_message("clear only %s reset output",
                       reset_outputs[selected].name);
        rcc_writel(qts, RCC_AHB2RSTR,
                   AHB2RSTR_GPIO_MASK & ~selected_bit);
        for (unsigned int i = 0; i < RCC_RESET_COUNT; i++) {
            bool expected = i >= RCC_RESET_GPIOA &&
                            i <= RCC_RESET_GPIOG &&
                            i != selected;

            g_assert_cmpint(qtest_get_irq(qts, i), ==, expected);
        }
        rcc_writel(qts, RCC_AHB2RSTR, AHB2RSTR_GPIO_MASK);
    }
    rcc_writel(qts, RCC_AHB2RSTR, 0);
    for (unsigned int i = 0; i < RCC_RESET_COUNT; i++) {
        g_assert_false(qtest_get_irq(qts, i));
    }

    rcc_writel(qts, RCC_AHB1RSTR, AHB1RSTR_FLASHRST);
    rcc_writel(qts, RCC_AHB2RSTR, AHB2RSTR_GPIO_MASK);
    rcc_writel(qts, RCC_APB2RSTR,
               APB2RSTR_SYSCFGRST | APB2RSTR_USART1RST);
    rcc_writel(qts, RCC_APB1RSTR1,
               APB1RSTR1_USART2RST | APB1RSTR1_UART4RST |
               APB1RSTR1_USBRST | APB1RSTR1_FDCANRST |
               APB1RSTR1_PWRRST);
    for (unsigned int i = 0; i < RCC_RESET_COUNT; i++) {
        g_assert_true(qtest_get_irq(qts, i));
    }
    qtest_system_reset(qts);
    for (unsigned int i = 0; i < RCC_RESET_COUNT; i++) {
        g_assert_false(qtest_get_irq(qts, i));
    }

    g_assert_cmphex(rcc_readl(qts, RCC_AHB1ENR), ==, AHB1ENR_FLASHEN);
    assert_reset_clocks(qts);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/stm32g474/rcc/1-mmio-reset", test_rcc_mmio_reset);
    qtest_add_func("/stm32g474/rcc/2-core-clocks", test_rcc_core_clocks);
    qtest_add_func("/stm32g474/rcc/3-ready-interrupts",
                   test_rcc_ready_interrupts);
    qtest_add_func("/stm32g474/rcc/4-gates-muxes-resets",
                   test_rcc_gates_muxes_resets);
    qtest_add_func("/stm32g474/rcc/5-pll-ranges",
                   test_rcc_pll_ranges);

    return g_test_run();
}
