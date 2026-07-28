/*
 * QTest for the STM32G474 USB FS controller
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
#include "qobject/qlist.h"

#define STM32G474_MACHINE "stm32g474"
#define MCU_QOM_PATH "/machine/mcu"
#define USBFS_QOM_PATH MCU_QOM_PATH "/usbfs"
#define USBFS_PCLK_QOM_PATH USBFS_QOM_PATH "/pclk"
#define USBFS_CLOCK_QOM_PATH USBFS_QOM_PATH "/usb"

#define USBFS_BASE 0x40005c00ULL
#define USBFS_SIZE 0x400
#define USBFS_PMA_BASE 0x40006000ULL
#define USBFS_PMA_SIZE 0x400

#define USBFS_EP0R 0x00
#define USBFS_EP1R 0x04
#define USBFS_EP2R 0x08
#define USBFS_EP3R 0x0c
#define USBFS_CNTR 0x40
#define USBFS_ISTR 0x44
#define USBFS_FNR 0x48
#define USBFS_DADDR 0x4c
#define USBFS_BTABLE 0x50
#define USBFS_LPMCSR 0x54
#define USBFS_BCDR 0x58

#define USBFS_CNTR_FRES BIT(0)
#define USBFS_CNTR_RESETM BIT(10)
#define USBFS_CNTR_CTRM BIT(15)
#define USBFS_ISTR_DIR BIT(4)
#define USBFS_ISTR_RESET BIT(10)
#define USBFS_ISTR_CTR BIT(15)
#define USBFS_EP_KIND BIT(8)
#define USBFS_EP_TYPE_CONTROL (1U << 9)
#define USBFS_EP_TYPE_ISOCHRONOUS (2U << 9)
#define USBFS_EP_CTR_TX BIT(7)
#define USBFS_EP_CTR_RX BIT(15)
#define USBFS_EP_CTR_MASK (BIT(15) | BIT(7))
#define USBFS_QTEST_CTR_EP_SHIFT 16

#define RCC_BASE 0x40021000ULL
#define RCC_CR 0x00
#define RCC_PLLCFGR 0x0c
#define RCC_APB1RSTR1 0x38
#define RCC_APB1ENR1 0x58
#define RCC_CCIPR 0x88
#define RCC_CRRCR 0x98
#define RCC_CR_PLLON BIT(24)
#define RCC_PLLCFGR_HSI16_170MHZ 0x01105532U
#define RCC_USB_BIT BIT(23)
#define RCC_CCIPR_CLK48_SHIFT 26
#define RCC_CCIPR_CLK48_MASK (3U << RCC_CCIPR_CLK48_SHIFT)
#define RCC_CCIPR_CLK48_HSI48 (0U << RCC_CCIPR_CLK48_SHIFT)
#define RCC_CCIPR_CLK48_PLLQ (2U << RCC_CCIPR_CLK48_SHIFT)
#define RCC_CRRCR_HSI48ON BIT(0)

#define NVIC_ISER 0xe000e100ULL
#define NVIC_ISPR 0xe000e200ULL
#define NVIC_ICPR 0xe000e280ULL
#define USBFS_HP_IRQ 19
#define USBFS_LP_IRQ 20

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

static uint16_t usbfs_readw(QTestState *qts, uint32_t offset)
{
    return qtest_readw(qts, USBFS_BASE + offset);
}

static uint32_t usbfs_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, USBFS_BASE + offset);
}

static void usbfs_writew(QTestState *qts, uint32_t offset, uint16_t value)
{
    qtest_writew(qts, USBFS_BASE + offset, value);
}

static void usbfs_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, USBFS_BASE + offset, value);
}

static uint8_t pma_readb(QTestState *qts, uint32_t offset)
{
    return qtest_readb(qts, USBFS_PMA_BASE + offset);
}

static uint16_t pma_readw(QTestState *qts, uint32_t offset)
{
    return qtest_readw(qts, USBFS_PMA_BASE + offset);
}

static void pma_writeb(QTestState *qts, uint32_t offset, uint8_t value)
{
    qtest_writeb(qts, USBFS_PMA_BASE + offset, value);
}

static void pma_writew(QTestState *qts, uint32_t offset, uint16_t value)
{
    qtest_writew(qts, USBFS_PMA_BASE + offset, value);
}

static uint32_t rcc_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, RCC_BASE + offset);
}

static void rcc_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, RCC_BASE + offset, value);
}

static void assert_qom_child(QTestState *qts, const char *parent,
                             const char *name, const char *type)
{
    g_autofree char *expected_type = g_strdup_printf("child<%s>", type);
    QDict *response;
    QList *properties;
    QListEntry *entry;

    response = qtest_qmp(qts,
                         "{ 'execute': 'qom-list',"
                         "  'arguments': { 'path': %s } }", parent);
    g_assert_false(qdict_haskey(response, "error"));
    properties = qdict_get_qlist(response, "return");
    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *property = qobject_to(QDict, qlist_entry_obj(entry));

        if (!g_strcmp0(qdict_get_str(property, "name"), name)) {
            g_assert_cmpstr(qdict_get_str(property, "type"), ==,
                            expected_type);
            qobject_unref(response);
            return;
        }
    }

    qobject_unref(response);
    g_error("QOM child '%s' not found below '%s'", name, parent);
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

static void set_usb_clock_valid(QTestState *qts)
{
    uint32_t ccipr = rcc_readl(qts, RCC_CCIPR);

    ccipr &= ~RCC_CCIPR_CLK48_MASK;
    ccipr |= RCC_CCIPR_CLK48_HSI48;
    rcc_writel(qts, RCC_CCIPR, ccipr);
    rcc_writel(qts, RCC_CRRCR,
               rcc_readl(qts, RCC_CRRCR) | RCC_CRRCR_HSI48ON);
    rcc_writel(qts, RCC_APB1ENR1,
               rcc_readl(qts, RCC_APB1ENR1) | RCC_USB_BIT);
}

static void set_usb_clock_invalid_170mhz(QTestState *qts)
{
    uint32_t ccipr = rcc_readl(qts, RCC_CCIPR);

    rcc_writel(qts, RCC_PLLCFGR, RCC_PLLCFGR_HSI16_170MHZ);
    rcc_writel(qts, RCC_CR,
               rcc_readl(qts, RCC_CR) | RCC_CR_PLLON);
    ccipr &= ~RCC_CCIPR_CLK48_MASK;
    ccipr |= RCC_CCIPR_CLK48_PLLQ;
    rcc_writel(qts, RCC_CCIPR, ccipr);
    rcc_writel(qts, RCC_APB1ENR1,
               rcc_readl(qts, RCC_APB1ENR1) | RCC_USB_BIT);
}

static void set_usb_gate(QTestState *qts, bool enabled)
{
    uint32_t value = rcc_readl(qts, RCC_APB1ENR1);

    value = enabled ? value | RCC_USB_BIT : value & ~RCC_USB_BIT;
    rcc_writel(qts, RCC_APB1ENR1, value);
}

static uint64_t nvic_reg(uint64_t base, unsigned int irq)
{
    return base + irq / 32 * sizeof(uint32_t);
}

static uint32_t nvic_mask(unsigned int irq)
{
    return BIT(irq % 32);
}

static void nvic_enable(QTestState *qts, unsigned int irq)
{
    qtest_writel(qts, nvic_reg(NVIC_ISER, irq), nvic_mask(irq));
}

static void nvic_clear(QTestState *qts, unsigned int irq)
{
    qtest_writel(qts, nvic_reg(NVIC_ICPR, irq), nvic_mask(irq));
}

static bool nvic_pending(QTestState *qts, unsigned int irq)
{
    return (qtest_readl(qts, nvic_reg(NVIC_ISPR, irq)) &
            nvic_mask(irq)) != 0;
}

static void usbfs_set_ctr(QTestState *qts, unsigned int ep, uint32_t ctr)
{
    QDict *response;

    response = qtest_qmp(qts,
        "{ 'execute': 'qom-set', 'arguments': { 'path': %s, "
        "'property': 'qtest-set-ctr', 'value': %u } }",
        USBFS_QOM_PATH, ep << USBFS_QTEST_CTR_EP_SHIFT | ctr);
    g_assert_false(qdict_haskey(response, "error"));
    qobject_unref(response);
}

static void usbfs_release_forced_reset(QTestState *qts)
{
    usbfs_writew(qts, USBFS_CNTR, 0);
}

static void assert_reset_image(QTestState *qts)
{
    for (unsigned int ep = 0; ep < 8; ep++) {
        g_assert_cmphex(usbfs_readw(qts, ep * sizeof(uint32_t)), ==, 0);
    }
    g_assert_cmphex(usbfs_readw(qts, USBFS_CNTR), ==, 0x0003);
    g_assert_cmphex(usbfs_readw(qts, USBFS_ISTR), ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_FNR), ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_DADDR), ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_BTABLE), ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_LPMCSR), ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_BCDR), ==, 0);
}

static void configure_ep0(QTestState *qts)
{
    usbfs_writew(qts, USBFS_EP0R, 0x8280);
    usbfs_writew(qts, USBFS_EP0R, 0xb280);
    usbfs_writew(qts, USBFS_EP0R, 0x8280);
    usbfs_writew(qts, USBFS_EP0R, 0x82a0);
}

static void test_constructs_clocks_and_reset(void)
{
    QTestState *qts = stm32g474_qtest_start();

    assert_qom_child(qts, MCU_QOM_PATH, "usbfs", "stm32g474-usbfs");
    assert_clock_hz(qts, USBFS_PCLK_QOM_PATH, 16000000);
    assert_clock_hz(qts, USBFS_CLOCK_QOM_PATH, 0);
    assert_reset_image(qts);
    g_assert_cmphex(pma_readw(qts, 0), ==, 0);
    g_assert_cmphex(pma_readw(qts, USBFS_PMA_SIZE - 2), ==, 0);

    set_usb_clock_valid(qts);
    assert_clock_hz(qts, USBFS_PCLK_QOM_PATH, 16000000);
    assert_clock_hz(qts, USBFS_CLOCK_QOM_PATH, 48000000);

    qtest_quit(qts);
}

static void test_fixed_register_access_and_masks(void)
{
    QTestState *qts = stm32g474_qtest_start();

    usbfs_release_forced_reset(qts);
    usbfs_writew(qts, USBFS_DADDR, UINT16_MAX);
    g_assert_cmphex(usbfs_readw(qts, USBFS_DADDR), ==, 0x00ff);
    usbfs_writel(qts, USBFS_DADDR, 0x55);
    g_assert_cmphex(usbfs_readl(qts, USBFS_DADDR), ==, 0x55);

    usbfs_writew(qts, USBFS_BTABLE, UINT16_MAX);
    g_assert_cmphex(usbfs_readw(qts, USBFS_BTABLE), ==, 0xfff8);
    usbfs_writew(qts, USBFS_LPMCSR, UINT16_MAX);
    g_assert_cmphex(usbfs_readw(qts, USBFS_LPMCSR), ==, 0x0003);
    usbfs_writel(qts, USBFS_BCDR, UINT32_MAX);
    g_assert_cmphex(usbfs_readl(qts, USBFS_BCDR), ==, 0x800f);
    usbfs_writel(qts, USBFS_FNR, UINT32_MAX);
    g_assert_cmphex(usbfs_readl(qts, USBFS_FNR), ==, 0);

    qtest_quit(qts);
}

static void test_endpoint_toggle_and_rc_w0(void)
{
    QTestState *qts = stm32g474_qtest_start();

    usbfs_release_forced_reset(qts);
    usbfs_writew(qts, USBFS_EP0R, 0x8280);
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0x0200);
    usbfs_writew(qts, USBFS_EP0R, 0xb280);
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0x3200);
    usbfs_writew(qts, USBFS_EP0R, 0x8280);
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0x3200);
    usbfs_writew(qts, USBFS_EP0R, 0x82a0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0x3220);

    qtest_system_reset(qts);
    usbfs_release_forced_reset(qts);
    usbfs_writew(qts, USBFS_EP0R, UINT16_MAX);
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R) &
                    USBFS_EP_CTR_MASK, ==, 0);

    qtest_quit(qts);
}

static void test_pma_lanes_bdt_relocation_and_bounds(void)
{
    QTestState *qts = stm32g474_qtest_start();

    usbfs_release_forced_reset(qts);
    pma_writew(qts, 0x100, 0xabcd);
    g_assert_cmphex(pma_readb(qts, 0x100), ==, 0xcd);
    g_assert_cmphex(pma_readb(qts, 0x101), ==, 0xab);
    pma_writeb(qts, 0x101, 0x12);
    pma_writeb(qts, 0x100, 0x34);
    g_assert_cmphex(pma_readw(qts, 0x100), ==, 0x1234);

    pma_writew(qts, 0x00, UINT16_MAX);
    g_assert_cmphex(pma_readw(qts, 0x00), ==, 0xfffe);
    pma_writew(qts, 0x06, UINT16_MAX);
    g_assert_cmphex(pma_readw(qts, 0x06), ==, 0xfc00);
    pma_writeb(qts, 0x06, UINT8_MAX);
    pma_writeb(qts, 0x07, 0x84);
    g_assert_cmphex(pma_readw(qts, 0x06), ==, 0x8400);

    usbfs_writew(qts, USBFS_BTABLE, 0x0080);
    g_assert_cmphex(pma_readw(qts, 0x00), ==, 0xfffe);
    pma_writew(qts, 0x00, UINT16_MAX);
    g_assert_cmphex(pma_readw(qts, 0x00), ==, UINT16_MAX);
    pma_writew(qts, 0x80, UINT16_MAX);
    g_assert_cmphex(pma_readw(qts, 0x80), ==, 0xfffe);
    pma_writew(qts, 0x86, UINT16_MAX);
    g_assert_cmphex(pma_readw(qts, 0x86), ==, 0xfc00);

    usbfs_writew(qts, USBFS_BTABLE, 0x03c0);
    pma_writew(qts, 0x3c0, UINT16_MAX);
    pma_writew(qts, 0x3c6, UINT16_MAX);
    g_assert_cmphex(pma_readw(qts, 0x3c0), ==, 0xfffe);
    g_assert_cmphex(pma_readw(qts, 0x3c6), ==, 0xfc00);

    usbfs_writew(qts, USBFS_BTABLE, 0x03c8);
    pma_writew(qts, 0x3c8, UINT16_MAX);
    g_assert_cmphex(pma_readw(qts, 0x3c8), ==, UINT16_MAX);
    usbfs_writew(qts, USBFS_BTABLE, 0xfff8);
    pma_writew(qts, USBFS_PMA_SIZE - 2, 0x5aa5);
    g_assert_cmphex(pma_readw(qts, USBFS_PMA_SIZE - 2), ==, 0x5aa5);

    qtest_quit(qts);
}

static void test_fres_clock_and_irq_levels(void)
{
    QTestState *qts = stm32g474_qtest_start();

    qtest_irq_intercept_out_named(qts, USBFS_QOM_PATH, "sysbus-irq");
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    set_usb_clock_invalid_170mhz(qts);
    assert_clock_hz(qts, USBFS_CLOCK_QOM_PATH, 170000000);
    usbfs_writew(qts, USBFS_CNTR, USBFS_CNTR_RESETM);
    usbfs_writew(qts, USBFS_BTABLE, 0x0080);
    usbfs_writew(qts, USBFS_BCDR, 0x8000);
    usbfs_writew(qts, USBFS_DADDR, 0x00d5);
    configure_ep0(qts);
    pma_writew(qts, 0x100, 0xa55a);
    usbfs_writew(qts, USBFS_CNTR,
                 USBFS_CNTR_RESETM | USBFS_CNTR_FRES);
    g_assert_cmphex(usbfs_readw(qts, USBFS_ISTR) &
                    USBFS_ISTR_RESET, ==, 0);
    usbfs_writew(qts, USBFS_DADDR, 0x00aa);
    usbfs_writew(qts, USBFS_EP0R, UINT16_MAX);
    usbfs_set_ctr(qts, 1, USBFS_EP_CTR_RX);
    g_assert_cmphex(usbfs_readw(qts, USBFS_DADDR), ==, 0x00d5);
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0x3220);
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP1R), ==, 0);

    set_usb_clock_valid(qts);
    assert_clock_hz(qts, USBFS_CLOCK_QOM_PATH, 48000000);
    g_assert_cmphex(usbfs_readw(qts, USBFS_ISTR) &
                    USBFS_ISTR_RESET, ==, USBFS_ISTR_RESET);
    g_assert_cmphex(usbfs_readw(qts, USBFS_DADDR), ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_BTABLE), ==, 0x0080);
    g_assert_cmphex(usbfs_readw(qts, USBFS_BCDR), ==, 0x8000);
    g_assert_cmphex(pma_readw(qts, 0x100), ==, 0xa55a);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    usbfs_writew(qts, USBFS_DADDR, 0x00aa);
    configure_ep0(qts);
    usbfs_set_ctr(qts, 1, USBFS_EP_CTR_RX);
    usbfs_writew(qts, USBFS_ISTR, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_DADDR), ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP1R), ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_ISTR) &
                    USBFS_ISTR_RESET, ==, USBFS_ISTR_RESET);

    usbfs_writew(qts, USBFS_CNTR, USBFS_CNTR_RESETM);
    usbfs_writew(qts, USBFS_ISTR, 0);
    usbfs_writew(qts, USBFS_DADDR, 0x00aa);
    configure_ep0(qts);
    g_assert_cmphex(usbfs_readw(qts, USBFS_ISTR) &
                    USBFS_ISTR_RESET, ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_DADDR), ==, 0x00aa);
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0x3220);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

static void test_nvic_routing_and_clock_gate(void)
{
    QTestState *qts = stm32g474_qtest_start();

    set_usb_clock_valid(qts);
    nvic_enable(qts, USBFS_HP_IRQ);
    nvic_enable(qts, USBFS_LP_IRQ);
    nvic_clear(qts, USBFS_HP_IRQ);
    nvic_clear(qts, USBFS_LP_IRQ);

    usbfs_writew(qts, USBFS_CNTR, USBFS_CNTR_RESETM);
    usbfs_writew(qts, USBFS_CNTR,
                 USBFS_CNTR_RESETM | USBFS_CNTR_FRES);
    g_assert_false(nvic_pending(qts, USBFS_HP_IRQ));
    g_assert_true(nvic_pending(qts, USBFS_LP_IRQ));

    set_usb_gate(qts, false);
    nvic_clear(qts, USBFS_LP_IRQ);
    g_assert_false(nvic_pending(qts, USBFS_LP_IRQ));
    g_assert_cmphex(usbfs_readw(qts, USBFS_ISTR) &
                    USBFS_ISTR_RESET, ==, USBFS_ISTR_RESET);

    set_usb_gate(qts, true);
    g_assert_true(nvic_pending(qts, USBFS_LP_IRQ));
    usbfs_writew(qts, USBFS_CNTR, USBFS_CNTR_RESETM);
    usbfs_writew(qts, USBFS_ISTR, 0);
    nvic_clear(qts, USBFS_LP_IRQ);
    g_assert_false(nvic_pending(qts, USBFS_HP_IRQ));
    g_assert_false(nvic_pending(qts, USBFS_LP_IRQ));

    qtest_quit(qts);
}

static void test_endpoint_irq_priority(void)
{
    QTestState *qts = stm32g474_qtest_start();

    qtest_irq_intercept_out_named(qts, USBFS_QOM_PATH, "sysbus-irq");
    set_usb_clock_valid(qts);
    usbfs_release_forced_reset(qts);

    usbfs_writew(qts, USBFS_EP1R, USBFS_EP_TYPE_ISOCHRONOUS);
    usbfs_set_ctr(qts, 1, USBFS_EP_CTR_RX);
    g_assert_cmphex(usbfs_readw(qts, USBFS_ISTR), ==,
                    USBFS_ISTR_CTR | USBFS_ISTR_DIR | 1);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    usbfs_writew(qts, USBFS_EP1R,
                 usbfs_readw(qts, USBFS_EP1R) & ~USBFS_EP_CTR_RX);
    g_assert_false(qtest_get_irq(qts, 0));

    usbfs_writew(qts, USBFS_EP2R, USBFS_EP_TYPE_CONTROL);
    usbfs_set_ctr(qts, 2, USBFS_EP_CTR_TX);
    g_assert_cmphex(usbfs_readw(qts, USBFS_ISTR), ==,
                    USBFS_ISTR_CTR | 2);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    usbfs_writew(qts, USBFS_CNTR, USBFS_CNTR_CTRM);
    g_assert_true(qtest_get_irq(qts, 1));
    usbfs_writew(qts, USBFS_EP2R,
                 usbfs_readw(qts, USBFS_EP2R) & ~USBFS_EP_CTR_TX);
    g_assert_false(qtest_get_irq(qts, 1));

    usbfs_writew(qts, USBFS_CNTR, 0);
    usbfs_writew(qts, USBFS_EP3R, USBFS_EP_KIND);
    usbfs_set_ctr(qts, 3, USBFS_EP_CTR_TX);
    g_assert_cmphex(usbfs_readw(qts, USBFS_ISTR), ==,
                    USBFS_ISTR_CTR | 3);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

static void test_rcc_held_reset(void)
{
    QTestState *qts = stm32g474_qtest_start();

    usbfs_release_forced_reset(qts);
    usbfs_writew(qts, USBFS_DADDR, 0x00aa);
    pma_writew(qts, 0x100, 0x55aa);
    rcc_writel(qts, RCC_APB1RSTR1, RCC_USB_BIT);
    g_assert_cmphex(rcc_readl(qts, RCC_APB1RSTR1) & RCC_USB_BIT,
                    ==, RCC_USB_BIT);
    assert_reset_image(qts);
    g_assert_cmphex(pma_readw(qts, 0x100), ==, 0);

    usbfs_writew(qts, USBFS_CNTR, 0);
    usbfs_writew(qts, USBFS_DADDR, 0x0055);
    pma_writew(qts, 0x100, 0xa55a);
    assert_reset_image(qts);
    g_assert_cmphex(pma_readw(qts, 0x100), ==, 0);

    rcc_writel(qts, RCC_APB1RSTR1, 0);
    usbfs_writew(qts, USBFS_CNTR, 0);
    usbfs_writew(qts, USBFS_DADDR, 0x0055);
    pma_writew(qts, 0x100, 0xa55a);
    g_assert_cmphex(usbfs_readw(qts, USBFS_CNTR), ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_DADDR), ==, 0x0055);
    g_assert_cmphex(pma_readw(qts, 0x100), ==, 0xa55a);

    qtest_quit(qts);
}

static void test_system_reset(void)
{
    QTestState *qts = stm32g474_qtest_start();

    set_usb_clock_valid(qts);
    usbfs_writew(qts, USBFS_CNTR, USBFS_CNTR_RESETM);
    usbfs_writew(qts, USBFS_DADDR, 0x00aa);
    configure_ep0(qts);
    pma_writew(qts, 0x100, 0xa55a);
    usbfs_writew(qts, USBFS_CNTR,
                 USBFS_CNTR_RESETM | USBFS_CNTR_FRES);
    usbfs_writew(qts, USBFS_DADDR, 0x0055);
    configure_ep0(qts);
    g_assert_cmphex(usbfs_readw(qts, USBFS_ISTR) &
                    USBFS_ISTR_RESET, ==, USBFS_ISTR_RESET);
    g_assert_cmphex(usbfs_readw(qts, USBFS_DADDR), ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0);
    g_assert_cmphex(pma_readw(qts, 0x100), ==, 0xa55a);
    g_assert_cmphex(rcc_readl(qts, RCC_APB1RSTR1) & RCC_USB_BIT, ==, 0);

    qtest_system_reset(qts);
    assert_reset_image(qts);
    g_assert_cmphex(pma_readw(qts, 0x100), ==, 0);
    g_assert_cmphex(rcc_readl(qts, RCC_APB1RSTR1) & RCC_USB_BIT, ==, 0);
    g_assert_cmphex(rcc_readl(qts, RCC_APB1ENR1) & RCC_USB_BIT, ==, 0);
    assert_clock_hz(qts, USBFS_PCLK_QOM_PATH, 16000000);
    assert_clock_hz(qts, USBFS_CLOCK_QOM_PATH, 0);

    qtest_quit(qts);
}

static void run_hostless_hal_sequence(QTestState *qts)
{
    unsigned int step = 0;

    g_assert_cmphex(usbfs_readw(qts, USBFS_CNTR), ==, 0x0003); step++;
    usbfs_writew(qts, USBFS_CNTR, 0x0003); step++;
    usbfs_writew(qts, USBFS_CNTR, 0x0001); step++;
    usbfs_writew(qts, USBFS_CNTR, 0x0000); step++;
    usbfs_writew(qts, USBFS_ISTR, 0); step++;
    usbfs_writew(qts, USBFS_BTABLE, 0); step++;
    g_assert_cmphex(usbfs_readw(qts, USBFS_CNTR), ==, 0); step++;
    usbfs_writew(qts, USBFS_CNTR, 0); step++;
    g_assert_cmphex(usbfs_readw(qts, USBFS_BCDR), ==, 0); step++;
    usbfs_writew(qts, USBFS_BCDR, 0); step++;
    usbfs_writew(qts, USBFS_ISTR, 0); step++;
    usbfs_writew(qts, USBFS_CNTR, 0xbf80); step++;
    g_assert_cmphex(usbfs_readw(qts, USBFS_BCDR), ==, 0); step++;
    usbfs_writew(qts, USBFS_BCDR, 0x8000); step++;
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0); step++;
    usbfs_writew(qts, USBFS_EP0R, 0x8280); step++;
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0x0200); step++;
    usbfs_writew(qts, USBFS_EP0R, 0x8280); step++;
    g_assert_cmphex(usbfs_readw(qts, USBFS_BTABLE), ==, 0); step++;
    pma_writew(qts, 0x04, 0x0040); step++;
    g_assert_cmphex(usbfs_readw(qts, USBFS_BTABLE), ==, 0); step++;
    g_assert_cmphex(pma_readw(qts, 0x06), ==, 0); step++;
    pma_writew(qts, 0x06, 0); step++;
    g_assert_cmphex(pma_readw(qts, 0x06), ==, 0); step++;
    pma_writew(qts, 0x06, 0x8400); step++;
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R) & BIT(14), ==, 0); step++;
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0x0200); step++;
    usbfs_writew(qts, USBFS_EP0R, 0xb280); step++;
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0x3200); step++;
    usbfs_writew(qts, USBFS_EP0R, 0x8280); step++;
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0x3200); step++;
    usbfs_writew(qts, USBFS_EP0R, 0x8280); step++;
    g_assert_cmphex(usbfs_readw(qts, USBFS_BTABLE), ==, 0); step++;
    pma_writew(qts, 0x00, 0x0080); step++;
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R) & BIT(6), ==, 0); step++;
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0x3200); step++;
    usbfs_writew(qts, USBFS_EP0R, 0x82a0); step++;

    g_assert_cmpuint(step, ==, 37);
}

static void test_hostless_hal_sequence(void)
{
    QTestState *qts = stm32g474_qtest_start();

    set_usb_clock_valid(qts);
    run_hostless_hal_sequence(qts);
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0x3220);
    g_assert_cmphex(usbfs_readw(qts, USBFS_BTABLE), ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_BCDR), ==, 0x8000);
    g_assert_cmphex(usbfs_readw(qts, USBFS_ISTR), ==, 0);
    g_assert_cmphex(pma_readw(qts, 0x00), ==, 0x0080);
    g_assert_cmphex(pma_readw(qts, 0x04), ==, 0x0040);
    g_assert_cmphex(pma_readw(qts, 0x06), ==, 0x8400);
    g_assert_false(nvic_pending(qts, USBFS_HP_IRQ));
    g_assert_false(nvic_pending(qts, USBFS_LP_IRQ));

    qtest_quit(qts);
}

static void configure_migration_source(QTestState *qts)
{
    set_usb_clock_invalid_170mhz(qts);
    usbfs_writew(qts, USBFS_CNTR, USBFS_CNTR_RESETM);
    usbfs_writew(qts, USBFS_BTABLE, 0x0080);
    usbfs_writew(qts, USBFS_DADDR, 0x00d5);
    usbfs_writew(qts, USBFS_BCDR, 0x8000);
    configure_ep0(qts);
    pma_writew(qts, 0x80, 0x0122);
    pma_writew(qts, 0x86, 0x8400);
    pma_writew(qts, 0x100, 0xa55a);
    usbfs_writew(qts, USBFS_CNTR,
                 USBFS_CNTR_RESETM | USBFS_CNTR_FRES);
}

static void assert_migrated_state(QTestState *qts)
{
    g_assert_cmphex(usbfs_readw(qts, USBFS_CNTR), ==,
                    USBFS_CNTR_RESETM | USBFS_CNTR_FRES);
    g_assert_cmphex(usbfs_readw(qts, USBFS_ISTR) &
                    USBFS_ISTR_RESET, ==, 0);
    g_assert_cmphex(usbfs_readw(qts, USBFS_BTABLE), ==, 0x0080);
    g_assert_cmphex(usbfs_readw(qts, USBFS_DADDR), ==, 0x00d5);
    g_assert_cmphex(usbfs_readw(qts, USBFS_BCDR), ==, 0x8000);
    g_assert_cmphex(usbfs_readw(qts, USBFS_EP0R), ==, 0x3220);
    g_assert_cmphex(pma_readw(qts, 0x80), ==, 0x0122);
    g_assert_cmphex(pma_readw(qts, 0x86), ==, 0x8400);
    g_assert_cmphex(pma_readw(qts, 0x100), ==, 0xa55a);
    assert_clock_hz(qts, USBFS_PCLK_QOM_PATH, 16000000);
    assert_clock_hz(qts, USBFS_CLOCK_QOM_PATH, 170000000);

    pma_writew(qts, 0x80, UINT16_MAX);
    g_assert_cmphex(pma_readw(qts, 0x80), ==, 0xfffe);
}

static void test_active_migration(void)
{
    QTestState *src;
    QTestState *dst;
    uint64_t raises;
    g_autofree char *tmpdir =
        g_dir_make_tmp("stm32g474-usbfs-migration-XXXXXX", NULL);
    g_autofree char *sock = g_strdup_printf("%s/migration.sock", tmpdir);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);

    g_assert_nonnull(tmpdir);
    src = stm32g474_qtest_start();
    dst = stm32g474_qtest_start_incoming();
    qtest_irq_intercept_out_named(dst, USBFS_QOM_PATH, "sysbus-irq");
    raises = qtest_get_irq_raise_count(dst, 1);

    configure_migration_source(src);
    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    qtest_qmp_eventwait(dst, "RESUME");

    assert_migrated_state(dst);
    g_assert_cmpuint(qtest_get_irq_raise_count(dst, 0), ==, 0);
    g_assert_cmpuint(qtest_get_irq_raise_count(dst, 1), ==, raises);
    g_assert_false(qtest_get_irq(dst, 0));
    g_assert_false(qtest_get_irq(dst, 1));

    usbfs_writew(dst, USBFS_DADDR, 0x0055);
    usbfs_set_ctr(dst, 1, USBFS_EP_CTR_RX);
    g_assert_cmphex(usbfs_readw(dst, USBFS_DADDR), ==, 0x00d5);
    g_assert_cmphex(usbfs_readw(dst, USBFS_EP1R), ==, 0);
    set_usb_clock_valid(dst);
    g_assert_cmphex(usbfs_readw(dst, USBFS_ISTR) &
                    USBFS_ISTR_RESET, ==, USBFS_ISTR_RESET);
    g_assert_cmphex(usbfs_readw(dst, USBFS_DADDR), ==, 0);
    g_assert_cmphex(usbfs_readw(dst, USBFS_EP0R), ==, 0);
    g_assert_cmpuint(qtest_get_irq_raise_count(dst, 1), ==, raises + 1);
    g_assert_true(qtest_get_irq(dst, 1));

    usbfs_writew(dst, USBFS_ISTR, 0);
    g_assert_true(qtest_get_irq(dst, 1));
    usbfs_writew(dst, USBFS_CNTR, USBFS_CNTR_RESETM);
    usbfs_writew(dst, USBFS_ISTR, 0);
    g_assert_false(qtest_get_irq(dst, 1));
    g_assert_cmpuint(qtest_get_irq_raise_count(dst, 1), ==, raises + 1);

    qtest_quit(src);
    qtest_quit(dst);
    g_unlink(sock);
    g_rmdir(tmpdir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/stm32g474/usbfs/1-constructs-clocks-reset",
                   test_constructs_clocks_and_reset);
    qtest_add_func("/stm32g474/usbfs/2-fixed-access-masks",
                   test_fixed_register_access_and_masks);
    qtest_add_func("/stm32g474/usbfs/3-endpoint-toggle-rc-w0",
                   test_endpoint_toggle_and_rc_w0);
    qtest_add_func("/stm32g474/usbfs/4-pma-bdt-relocation-bounds",
                   test_pma_lanes_bdt_relocation_and_bounds);
    qtest_add_func("/stm32g474/usbfs/5-fres-clock-irq-levels",
                   test_fres_clock_and_irq_levels);
    qtest_add_func("/stm32g474/usbfs/6-nvic-routing-clock-gate",
                   test_nvic_routing_and_clock_gate);
    qtest_add_func("/stm32g474/usbfs/7-endpoint-irq-priority",
                   test_endpoint_irq_priority);
    qtest_add_func("/stm32g474/usbfs/8-rcc-held-reset",
                   test_rcc_held_reset);
    qtest_add_func("/stm32g474/usbfs/9-system-reset",
                   test_system_reset);
    qtest_add_func("/stm32g474/usbfs/10-hostless-hal-sequence",
                   test_hostless_hal_sequence);
    qtest_add_func("/stm32g474/usbfs/11-active-migration",
                   test_active_migration);

    return g_test_run();
}
