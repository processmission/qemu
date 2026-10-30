/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register, IRQ and timer tests for the RK3566 I2S/TDM and PDM audio models.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "rk3566-test-common.h"

/* I2S/TDM register window at 0xfe410000 and its interrupt line. */
#define RK3566_I2S_BASE 0xfe410000ULL
#define RK3566_I2S_IRQ 53
#define RK3566_I2S_TXCR 0x000
#define RK3566_I2S_RXCR 0x004
#define RK3566_I2S_CKR 0x008
#define RK3566_I2S_TXFIFOLR 0x00c
#define RK3566_I2S_DMACR 0x010
#define RK3566_I2S_INTCR 0x014
#define RK3566_I2S_INTSR 0x018
#define RK3566_I2S_XFER 0x01c
#define RK3566_I2S_TXDR 0x024
#define RK3566_I2S_RXDR 0x028
#define RK3566_I2S_RXFIFOLR 0x02c
#define RK3566_I2S_TDM_TXCR 0x030
#define RK3566_I2S_TDM_RXCR 0x034
#define RK3566_I2S_CLKDIV 0x038
#define RK3566_I2S_INTSR_TX_EMPTY (1u << 0)
#define RK3566_I2S_INTSR_TX_UNDERRUN (1u << 1)
#define RK3566_I2S_INTCR_TX_EMPTY_EN (1u << 0)

/* PDM register window at 0xfe440000. */
#define RK3566_PDM_BASE 0xfe440000ULL
#define RK3566_PDM_SYSCONFIG 0x000
#define RK3566_PDM_CTRL0 0x004
#define RK3566_PDM_CTRL1 0x008
#define RK3566_PDM_CLK_CTRL 0x00c
#define RK3566_PDM_FIFO_CTRL 0x014
#define RK3566_PDM_DMA_CTRL 0x018
#define RK3566_PDM_INT_EN 0x01c
#define RK3566_PDM_INT_CLR 0x020
#define RK3566_PDM_INT_ST 0x024
#define RK3566_PDM_RXFIFO_DATA 0x030

#define RK3566_GIC_QOM "/machine/gic"

static uint32_t i2s_read(QTestState *qts, uint32_t offset)
{
    return rk3566_mmio_read32_at(qts, RK3566_I2S_BASE, offset);
}

static void i2s_write(QTestState *qts, uint32_t offset, uint32_t value)
{
    rk3566_mmio_write32_at(qts, RK3566_I2S_BASE, offset, value);
}

static uint32_t pdm_read(QTestState *qts, uint32_t offset)
{
    return rk3566_mmio_read32_at(qts, RK3566_PDM_BASE, offset);
}

static void pdm_write(QTestState *qts, uint32_t offset, uint32_t value)
{
    rk3566_mmio_write32_at(qts, RK3566_PDM_BASE, offset, value);
}

static bool rk3566_audio_available(QTestState *qts)
{
    g_autofree char *i2s = rk3566_qom_child_object_type(qts, "/machine",
                                                        "i2s1");
    g_autofree char *pdm = rk3566_qom_child_object_type(qts, "/machine",
                                                        "pdm");

    return i2s && pdm;
}

static void test_rk3566_audio_reset_values(void)
{
    QTestState *qts = rk3566_qtest_start();
    g_autofree char *i2s = NULL;
    g_autofree char *pdm = NULL;

    if (!rk3566_audio_available(qts)) {
        g_test_skip("RK3566 audio devices are not available");
        qtest_quit(qts);
        return;
    }

    i2s = rk3566_qom_child_object_type(qts, "/machine", "i2s1");
    pdm = rk3566_qom_child_object_type(qts, "/machine", "pdm");
    g_assert_cmpstr(i2s, ==, "rk3566-i2s-tdm");
    g_assert_cmpstr(pdm, ==, "rk3566-pdm");

    /* I2S/TDM reset defaults come from the register table. */
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_TXCR), ==, 0x7200000f);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_RXCR), ==, 0x01c8000f);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_CKR), ==, 0x1f1f);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_DMACR), ==, 0x001f0000);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_INTCR), ==, 0x01f00000);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_TXCR), ==, 0x7200000f);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_TDM_TXCR), ==, 0x3eff);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_TDM_RXCR), ==, 0x3eff);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_CLKDIV), ==, 0x0707);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_TXFIFOLR), ==, 0);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_INTSR), ==,
                    RK3566_I2S_INTSR_TX_EMPTY);

    /* PDM reset defaults come from the register table. */
    g_assert_cmphex(pdm_read(qts, RK3566_PDM_SYSCONFIG), ==, 0);
    g_assert_cmphex(pdm_read(qts, RK3566_PDM_CTRL0), ==, 0x78000017);
    g_assert_cmphex(pdm_read(qts, RK3566_PDM_CTRL1), ==, 0x0bb8ea60);
    g_assert_cmphex(pdm_read(qts, RK3566_PDM_CLK_CTRL), ==, 0x0000e401);
    g_assert_cmphex(pdm_read(qts, RK3566_PDM_DMA_CTRL), ==, 0x1f);
    g_assert_cmphex(pdm_read(qts, RK3566_PDM_INT_ST), ==, 0);
    g_assert_cmphex(pdm_read(qts, RK3566_PDM_FIFO_CTRL), ==, 0);

    qtest_quit(qts);
}

static void test_rk3566_audio_fifo_and_masks(void)
{
    QTestState *qts = rk3566_qtest_start();

    if (!rk3566_audio_available(qts)) {
        g_test_skip("RK3566 audio devices are not available");
        qtest_quit(qts);
        return;
    }

    /* Control words accept the full 32-bit write and read back. */
    i2s_write(qts, RK3566_I2S_TXCR, 0xffffffff);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_TXCR), ==, 0xffffffff);
    i2s_write(qts, RK3566_I2S_TXCR, 0x7200000f);

    /* INTSR is read-only; bit 0 reports the empty transmit FIFO. */
    i2s_write(qts, RK3566_I2S_INTSR, 0xffffffff);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_INTSR), ==,
                    RK3566_I2S_INTSR_TX_EMPTY);

    /* INTCR bits 2 and 18 are clear triggers and are not stored. */
    i2s_write(qts, RK3566_I2S_INTCR,
              RK3566_I2S_INTCR_TX_EMPTY_EN | (1u << 2));
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_INTCR), ==,
                    RK3566_I2S_INTCR_TX_EMPTY_EN);

    /* TXDR pushes words into the FIFO; TXFIFOLR reports the level. */
    i2s_write(qts, RK3566_I2S_TXDR, 0x11223344);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_TXFIFOLR), ==, 1);
    i2s_write(qts, RK3566_I2S_TXDR, 0x55667788);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_TXFIFOLR), ==, 2);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_TXDR), ==, 0);

    /* RXDR pops the receive FIFO and ignores guest writes. */
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_RXDR), ==, 0);
    i2s_write(qts, RK3566_I2S_RXDR, 0xffffffff);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_RXDR), ==, 0);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_RXFIFOLR), ==, 0);

    /* SYSCONFIG bit 0 flushes the PDM RX FIFO and is not stored. */
    pdm_write(qts, RK3566_PDM_SYSCONFIG, 0xffffffff);
    g_assert_cmphex(pdm_read(qts, RK3566_PDM_SYSCONFIG), ==, 0xfffffffe);
    g_assert_cmphex(pdm_read(qts, RK3566_PDM_FIFO_CTRL), ==, 0);

    /* PDM status and data registers ignore guest writes. */
    pdm_write(qts, RK3566_PDM_INT_ST, 0xffffffff);
    g_assert_cmphex(pdm_read(qts, RK3566_PDM_INT_ST), ==, 0);
    pdm_write(qts, RK3566_PDM_FIFO_CTRL, 0xffffffff);
    g_assert_cmphex(pdm_read(qts, RK3566_PDM_FIFO_CTRL), ==, 0);
    pdm_write(qts, RK3566_PDM_RXFIFO_DATA, 0xffffffff);
    g_assert_cmphex(pdm_read(qts, RK3566_PDM_RXFIFO_DATA), ==, 0);

    /* Words outside the register table keep the backing-store fallback. */
    i2s_write(qts, 0x03c, 0x5a5a5a5a);
    g_assert_cmphex(i2s_read(qts, 0x03c), ==, 0x5a5a5a5a);

    /* Accesses beyond the 0x100-byte register file read as zero. */
    i2s_write(qts, 0x0ffc, 0xffffffff);
    g_assert_cmphex(i2s_read(qts, 0x0ffc), ==, 0);

    qtest_quit(qts);
}

static void test_rk3566_audio_irq_and_timer(void)
{
    QTestState *qts = rk3566_qtest_start();

    if (!rk3566_audio_available(qts)) {
        g_test_skip("RK3566 audio devices are not available");
        qtest_quit(qts);
        return;
    }

    qtest_irq_intercept_in(qts, RK3566_GIC_QOM);

    /* The TX empty interrupt follows the INTCR enable bit. */
    i2s_write(qts, RK3566_I2S_INTCR, RK3566_I2S_INTCR_TX_EMPTY_EN);
    g_assert_true(qtest_get_irq(qts, RK3566_I2S_IRQ));
    i2s_write(qts, RK3566_I2S_INTCR, 0);
    g_assert_false(qtest_get_irq(qts, RK3566_I2S_IRQ));

    /*
     * Program a divided clock that the model accepts, enable the
     * transmitter and let one half-frame timer period pass.
     */
    i2s_write(qts, RK3566_I2S_CLKDIV, 0xff);
    i2s_write(qts, RK3566_I2S_CKR, 0xff);
    i2s_write(qts, RK3566_I2S_XFER, 1);
    qtest_clock_step(qts, 100000);
    g_assert_cmphex(i2s_read(qts, RK3566_I2S_INTSR) &
                    RK3566_I2S_INTSR_TX_UNDERRUN, ==,
                    RK3566_I2S_INTSR_TX_UNDERRUN);
    i2s_write(qts, RK3566_I2S_XFER, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566/audio-reset-values",
                   test_rk3566_audio_reset_values);
    qtest_add_func("/rk3566/audio-fifo-and-masks",
                   test_rk3566_audio_fifo_and_masks);
    qtest_add_func("/rk3566/audio-irq-and-timer",
                   test_rk3566_audio_irq_and_timer);

    return g_test_run();
}
