/*
 * QTest for the STM32G474 flash interface
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "qobject/qdict.h"

#define STM32G474_MACHINE "stm32g474"

#define FLASH_BASE 0x40022000ULL
#define MAIN_FLASH_BASE 0x08000000ULL
#define MAIN_FLASH_SIZE (512 * KiB)
#define FLASH_SIZE_BASE 0x1fff75e0ULL

#define RCC_BASE 0x40021000ULL
#define PWR_BASE 0x40007000ULL

#define FLASH_CLOCK_QOM_PATH "/machine/mcu/flash/clk"
#define RCC_SYSCLK_QOM_PATH "/machine/mcu/rcc/sysclk"

#define FLASH_ACR 0x00
#define FLASH_PDKEYR 0x04
#define FLASH_KEYR 0x08
#define FLASH_OPTKEYR 0x0c
#define FLASH_SR 0x10
#define FLASH_CR 0x14
#define FLASH_OPTR 0x20

#define FLASH_ACR_RESET 0x00040601U
#define FLASH_SR_RESET 0x00000000U
#define FLASH_CR_RESET 0xc0000000U
#define FLASH_OPTR_RESET 0x00400000U
#define FLASH_SIZE_WORD 0x00000200U

#define FLASH_ACR_LATENCY_MASK 0x0000000fU
#define FLASH_ACR_LATENCY_4 0x00000004U
#define FLASH_ACR_PRFTEN (1U << 8)
#define FLASH_ACR_ICEN (1U << 9)
#define FLASH_ACR_DCEN (1U << 10)
#define FLASH_ACR_ICRST (1U << 11)
#define FLASH_ACR_DCRST (1U << 12)
#define FLASH_ACR_ALL_ONES_RUN_PD_PROTECTED_RESULT 0x0004470fU
#define FLASH_ACR_ICACHE_DISABLED 0x00040401U
#define FLASH_ACR_ICRST_RESULT 0x00040c01U
#define FLASH_ACR_ICRST_CACHE_REENABLE_RESULT 0x00040e01U
#define FLASH_ACR_DCACHE_DISABLED 0x00040201U
#define FLASH_ACR_DCRST_RESULT 0x00041201U
#define FLASH_ACR_DCRST_CACHE_REENABLE_RESULT 0x00041601U
#define FLASH_ACR_STARTUP_CACHE_RESULT 0x00040701U
#define FLASH_ACR_STARTUP_RESULT 0x00040704U

#define FLASH_SR_GUEST_STATUS_MASK 0x0000c3fbU
#define FLASH_CR_IRQ_ENABLE_MASK 0x07000000U
#define FLASH_OPTR_DBANK (1U << 22)

#define RCC_CR 0x00
#define RCC_CFGR 0x08
#define RCC_PLLCFGR 0x0c
#define RCC_AHB1RSTR 0x28
#define RCC_AHB1ENR 0x48
#define RCC_APB1ENR1 0x58

#define RCC_CR_PLLON (1U << 24)
#define RCC_CR_PLLRDY (1U << 25)
#define RCC_CFGR_SW_MASK (3U << 0)
#define RCC_CFGR_SW_PLL (3U << 0)
#define RCC_CFGR_SWS_MASK (3U << 2)
#define RCC_CFGR_SWS_PLL (3U << 2)
#define RCC_PLLCFGR_HSI16_170MHZ 0x01105532U
#define RCC_AHB1RSTR_FLASHRST (1U << 8)
#define RCC_AHB1ENR_FLASHEN (1U << 8)
#define RCC_APB1ENR1_PWREN (1U << 28)

#define PWR_CR1 0x00
#define PWR_CR3 0x08
#define PWR_CR5 0x80
#define PWR_CR1_VOS_MASK (3U << 9)
#define PWR_CR1_VOS_RANGE1 (1U << 9)
#define PWR_CR3_UCPD_DBDIS (1U << 14)
#define PWR_CR3_STARTUP_RESULT 0x0000c000U
#define PWR_CR5_R1MODE (1U << 8)

#define NVIC_ISER0 0xe000e100ULL
#define NVIC_ISPR0 0xe000e200ULL
#define NVIC_ICPR0 0xe000e280ULL
#define FLASH_IRQ_BIT (1U << 4)

#define CLOCK_PERIOD_1SEC (1000000000ULL << 32)
#define LOCKED_WRITE_VALUE 0x12345678U

#define TEST_INITIAL_MSP 0x20020000U
#define TEST_RESET_VECTOR 0x08000101U
#define TEST_FLASH_MARKER 0x5aa55aa5U
#define TEST_FLASH_MARKER_OFFSET 0x08
#define TEST_LOOP_OFFSET 0x100
#define TEST_IMAGE_SIZE 0x102

static QTestState *stm32g474_qtest_start(void)
{
    return qtest_init("-machine " STM32G474_MACHINE " -serial null");
}

static void cleanup_temp_file(void *path)
{
    qtest_remove_abrt_handler(path);
    g_unlink(path);
    g_free(path);
}

static QTestState *stm32g474_qtest_start_with_flash_marker(void)
{
    g_autoptr(GError) error = NULL;
    uint8_t image[TEST_IMAGE_SIZE] = {};
    char *image_path = g_strdup("stm32g474-flash-reset-XXXXXX");
    int fd;

    stl_le_p(image, TEST_INITIAL_MSP);
    stl_le_p(image + 4, TEST_RESET_VECTOR);
    stl_le_p(image + TEST_FLASH_MARKER_OFFSET, TEST_FLASH_MARKER);
    stw_le_p(image + TEST_LOOP_OFFSET, 0xe7fe);

    fd = g_mkstemp(image_path);
    g_assert_cmpint(fd, >=, 0);
    qtest_add_abrt_handler(cleanup_temp_file, image_path);
    g_test_queue_destroy(cleanup_temp_file, image_path);
    g_assert_cmpint(close(fd), ==, 0);
    g_assert_true(g_file_set_contents(image_path, (const char *)image,
                                      sizeof(image), &error));
    g_assert_no_error(error);

    return qtest_initf("-machine " STM32G474_MACHINE
                       " -serial null -kernel %s", image_path);
}

static uint32_t flash_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, FLASH_BASE + offset);
}

static void flash_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, FLASH_BASE + offset, value);
}

static uint32_t rcc_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, RCC_BASE + offset);
}

static void rcc_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, RCC_BASE + offset, value);
}

static uint32_t pwr_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, PWR_BASE + offset);
}

static void pwr_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, PWR_BASE + offset, value);
}

static uint64_t clock_period_from_hz(uint64_t hz)
{
    return hz ? CLOCK_PERIOD_1SEC / hz : 0;
}

static uint64_t get_clock_period(QTestState *qts, const char *path)
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
    g_assert_cmphex(get_clock_period(qts, path), ==,
                    clock_period_from_hz(hz));
}

static void set_flash_clock_gate(QTestState *qts, bool enabled)
{
    uint32_t value = rcc_readl(qts, RCC_AHB1ENR);

    if (enabled) {
        value |= RCC_AHB1ENR_FLASHEN;
    } else {
        value &= ~RCC_AHB1ENR_FLASHEN;
    }
    rcc_writel(qts, RCC_AHB1ENR, value);
}

static void assert_flash_reset_image(QTestState *qts)
{
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==, FLASH_ACR_RESET);
    g_assert_cmphex(flash_readl(qts, FLASH_PDKEYR), ==, 0);
    g_assert_cmphex(flash_readl(qts, FLASH_KEYR), ==, 0);
    g_assert_cmphex(flash_readl(qts, FLASH_OPTKEYR), ==, 0);
    g_assert_cmphex(flash_readl(qts, FLASH_SR), ==, FLASH_SR_RESET);
    g_assert_cmphex(flash_readl(qts, FLASH_CR), ==, FLASH_CR_RESET);
    g_assert_cmphex(flash_readl(qts, FLASH_OPTR), ==, FLASH_OPTR_RESET);
}

static void assert_erased_flash_alias(QTestState *qts)
{
    static const uint32_t offsets[] = {
        0,
        MAIN_FLASH_SIZE - sizeof(uint32_t),
    };

    for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
        g_assert_cmphex(qtest_readl(qts, MAIN_FLASH_BASE + offsets[i]),
                        ==, UINT32_MAX);
        g_assert_cmphex(qtest_readl(qts, offsets[i]), ==, UINT32_MAX);
        g_assert_cmphex(qtest_readl(qts, MAIN_FLASH_BASE + offsets[i]),
                        ==, qtest_readl(qts, offsets[i]));
    }
}

static void assert_erased_access_probe(QTestState *qts)
{
    static const uint32_t offsets[] = {
        0,
        sizeof(uint32_t),
    };

    for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
        g_assert_cmphex(qtest_readl(qts, MAIN_FLASH_BASE + offsets[i]),
                        ==, UINT32_MAX);
        g_assert_cmphex(qtest_readl(qts, offsets[i]), ==, UINT32_MAX);
    }
}

static void assert_flash_reset_marker(QTestState *qts)
{
    g_assert_cmphex(qtest_readl(qts, MAIN_FLASH_BASE +
                                TEST_FLASH_MARKER_OFFSET), ==,
                    TEST_FLASH_MARKER);
    g_assert_cmphex(qtest_readl(qts, TEST_FLASH_MARKER_OFFSET), ==,
                    TEST_FLASH_MARKER);
}

static bool flash_irq_pending(QTestState *qts)
{
    return (qtest_readl(qts, NVIC_ISPR0) & FLASH_IRQ_BIT) != 0;
}

static void test_flash_mmio_reset_mask(void)
{
    QTestState *qts = stm32g474_qtest_start();
    uint32_t acr;

    acr = flash_readl(qts, FLASH_ACR);
    g_test_message("FLASH_ACR expected 0x%08x, observed 0x%08x",
                   FLASH_ACR_RESET, acr);
    g_assert_cmphex(acr, ==, FLASH_ACR_RESET);
    assert_flash_reset_image(qts);

    qtest_writeb(qts, FLASH_BASE + FLASH_ACR, 0);
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==, FLASH_ACR_RESET);
    qtest_writew(qts, FLASH_BASE + FLASH_ACR, 0);
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==, FLASH_ACR_RESET);
    qtest_writel(qts, FLASH_BASE + FLASH_ACR + 1, 0);
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==, FLASH_ACR_RESET);

    flash_writel(qts, FLASH_PDKEYR, UINT32_MAX);
    flash_writel(qts, FLASH_KEYR, UINT32_MAX);
    flash_writel(qts, FLASH_OPTKEYR, UINT32_MAX);
    g_assert_cmphex(flash_readl(qts, FLASH_PDKEYR), ==, 0);
    g_assert_cmphex(flash_readl(qts, FLASH_KEYR), ==, 0);
    g_assert_cmphex(flash_readl(qts, FLASH_OPTKEYR), ==, 0);

    flash_writel(qts, FLASH_ACR, UINT32_MAX);
    /* RUN_PD retains reset state without its PDKEYR sequence. */
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==,
                    FLASH_ACR_ALL_ONES_RUN_PD_PROTECTED_RESULT);

    flash_writel(qts, FLASH_ACR, FLASH_ACR_RESET & ~FLASH_ACR_ICEN);
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==,
                    FLASH_ACR_ICACHE_DISABLED);
    flash_writel(qts, FLASH_ACR,
                 FLASH_ACR_ICACHE_DISABLED | FLASH_ACR_ICRST);
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==,
                    FLASH_ACR_ICRST_RESULT);
    flash_writel(qts, FLASH_ACR,
                 FLASH_ACR_ICRST_RESULT | FLASH_ACR_ICEN);
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==,
                    FLASH_ACR_ICRST_CACHE_REENABLE_RESULT);

    flash_writel(qts, FLASH_ACR, FLASH_ACR_RESET & ~FLASH_ACR_DCEN);
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==,
                    FLASH_ACR_DCACHE_DISABLED);
    flash_writel(qts, FLASH_ACR,
                 FLASH_ACR_DCACHE_DISABLED | FLASH_ACR_DCRST);
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==,
                    FLASH_ACR_DCRST_RESULT);
    flash_writel(qts, FLASH_ACR,
                 FLASH_ACR_DCRST_RESULT | FLASH_ACR_DCEN);
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==,
                    FLASH_ACR_DCRST_CACHE_REENABLE_RESULT);

    flash_writel(qts, FLASH_SR, UINT32_MAX);
    g_assert_cmphex(flash_readl(qts, FLASH_SR), ==, FLASH_SR_RESET);

    flash_writel(qts, FLASH_CR, 0);
    g_assert_cmphex(flash_readl(qts, FLASH_CR), ==, FLASH_CR_RESET);
    flash_writel(qts, FLASH_CR, UINT32_MAX);
    g_assert_cmphex(flash_readl(qts, FLASH_CR), ==, FLASH_CR_RESET);

    flash_writel(qts, FLASH_OPTR, UINT32_MAX);
    g_assert_cmphex(flash_readl(qts, FLASH_OPTR), ==, FLASH_OPTR_RESET);

    for (unsigned int reset = 0; reset < 2; reset++) {
        flash_writel(qts, FLASH_ACR, FLASH_ACR_LATENCY_4);
        g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==,
                        FLASH_ACR_LATENCY_4);
        qtest_system_reset(qts);
        assert_flash_reset_image(qts);
    }

    qtest_quit(qts);
}

static void test_flash_ardep_startup(void)
{
    QTestState *qts = stm32g474_qtest_start();
    uint32_t value;

    value = flash_readl(qts, FLASH_ACR);
    value |= FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_PRFTEN;
    flash_writel(qts, FLASH_ACR, value);
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==,
                    FLASH_ACR_STARTUP_CACHE_RESULT);

    value = rcc_readl(qts, RCC_APB1ENR1) | RCC_APB1ENR1_PWREN;
    rcc_writel(qts, RCC_APB1ENR1, value);

    value = pwr_readl(qts, PWR_CR3) | PWR_CR3_UCPD_DBDIS;
    pwr_writel(qts, PWR_CR3, value);
    g_assert_cmphex(pwr_readl(qts, PWR_CR3), ==,
                    PWR_CR3_STARTUP_RESULT);
    g_assert_cmphex(pwr_readl(qts, PWR_CR1) & PWR_CR1_VOS_MASK, ==,
                    PWR_CR1_VOS_RANGE1);
    g_assert_cmphex(pwr_readl(qts, PWR_CR5) & PWR_CR5_R1MODE, ==,
                    PWR_CR5_R1MODE);

    value = flash_readl(qts, FLASH_ACR) & ~FLASH_ACR_LATENCY_MASK;
    flash_writel(qts, FLASH_ACR, value | FLASH_ACR_LATENCY_4);
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==,
                    FLASH_ACR_STARTUP_RESULT);

    value = pwr_readl(qts, PWR_CR5) & ~PWR_CR5_R1MODE;
    pwr_writel(qts, PWR_CR5, value);
    g_assert_cmphex(pwr_readl(qts, PWR_CR5), ==, 0);

    rcc_writel(qts, RCC_PLLCFGR, RCC_PLLCFGR_HSI16_170MHZ);
    rcc_writel(qts, RCC_CR, rcc_readl(qts, RCC_CR) | RCC_CR_PLLON);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) &
                    (RCC_CR_PLLON | RCC_CR_PLLRDY), ==,
                    RCC_CR_PLLON | RCC_CR_PLLRDY);

    value = rcc_readl(qts, RCC_CFGR) & ~RCC_CFGR_SW_MASK;
    rcc_writel(qts, RCC_CFGR, value | RCC_CFGR_SW_PLL);
    g_assert_cmphex(rcc_readl(qts, RCC_CFGR) &
                    (RCC_CFGR_SW_MASK | RCC_CFGR_SWS_MASK), ==,
                    RCC_CFGR_SW_PLL | RCC_CFGR_SWS_PLL);
    assert_clock_hz(qts, RCC_SYSCLK_QOM_PATH, 170000000);
    assert_clock_hz(qts, FLASH_CLOCK_QOM_PATH, 170000000);

    g_assert_cmphex(flash_readl(qts, FLASH_OPTR) & FLASH_OPTR_DBANK, ==,
                    FLASH_OPTR_DBANK);
    g_assert_cmphex(qtest_readl(qts, FLASH_SIZE_BASE), ==,
                    FLASH_SIZE_WORD);

    qtest_quit(qts);
}

static void test_flash_storage_alias_reset(void)
{
    QTestState *qts = stm32g474_qtest_start();

    assert_erased_flash_alias(qts);

    qtest_writeb(qts, MAIN_FLASH_BASE, 0);
    assert_erased_access_probe(qts);
    qtest_writew(qts, MAIN_FLASH_BASE, 0);
    assert_erased_access_probe(qts);
    qtest_writel(qts, MAIN_FLASH_BASE + 1, 0);
    assert_erased_access_probe(qts);

    qtest_writel(qts, MAIN_FLASH_BASE, LOCKED_WRITE_VALUE);
    assert_erased_flash_alias(qts);

    /*
     * This has no host firmware, so it does not prove that guest changes to
     * host-loader-covered bytes survive a system reset.
     */
    for (unsigned int reset = 0; reset < 2; reset++) {
        qtest_system_reset(qts);
        assert_erased_flash_alias(qts);
    }

    qtest_quit(qts);
}

static void test_flash_clock_reset_irq(void)
{
    QTestState *qts = stm32g474_qtest_start_with_flash_marker();
    uint32_t value;

    assert_flash_reset_marker(qts);
    assert_clock_hz(qts, FLASH_CLOCK_QOM_PATH, 16000000);
    set_flash_clock_gate(qts, false);
    assert_clock_hz(qts, FLASH_CLOCK_QOM_PATH, 0);
    set_flash_clock_gate(qts, true);
    assert_clock_hz(qts, FLASH_CLOCK_QOM_PATH, 16000000);

    flash_writel(qts, FLASH_ACR, FLASH_ACR_STARTUP_RESULT);
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==,
                    FLASH_ACR_STARTUP_RESULT);
    assert_flash_reset_marker(qts);

    /* This proves reset wiring; hardware additionally requires RUN_PD. */
    value = rcc_readl(qts, RCC_AHB1RSTR) | RCC_AHB1RSTR_FLASHRST;
    rcc_writel(qts, RCC_AHB1RSTR, value);
    assert_flash_reset_image(qts);
    g_assert_cmphex(rcc_readl(qts, RCC_AHB1RSTR) &
                    RCC_AHB1RSTR_FLASHRST, ==, RCC_AHB1RSTR_FLASHRST);
    assert_clock_hz(qts, FLASH_CLOCK_QOM_PATH, 16000000);

    assert_flash_reset_marker(qts);
    g_assert_cmphex(qtest_readl(qts, FLASH_SIZE_BASE), ==,
                    FLASH_SIZE_WORD);
    g_assert_cmphex(flash_readl(qts, FLASH_OPTR), ==, FLASH_OPTR_RESET);

    flash_writel(qts, FLASH_ACR, FLASH_ACR_STARTUP_RESULT);
    flash_writel(qts, FLASH_SR, FLASH_SR_GUEST_STATUS_MASK);
    flash_writel(qts, FLASH_CR,
                 FLASH_CR_RESET | FLASH_CR_IRQ_ENABLE_MASK);
    assert_flash_reset_image(qts);

    set_flash_clock_gate(qts, false);
    assert_clock_hz(qts, FLASH_CLOCK_QOM_PATH, 0);
    g_assert_cmphex(rcc_readl(qts, RCC_AHB1RSTR) &
                    RCC_AHB1RSTR_FLASHRST, ==, RCC_AHB1RSTR_FLASHRST);
    flash_writel(qts, FLASH_ACR, FLASH_ACR_LATENCY_4);
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==, FLASH_ACR_RESET);
    set_flash_clock_gate(qts, true);

    value = rcc_readl(qts, RCC_AHB1RSTR) & ~RCC_AHB1RSTR_FLASHRST;
    rcc_writel(qts, RCC_AHB1RSTR, value);
    flash_writel(qts, FLASH_ACR, FLASH_ACR_STARTUP_RESULT);
    g_assert_cmphex(flash_readl(qts, FLASH_ACR), ==,
                    FLASH_ACR_STARTUP_RESULT);
    assert_clock_hz(qts, FLASH_CLOCK_QOM_PATH, 16000000);

    qtest_writel(qts, NVIC_ISER0, FLASH_IRQ_BIT);
    qtest_writel(qts, NVIC_ICPR0, FLASH_IRQ_BIT);
    g_assert_false(flash_irq_pending(qts));

    flash_writel(qts, FLASH_CR,
                 FLASH_CR_RESET | FLASH_CR_IRQ_ENABLE_MASK);
    flash_writel(qts, FLASH_SR, FLASH_SR_GUEST_STATUS_MASK);
    g_assert_cmphex(flash_readl(qts, FLASH_CR), ==, FLASH_CR_RESET);
    g_assert_cmphex(flash_readl(qts, FLASH_SR), ==, FLASH_SR_RESET);
    g_assert_false(flash_irq_pending(qts));

    assert_flash_reset_marker(qts);
    g_assert_cmphex(qtest_readl(qts, FLASH_SIZE_BASE), ==,
                    FLASH_SIZE_WORD);
    g_assert_cmphex(flash_readl(qts, FLASH_OPTR), ==, FLASH_OPTR_RESET);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/stm32g474/flash/1-mmio-reset-mask",
                   test_flash_mmio_reset_mask);
    qtest_add_func("/stm32g474/flash/2-ardep-startup",
                   test_flash_ardep_startup);
    qtest_add_func("/stm32g474/flash/3-storage-alias-reset",
                   test_flash_storage_alias_reset);
    qtest_add_func("/stm32g474/flash/4-clock-reset-irq",
                   test_flash_clock_reset_irq);

    return g_test_run();
}
