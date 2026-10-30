/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register tests for the Rockchip RK3566 CRU and PMUCRU banks.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "rk3566-test-common.h"

#define RK3566_CRU_BASE          0xfdd20000ULL
#define RK3566_PMUCRU_BASE       0xfdd00000ULL

#define RK3566_CRU_PLL0_CON0     0x000
#define RK3566_CRU_PLL0_CON1     0x004
#define RK3566_CRU_PLL0_CON2     0x008
#define RK3566_CRU_GPLL_CON0     0x040
#define RK3566_CRU_GPLL_CON1     0x044
#define RK3566_CRU_GPLL_CON2     0x048
#define RK3566_CRU_CPLL_CON1     0x064
#define RK3566_CRU_NPLL_CON0     0x080
#define RK3566_CRU_MODE          0x0c0
#define RK3566_CRU_PMU_MODE      0x080
#define RK3566_CRU_CLKSEL(n)     (0x100 + 4 * (n))
#define RK3566_CRU_CLKGATE(n)    (0x300 + 4 * (n))
#define RK3566_CRU_SOFTRST(n)    (0x400 + 4 * (n))

/* GPLL runs at 1.2 GHz and CLKSEL_CON76 divides it by 6. */
#define RK3566_CRU_SPI3_PERIOD  ((1000000000ULL << 32) / 200000000ULL)

static uint32_t cru_read(QTestState *qts, uint64_t base, uint32_t offset)
{
    return rk3566_mmio_read32_at(qts, base, offset);
}

static void cru_write(QTestState *qts, uint64_t base, uint32_t offset,
                      uint32_t value)
{
    rk3566_mmio_write32_at(qts, base, offset, value);
}

static void test_rk3566_cru_reset_values(void)
{
    QTestState *qts = rk3566_qtest_start();

    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_PLL0_CON0), ==, 0x102e);
    /* CON1 reports the derived LOCK bit 10 next to its reset value. */
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_PLL0_CON1), ==, 0x1441);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_PLL0_CON2), ==, 0);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_GPLL_CON0), ==, 0x1032);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_GPLL_CON1), ==, 0x1441);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_GPLL_CON2), ==, 0);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_CPLL_CON1), ==, 0x1443);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_NPLL_CON0), ==, 0x1032);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE, RK3566_CRU_MODE),
                    ==, 0x1455);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_CLKSEL(75)), ==, 0x0302);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_CLKSEL(82)), ==, 0x1f09);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_CLKGATE(35)), ==, 0);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_SOFTRST(0)), ==, 0);

    qtest_quit(qts);
}

static void test_rk3566_cru_masked_write(void)
{
    QTestState *qts = rk3566_qtest_start();

    /*
     * The upper 16 bits of a write select the bits that are updated.  Bit 10
     * of CLKGATE_CON6 gates the I2S1 transmit clock.
     */
    cru_write(qts, RK3566_CRU_BASE, RK3566_CRU_CLKGATE(6),
              (1u << 26) | (1u << 10));
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_CLKGATE(6)), ==, 1u << 10);
    cru_write(qts, RK3566_CRU_BASE, RK3566_CRU_CLKGATE(6), 1u << 26);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_CLKGATE(6)), ==, 0);
    /* Bits outside the write mask keep the stored value. */
    cru_write(qts, RK3566_CRU_BASE, RK3566_CRU_CLKGATE(6),
              (0x3u << 16) | 0x3u);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_CLKGATE(6)), ==, 0x3);
    cru_write(qts, RK3566_CRU_BASE, RK3566_CRU_CLKGATE(6), 1u << 16);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_CLKGATE(6)), ==, 0x2);

    qtest_quit(qts);
}

static void test_rk3566_cru_fractional_write(void)
{
    QTestState *qts = rk3566_qtest_start();

    /*
     * Fractional divider words are stored in full, while the selector word
     * keeps the masked write.  Together they produce the 12.288 MHz I2S1
     * master clock from GPLL 1.2 GHz divided by 4 and 128/3125.
     */
    cru_write(qts, RK3566_CRU_BASE, RK3566_CRU_CLKSEL(15),
              (0xc7fu << 16) | 0x403u);
    cru_write(qts, RK3566_CRU_BASE, RK3566_CRU_CLKSEL(16), 0x00800c35u);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_CLKSEL(15)), ==, 0x403);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_CLKSEL(16)), ==, 0x00800c35);
    g_assert_cmpuint(rk3566_qom_get_uint64(qts, "/machine/cru/mclk-i2s1",
                                           "qtest-clock-period"), ==,
                     (1000000000ULL << 32) / 12288000ULL);

    qtest_quit(qts);
}

static void test_rk3566_cru_pll_lock(void)
{
    QTestState *qts = rk3566_qtest_start();

    /* The reset configuration is locked and reports LOCK. */
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_GPLL_CON1), ==, 0x1441);
    /* Clearing the reference divider makes the PLL unconfigured. */
    cru_write(qts, RK3566_CRU_BASE, RK3566_CRU_GPLL_CON1, 0x003f0000u);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_GPLL_CON1), ==, 0x1040);
    /* Restoring the divider brings LOCK back. */
    cru_write(qts, RK3566_CRU_BASE, RK3566_CRU_GPLL_CON1, 0x003f0041u);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_GPLL_CON1), ==, 0x1441);
    /* Bit 13 disables the PLL and clears the derived LOCK bit. */
    cru_write(qts, RK3566_CRU_BASE, RK3566_CRU_GPLL_CON1,
              (1u << 13) << 16 | (1u << 13));
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_GPLL_CON1), ==, 0x3041);

    qtest_quit(qts);
}

static void test_rk3566_cru_clock_gate(void)
{
    QTestState *qts = rk3566_qtest_start();

    g_assert_cmpuint(rk3566_qom_get_uint64(qts, "/machine/cru/sclk-spi3",
                                           "qtest-clock-period"), ==,
                     RK3566_CRU_SPI3_PERIOD);
    /* Bit 1 of CLKGATE_CON31 gates SCLK_SPI3. */
    cru_write(qts, RK3566_CRU_BASE, RK3566_CRU_CLKGATE(31),
              ((1u << 1) << 16) | (1u << 1));
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_CLKGATE(31)), ==, 0x2);
    g_assert_cmpuint(rk3566_qom_get_uint64(qts, "/machine/cru/sclk-spi3",
                                           "qtest-clock-period"), ==, 0);
    cru_write(qts, RK3566_CRU_BASE, RK3566_CRU_CLKGATE(31), 1u << 17);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE,
                             RK3566_CRU_CLKGATE(31)), ==, 0);
    g_assert_cmpuint(rk3566_qom_get_uint64(qts, "/machine/cru/sclk-spi3",
                                           "qtest-clock-period"), ==,
                     RK3566_CRU_SPI3_PERIOD);

    qtest_quit(qts);
}

static void test_rk3566_cru_pmu_variant(void)
{
    QTestState *qts = rk3566_qtest_start();

    g_assert_cmphex(cru_read(qts, RK3566_PMUCRU_BASE, 0x000), ==, 0x1032);
    g_assert_cmphex(cru_read(qts, RK3566_PMUCRU_BASE, 0x004), ==, 0x1441);
    g_assert_cmphex(cru_read(qts, RK3566_PMUCRU_BASE, 0x044), ==, 0x1441);
    g_assert_cmphex(cru_read(qts, RK3566_PMUCRU_BASE, RK3566_CRU_PMU_MODE),
                    ==, 0x5);

    /* CLKSEL_CON1 is a fractional word on the PMUCRU variant. */
    cru_write(qts, RK3566_PMUCRU_BASE, RK3566_CRU_CLKSEL(1), 0x00800c35u);
    g_assert_cmphex(cru_read(qts, RK3566_PMUCRU_BASE,
                             RK3566_CRU_CLKSEL(1)), ==, 0x00800c35);
    /* CLKSEL_CON0 keeps the masked write. */
    cru_write(qts, RK3566_PMUCRU_BASE, RK3566_CRU_CLKSEL(0),
              (0x1u << 16) | 0x1u);
    g_assert_cmphex(cru_read(qts, RK3566_PMUCRU_BASE,
                             RK3566_CRU_CLKSEL(0)), ==, 0x1);
    cru_write(qts, RK3566_PMUCRU_BASE, RK3566_CRU_CLKSEL(0), 1u << 16);
    g_assert_cmphex(cru_read(qts, RK3566_PMUCRU_BASE,
                             RK3566_CRU_CLKSEL(0)), ==, 0);

    qtest_quit(qts);
}

static void test_rk3566_cru_fallback_registers(void)
{
    QTestState *qts = rk3566_qtest_start();

    /*
     * Addresses outside the register table keep the legacy masked storage:
     * the high half selects the written bits and reads return the array.
     */
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE, 0xc04), ==, 0);
    cru_write(qts, RK3566_CRU_BASE, 0xc04, (0x5u << 16) | 0x5u);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE, 0xc04), ==, 0x5);
    cru_write(qts, RK3566_CRU_BASE, 0xc04, 0xffff0000u);
    g_assert_cmphex(cru_read(qts, RK3566_CRU_BASE, 0xc04), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566/cru/reset-values", test_rk3566_cru_reset_values);
    qtest_add_func("/rk3566/cru/masked-write", test_rk3566_cru_masked_write);
    qtest_add_func("/rk3566/cru/fractional-write",
                   test_rk3566_cru_fractional_write);
    qtest_add_func("/rk3566/cru/pll-lock", test_rk3566_cru_pll_lock);
    qtest_add_func("/rk3566/cru/clock-gate", test_rk3566_cru_clock_gate);
    qtest_add_func("/rk3566/cru/pmu-variant", test_rk3566_cru_pmu_variant);
    qtest_add_func("/rk3566/cru/fallback-registers",
                   test_rk3566_cru_fallback_registers);

    return g_test_run();
}
