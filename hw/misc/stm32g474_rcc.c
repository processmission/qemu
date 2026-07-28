/*
 * STM32G474 reset and clock control
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "hw/misc/stm32g474_rcc.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define STM32G474_RCC_R_MAX (0xa0 / sizeof(uint32_t))

struct Stm32g474RccState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    uint32_t regs[STM32G474_RCC_R_MAX];
    RegisterInfo regs_info[STM32G474_RCC_R_MAX];

    Clock *hsi16_in;
    Clock *hse_in;
    Clock *hsi48_in;
    Clock *lsi_in;

    Clock *sysclk;
    Clock *hclk;
    Clock *pclk1;
    Clock *pclk2;
    Clock *cortex_refclk;
    Clock *pll_p;
    Clock *pll_q;
    Clock *pll_r;
    Clock *gate[STM32G474_RCC_GATE_COUNT];

    qemu_irq irq;
    qemu_irq peripheral_reset[STM32G474_RCC_RESET_COUNT];
    bool resetting;
};

REG32(CR, 0x00)
    FIELD(CR, HSION, 8, 1)
    FIELD(CR, HSIKERON, 9, 1)
    FIELD(CR, HSIRDY, 10, 1)
    FIELD(CR, HSEON, 16, 1)
    FIELD(CR, HSERDY, 17, 1)
    FIELD(CR, HSEBYP, 18, 1)
    FIELD(CR, CSSON, 19, 1)
    FIELD(CR, PLLON, 24, 1)
    FIELD(CR, PLLRDY, 25, 1)
REG32(ICSCR, 0x04)
    FIELD(ICSCR, HSICAL, 16, 8)
    FIELD(ICSCR, HSITRIM, 24, 7)
REG32(CFGR, 0x08)
    FIELD(CFGR, SW, 0, 2)
    FIELD(CFGR, SWS, 2, 2)
    FIELD(CFGR, HPRE, 4, 4)
    FIELD(CFGR, PPRE1, 8, 3)
    FIELD(CFGR, PPRE2, 11, 3)
    FIELD(CFGR, MCOSEL, 24, 4)
    FIELD(CFGR, MCOPRE, 28, 3)
REG32(PLLCFGR, 0x0c)
    FIELD(PLLCFGR, PLLSRC, 0, 2)
    FIELD(PLLCFGR, PLLM, 4, 4)
    FIELD(PLLCFGR, PLLN, 8, 7)
    FIELD(PLLCFGR, PLLPEN, 16, 1)
    FIELD(PLLCFGR, PLLP, 17, 1)
    FIELD(PLLCFGR, PLLQEN, 20, 1)
    FIELD(PLLCFGR, PLLQ, 21, 2)
    FIELD(PLLCFGR, PLLREN, 24, 1)
    FIELD(PLLCFGR, PLLR, 25, 2)
    FIELD(PLLCFGR, PLLPDIV, 27, 5)
REG32(CIER, 0x18)
    FIELD(CIER, LSIRDYIE, 0, 1)
    FIELD(CIER, LSERDYIE, 1, 1)
    FIELD(CIER, HSIRDYIE, 3, 1)
    FIELD(CIER, HSERDYIE, 4, 1)
    FIELD(CIER, PLLRDYIE, 5, 1)
    FIELD(CIER, LSECSSIE, 9, 1)
    FIELD(CIER, HSI48RDYIE, 10, 1)
REG32(CIFR, 0x1c)
    FIELD(CIFR, LSIRDYF, 0, 1)
    FIELD(CIFR, LSERDYF, 1, 1)
    FIELD(CIFR, HSIRDYF, 3, 1)
    FIELD(CIFR, HSERDYF, 4, 1)
    FIELD(CIFR, PLLRDYF, 5, 1)
    FIELD(CIFR, CSSF, 8, 1)
    FIELD(CIFR, LSECSSF, 9, 1)
    FIELD(CIFR, HSI48RDYF, 10, 1)
REG32(CICR, 0x20)
REG32(AHB1RSTR, 0x28)
    FIELD(AHB1RSTR, FLASHRST, 8, 1)
REG32(AHB2RSTR, 0x2c)
    FIELD(AHB2RSTR, GPIOARST, 0, 1)
    FIELD(AHB2RSTR, GPIOBRST, 1, 1)
    FIELD(AHB2RSTR, GPIOCRST, 2, 1)
    FIELD(AHB2RSTR, GPIODRST, 3, 1)
    FIELD(AHB2RSTR, GPIOERST, 4, 1)
    FIELD(AHB2RSTR, GPIOFRST, 5, 1)
    FIELD(AHB2RSTR, GPIOGRST, 6, 1)
REG32(AHB3RSTR, 0x30)
REG32(APB1RSTR1, 0x38)
    FIELD(APB1RSTR1, USART2RST, 17, 1)
    FIELD(APB1RSTR1, UART4RST, 19, 1)
    FIELD(APB1RSTR1, USBRST, 23, 1)
    FIELD(APB1RSTR1, FDCANRST, 25, 1)
    FIELD(APB1RSTR1, PWRRST, 28, 1)
REG32(APB1RSTR2, 0x3c)
REG32(APB2RSTR, 0x40)
    FIELD(APB2RSTR, SYSCFGRST, 0, 1)
    FIELD(APB2RSTR, USART1RST, 14, 1)
REG32(AHB1ENR, 0x48)
    FIELD(AHB1ENR, DMA1EN, 0, 1)
    FIELD(AHB1ENR, FLASHEN, 8, 1)
REG32(AHB2ENR, 0x4c)
    FIELD(AHB2ENR, GPIOAEN, 0, 1)
    FIELD(AHB2ENR, GPIOBEN, 1, 1)
    FIELD(AHB2ENR, GPIOCEN, 2, 1)
    FIELD(AHB2ENR, GPIODEN, 3, 1)
    FIELD(AHB2ENR, GPIOEEN, 4, 1)
    FIELD(AHB2ENR, GPIOFEN, 5, 1)
    FIELD(AHB2ENR, GPIOGEN, 6, 1)
REG32(AHB3ENR, 0x50)
REG32(APB1ENR1, 0x58)
    FIELD(APB1ENR1, USART2EN, 17, 1)
    FIELD(APB1ENR1, UART4EN, 19, 1)
    FIELD(APB1ENR1, USBEN, 23, 1)
    FIELD(APB1ENR1, FDCANEN, 25, 1)
    FIELD(APB1ENR1, PWREN, 28, 1)
REG32(APB1ENR2, 0x5c)
REG32(APB2ENR, 0x60)
    FIELD(APB2ENR, SYSCFGEN, 0, 1)
    FIELD(APB2ENR, USART1EN, 14, 1)
REG32(AHB1SMENR, 0x68)
REG32(AHB2SMENR, 0x6c)
REG32(AHB3SMENR, 0x70)
REG32(APB1SMENR1, 0x78)
REG32(APB1SMENR2, 0x7c)
REG32(APB2SMENR, 0x80)
REG32(CCIPR, 0x88)
    FIELD(CCIPR, USART1SEL, 0, 2)
    FIELD(CCIPR, USART2SEL, 2, 2)
    FIELD(CCIPR, UART4SEL, 6, 2)
    FIELD(CCIPR, FDCANSEL, 24, 2)
    FIELD(CCIPR, CLK48SEL, 26, 2)
REG32(BDCR, 0x90)
    FIELD(BDCR, LSERDY, 1, 1)
    FIELD(BDCR, LSECSSD, 6, 1)
REG32(CSR, 0x94)
    FIELD(CSR, LSION, 0, 1)
    FIELD(CSR, LSIRDY, 1, 1)
    FIELD(CSR, RMVF, 23, 1)
REG32(CRRCR, 0x98)
    FIELD(CRRCR, HSI48ON, 0, 1)
    FIELD(CRRCR, HSI48RDY, 1, 1)
    FIELD(CRRCR, HSI48CAL, 7, 9)
REG32(CCIPR2, 0x9c)

#define RCC_CR_IMPLEMENTED         0x030f0700U
#define RCC_ICSCR_IMPLEMENTED      0x7fff0000U
#define RCC_CFGR_IMPLEMENTED       0x7f003fffU
#define RCC_PLLCFGR_IMPLEMENTED    0xff737ff3U
#define RCC_CIER_IMPLEMENTED       0x0000063bU
#define RCC_CIFR_IMPLEMENTED       0x0000073bU
#define RCC_CICR_IMPLEMENTED       0x0000073bU
#define RCC_AHB1RSTR_IMPLEMENTED   0x0000111fU
#define RCC_AHB2RSTR_IMPLEMENTED   0x040f607fU
#define RCC_AHB3RSTR_IMPLEMENTED   0x00000101U
#define RCC_APB1RSTR1_IMPLEMENTED  0xd2fec13fU
#define RCC_APB1RSTR2_IMPLEMENTED  0x00000103U
#define RCC_APB2RSTR_IMPLEMENTED   0x0437f801U
#define RCC_AHB1ENR_IMPLEMENTED    0x0000111fU
#define RCC_AHB2ENR_IMPLEMENTED    0x040f607fU
#define RCC_AHB3ENR_IMPLEMENTED    0x00000101U
#define RCC_APB1ENR1_IMPLEMENTED   0xd2fecd3fU
#define RCC_APB1ENR2_IMPLEMENTED   0x00000103U
#define RCC_APB2ENR_IMPLEMENTED    0x0437f801U
#define RCC_AHB1SMENR_IMPLEMENTED  0x0000131fU
#define RCC_AHB2SMENR_IMPLEMENTED  0x040f667fU
#define RCC_AHB3SMENR_IMPLEMENTED  0x00000101U
#define RCC_APB1SMENR1_IMPLEMENTED 0xd2fecd3fU
#define RCC_APB1SMENR2_IMPLEMENTED 0x00000103U
#define RCC_APB2SMENR_IMPLEMENTED  0x0437f801U
#define RCC_CCIPR_IMPLEMENTED      UINT32_MAX
#define RCC_BDCR_IMPLEMENTED       0x0301837fU
#define RCC_CSR_IMPLEMENTED        0xfe800003U
#define RCC_CRRCR_IMPLEMENTED      0x0000ff83U
#define RCC_CCIPR2_IMPLEMENTED     0x00300003U

#define RCC_CR_RO (R_CR_PLLRDY_MASK | R_CR_HSERDY_MASK | R_CR_HSIRDY_MASK)
#define RCC_CIER_READY_MASK 0x0000063bU
#define RCC_CSR_RESET_FLAGS 0xfe000000U
#define RCC_BDCR_RO (R_BDCR_LSERDY_MASK | R_BDCR_LSECSSD_MASK)
#define RCC_CRRCR_RO (R_CRRCR_HSI48RDY_MASK | R_CRRCR_HSI48CAL_MASK)

#define RCC_PLLCFGR_PROTECTED_MASK 0xfe627ff3U
/* DS12288 Rev 6, 5.3.9, Range 1 boost operating limits. */
#define RCC_PLL_MIN_INPUT_HZ 2660000ULL
#define RCC_PLL_MAX_INPUT_HZ 16000000ULL
#define RCC_PLL_MIN_VCO_HZ 96000000ULL
#define RCC_PLL_MAX_VCO_HZ 344000000ULL
#define RCC_PLL_MAX_OUTPUT_HZ 170000000ULL

/*
 * RM0440 7.4.3 reserves zero and uses the same non-zero encoding for SW
 * and SWS.  This differs from several other STM32 families.
 */
enum {
    RCC_SYSCLK_RESERVED,
    RCC_SYSCLK_HSI16,
    RCC_SYSCLK_HSE,
    RCC_SYSCLK_PLL,
};

#define RCC_AHB1RSTR_MODELED R_AHB1RSTR_FLASHRST_MASK
#define RCC_AHB2RSTR_MODELED \
    (R_AHB2RSTR_GPIOARST_MASK | R_AHB2RSTR_GPIOBRST_MASK | \
     R_AHB2RSTR_GPIOCRST_MASK | R_AHB2RSTR_GPIODRST_MASK | \
     R_AHB2RSTR_GPIOERST_MASK | R_AHB2RSTR_GPIOFRST_MASK | \
     R_AHB2RSTR_GPIOGRST_MASK)
#define RCC_APB1RSTR1_MODELED \
    (R_APB1RSTR1_USART2RST_MASK | R_APB1RSTR1_UART4RST_MASK | \
     R_APB1RSTR1_USBRST_MASK | R_APB1RSTR1_FDCANRST_MASK | \
     R_APB1RSTR1_PWRRST_MASK)
#define RCC_APB2RSTR_MODELED \
    (R_APB2RSTR_SYSCFGRST_MASK | R_APB2RSTR_USART1RST_MASK)
#define RCC_AHB1ENR_MODELED \
    (R_AHB1ENR_DMA1EN_MASK | R_AHB1ENR_FLASHEN_MASK)
#define RCC_AHB2ENR_MODELED \
    (R_AHB2ENR_GPIOAEN_MASK | R_AHB2ENR_GPIOBEN_MASK | \
     R_AHB2ENR_GPIOCEN_MASK | R_AHB2ENR_GPIODEN_MASK | \
     R_AHB2ENR_GPIOEEN_MASK | R_AHB2ENR_GPIOFEN_MASK | \
     R_AHB2ENR_GPIOGEN_MASK)
#define RCC_APB1ENR1_MODELED \
    (R_APB1ENR1_USART2EN_MASK | R_APB1ENR1_UART4EN_MASK | \
     R_APB1ENR1_USBEN_MASK | R_APB1ENR1_FDCANEN_MASK | \
     R_APB1ENR1_PWREN_MASK)
#define RCC_APB2ENR_MODELED \
    (R_APB2ENR_SYSCFGEN_MASK | R_APB2ENR_USART1EN_MASK)
#define RCC_CCIPR_MODELED \
    (R_CCIPR_USART1SEL_MASK | R_CCIPR_USART2SEL_MASK | \
     R_CCIPR_UART4SEL_MASK | R_CCIPR_FDCANSEL_MASK | \
     R_CCIPR_CLK48SEL_MASK)

typedef struct Stm32g474PllRates {
    uint64_t p;
    uint64_t q;
    uint64_t r;
    bool ready;
} Stm32g474PllRates;

static void stm32g474_rcc_update_clocks(Stm32g474RccState *s,
                                         bool propagate);
static void stm32g474_rcc_reconcile_ready(Stm32g474RccState *s,
                                           bool generate_events);

static bool stm32g474_rcc_hsi16_ready(Stm32g474RccState *s)
{
    return (s->regs[R_CR] & R_CR_HSIRDY_MASK) != 0;
}

static bool stm32g474_rcc_hse_ready(Stm32g474RccState *s)
{
    return (s->regs[R_CR] & R_CR_HSERDY_MASK) != 0;
}

static uint64_t stm32g474_rcc_hsi16_hz(Stm32g474RccState *s)
{
    return stm32g474_rcc_hsi16_ready(s) ?
           clock_get_hz(s->hsi16_in) : 0;
}

static uint64_t stm32g474_rcc_hsi16_kernel_hz(Stm32g474RccState *s)
{
    uint32_t enabled = R_CR_HSION_MASK | R_CR_HSIKERON_MASK;

    return (s->regs[R_CR] & enabled) ? clock_get_hz(s->hsi16_in) : 0;
}

static uint64_t stm32g474_rcc_hse_hz(Stm32g474RccState *s)
{
    return stm32g474_rcc_hse_ready(s) ? clock_get_hz(s->hse_in) : 0;
}

static uint64_t stm32g474_rcc_pll_output(uint64_t vco, uint32_t divisor)
{
    uint64_t rate;

    if (!divisor) {
        return 0;
    }

    rate = vco / divisor;
    return rate <= RCC_PLL_MAX_OUTPUT_HZ ? rate : 0;
}

static Stm32g474PllRates
stm32g474_rcc_calculate_pll(Stm32g474RccState *s)
{
    Stm32g474PllRates rates = { 0 };
    uint32_t pllcfgr = s->regs[R_PLLCFGR];
    uint32_t source_sel = FIELD_EX32(pllcfgr, PLLCFGR, PLLSRC);
    uint32_t m = FIELD_EX32(pllcfgr, PLLCFGR, PLLM) + 1;
    uint32_t n = FIELD_EX32(pllcfgr, PLLCFGR, PLLN);
    uint32_t pdiv = FIELD_EX32(pllcfgr, PLLCFGR, PLLPDIV);
    uint32_t p;
    uint64_t source;
    uint64_t vco;
    uint64_t vco_numerator;

    if (source_sel == 2) {
        source = stm32g474_rcc_hsi16_hz(s);
    } else if (source_sel == 3) {
        source = stm32g474_rcc_hse_hz(s);
    } else {
        source = 0;
    }

    if (!source || n < 8 || n > 127 ||
        source < RCC_PLL_MIN_INPUT_HZ * m ||
        source > RCC_PLL_MAX_INPUT_HZ * m) {
        return rates;
    }

    vco_numerator = source * n;
    if (vco_numerator < RCC_PLL_MIN_VCO_HZ * m ||
        vco_numerator > RCC_PLL_MAX_VCO_HZ * m) {
        return rates;
    }
    vco = vco_numerator / m;

    if (pdiv == 0) {
        p = FIELD_EX32(pllcfgr, PLLCFGR, PLLP) ? 17 : 7;
    } else if (pdiv == 1) {
        p = 0;
    } else {
        p = pdiv;
    }

    if (pllcfgr & R_PLLCFGR_PLLPEN_MASK) {
        rates.p = stm32g474_rcc_pll_output(vco, p);
    }
    if (pllcfgr & R_PLLCFGR_PLLQEN_MASK) {
        rates.q = stm32g474_rcc_pll_output(
            vco, 2 * (FIELD_EX32(pllcfgr, PLLCFGR, PLLQ) + 1));
    }
    if (pllcfgr & R_PLLCFGR_PLLREN_MASK) {
        rates.r = stm32g474_rcc_pll_output(
            vco, 2 * (FIELD_EX32(pllcfgr, PLLCFGR, PLLR) + 1));
    }

    if (((pllcfgr & R_PLLCFGR_PLLPEN_MASK) && !rates.p) ||
        ((pllcfgr & R_PLLCFGR_PLLQEN_MASK) && !rates.q) ||
        ((pllcfgr & R_PLLCFGR_PLLREN_MASK) && !rates.r)) {
        rates.p = 0;
        rates.q = 0;
        rates.r = 0;
        return rates;
    }

    rates.ready = true;
    return rates;
}

static uint32_t stm32g474_rcc_hpre_divisor(uint32_t encoding)
{
    static const uint16_t divisors[] = {
        2, 4, 8, 16, 64, 128, 256, 512,
    };

    return encoding < 8 ? 1 : divisors[encoding - 8];
}

static uint32_t stm32g474_rcc_ppre_divisor(uint32_t encoding)
{
    return encoding < 4 ? 1 : 1U << (encoding - 3);
}

static void stm32g474_rcc_set_clock(Clock *clock, uint64_t hz,
                                    bool propagate)
{
    g_assert(hz <= UINT_MAX);

    if (propagate) {
        clock_update_hz(clock, hz);
    } else {
        clock_set_hz(clock, hz);
    }
}

static uint64_t stm32g474_rcc_uart_hz(Stm32g474RccState *s,
                                      uint32_t selection,
                                      uint64_t pclk, uint64_t sysclk)
{
    switch (selection) {
    case 0:
        return pclk;
    case 1:
        return sysclk;
    case 2:
        return stm32g474_rcc_hsi16_kernel_hz(s);
    default:
        return 0;
    }
}

static uint64_t stm32g474_rcc_clk48_hz(Stm32g474RccState *s,
                                       uint32_t selection,
                                       const Stm32g474PllRates *pll)
{
    switch (selection) {
    case 0:
        return (s->regs[R_CRRCR] & R_CRRCR_HSI48RDY_MASK) ?
               clock_get_hz(s->hsi48_in) : 0;
    case 2:
        return pll->q;
    default:
        return 0;
    }
}

static uint64_t stm32g474_rcc_fdcan_hz(Stm32g474RccState *s,
                                       uint32_t selection, uint64_t pclk1,
                                       const Stm32g474PllRates *pll)
{
    switch (selection) {
    case 0:
        return stm32g474_rcc_hse_hz(s);
    case 1:
        return pll->q;
    case 2:
        return pclk1;
    default:
        return 0;
    }
}

static void stm32g474_rcc_update_clocks(Stm32g474RccState *s,
                                         bool propagate)
{
    Stm32g474PllRates pll = stm32g474_rcc_calculate_pll(s);
    uint32_t cfgr = s->regs[R_CFGR];
    uint32_t ccipr = s->regs[R_CCIPR];
    uint32_t sws = FIELD_EX32(cfgr, CFGR, SWS);
    uint64_t sysclk;
    uint64_t hclk;
    uint64_t pclk1;
    uint64_t pclk2;
    uint64_t gates[STM32G474_RCC_GATE_COUNT] = { 0 };

    if (!(s->regs[R_CR] & R_CR_PLLRDY_MASK)) {
        pll.p = 0;
        pll.q = 0;
        pll.r = 0;
    }

    switch (sws) {
    case RCC_SYSCLK_HSI16:
        sysclk = stm32g474_rcc_hsi16_hz(s);
        break;
    case RCC_SYSCLK_HSE:
        sysclk = stm32g474_rcc_hse_hz(s);
        break;
    case RCC_SYSCLK_PLL:
        sysclk = pll.r;
        break;
    default:
        sysclk = 0;
        break;
    }

    hclk = sysclk /
        stm32g474_rcc_hpre_divisor(FIELD_EX32(cfgr, CFGR, HPRE));
    pclk1 = hclk /
        stm32g474_rcc_ppre_divisor(FIELD_EX32(cfgr, CFGR, PPRE1));
    pclk2 = hclk /
        stm32g474_rcc_ppre_divisor(FIELD_EX32(cfgr, CFGR, PPRE2));

    if (s->regs[R_AHB1ENR] & R_AHB1ENR_DMA1EN_MASK) {
        gates[STM32G474_RCC_GATE_DMA1] = hclk;
    }
    if (s->regs[R_AHB1ENR] & R_AHB1ENR_FLASHEN_MASK) {
        gates[STM32G474_RCC_GATE_FLASH] = hclk;
    }
    for (unsigned int i = 0; i < 7; i++) {
        if (s->regs[R_AHB2ENR] & (1U << i)) {
            gates[STM32G474_RCC_GATE_GPIOA + i] = hclk;
        }
    }
    if (s->regs[R_APB1ENR1] & R_APB1ENR1_PWREN_MASK) {
        gates[STM32G474_RCC_GATE_PWR] = pclk1;
    }
    if (s->regs[R_APB1ENR1] & R_APB1ENR1_USBEN_MASK) {
        gates[STM32G474_RCC_GATE_USB] = stm32g474_rcc_clk48_hz(
            s, FIELD_EX32(ccipr, CCIPR, CLK48SEL), &pll);
    }
    if (s->regs[R_APB1ENR1] & R_APB1ENR1_FDCANEN_MASK) {
        gates[STM32G474_RCC_GATE_FDCAN] = stm32g474_rcc_fdcan_hz(
            s, FIELD_EX32(ccipr, CCIPR, FDCANSEL), pclk1, &pll);
    }
    if (s->regs[R_APB2ENR] & R_APB2ENR_USART1EN_MASK) {
        gates[STM32G474_RCC_GATE_USART1] = stm32g474_rcc_uart_hz(
            s, FIELD_EX32(ccipr, CCIPR, USART1SEL), pclk2, sysclk);
    }
    if (s->regs[R_APB2ENR] & R_APB2ENR_SYSCFGEN_MASK) {
        gates[STM32G474_RCC_GATE_SYSCFG] = pclk2;
    }
    if (s->regs[R_APB1ENR1] & R_APB1ENR1_USART2EN_MASK) {
        gates[STM32G474_RCC_GATE_USART2] = stm32g474_rcc_uart_hz(
            s, FIELD_EX32(ccipr, CCIPR, USART2SEL), pclk1, sysclk);
    }
    if (s->regs[R_APB1ENR1] & R_APB1ENR1_UART4EN_MASK) {
        gates[STM32G474_RCC_GATE_UART4] = stm32g474_rcc_uart_hz(
            s, FIELD_EX32(ccipr, CCIPR, UART4SEL), pclk1, sysclk);
    }

    stm32g474_rcc_set_clock(s->sysclk, sysclk, propagate);
    stm32g474_rcc_set_clock(s->hclk, hclk, propagate);
    stm32g474_rcc_set_clock(s->pclk1, pclk1, propagate);
    stm32g474_rcc_set_clock(s->pclk2, pclk2, propagate);
    stm32g474_rcc_set_clock(s->cortex_refclk, hclk / 8, propagate);
    stm32g474_rcc_set_clock(s->pll_p, pll.p, propagate);
    stm32g474_rcc_set_clock(s->pll_q, pll.q, propagate);
    stm32g474_rcc_set_clock(s->pll_r, pll.r, propagate);
    for (unsigned int i = 0; i < STM32G474_RCC_GATE_COUNT; i++) {
        stm32g474_rcc_set_clock(s->gate[i], gates[i], propagate);
    }
}

static bool stm32g474_rcc_sws_ready(Stm32g474RccState *s, uint32_t sws)
{
    Stm32g474PllRates pll;

    switch (sws) {
    case RCC_SYSCLK_HSI16:
        return stm32g474_rcc_hsi16_ready(s);
    case RCC_SYSCLK_HSE:
        return stm32g474_rcc_hse_ready(s);
    case RCC_SYSCLK_PLL:
        pll = stm32g474_rcc_calculate_pll(s);
        return (s->regs[R_CR] & R_CR_PLLRDY_MASK) && pll.r;
    default:
        return false;
    }
}

static void stm32g474_rcc_retry_switch(Stm32g474RccState *s)
{
    uint32_t requested = FIELD_EX32(s->regs[R_CFGR], CFGR, SW);
    uint32_t current = FIELD_EX32(s->regs[R_CFGR], CFGR, SWS);

    if (!stm32g474_rcc_sws_ready(s, current) &&
        stm32g474_rcc_hsi16_ready(s)) {
        current = RCC_SYSCLK_HSI16;
    }
    if (requested != RCC_SYSCLK_RESERVED &&
        stm32g474_rcc_sws_ready(s, requested)) {
        current = requested;
    }
    s->regs[R_CFGR] = FIELD_DP32(s->regs[R_CFGR], CFGR, SWS, current);
}

static void stm32g474_rcc_set_ready(Stm32g474RccState *s,
                                    unsigned int reg_index,
                                    uint32_t ready_mask,
                                    uint32_t interrupt_mask, bool ready,
                                    bool generate_events)
{
    bool was_ready = (s->regs[reg_index] & ready_mask) != 0;

    if (ready) {
        s->regs[reg_index] |= ready_mask;
    } else {
        s->regs[reg_index] &= ~ready_mask;
    }

    if (generate_events && !was_ready && ready &&
        (s->regs[R_CIER] & interrupt_mask)) {
        s->regs[R_CIFR] |= interrupt_mask;
    }
}

static void stm32g474_rcc_reconcile_ready(Stm32g474RccState *s,
                                           bool generate_events)
{
    Stm32g474PllRates pll;
    bool hsi16;
    bool hse;
    bool lsi;
    bool hsi48;

    hsi16 = (s->regs[R_CR] & R_CR_HSION_MASK) &&
            clock_has_source(s->hsi16_in) &&
            clock_get_hz(s->hsi16_in);
    hse = (s->regs[R_CR] & R_CR_HSEON_MASK) &&
          clock_has_source(s->hse_in) && clock_get_hz(s->hse_in);

    stm32g474_rcc_set_ready(s, R_CR, R_CR_HSIRDY_MASK,
                            R_CIFR_HSIRDYF_MASK, hsi16, generate_events);
    stm32g474_rcc_set_ready(s, R_CR, R_CR_HSERDY_MASK,
                            R_CIFR_HSERDYF_MASK, hse, generate_events);

    pll = stm32g474_rcc_calculate_pll(s);
    stm32g474_rcc_set_ready(s, R_CR, R_CR_PLLRDY_MASK,
                            R_CIFR_PLLRDYF_MASK,
                            (s->regs[R_CR] & R_CR_PLLON_MASK) && pll.ready,
                            generate_events);

    lsi = (s->regs[R_CSR] & R_CSR_LSION_MASK) &&
          clock_has_source(s->lsi_in) && clock_get_hz(s->lsi_in);
    hsi48 = (s->regs[R_CRRCR] & R_CRRCR_HSI48ON_MASK) &&
            clock_has_source(s->hsi48_in) &&
            clock_get_hz(s->hsi48_in);
    stm32g474_rcc_set_ready(s, R_CSR, R_CSR_LSIRDY_MASK,
                            R_CIFR_LSIRDYF_MASK, lsi, generate_events);
    stm32g474_rcc_set_ready(s, R_CRRCR, R_CRRCR_HSI48RDY_MASK,
                            R_CIFR_HSI48RDYF_MASK, hsi48,
                            generate_events);
    stm32g474_rcc_retry_switch(s);
}

static void stm32g474_rcc_update_irq(Stm32g474RccState *s)
{
    bool level;

    if (s->resetting) {
        return;
    }
    level = (s->regs[R_CIER] & s->regs[R_CIFR] &
             RCC_CIER_READY_MASK) != 0;
    qemu_set_irq(s->irq, level);
}

static void stm32g474_rcc_update_resets(Stm32g474RccState *s)
{
    if (s->resetting) {
        return;
    }

    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_USART1],
                 (s->regs[R_APB2RSTR] & R_APB2RSTR_USART1RST_MASK) != 0);
    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_USART2],
                 (s->regs[R_APB1RSTR1] &
                  R_APB1RSTR1_USART2RST_MASK) != 0);
    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_UART4],
                 (s->regs[R_APB1RSTR1] &
                  R_APB1RSTR1_UART4RST_MASK) != 0);
    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_PWR],
                 (s->regs[R_APB1RSTR1] &
                  R_APB1RSTR1_PWRRST_MASK) != 0);
    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_FLASH],
                 (s->regs[R_AHB1RSTR] &
                  R_AHB1RSTR_FLASHRST_MASK) != 0);
    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_GPIOA],
                 (s->regs[R_AHB2RSTR] &
                  R_AHB2RSTR_GPIOARST_MASK) != 0);
    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_GPIOB],
                 (s->regs[R_AHB2RSTR] &
                  R_AHB2RSTR_GPIOBRST_MASK) != 0);
    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_GPIOC],
                 (s->regs[R_AHB2RSTR] &
                  R_AHB2RSTR_GPIOCRST_MASK) != 0);
    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_GPIOD],
                 (s->regs[R_AHB2RSTR] &
                  R_AHB2RSTR_GPIODRST_MASK) != 0);
    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_GPIOE],
                 (s->regs[R_AHB2RSTR] &
                  R_AHB2RSTR_GPIOERST_MASK) != 0);
    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_GPIOF],
                 (s->regs[R_AHB2RSTR] &
                  R_AHB2RSTR_GPIOFRST_MASK) != 0);
    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_GPIOG],
                 (s->regs[R_AHB2RSTR] &
                  R_AHB2RSTR_GPIOGRST_MASK) != 0);
    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_SYSCFG],
                 (s->regs[R_APB2RSTR] &
                  R_APB2RSTR_SYSCFGRST_MASK) != 0);
    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_FDCAN],
                 (s->regs[R_APB1RSTR1] &
                  R_APB1RSTR1_FDCANRST_MASK) != 0);
    qemu_set_irq(s->peripheral_reset[STM32G474_RCC_RESET_USB],
                 (s->regs[R_APB1RSTR1] &
                  R_APB1RSTR1_USBRST_MASK) != 0);
}

static void stm32g474_rcc_register_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474RccState *s = STM32G474_RCC(reg->opaque);

    if (s->resetting) {
        return;
    }
    stm32g474_rcc_reconcile_ready(s, true);
    stm32g474_rcc_update_clocks(s, true);
    stm32g474_rcc_update_irq(s);
}

static uint64_t stm32g474_rcc_cr_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474RccState *s = STM32G474_RCC(reg->opaque);
    uint32_t old = s->regs[R_CR];
    uint32_t sws = FIELD_EX32(s->regs[R_CFGR], CFGR, SWS);
    uint32_t pll_source = FIELD_EX32(s->regs[R_PLLCFGR],
                                     PLLCFGR, PLLSRC);

    val |= old & R_CR_CSSON_MASK;
    if ((old & R_CR_HSEON_MASK) &&
        ((old ^ val) & R_CR_HSEBYP_MASK)) {
        val = (val & ~R_CR_HSEBYP_MASK) | (old & R_CR_HSEBYP_MASK);
    }
    if (!(val & R_CR_HSION_MASK) &&
        (sws == RCC_SYSCLK_HSI16 ||
         (sws == RCC_SYSCLK_PLL && pll_source == 2))) {
        val |= R_CR_HSION_MASK;
    }
    if (!(val & R_CR_HSEON_MASK) &&
        (sws == RCC_SYSCLK_HSE ||
         (sws == RCC_SYSCLK_PLL && pll_source == 3))) {
        val |= R_CR_HSEON_MASK;
    }
    if (!(val & R_CR_PLLON_MASK) && sws == RCC_SYSCLK_PLL) {
        val |= R_CR_PLLON_MASK;
    }

    return val;
}

static void stm32g474_rcc_update_hsi_calibration(Stm32g474RccState *s)
{
    uint32_t trim = FIELD_EX32(s->regs[R_ICSCR], ICSCR, HSITRIM);

    s->regs[R_ICSCR] = FIELD_DP32(s->regs[R_ICSCR], ICSCR, HSICAL, trim);
}

static void stm32g474_rcc_icscr_post_write(RegisterInfo *reg, uint64_t val)
{
    stm32g474_rcc_update_hsi_calibration(STM32G474_RCC(reg->opaque));
}

static uint64_t stm32g474_rcc_cfgr_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474RccState *s = STM32G474_RCC(reg->opaque);

    if (FIELD_EX32(val, CFGR, SW) == RCC_SYSCLK_RESERVED) {
        val = FIELD_DP32(val, CFGR, SW,
                         FIELD_EX32(s->regs[R_CFGR], CFGR, SW));
    }
    return val;
}

static uint64_t
stm32g474_rcc_pllcfgr_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474RccState *s = STM32G474_RCC(reg->opaque);
    uint32_t old = s->regs[R_PLLCFGR];

    if (s->regs[R_CR] & R_CR_PLLON_MASK) {
        val = (val & ~RCC_PLLCFGR_PROTECTED_MASK) |
              (old & RCC_PLLCFGR_PROTECTED_MASK);
    }
    if (FIELD_EX32(s->regs[R_CFGR], CFGR, SWS) == RCC_SYSCLK_PLL) {
        val = (val & ~R_PLLCFGR_PLLREN_MASK) |
              (old & R_PLLCFGR_PLLREN_MASK);
    }
    return val;
}

static void stm32g474_rcc_cier_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474RccState *s = STM32G474_RCC(reg->opaque);

    stm32g474_rcc_update_irq(s);
}

static uint64_t stm32g474_rcc_cicr_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474RccState *s = STM32G474_RCC(reg->opaque);

    s->regs[R_CIFR] &= ~(val & RCC_CICR_IMPLEMENTED);
    stm32g474_rcc_update_irq(s);
    return 0;
}

static void stm32g474_rcc_gate_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474RccState *s = STM32G474_RCC(reg->opaque);

    if (!s->resetting) {
        stm32g474_rcc_update_clocks(s, true);
    }
}

static void stm32g474_rcc_reset_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474RccState *s = STM32G474_RCC(reg->opaque);

    stm32g474_rcc_update_resets(s);
}

static uint64_t stm32g474_rcc_csr_pre_write(RegisterInfo *reg, uint64_t val)
{
    if (val & R_CSR_RMVF_MASK) {
        val &= ~(R_CSR_RMVF_MASK | RCC_CSR_RESET_FLAGS);
    }
    return val;
}

static void stm32g474_rcc_input_clock_update(void *opaque, ClockEvent event)
{
    Stm32g474RccState *s = STM32G474_RCC(opaque);

    if (s->resetting) {
        return;
    }
    stm32g474_rcc_reconcile_ready(s, true);
    stm32g474_rcc_update_clocks(s, true);
    stm32g474_rcc_update_irq(s);
}

#define RCC_REG(_name, _reset, _implemented) \
    { .name = #_name, .addr = A_##_name, .reset = (_reset), \
      .rsvd = UINT32_MAX & ~(_implemented), .unimp = (_implemented) }

static const RegisterAccessInfo stm32g474_rcc_regs_info[] = {
    {
        .name = "CR",
        .addr = A_CR,
        .reset = 0x00000500,
        .rsvd = UINT32_MAX & ~RCC_CR_IMPLEMENTED,
        .ro = RCC_CR_RO,
        .unimp = R_CR_CSSON_MASK,
        .pre_write = stm32g474_rcc_cr_pre_write,
        .post_write = stm32g474_rcc_register_post_write,
    }, {
        .name = "ICSCR",
        .addr = A_ICSCR,
        .reset = 0x40400000,
        .rsvd = UINT32_MAX & ~RCC_ICSCR_IMPLEMENTED,
        .ro = R_ICSCR_HSICAL_MASK,
        .post_write = stm32g474_rcc_icscr_post_write,
    }, {
        .name = "CFGR",
        .addr = A_CFGR,
        .reset = 0x00000005,
        .rsvd = UINT32_MAX & ~RCC_CFGR_IMPLEMENTED,
        .ro = R_CFGR_SWS_MASK,
        .unimp = R_CFGR_MCOSEL_MASK | R_CFGR_MCOPRE_MASK,
        .pre_write = stm32g474_rcc_cfgr_pre_write,
        .post_write = stm32g474_rcc_register_post_write,
    }, {
        .name = "PLLCFGR",
        .addr = A_PLLCFGR,
        .reset = 0x00001000,
        .rsvd = UINT32_MAX & ~RCC_PLLCFGR_IMPLEMENTED,
        .pre_write = stm32g474_rcc_pllcfgr_pre_write,
        .post_write = stm32g474_rcc_register_post_write,
    }, {
        .name = "CIER",
        .addr = A_CIER,
        .rsvd = UINT32_MAX & ~RCC_CIER_IMPLEMENTED,
        .unimp = R_CIER_LSERDYIE_MASK | R_CIER_LSECSSIE_MASK,
        .post_write = stm32g474_rcc_cier_post_write,
    }, {
        .name = "CIFR",
        .addr = A_CIFR,
        .rsvd = UINT32_MAX & ~RCC_CIFR_IMPLEMENTED,
        .ro = RCC_CIFR_IMPLEMENTED,
    }, {
        .name = "CICR",
        .addr = A_CICR,
        .rsvd = UINT32_MAX & ~RCC_CICR_IMPLEMENTED,
        .pre_write = stm32g474_rcc_cicr_pre_write,
    }, {
        .name = "AHB1RSTR",
        .addr = A_AHB1RSTR,
        .rsvd = UINT32_MAX & ~RCC_AHB1RSTR_IMPLEMENTED,
        .unimp = RCC_AHB1RSTR_IMPLEMENTED & ~RCC_AHB1RSTR_MODELED,
        .post_write = stm32g474_rcc_reset_post_write,
    }, {
        .name = "AHB2RSTR",
        .addr = A_AHB2RSTR,
        .rsvd = UINT32_MAX & ~RCC_AHB2RSTR_IMPLEMENTED,
        .unimp = RCC_AHB2RSTR_IMPLEMENTED & ~RCC_AHB2RSTR_MODELED,
        .post_write = stm32g474_rcc_reset_post_write,
    }, {
        .name = "AHB3RSTR",
        .addr = A_AHB3RSTR,
        .rsvd = UINT32_MAX & ~RCC_AHB3RSTR_IMPLEMENTED,
        .unimp = RCC_AHB3RSTR_IMPLEMENTED,
    }, {
        .name = "APB1RSTR1",
        .addr = A_APB1RSTR1,
        .rsvd = UINT32_MAX & ~RCC_APB1RSTR1_IMPLEMENTED,
        .unimp = RCC_APB1RSTR1_IMPLEMENTED & ~RCC_APB1RSTR1_MODELED,
        .post_write = stm32g474_rcc_reset_post_write,
    }, {
        .name = "APB1RSTR2",
        .addr = A_APB1RSTR2,
        .rsvd = UINT32_MAX & ~RCC_APB1RSTR2_IMPLEMENTED,
        .unimp = RCC_APB1RSTR2_IMPLEMENTED,
    }, {
        .name = "APB2RSTR",
        .addr = A_APB2RSTR,
        .rsvd = UINT32_MAX & ~RCC_APB2RSTR_IMPLEMENTED,
        .unimp = RCC_APB2RSTR_IMPLEMENTED & ~RCC_APB2RSTR_MODELED,
        .post_write = stm32g474_rcc_reset_post_write,
    }, {
        .name = "AHB1ENR",
        .addr = A_AHB1ENR,
        .reset = 0x00000100,
        .rsvd = UINT32_MAX & ~RCC_AHB1ENR_IMPLEMENTED,
        .unimp = RCC_AHB1ENR_IMPLEMENTED & ~RCC_AHB1ENR_MODELED,
        .post_write = stm32g474_rcc_gate_post_write,
    }, {
        .name = "AHB2ENR",
        .addr = A_AHB2ENR,
        .rsvd = UINT32_MAX & ~RCC_AHB2ENR_IMPLEMENTED,
        .unimp = RCC_AHB2ENR_IMPLEMENTED & ~RCC_AHB2ENR_MODELED,
        .post_write = stm32g474_rcc_gate_post_write,
    }, {
        .name = "AHB3ENR",
        .addr = A_AHB3ENR,
        .rsvd = UINT32_MAX & ~RCC_AHB3ENR_IMPLEMENTED,
        .unimp = RCC_AHB3ENR_IMPLEMENTED,
    }, {
        .name = "APB1ENR1",
        .addr = A_APB1ENR1,
        .reset = 0x00000400,
        .rsvd = UINT32_MAX & ~RCC_APB1ENR1_IMPLEMENTED,
        .unimp = RCC_APB1ENR1_IMPLEMENTED & ~RCC_APB1ENR1_MODELED,
        .post_write = stm32g474_rcc_gate_post_write,
    }, {
        .name = "APB1ENR2",
        .addr = A_APB1ENR2,
        .rsvd = UINT32_MAX & ~RCC_APB1ENR2_IMPLEMENTED,
        .unimp = RCC_APB1ENR2_IMPLEMENTED,
    }, {
        .name = "APB2ENR",
        .addr = A_APB2ENR,
        .rsvd = UINT32_MAX & ~RCC_APB2ENR_IMPLEMENTED,
        .unimp = RCC_APB2ENR_IMPLEMENTED & ~RCC_APB2ENR_MODELED,
        .post_write = stm32g474_rcc_gate_post_write,
    },
    RCC_REG(AHB1SMENR, 0x0000131f, RCC_AHB1SMENR_IMPLEMENTED),
    RCC_REG(AHB2SMENR, 0x040f667f, RCC_AHB2SMENR_IMPLEMENTED),
    RCC_REG(AHB3SMENR, 0x00000101, RCC_AHB3SMENR_IMPLEMENTED),
    RCC_REG(APB1SMENR1, 0xd2fecd3f, RCC_APB1SMENR1_IMPLEMENTED),
    RCC_REG(APB1SMENR2, 0x00000103, RCC_APB1SMENR2_IMPLEMENTED),
    RCC_REG(APB2SMENR, 0x0437f801, RCC_APB2SMENR_IMPLEMENTED),
    {
        .name = "CCIPR",
        .addr = A_CCIPR,
        .rsvd = UINT32_MAX & ~RCC_CCIPR_IMPLEMENTED,
        .unimp = RCC_CCIPR_IMPLEMENTED & ~RCC_CCIPR_MODELED,
        .post_write = stm32g474_rcc_gate_post_write,
    }, {
        .name = "BDCR",
        .addr = A_BDCR,
        .rsvd = UINT32_MAX & ~RCC_BDCR_IMPLEMENTED,
        .ro = RCC_BDCR_RO,
        .unimp = RCC_BDCR_IMPLEMENTED & ~RCC_BDCR_RO,
    }, {
        .name = "CSR",
        .addr = A_CSR,
        .reset = 0x0c000000,
        .rsvd = UINT32_MAX & ~RCC_CSR_IMPLEMENTED,
        .ro = R_CSR_LSIRDY_MASK | RCC_CSR_RESET_FLAGS,
        .pre_write = stm32g474_rcc_csr_pre_write,
        .post_write = stm32g474_rcc_register_post_write,
    }, {
        .name = "CRRCR",
        .addr = A_CRRCR,
        .rsvd = UINT32_MAX & ~RCC_CRRCR_IMPLEMENTED,
        .ro = RCC_CRRCR_RO,
        .post_write = stm32g474_rcc_register_post_write,
    }, {
        .name = "CCIPR2",
        .addr = A_CCIPR2,
        .rsvd = UINT32_MAX & ~RCC_CCIPR2_IMPLEMENTED,
        .unimp = RCC_CCIPR2_IMPLEMENTED,
    },
};

#undef RCC_REG

static RegisterInfo *
stm32g474_rcc_find_register(RegisterInfoArray *reg_array, hwaddr addr)
{
    for (int i = 0; i < reg_array->num_elements; i++) {
        if (reg_array->r[i]->access->addr == addr) {
            return reg_array->r[i];
        }
    }
    return NULL;
}

static uint64_t stm32g474_rcc_read(void *opaque, hwaddr addr, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    hwaddr aligned = addr & ~0x3;
    unsigned int shift = (addr & 0x3) * 8;
    RegisterInfo *reg;
    uint64_t mask;

    if ((addr & 0x3) + size > 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: register read crosses a word boundary at 0x%"
                      HWADDR_PRIx "\n", reg_array->prefix, addr);
        return 0;
    }

    reg = stm32g474_rcc_find_register(reg_array, aligned);
    if (!reg) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read from unimplemented register at 0x%"
                      HWADDR_PRIx "\n", reg_array->prefix, addr);
        return 0;
    }

    mask = MAKE_64BIT_MASK(shift, size * 8);
    return register_read(reg, mask, reg_array->prefix,
                         reg_array->debug) >> shift;
}

static void stm32g474_rcc_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    hwaddr aligned = addr & ~0x3;
    unsigned int shift = (addr & 0x3) * 8;
    RegisterInfo *reg;
    uint64_t mask;

    if ((addr & 0x3) + size > 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: register write crosses a word boundary at 0x%"
                      HWADDR_PRIx "\n", reg_array->prefix, addr);
        return;
    }

    reg = stm32g474_rcc_find_register(reg_array, aligned);
    if (!reg) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to unimplemented register at 0x%"
                      HWADDR_PRIx "\n", reg_array->prefix, addr);
        return;
    }

    mask = MAKE_64BIT_MASK(shift, size * 8);
    register_write(reg, value << shift, mask, reg_array->prefix,
                   reg_array->debug);
}

static const MemoryRegionOps stm32g474_rcc_ops = {
    .read = stm32g474_rcc_read,
    .write = stm32g474_rcc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void stm32g474_rcc_zero_outputs(Stm32g474RccState *s)
{
    Clock *core[] = {
        s->sysclk, s->hclk, s->pclk1, s->pclk2, s->cortex_refclk,
        s->pll_p, s->pll_q, s->pll_r,
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(core); i++) {
        clock_update_hz(core[i], 0);
    }
    for (unsigned int i = 0; i < STM32G474_RCC_GATE_COUNT; i++) {
        clock_update_hz(s->gate[i], 0);
    }
}

static void stm32g474_rcc_reset_enter(Object *obj, ResetType type)
{
    Stm32g474RccState *s = STM32G474_RCC(obj);
    uint32_t hsebyp = s->regs[R_CR] & R_CR_HSEBYP_MASK;

    s->resetting = true;
    for (unsigned int i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        register_reset(&s->regs_info[i]);
    }
    s->regs[R_CR] |= hsebyp;
}

static void stm32g474_rcc_reset_hold(Object *obj, ResetType type)
{
    Stm32g474RccState *s = STM32G474_RCC(obj);

    qemu_set_irq(s->irq, 0);
    stm32g474_rcc_zero_outputs(s);
    for (unsigned int i = 0; i < STM32G474_RCC_RESET_COUNT; i++) {
        qemu_set_irq(s->peripheral_reset[i], 0);
    }
}

static void stm32g474_rcc_reset_exit(Object *obj, ResetType type)
{
    Stm32g474RccState *s = STM32G474_RCC(obj);

    stm32g474_rcc_reconcile_ready(s, false);
    s->regs[R_CIFR] = 0;
    s->regs[R_CICR] = 0;
    s->resetting = false;
    stm32g474_rcc_update_clocks(s, true);
    stm32g474_rcc_update_resets(s);
    stm32g474_rcc_update_irq(s);
}

static int stm32g474_rcc_post_load(void *opaque, int version_id)
{
    Stm32g474RccState *s = STM32G474_RCC(opaque);

    for (unsigned int i = 0; i < ARRAY_SIZE(stm32g474_rcc_regs_info); i++) {
        const RegisterAccessInfo *access = &stm32g474_rcc_regs_info[i];
        unsigned int index = access->addr / 4;

        s->regs[index] = (s->regs[index] & ~access->rsvd) |
                         (access->reset & access->rsvd);
    }
    s->regs[R_CICR] = 0;
    s->regs[R_CSR] &= ~R_CSR_RMVF_MASK;
    s->regs[R_BDCR] &= ~RCC_BDCR_RO;
    s->regs[R_CRRCR] &= ~R_CRRCR_HSI48CAL_MASK;
    stm32g474_rcc_update_hsi_calibration(s);
    if (FIELD_EX32(s->regs[R_CFGR], CFGR, SW) ==
        RCC_SYSCLK_RESERVED) {
        s->regs[R_CFGR] = FIELD_DP32(s->regs[R_CFGR], CFGR, SW,
                                     RCC_SYSCLK_HSI16);
    }
    if (FIELD_EX32(s->regs[R_CFGR], CFGR, SWS) ==
        RCC_SYSCLK_RESERVED) {
        s->regs[R_CFGR] = FIELD_DP32(s->regs[R_CFGR], CFGR, SWS,
                                     RCC_SYSCLK_HSI16);
    }

    stm32g474_rcc_reconcile_ready(s, false);
    stm32g474_rcc_update_clocks(s, false);
    stm32g474_rcc_update_resets(s);
    stm32g474_rcc_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_stm32g474_rcc = {
    .name = TYPE_STM32G474_RCC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stm32g474_rcc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Stm32g474RccState,
                             STM32G474_RCC_R_MAX),
        VMSTATE_END_OF_LIST()
    },
};

static void stm32g474_rcc_realize(DeviceState *dev, Error **errp)
{
    Stm32g474RccState *s = STM32G474_RCC(dev);

    if (!clock_has_source(s->hsi16_in)) {
        error_setg(errp, TYPE_STM32G474_RCC
                   ": hsi16-in clock must be connected");
        return;
    }
    if (!clock_has_source(s->hsi48_in)) {
        error_setg(errp, TYPE_STM32G474_RCC
                   ": hsi48-in clock must be connected");
        return;
    }
    if (!clock_has_source(s->lsi_in)) {
        error_setg(errp, TYPE_STM32G474_RCC
                   ": lsi-in clock must be connected");
    }
}

static void stm32g474_rcc_init(Object *obj)
{
    static const char *const gate_names[] = {
        [STM32G474_RCC_GATE_DMA1] = "dma1",
        [STM32G474_RCC_GATE_FLASH] = "flash",
        [STM32G474_RCC_GATE_GPIOA] = "gpioa",
        [STM32G474_RCC_GATE_GPIOB] = "gpiob",
        [STM32G474_RCC_GATE_GPIOC] = "gpioc",
        [STM32G474_RCC_GATE_GPIOD] = "gpiod",
        [STM32G474_RCC_GATE_GPIOE] = "gpioe",
        [STM32G474_RCC_GATE_GPIOF] = "gpiof",
        [STM32G474_RCC_GATE_GPIOG] = "gpiog",
        [STM32G474_RCC_GATE_PWR] = "pwr",
        [STM32G474_RCC_GATE_USB] = "usb",
        [STM32G474_RCC_GATE_FDCAN] = "fdcan",
        [STM32G474_RCC_GATE_USART1] = "usart1",
        [STM32G474_RCC_GATE_USART2] = "usart2",
        [STM32G474_RCC_GATE_UART4] = "uart4",
        [STM32G474_RCC_GATE_SYSCFG] = "syscfg",
    };
    Stm32g474RccState *s = STM32G474_RCC(obj);
    DeviceState *dev = DEVICE(obj);

    s->hsi16_in = qdev_init_clock_in(dev, "hsi16-in",
                                     stm32g474_rcc_input_clock_update, s,
                                     ClockUpdate);
    s->hse_in = qdev_init_clock_in(dev, "hse-in",
                                   stm32g474_rcc_input_clock_update, s,
                                   ClockUpdate);
    s->hsi48_in = qdev_init_clock_in(dev, "hsi48-in",
                                     stm32g474_rcc_input_clock_update, s,
                                     ClockUpdate);
    s->lsi_in = qdev_init_clock_in(dev, "lsi-in",
                                   stm32g474_rcc_input_clock_update, s,
                                   ClockUpdate);

    s->sysclk = qdev_init_clock_out(dev, "sysclk");
    s->hclk = qdev_init_clock_out(dev, "hclk");
    s->pclk1 = qdev_init_clock_out(dev, "pclk1");
    s->pclk2 = qdev_init_clock_out(dev, "pclk2");
    s->cortex_refclk = qdev_init_clock_out(dev, "cortex-refclk");
    s->pll_p = qdev_init_clock_out(dev, "pll-p");
    s->pll_q = qdev_init_clock_out(dev, "pll-q");
    s->pll_r = qdev_init_clock_out(dev, "pll-r");
    for (unsigned int i = 0; i < ARRAY_SIZE(gate_names); i++) {
        s->gate[i] = qdev_init_clock_out(dev, gate_names[i]);
    }

    s->reg_array = register_init_block32(
        dev, stm32g474_rcc_regs_info,
        ARRAY_SIZE(stm32g474_rcc_regs_info), s->regs_info, s->regs,
        &stm32g474_rcc_ops, false, STM32G474_RCC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->reg_array->mem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(dev, s->peripheral_reset,
                             "peripheral-reset",
                             STM32G474_RCC_RESET_COUNT);
}

static void stm32g474_rcc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = stm32g474_rcc_realize;
    dc->vmsd = &vmstate_stm32g474_rcc;
    dc->user_creatable = false;
    rc->phases.enter = stm32g474_rcc_reset_enter;
    rc->phases.hold = stm32g474_rcc_reset_hold;
    rc->phases.exit = stm32g474_rcc_reset_exit;
}

static const TypeInfo stm32g474_rcc_info = {
    .name = TYPE_STM32G474_RCC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stm32g474RccState),
    .instance_init = stm32g474_rcc_init,
    .class_init = stm32g474_rcc_class_init,
};

static void stm32g474_rcc_register_types(void)
{
    type_register_static(&stm32g474_rcc_info);
}

type_init(stm32g474_rcc_register_types)
