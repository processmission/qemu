/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register tests for the Rockchip RK3566 system controller variants.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "rk3566-test-common.h"

#define RK3566_SYSTEM_PMU_BASE    0xfdd90000
#define RK3566_SYSTEM_RNG_BASE    0xfe388000
#define RK3566_SYSTEM_PVTM_BASE   0xfde00000

/* PMU shim registers that are mirrored on write. */
#define RK3566_SYSTEM_PMU_MIRROR0    0x50
#define RK3566_SYSTEM_PMU_MIRROR0_LO 0x60
#define RK3566_SYSTEM_PMU_MIRROR0_HI 0x68
#define RK3566_SYSTEM_PMU_MIRROR1    0xa0
#define RK3566_SYSTEM_PMU_MIRROR1_LO 0x98

/* RNG control block. */
#define RK3566_SYSTEM_RNG_CTRL       0x400
#define RK3566_SYSTEM_RNG_DATA       0x410

/* PVTM control block. */
#define RK3566_SYSTEM_PVTM_CTRL      0x04
#define RK3566_SYSTEM_PVTM_PERIOD    0x08
#define RK3566_SYSTEM_PVTM_STATUS    0x80
#define RK3566_SYSTEM_PVTM_RESULT    0x84

/* The PVTM model divides the oscillator by a 24 MHz reference clock. */
#define RK3566_SYSTEM_PVTM_CLOCK_HZ      24000000
#define RK3566_SYSTEM_PVTM_OSCILLATOR_HZ 84000000

/* Virtual time for one period of the reference clock. */
#define RK3566_SYSTEM_PVTM_PERIOD_NS     1000000000LL

/* An unused word of the window, to check the fallback write path. */
#define RK3566_SYSTEM_SPARE          0x100

static void test_rk3566_system_variants(void)
{
    QTestState *qts = rk3566_qtest_start();
    g_autofree char *pmu_type =
        rk3566_qom_child_object_type(qts, "/machine", "pmu");
    g_autofree char *rng_type =
        rk3566_qom_child_object_type(qts, "/machine", "rng");
    g_autofree char *pvtm_type =
        rk3566_qom_child_object_type(qts, "/machine", "pvtm0");

    g_assert_cmpstr(pmu_type, ==, "rk3566-system");
    g_assert_cmpstr(rng_type, ==, "rk3566-system");
    g_assert_cmpstr(pvtm_type, ==, "rk3566-system");

    qtest_quit(qts);
}

static void test_rk3566_system_pmu_window(void)
{
    QTestState *qts = rk3566_qtest_start();

    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PMU_BASE,
                                        RK3566_SYSTEM_PMU_MIRROR0), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PMU_BASE,
                                        RK3566_SYSTEM_PMU_MIRROR0_LO), ==, 0);

    /* Writes carry the update mask in the upper half-word. */
    rk3566_mmio_write32_at(qts, RK3566_SYSTEM_PMU_BASE,
                           RK3566_SYSTEM_PMU_MIRROR0, 0x000f0005);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PMU_BASE,
                                        RK3566_SYSTEM_PMU_MIRROR0), ==, 5);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PMU_BASE,
                                        RK3566_SYSTEM_PMU_MIRROR0_LO), ==, 5);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PMU_BASE,
                                        RK3566_SYSTEM_PMU_MIRROR0_HI), ==, 5);

    /* A zero mask leaves the register unchanged. */
    rk3566_mmio_write32_at(qts, RK3566_SYSTEM_PMU_BASE,
                           RK3566_SYSTEM_PMU_MIRROR0, 0x0000000a);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PMU_BASE,
                                        RK3566_SYSTEM_PMU_MIRROR0), ==, 5);

    rk3566_mmio_write32_at(qts, RK3566_SYSTEM_PMU_BASE,
                           RK3566_SYSTEM_PMU_MIRROR1, 0x000f0007);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PMU_BASE,
                                        RK3566_SYSTEM_PMU_MIRROR1), ==, 7);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PMU_BASE,
                                        RK3566_SYSTEM_PMU_MIRROR1_LO), ==, 7);

    /* Unlisted addresses keep the masked-storage fallback. */
    rk3566_mmio_write32_at(qts, RK3566_SYSTEM_PMU_BASE, RK3566_SYSTEM_SPARE,
                           0x000f0003);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PMU_BASE,
                                        RK3566_SYSTEM_SPARE), ==, 3);

    qtest_quit(qts);
}

static void test_rk3566_system_rng_window(void)
{
    QTestState *qts = rk3566_qtest_start();

    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_RNG_BASE,
                                        RK3566_SYSTEM_RNG_CTRL), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_RNG_BASE,
                                        RK3566_SYSTEM_RNG_DATA), ==, 0);

    /* The RNG variant stores unlisted addresses without a write mask. */
    rk3566_mmio_write32_at(qts, RK3566_SYSTEM_RNG_BASE, RK3566_SYSTEM_SPARE,
                           0x00000005);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_RNG_BASE,
                                        RK3566_SYSTEM_SPARE), ==, 5);

    /* The data window is plain storage until the generator is started. */
    rk3566_mmio_write32_at(qts, RK3566_SYSTEM_RNG_BASE,
                           RK3566_SYSTEM_RNG_DATA + 8, 0x12345678);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_RNG_BASE,
                                        RK3566_SYSTEM_RNG_DATA + 8), ==,
                    0x12345678);

    /* The control register takes the mask and then clears the start bit. */
    rk3566_mmio_write32_at(qts, RK3566_SYSTEM_RNG_BASE,
                           RK3566_SYSTEM_RNG_CTRL, 0x00030003);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_RNG_BASE,
                                        RK3566_SYSTEM_RNG_CTRL), ==, 2);

    qtest_quit(qts);
}

static void test_rk3566_system_pvtm_window(void)
{
    QTestState *qts = rk3566_qtest_start();

    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PVTM_BASE,
                                        RK3566_SYSTEM_PVTM_CTRL), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PVTM_BASE,
                                        RK3566_SYSTEM_PVTM_STATUS), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PVTM_BASE,
                                        RK3566_SYSTEM_PVTM_RESULT), ==, 0);

    /* The period register is a plain store. */
    rk3566_mmio_write32_at(qts, RK3566_SYSTEM_PVTM_BASE,
                           RK3566_SYSTEM_PVTM_PERIOD,
                           RK3566_SYSTEM_PVTM_CLOCK_HZ);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PVTM_BASE,
                                        RK3566_SYSTEM_PVTM_PERIOD), ==,
                    RK3566_SYSTEM_PVTM_CLOCK_HZ);

    /* The control register takes the hiword mask and starts a measurement. */
    rk3566_mmio_write32_at(qts, RK3566_SYSTEM_PVTM_BASE,
                           RK3566_SYSTEM_PVTM_CTRL, 0x00030003);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PVTM_BASE,
                                        RK3566_SYSTEM_PVTM_CTRL), ==, 3);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PVTM_BASE,
                                        RK3566_SYSTEM_PVTM_STATUS), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PVTM_BASE,
                                        RK3566_SYSTEM_PVTM_RESULT), ==, 0);

    /* Once the period elapsed the read path derives the result. */
    qtest_clock_step(qts, RK3566_SYSTEM_PVTM_PERIOD_NS + 1000);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PVTM_BASE,
                                        RK3566_SYSTEM_PVTM_STATUS), ==, 1);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PVTM_BASE,
                                        RK3566_SYSTEM_PVTM_RESULT), ==,
                    RK3566_SYSTEM_PVTM_OSCILLATOR_HZ);

    /* The derived registers are read-only. */
    rk3566_mmio_write32_at(qts, RK3566_SYSTEM_PVTM_BASE,
                           RK3566_SYSTEM_PVTM_RESULT, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SYSTEM_PVTM_BASE,
                                        RK3566_SYSTEM_PVTM_RESULT), ==,
                    RK3566_SYSTEM_PVTM_OSCILLATOR_HZ);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566/system-variants",
                   test_rk3566_system_variants);
    qtest_add_func("/rk3566/system-pmu-window",
                   test_rk3566_system_pmu_window);
    qtest_add_func("/rk3566/system-rng-window",
                   test_rk3566_system_rng_window);
    qtest_add_func("/rk3566/system-pvtm-window",
                   test_rk3566_system_pvtm_window);

    return g_test_run();
}
