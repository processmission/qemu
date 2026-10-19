/*
 * QTest for the SiFli SF32LB52X SPI controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Covers the SPI register model (SVD SPI1, vendor spi.h, Zephyr
 * spi_sf32lb): reset values, register store/readback, loopback transceive
 * poll sequence, FIFO depth and levels,
 * W1C overrun, threshold IRQs, 16-bit frames and both instances.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "sf32lb52-devkit-lcd"
#define SPI1_BASE 0x50095000
#define SPI2_BASE 0x50096000

/* Register offsets (SVD SPI1 dump). */
#define SPI_TOP_CTRL     0x00
#define SPI_FIFO_CTRL    0x04
#define SPI_INTE         0x08
#define SPI_TO           0x0c
#define SPI_DATA         0x10
#define SPI_STATUS       0x14
#define SPI_PSP_CTRL     0x18
#define SPI_NW_CTRL      0x1c
#define SPI_NW_STATUS    0x20
#define SPI_CLK_CTRL     0x3c
#define SPI_TRIWIRE_CTRL 0x54

/* TOP_CTRL */
#define TOP_SSE  (1U << 0)
#define TOP_LBM  (1U << 12)
#define TOP_DSS_16 (15U << 5)

/* FIFO_CTRL */
#define FIFO_TFT(n)  ((n) & 0x1fU)
#define FIFO_RFT(n)  (((n) & 0x1fU) << 5)
#define FIFO_TSRE    (1U << 10)
#define FIFO_RSRE    (1U << 11)

/* INTE */
#define INTE_TINTE (1U << 1)
#define INTE_RIE   (1U << 2)
#define INTE_TIE   (1U << 3)
#define INTE_RIM   (1U << 4)
#define INTE_TIM   (1U << 5)

/* STATUS */
#define ST_BSY (1U << 0)
#define ST_TINT (1U << 3)
#define ST_TFS (1U << 5)
#define ST_TNF (1U << 6)
#define ST_TFL(reg) (((reg) >> 7) & 0xfU)
#define ST_TUR (1U << 12)
#define ST_RFS (1U << 13)
#define ST_RNE (1U << 14)
#define ST_RFL_Msk (0xfU << 15)
#define ST_RFL(reg) (((reg) >> 15) & 0xfU)
#define ST_ROR (1U << 20)
#define STATUS_RESET (ST_TNF | (0xfU << 15))

#define SPI_SHIFT_NS 1000
#define SPI_IRQ_NAME "sysbus-irq"
#define SPI1_QOM_PATH "/machine/soc/spi[0]"

static QTestState *spi_qtest_start(void)
{
    return qtest_init("-machine " MACHINE " -serial null");
}

static uint32_t spi_readl(QTestState *qts, uint64_t base, uint32_t offset)
{
    return qtest_readl(qts, base + offset);
}

static void spi_writel(QTestState *qts, uint64_t base, uint32_t offset,
                       uint32_t value)
{
    qtest_writel(qts, base + offset, value);
}

static void spi_irq_intercept(QTestState *qts)
{
    qtest_irq_intercept_out_named(qts, SPI1_QOM_PATH, SPI_IRQ_NAME);
}

/* Run the framing engine forward by n frames (1 us each). */
static void spi_step(QTestState *qts, unsigned frames)
{
    qtest_clock_step(qts, (int64_t)frames * SPI_SHIFT_NS);
}

static void test_reset_values(void)
{
    QTestState *qts = spi_qtest_start();
    uint64_t bases[] = { SPI1_BASE, SPI2_BASE };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(bases); i++) {
        uint64_t base = bases[i];

        g_assert_cmphex(spi_readl(qts, base, SPI_TOP_CTRL), ==, 0);
        g_assert_cmphex(spi_readl(qts, base, SPI_FIFO_CTRL), ==, 0);
        g_assert_cmphex(spi_readl(qts, base, SPI_INTE), ==, 0);
        g_assert_cmphex(spi_readl(qts, base, SPI_TO), ==, 0);
        g_assert_cmphex(spi_readl(qts, base, SPI_CLK_CTRL), ==, 0);
        g_assert_cmphex(spi_readl(qts, base, SPI_TRIWIRE_CTRL), ==, 0);
        /* TX empty (TNF=1) and RX empty encoded as RFL=0xf. */
        g_assert_cmphex(spi_readl(qts, base, SPI_STATUS), ==, STATUS_RESET);
        /* HAL-named PSP/NW registers are unimplemented (read 0). */
        g_assert_cmphex(spi_readl(qts, base, SPI_PSP_CTRL), ==, 0);
        g_assert_cmphex(spi_readl(qts, base, SPI_NW_CTRL), ==, 0);
        g_assert_cmphex(spi_readl(qts, base, SPI_NW_STATUS), ==, 0);
        /* Reserved offset: guest error, no fault. */
        g_assert_cmphex(qtest_readl(qts, base + 0xffc), ==, 0);
    }

    qtest_quit(qts);
}

static void test_register_store_readback(void)
{
    QTestState *qts = spi_qtest_start();
    uint32_t top = TOP_SSE | TOP_LBM | TOP_DSS_16;
    uint32_t fifo;

    spi_writel(qts, SPI1_BASE, SPI_TOP_CTRL, top);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_TOP_CTRL), ==, top);

    fifo = FIFO_TFT(2) | FIFO_RFT(3) | FIFO_TSRE | FIFO_RSRE;
    spi_writel(qts, SPI1_BASE, SPI_FIFO_CTRL, fifo);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_FIFO_CTRL), ==,
                    FIFO_TFT(2) | FIFO_RFT(3));

    spi_writel(qts, SPI1_BASE, SPI_INTE,
               INTE_RIE | INTE_TIE | INTE_RIM | INTE_TIM);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_INTE), ==,
                    INTE_RIE | INTE_TIE | INTE_RIM | INTE_TIM);

    spi_writel(qts, SPI1_BASE, SPI_TO, 0x123456);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_TO), ==, 0x123456);

    spi_writel(qts, SPI1_BASE, SPI_CLK_CTRL, 0x1ff);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_CLK_CTRL), ==, 0x1ff);

    spi_writel(qts, SPI1_BASE, SPI_TRIWIRE_CTRL, 0x7);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_TRIWIRE_CTRL), ==, 0x7);

    qtest_quit(qts);
}

static void test_loopback_transceive_poll(void)
{
    QTestState *qts = spi_qtest_start();
    uint32_t status;

    /* Zephyr spi_sf32lb_transceive_poll sequence (spi_sf32lb.c:595-642). */
    spi_writel(qts, SPI1_BASE, SPI_TOP_CTRL, TOP_SSE | TOP_LBM);
    spi_writel(qts, SPI1_BASE, SPI_FIFO_CTRL, FIFO_TSRE | FIFO_RSRE);
    qtest_writel(qts, SPI1_BASE + SPI_STATUS, ST_ROR | ST_TUR | ST_TINT);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_STATUS) & ST_RNE, ==, 0);

    /* One 8-bit frame: write DATA, wait for the shifted RX frame. */
    qtest_writeb(qts, SPI1_BASE + SPI_DATA, 0x5a);
    status = spi_readl(qts, SPI1_BASE, SPI_STATUS);
    g_assert_cmphex(status & ST_BSY, ==, ST_BSY);
    spi_step(qts, 1);
    status = spi_readl(qts, SPI1_BASE, SPI_STATUS);
    g_assert_cmphex(status & ST_RNE, ==, ST_RNE);
    g_assert_cmphex(status & ST_RFL_Msk, ==, 0);
    g_assert_cmphex(qtest_readb(qts, SPI1_BASE + SPI_DATA), ==, 0x5a);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_STATUS) & ST_RNE, ==, 0);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_STATUS) & ST_BSY, ==, 0);

    qtest_quit(qts);
}

static void test_fifo_depth_and_levels(void)
{
    QTestState *qts = spi_qtest_start();
    uint32_t status;
    unsigned i;

    spi_writel(qts, SPI1_BASE, SPI_TOP_CTRL, TOP_SSE | TOP_LBM);

    /* 16 frames fill the TX FIFO; no clock step yet, so nothing shifts. */
    for (i = 0; i < 16; i++) {
        qtest_writeb(qts, SPI1_BASE + SPI_DATA, i + 1);
    }
    status = spi_readl(qts, SPI1_BASE, SPI_STATUS);
    g_assert_cmphex(status & ST_TNF, ==, 0);
    g_assert_cmpuint(ST_TFL(status), ==, 15);
    g_assert_cmphex(status & ST_BSY, ==, ST_BSY);

    /* A 17th write is dropped while TX is full. */
    qtest_writeb(qts, SPI1_BASE + SPI_DATA, 0xaa);
    g_assert_cmpuint(ST_TFL(spi_readl(qts, SPI1_BASE, SPI_STATUS)), ==, 15);

    /* Drain TX into RX; RX becomes full with all 16 looped-back frames. */
    spi_step(qts, 16);
    status = spi_readl(qts, SPI1_BASE, SPI_STATUS);
    g_assert_cmphex(status & ST_TNF, ==, ST_TNF);
    g_assert_cmpuint(ST_TFL(status), ==, 0);
    g_assert_cmphex(status & ST_RNE, ==, ST_RNE);
    g_assert_cmphex(status & ST_BSY, ==, 0);
    g_assert_cmpuint(ST_RFL(status), ==, 15); /* full encodes like empty */

    for (i = 0; i < 16; i++) {
        g_assert_cmphex(qtest_readb(qts, SPI1_BASE + SPI_DATA), ==, i + 1);
    }
    status = spi_readl(qts, SPI1_BASE, SPI_STATUS);
    g_assert_cmphex(status & ST_RNE, ==, 0);
    g_assert_cmpuint(ST_RFL(status), ==, 15);

    qtest_quit(qts);
}

static void test_overrun_ror_and_w1c(void)
{
    QTestState *qts = spi_qtest_start();
    unsigned i;

    spi_irq_intercept(qts);
    spi_writel(qts, SPI1_BASE, SPI_TOP_CTRL, TOP_SSE | TOP_LBM);

    /* Fill the RX FIFO. */
    for (i = 0; i < 16; i++) {
        qtest_writeb(qts, SPI1_BASE + SPI_DATA, i);
    }
    spi_step(qts, 16);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_STATUS) & ST_ROR, ==, 0);

    /* One more shifted frame overruns RX: ROR sets and raises IRQ 60. */
    qtest_writeb(qts, SPI1_BASE + SPI_DATA, 0x77);
    spi_step(qts, 1);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_STATUS) & ST_ROR, ==, ST_ROR);
    g_assert_cmpint(qtest_get_irq(qts, 0), ==, 1);

    /* W1C: write 1 to ROR clears it (Zephyr spi.c:625). */
    qtest_writel(qts, SPI1_BASE + SPI_STATUS, ST_ROR);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_STATUS) & ST_ROR, ==, 0);
    g_assert_cmpint(qtest_get_irq(qts, 0), ==, 0);

    /* Writing 0 keeps the flag clear; TUR/TINT stay clear too. */
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_STATUS) &
                    (ST_TUR | ST_TINT), ==, 0);

    qtest_quit(qts);
}

static void test_irq_thresholds(void)
{
    QTestState *qts = spi_qtest_start();

    spi_irq_intercept(qts);
    spi_writel(qts, SPI1_BASE, SPI_TOP_CTRL, TOP_SSE | TOP_LBM);
    /* RFT = 0: one RX frame exceeds the threshold and raises IRQ. */
    spi_writel(qts, SPI1_BASE, SPI_FIFO_CTRL, FIFO_RFT(0));
    spi_writel(qts, SPI1_BASE, SPI_INTE, INTE_RIE);
    g_assert_cmpint(qtest_get_irq(qts, 0), ==, 0);

    qtest_writeb(qts, SPI1_BASE + SPI_DATA, 0x42);
    spi_step(qts, 1);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_STATUS) & ST_RFS, ==, ST_RFS);
    g_assert_cmpint(qtest_get_irq(qts, 0), ==, 1);

    qtest_readb(qts, SPI1_BASE + SPI_DATA);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_STATUS) & ST_RFS, ==, 0);
    g_assert_cmpint(qtest_get_irq(qts, 0), ==, 0);

    /* TIE/TFT=0: empty TX is at the threshold, filling 16 drops TFS. */
    spi_writel(qts, SPI1_BASE, SPI_INTE, INTE_TIE);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_STATUS) & ST_TFS, ==, ST_TFS);
    g_assert_cmpint(qtest_get_irq(qts, 0), ==, 1);
    {
        unsigned i;

        for (i = 0; i < 16; i++) {
            qtest_writeb(qts, SPI1_BASE + SPI_DATA, i);
        }
    }
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_STATUS) & ST_TFS, ==, 0);
    g_assert_cmpint(qtest_get_irq(qts, 0), ==, 0);

    qtest_quit(qts);
}

static void test_16bit_frame(void)
{
    QTestState *qts = spi_qtest_start();

    spi_writel(qts, SPI1_BASE, SPI_TOP_CTRL, TOP_SSE | TOP_LBM | TOP_DSS_16);
    qtest_writel(qts, SPI1_BASE + SPI_DATA, 0x1234);
    spi_step(qts, 1);
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_STATUS) & ST_RNE, ==, ST_RNE);
    g_assert_cmphex(qtest_readl(qts, SPI1_BASE + SPI_DATA), ==, 0x1234);

    qtest_quit(qts);
}

static void test_both_instances(void)
{
    QTestState *qts = spi_qtest_start();

    spi_writel(qts, SPI1_BASE, SPI_TOP_CTRL, TOP_SSE | TOP_LBM);
    spi_writel(qts, SPI2_BASE, SPI_TOP_CTRL, TOP_SSE | TOP_LBM);

    qtest_writeb(qts, SPI2_BASE + SPI_DATA, 0x33);
    spi_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, SPI2_BASE + SPI_DATA), ==, 0x33);
    /* SPI1 stays idle: no frame was written to it. */
    g_assert_cmphex(spi_readl(qts, SPI1_BASE, SPI_STATUS) & ST_RNE, ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sf32lb52x/spi/reset", test_reset_values);
    qtest_add_func("/sf32lb52x/spi/register-store",
                   test_register_store_readback);
    qtest_add_func("/sf32lb52x/spi/loopback-transceive",
                   test_loopback_transceive_poll);
    qtest_add_func("/sf32lb52x/spi/fifo-depth",
                   test_fifo_depth_and_levels);
    qtest_add_func("/sf32lb52x/spi/overrun-w1c", test_overrun_ror_and_w1c);
    qtest_add_func("/sf32lb52x/spi/irq-thresholds", test_irq_thresholds);
    qtest_add_func("/sf32lb52x/spi/16bit-frame", test_16bit_frame);
    qtest_add_func("/sf32lb52x/spi/both-instances", test_both_instances);

    return g_test_run();
}
