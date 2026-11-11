/*
 * QEMU ROCK 5B SARADC tests
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"

#define ROCK5B_MACHINE_NAME     "rock-5b"
#define ROCK5B_GIC_DIST_BASE    0xfe600000ULL
#define ROCK5B_SARADC_BASE      0xfec10000ULL

/* SARADC interrupt, as wired in the RK3588 device tree. */
#define ROCK5B_SARADC_SPI       398

/* GIC SPI N is INTID 32+N on the distributor; GICD_ISPENDR shows the level. */
#define ROCK5B_GIC_SPI_INTID(spi)   ((spi) + 32)
#define ROCK5B_GIC_ISPENDR(intid) \
    (ROCK5B_GIC_DIST_BASE + 0x200 + 4 * ((intid) / 32))

/* Rockchip SARADC v2 register bank, see hw/adc/rk3588_saradc.c. */
enum {
    SARADC_CONV_CON   = 0x000, SARADC_T_PD_SOC   = 0x004,
    SARADC_T_DAS_SOC  = 0x00c, SARADC_END_INT_EN = 0x104,
    SARADC_ST_CON     = 0x108, SARADC_STATUS     = 0x10c,
    SARADC_END_INT_ST = 0x110, SARADC_DATA_BASE  = 0x120,
};

#define SARADC_EN_END_INT       BIT(0)
#define SARADC_START            BIT(4)
#define SARADC_SINGLE_MODE      BIT(5)
#define SARADC_STATUS_BUSY      BIT(0)
#define SARADC_CHANNEL_MASK     0xf

#define SARADC_FULL_SCALE       0xfff

/* Linux programs 32 power-up and 12 sampling clocks, at 1 MHz. */
#define SARADC_STEP_NS          100000

#define SARADC_READ(reg)        qtest_readl(qts, ROCK5B_SARADC_BASE + (reg))
#define SARADC_WRITE(reg, v)    \
    qtest_writel(qts, ROCK5B_SARADC_BASE + (reg), (v))
#define SARADC_DATA(ch)         (SARADC_DATA_BASE + (ch) * 4)

/* A write-masked register takes its enable bits from the upper half. */
#define SARADC_WRITE_MASKED(reg, v)  SARADC_WRITE(reg, (v) | ((v) << 16))

static QTestState *rock5b_saradc_start(const char *extra)
{
    g_autofree char *cmdline = g_strdup_printf(
        "-machine " ROCK5B_MACHINE_NAME
        " -m 256M -serial null -display none %s", extra ? extra : "");

    return qtest_init(cmdline);
}

static void saradc_start_conversion(QTestState *qts, unsigned int ch)
{
    /*
     * Linux enables every bit of the field it programs
     * (SARADC2_START | SARADC2_SINGLE_MODE | SARADC2_CONV_CHANNELS) in the
     * upper half, so a conversion overwrites the channel selection instead
     * of merging with whatever the previous one left behind.
     */
    uint32_t mask = SARADC_START | SARADC_SINGLE_MODE |
                    SARADC_CHANNEL_MASK;
    uint32_t val = mask & (SARADC_START | SARADC_SINGLE_MODE | ch);

    SARADC_WRITE(SARADC_CONV_CON, val | (mask << 16));
}

static uint32_t saradc_gic_mask(void)
{
    return BIT(ROCK5B_GIC_SPI_INTID(ROCK5B_SARADC_SPI) % 32);
}

static uint32_t saradc_gic_level(QTestState *qts)
{
    unsigned int intid = ROCK5B_GIC_SPI_INTID(ROCK5B_SARADC_SPI);

    return qtest_readl(qts, ROCK5B_GIC_ISPENDR(intid)) & saradc_gic_mask();
}

static void test_saradc_reset_values(void)
{
    static const uint32_t data_regs[] = {
        SARADC_DATA(0), SARADC_DATA(1), SARADC_DATA(2), SARADC_DATA(3),
        SARADC_DATA(4), SARADC_DATA(5), SARADC_DATA(6), SARADC_DATA(7),
    };
    QTestState *qts = rock5b_saradc_start(NULL);

    /*
     * A modelled block is no longer the unimplemented stub, so the data
     * registers and the status register are the ones that must behave:
     * every channel reads zero until a conversion has run.
     */
    g_assert_cmphex(SARADC_READ(SARADC_CONV_CON), ==, 0);
    g_assert_cmphex(SARADC_READ(SARADC_T_PD_SOC), ==, 0);
    g_assert_cmphex(SARADC_READ(SARADC_T_DAS_SOC), ==, 0);
    g_assert_cmphex(SARADC_READ(SARADC_END_INT_EN), ==, 0);
    g_assert_cmphex(SARADC_READ(SARADC_ST_CON), ==, 0);
    g_assert_cmphex(SARADC_READ(SARADC_STATUS), ==, 0);
    g_assert_cmphex(SARADC_READ(SARADC_END_INT_ST), ==, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(data_regs); i++) {
        g_assert_cmphex(SARADC_READ(data_regs[i]), ==, 0);
    }

    /* The delay registers keep what Linux programs into them. */
    SARADC_WRITE(SARADC_T_PD_SOC, 0x20);
    SARADC_WRITE(SARADC_T_DAS_SOC, 0xc);
    g_assert_cmphex(SARADC_READ(SARADC_T_PD_SOC), ==, 0x20);
    g_assert_cmphex(SARADC_READ(SARADC_T_DAS_SOC), ==, 0xc);

    qtest_quit(qts);
}

static void test_saradc_write_masking(void)
{
    QTestState *qts = rock5b_saradc_start(NULL);

    /*
     * The upper half of the value carries the write-enable bits.  Without
     * them nothing changes, which is what keeps Linux's "val | val << 16"
     * sequence from clobbering the neighbouring fields.
     */
    SARADC_WRITE(SARADC_CONV_CON, SARADC_SINGLE_MODE | 3);
    g_assert_cmphex(SARADC_READ(SARADC_CONV_CON), ==, 0);

    SARADC_WRITE_MASKED(SARADC_CONV_CON, SARADC_SINGLE_MODE | 3);
    g_assert_cmphex(SARADC_READ(SARADC_CONV_CON), ==,
                    SARADC_SINGLE_MODE | 3);

    /* Enabling only the upper half leaves the interrupt disabled. */
    SARADC_WRITE(SARADC_END_INT_EN, SARADC_EN_END_INT << 16);
    g_assert_cmphex(SARADC_READ(SARADC_END_INT_EN), ==, 0);

    SARADC_WRITE_MASKED(SARADC_END_INT_EN, SARADC_EN_END_INT);
    g_assert_cmphex(SARADC_READ(SARADC_END_INT_EN), ==, SARADC_EN_END_INT);

    /* A plain zero write carries no enable bits, so it changes nothing. */
    SARADC_WRITE(SARADC_END_INT_EN, 0);
    g_assert_cmphex(SARADC_READ(SARADC_END_INT_EN), ==, SARADC_EN_END_INT);

    /* Clearing it again needs the enable bit in the upper half. */
    SARADC_WRITE(SARADC_END_INT_EN, SARADC_EN_END_INT << 16);
    g_assert_cmphex(SARADC_READ(SARADC_END_INT_EN), ==, 0);

    qtest_quit(qts);
}

static void test_saradc_conversion(void)
{
    QTestState *qts = rock5b_saradc_start(
        "-global rk3588-saradc.vin4-uv=1800000 "
        "-global rk3588-saradc.vin5-uv=900000");

    SARADC_WRITE(SARADC_T_PD_SOC, 0x20);
    SARADC_WRITE(SARADC_T_DAS_SOC, 0xc);

    /*
     * Channel 4 is the 40-pin header input on pin 22, wired here to the
     * full 1.8 V reference.  While the conversion runs STATUS reports
     * BUSY and the result register still holds the previous sample.
     */
    saradc_start_conversion(qts, 4);
    g_assert_cmphex(SARADC_READ(SARADC_STATUS), ==, SARADC_STATUS_BUSY);
    g_assert_cmphex(SARADC_READ(SARADC_DATA(4)), ==, 0);

    qtest_clock_step(qts, SARADC_STEP_NS);
    g_assert_cmphex(SARADC_READ(SARADC_STATUS), ==, 0);
    g_assert_cmphex(SARADC_READ(SARADC_DATA(4)), ==, SARADC_FULL_SCALE);

    /* START is self-clearing, the channel selection stays. */
    g_assert_cmphex(SARADC_READ(SARADC_CONV_CON), ==,
                    SARADC_SINGLE_MODE | 4);

    /* Other channels keep their own result. */
    g_assert_cmphex(SARADC_READ(SARADC_DATA(5)), ==, 0);

    /* 0.9 V is half of the reference, so half of the 12-bit range. */
    saradc_start_conversion(qts, 5);
    qtest_clock_step(qts, SARADC_STEP_NS);
    g_assert_cmphex(SARADC_READ(SARADC_DATA(5)), ==, SARADC_FULL_SCALE / 2);
    g_assert_cmphex(SARADC_READ(SARADC_DATA(4)), ==, SARADC_FULL_SCALE);

    /* Results are written by the conversion alone. */
    SARADC_WRITE(SARADC_DATA(4), 0xdeadbeef);
    g_assert_cmphex(SARADC_READ(SARADC_DATA(4)), ==, SARADC_FULL_SCALE);

    qtest_quit(qts);
}

static void test_saradc_conversion_delay(void)
{
    QTestState *qts = rock5b_saradc_start(
        "-global rk3588-saradc.vin4-uv=1800000");

    /* Without the programmed delays the core still needs its 12 clocks. */
    SARADC_WRITE(SARADC_T_PD_SOC, 0);
    SARADC_WRITE(SARADC_T_DAS_SOC, 0);
    saradc_start_conversion(qts, 4);
    qtest_clock_step(qts, 11000);
    g_assert_cmphex(SARADC_READ(SARADC_STATUS), ==, SARADC_STATUS_BUSY);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(SARADC_READ(SARADC_STATUS), ==, 0);
    g_assert_cmphex(SARADC_READ(SARADC_DATA(4)), ==, SARADC_FULL_SCALE);

    /* Programmed delays stretch the conversion accordingly. */
    SARADC_WRITE(SARADC_T_PD_SOC, 0x20);
    SARADC_WRITE(SARADC_T_DAS_SOC, 0xc);
    saradc_start_conversion(qts, 4);
    qtest_clock_step(qts, 40000);
    g_assert_cmphex(SARADC_READ(SARADC_STATUS), ==, SARADC_STATUS_BUSY);
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(SARADC_READ(SARADC_STATUS), ==, 0);

    qtest_quit(qts);
}

static void test_saradc_irq(void)
{
    QTestState *qts = rock5b_saradc_start(
        "-global rk3588-saradc.vin4-uv=1800000");

    g_assert_cmphex(saradc_gic_level(qts), ==, 0);

    /* A conversion with the interrupt masked stays quiet. */
    saradc_start_conversion(qts, 4);
    qtest_clock_step(qts, SARADC_STEP_NS);
    g_assert_cmphex(SARADC_READ(SARADC_END_INT_ST), ==, SARADC_EN_END_INT);
    g_assert_cmphex(saradc_gic_level(qts), ==, 0);

    /* Enabling the interrupt raises the line, since the flag is pending. */
    SARADC_WRITE_MASKED(SARADC_END_INT_EN, SARADC_EN_END_INT);
    g_assert_cmphex(saradc_gic_level(qts), ==, saradc_gic_mask());

    /* END_INT_ST is write-one-to-clear, and clearing it drops the line. */
    SARADC_WRITE(SARADC_END_INT_ST, SARADC_EN_END_INT);
    g_assert_cmphex(SARADC_READ(SARADC_END_INT_ST), ==, 0);
    g_assert_cmphex(saradc_gic_level(qts), ==, 0);

    /* The next conversion latches it again. */
    saradc_start_conversion(qts, 4);
    qtest_clock_step(qts, SARADC_STEP_NS);
    g_assert_cmphex(saradc_gic_level(qts), ==, saradc_gic_mask());

    qtest_quit(qts);
}

static void test_saradc_invalid_channel(void)
{
    QTestState *qts = rock5b_saradc_start(
        "-global rk3588-saradc.vin4-uv=1800000");

    SARADC_WRITE_MASKED(SARADC_END_INT_EN, SARADC_EN_END_INT);

    /* Only eight channels exist; the rest must not start a conversion. */
    saradc_start_conversion(qts, 9);
    g_assert_cmphex(SARADC_READ(SARADC_STATUS), ==, 0);
    g_assert_cmphex(SARADC_READ(SARADC_CONV_CON), ==, SARADC_SINGLE_MODE | 9);

    qtest_clock_step(qts, SARADC_STEP_NS);
    g_assert_cmphex(SARADC_READ(SARADC_END_INT_ST), ==, 0);
    g_assert_cmphex(saradc_gic_level(qts), ==, 0);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(SARADC_READ(SARADC_DATA(i)), ==, 0);
    }

    /* The last valid channel still works afterwards. */
    saradc_start_conversion(qts, 7);
    qtest_clock_step(qts, SARADC_STEP_NS);
    g_assert_cmphex(SARADC_READ(SARADC_END_INT_ST), ==, SARADC_EN_END_INT);

    qtest_quit(qts);
}

static void test_saradc_reset(void)
{
    QTestState *qts = rock5b_saradc_start(
        "-global rk3588-saradc.vin4-uv=1800000");

    SARADC_WRITE(SARADC_T_PD_SOC, 0x20);
    SARADC_WRITE_MASKED(SARADC_END_INT_EN, SARADC_EN_END_INT);
    saradc_start_conversion(qts, 4);
    qtest_clock_step(qts, SARADC_STEP_NS);

    g_assert_cmphex(SARADC_READ(SARADC_DATA(4)), ==, SARADC_FULL_SCALE);
    g_assert_cmphex(saradc_gic_level(qts), ==, saradc_gic_mask());

    qtest_system_reset(qts);

    g_assert_cmphex(SARADC_READ(SARADC_CONV_CON), ==, 0);
    g_assert_cmphex(SARADC_READ(SARADC_T_PD_SOC), ==, 0);
    g_assert_cmphex(SARADC_READ(SARADC_END_INT_EN), ==, 0);
    g_assert_cmphex(SARADC_READ(SARADC_END_INT_ST), ==, 0);
    g_assert_cmphex(SARADC_READ(SARADC_STATUS), ==, 0);
    g_assert_cmphex(SARADC_READ(SARADC_DATA(4)), ==, 0);
    g_assert_cmphex(saradc_gic_level(qts), ==, 0);

    /* A fresh conversion starts from the reset state. */
    saradc_start_conversion(qts, 4);
    g_assert_cmphex(SARADC_READ(SARADC_STATUS), ==, SARADC_STATUS_BUSY);
    qtest_clock_step(qts, SARADC_STEP_NS);
    g_assert_cmphex(SARADC_READ(SARADC_DATA(4)), ==, SARADC_FULL_SCALE);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(ROCK5B_MACHINE_NAME)) {
        g_test_skip(ROCK5B_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3588-saradc/reset-values",
                   test_saradc_reset_values);
    qtest_add_func("/rk3588-saradc/write-masking",
                   test_saradc_write_masking);
    qtest_add_func("/rk3588-saradc/conversion", test_saradc_conversion);
    qtest_add_func("/rk3588-saradc/conversion-delay",
                   test_saradc_conversion_delay);
    qtest_add_func("/rk3588-saradc/irq", test_saradc_irq);
    qtest_add_func("/rk3588-saradc/invalid-channel",
                   test_saradc_invalid_channel);
    qtest_add_func("/rk3588-saradc/reset", test_saradc_reset);

    return g_test_run();
}
