/*
 * QTest for the SiFli SF32LB52X AUDIO codec
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Falsifiable coverage of the AUDCODEC register behavior (SVD AUDCODEC,
 * vendor HAL audcodec.h, Zephyr drivers/audio/sf32lb_codec.c):
 *
 *  - reset values, including ID == 0xC0DEC000 and the instantaneous PLL
 *    calibration DONE bit / locked PLL_STAT;
 *  - configuration store/readback (CFG, DAC/ADC_CFG, channel configs,
 *    COMMON_CFG, PLL_CFG0..6);
 *  - PLL calibration/lock handshake used by the driver's unbounded waits;
 *  - IRQ raw status W1C (DAC FIFO overflow, ADC FIFO underflow) and
 *    IRQ_MSK storage;
 *  - APB FIFO data path: DAC_CHn_ENTRY push, ADC_CHn_ENTRY pop/underflow,
 *    APB_STAT FIFO counts;
 *  - a driver-like init/calibrate/configure/start sequence that must
 *    terminate (this is the anti-hang regression);
 *  - reserved and unimplemented offsets read as zero and ignore writes.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "sf32lb52-devkit-lcd"
#define AUDCODEC_BASE 0x50088000

/* Register offsets (SVD AUDCODEC / HAL AUDCODEC_TypeDef). */
#define A_ID 0x00
#define A_CFG 0x04
#define A_IRQ 0x08
#define A_IRQ_MSK 0x0c
#define A_DAC_CFG 0x10
#define A_ADC_CFG 0x14
#define A_APB_STAT 0x18
#define A_ADC_CH0_CFG 0x20
#define A_ADC_CH1_CFG 0x24
#define A_DAC_CH0_CFG 0x30
#define A_DAC_CH0_CFG_EXT 0x34
#define A_DAC_CH1_CFG 0x38
#define A_DAC_CH1_CFG_EXT 0x3c
#define A_ADC_CH0_ENTRY 0x40
#define A_ADC_CH1_ENTRY 0x44
#define A_DAC_CH0_ENTRY 0x50
#define A_DAC_CH1_ENTRY 0x54
#define A_COMMON_CFG 0x70
#define A_BG_CFG0 0x74
#define A_BG_CFG1 0x78
#define A_BG_CFG2 0x7c
#define A_REFGEN_CFG 0x80
#define A_PLL_CFG0 0x84
#define A_PLL_CFG1 0x88
#define A_PLL_CFG2 0x8c
#define A_PLL_CFG3 0x90
#define A_PLL_CFG4 0x94
#define A_PLL_CFG5 0x98
#define A_PLL_CFG6 0x9c
#define A_PLL_STAT 0xa0
#define A_PLL_CAL_CFG 0xa4
#define A_PLL_CAL_RESULT 0xa8

/* CFG bits */
#define CFG_ADC_ENABLE (1U << 0)
#define CFG_DAC_ENABLE (1U << 1)
#define CFG_DAC_1K_MODE (1U << 2)
#define CFG_ADC_EN_DLY_SEL (3U << 3)
#define CFG_WRITABLE_MASK 0x1fU

/* IRQ bits (SVD) */
#define IRQ_DAC_CH0_APB_OF (1U << 0)
#define IRQ_DAC_CH1_APB_OF (1U << 3)
#define IRQ_ADC_CH0_APB_OF (1U << 16)
#define IRQ_ADC_CH0_APB_UF (1U << 17)
#define IRQ_ADC_CH1_APB_OF (1U << 19)
#define IRQ_ADC_CH1_APB_UF (1U << 20)
#define IRQ_W1C_MASK 0x003f003fU

/* PLL bits */
#define PLL_CAL_EN (1U << 0)
#define PLL_CAL_DONE (1U << 1)
#define PLL_CAL_LEN(n) (((n) & 0xffffU) << 16)
#define PLL_STAT_UNLOCK (1U << 0)

#define PLL_CNT_EXPECT 1918U
#define XTAL_CNT_EXPECT 2000U

#define APB_CNT_MASK 0xfU
#define APB_DAC0_SHIFT 0
#define APB_DAC1_SHIFT 4
#define APB_ADC0_SHIFT 16
#define APB_ADC1_SHIFT 20

#define ID_EXPECT 0xC0DEC000U

static QTestState *audcodec_qtest_start(void)
{
    return qtest_init("-machine " MACHINE " -serial null");
}

static uint32_t ac_read(QTestState *qts, unsigned int off)
{
    return qtest_readl(qts, AUDCODEC_BASE + off);
}

static void ac_write(QTestState *qts, unsigned int off, uint32_t value)
{
    qtest_writel(qts, AUDCODEC_BASE + off, value);
}

static uint32_t apb_cnt(uint32_t stat, unsigned int shift)
{
    return (stat >> shift) & APB_CNT_MASK;
}

static void test_reset(void)
{
    static const unsigned int reserved[] = {
        0x1c, 0x28, 0x2c, 0x48, 0x4c, 0x68, 0x6c,
        0xc8, 0xcc, 0xd0, 0xd4, 0xd8,
    };
    QTestState *qts = audcodec_qtest_start();
    uint32_t value;
    size_t i;

    g_assert_cmphex(ac_read(qts, A_ID), ==, ID_EXPECT);

    g_assert_cmphex(ac_read(qts, A_CFG), ==, 0);
    g_assert_cmphex(ac_read(qts, A_IRQ), ==, 0);
    g_assert_cmphex(ac_read(qts, A_IRQ_MSK), ==, 0);
    g_assert_cmphex(ac_read(qts, A_DAC_CFG), ==, 0);
    g_assert_cmphex(ac_read(qts, A_ADC_CFG), ==, 0);
    g_assert_cmphex(ac_read(qts, A_APB_STAT), ==, 0);
    g_assert_cmphex(ac_read(qts, A_ADC_CH0_CFG), ==, 0);
    g_assert_cmphex(ac_read(qts, A_ADC_CH1_CFG), ==, 0);
    g_assert_cmphex(ac_read(qts, A_DAC_CH0_CFG), ==, 0);
    g_assert_cmphex(ac_read(qts, A_DAC_CH0_CFG_EXT), ==, 0);
    g_assert_cmphex(ac_read(qts, A_DAC_CH1_CFG), ==, 0);
    g_assert_cmphex(ac_read(qts, A_DAC_CH1_CFG_EXT), ==, 0);
    g_assert_cmphex(ac_read(qts, A_COMMON_CFG), ==, 0);
    g_assert_cmphex(ac_read(qts, A_BG_CFG0), ==, 0);
    g_assert_cmphex(ac_read(qts, A_BG_CFG1), ==, 0);
    g_assert_cmphex(ac_read(qts, A_BG_CFG2), ==, 0);
    g_assert_cmphex(ac_read(qts, A_REFGEN_CFG), ==, 0);

    for (i = 0; i <= (A_PLL_CFG6 - A_PLL_CFG0) / 4; i++) {
        g_assert_cmphex(ac_read(qts, A_PLL_CFG0 + 4 * i), ==, 0);
    }

    /* PLL is calibrated and locked from reset (drives the driver's waits). */
    g_assert_cmphex(ac_read(qts, A_PLL_STAT) & PLL_STAT_UNLOCK, ==, 0);
    value = ac_read(qts, A_PLL_CAL_CFG);
    g_assert_cmphex(value & PLL_CAL_DONE, ==, PLL_CAL_DONE);
    g_assert_cmphex(value & PLL_CAL_EN, ==, 0);
    g_assert_cmphex(value >> 16, ==, 0);

    value = ac_read(qts, A_PLL_CAL_RESULT);
    g_assert_cmphex(value >> 16, ==, PLL_CNT_EXPECT);
    g_assert_cmphex(value & 0xffff, ==, XTAL_CNT_EXPECT);

    /* Reserved and unimplemented offsets read as zero. */
    for (i = 0; i < ARRAY_SIZE(reserved); i++) {
        g_assert_cmphex(ac_read(qts, reserved[i]), ==, 0);
    }
    g_assert_cmphex(ac_read(qts, 0xdc), ==, 0);
    g_assert_cmphex(ac_read(qts, 0x3fc), ==, 0);

    /* ID is read-only. */
    ac_write(qts, A_ID, 0xffffffff);
    g_assert_cmphex(ac_read(qts, A_ID), ==, ID_EXPECT);

    qtest_quit(qts);
}

static void test_config_readback(void)
{
    QTestState *qts = audcodec_qtest_start();

    ac_write(qts, A_CFG, CFG_ADC_ENABLE | CFG_DAC_ENABLE | CFG_DAC_1K_MODE |
                          CFG_ADC_EN_DLY_SEL);
    g_assert_cmphex(ac_read(qts, A_CFG), ==, CFG_WRITABLE_MASK);
    ac_write(qts, A_CFG, 0xffffffff);
    g_assert_cmphex(ac_read(qts, A_CFG), ==, CFG_WRITABLE_MASK);

    ac_write(qts, A_DAC_CFG, 0xffffffff);
    g_assert_cmphex(ac_read(qts, A_DAC_CFG), ==, 0xffffffff);
    ac_write(qts, A_ADC_CFG, 0xdeadbeef);
    g_assert_cmphex(ac_read(qts, A_ADC_CFG), ==, 0xdeadbeef);

    ac_write(qts, A_ADC_CH0_CFG, 0x0003c1ff);
    g_assert_cmphex(ac_read(qts, A_ADC_CH0_CFG), ==, 0x0003c1ff);
    ac_write(qts, A_ADC_CH1_CFG, 0x0002b3ee);
    g_assert_cmphex(ac_read(qts, A_ADC_CH1_CFG), ==, 0x0002b3ee);
    ac_write(qts, A_DAC_CH0_CFG, 0x7fffffff);
    g_assert_cmphex(ac_read(qts, A_DAC_CH0_CFG), ==, 0x7fffffff);
    ac_write(qts, A_DAC_CH0_CFG_EXT, 0x0000018f);
    g_assert_cmphex(ac_read(qts, A_DAC_CH0_CFG_EXT), ==, 0x0000018f);
    ac_write(qts, A_DAC_CH1_CFG, 0x55555555);
    g_assert_cmphex(ac_read(qts, A_DAC_CH1_CFG), ==, 0x55555555);
    ac_write(qts, A_DAC_CH1_CFG_EXT, 0x00000180);
    g_assert_cmphex(ac_read(qts, A_DAC_CH1_CFG_EXT), ==, 0x00000180);

    /* Analog/PLL tuning registers are storage-only. */
    ac_write(qts, A_COMMON_CFG, 0x000001c0);
    g_assert_cmphex(ac_read(qts, A_COMMON_CFG), ==, 0x000001c0);
    ac_write(qts, A_BG_CFG0, 0x00002121);
    g_assert_cmphex(ac_read(qts, A_BG_CFG0), ==, 0x00002121);
    ac_write(qts, A_REFGEN_CFG, 0x00000107);
    g_assert_cmphex(ac_read(qts, A_REFGEN_CFG), ==, 0x00000107);

    ac_write(qts, A_PLL_CFG0, 0x7eabc021);
    g_assert_cmphex(ac_read(qts, A_PLL_CFG0), ==, 0x7eabc021);
    ac_write(qts, A_PLL_CFG1, 0x0000f0f0);
    g_assert_cmphex(ac_read(qts, A_PLL_CFG1), ==, 0x0000f0f0);
    ac_write(qts, A_PLL_CFG2, 0x00004e00);
    g_assert_cmphex(ac_read(qts, A_PLL_CFG2), ==, 0x00004e00);
    ac_write(qts, A_PLL_CFG3, 0x7ffffff);
    g_assert_cmphex(ac_read(qts, A_PLL_CFG3), ==, 0x7ffffff);
    ac_write(qts, A_PLL_CFG4, 0x00ffc3ff);
    g_assert_cmphex(ac_read(qts, A_PLL_CFG4), ==, 0x00ffc3ff);
    ac_write(qts, A_PLL_CFG5, 0x1fffffff);
    g_assert_cmphex(ac_read(qts, A_PLL_CFG5), ==, 0x1fffffff);
    ac_write(qts, A_PLL_CFG6, 0x03ffffff);
    g_assert_cmphex(ac_read(qts, A_PLL_CFG6), ==, 0x03ffffff);

    /* IRQ_MSK keeps only the documented status bit positions. */
    ac_write(qts, A_IRQ_MSK, IRQ_W1C_MASK);
    g_assert_cmphex(ac_read(qts, A_IRQ_MSK), ==, IRQ_W1C_MASK);
    ac_write(qts, A_IRQ_MSK, 0xffffffff);
    g_assert_cmphex(ac_read(qts, A_IRQ_MSK), ==, IRQ_W1C_MASK);
    ac_write(qts, A_IRQ_MSK, 0);
    g_assert_cmphex(ac_read(qts, A_IRQ_MSK), ==, 0);

    qtest_quit(qts);
}

static void test_pll_handshake(void)
{
    QTestState *qts = audcodec_qtest_start();
    uint32_t value;
    int i;

    g_assert_cmphex(ac_read(qts, A_PLL_STAT), ==, 0);

    /* Driver pll_calibration: LEN=2000 with EN=0, then EN toggles. */
    ac_write(qts, A_PLL_CAL_CFG, PLL_CAL_LEN(2000));
    value = ac_read(qts, A_PLL_CAL_CFG);
    g_assert_cmphex(value & PLL_CAL_DONE, ==, PLL_CAL_DONE);
    g_assert_cmphex(value & PLL_CAL_EN, ==, 0);
    g_assert_cmphex(value >> 16, ==, 2000);

    ac_write(qts, A_PLL_CAL_CFG, PLL_CAL_LEN(2000) | PLL_CAL_EN);
    for (i = 0; i < 5; i++) {
        /* wait_pll_done() in the driver polls this bit without a timeout. */
        g_assert_cmphex(ac_read(qts, A_PLL_CAL_CFG) & PLL_CAL_DONE,
                        ==, PLL_CAL_DONE);
    }
    value = ac_read(qts, A_PLL_CAL_CFG);
    g_assert_cmphex(value & PLL_CAL_EN, ==, PLL_CAL_EN);
    g_assert_cmphex(value >> 16, ==, 2000);

    ac_write(qts, A_PLL_CAL_CFG, PLL_CAL_LEN(2000));
    g_assert_cmphex(ac_read(qts, A_PLL_CAL_CFG) & PLL_CAL_EN, ==, 0);

    /* Calibration counters are hardware-owned fixed results. */
    value = ac_read(qts, A_PLL_CAL_RESULT);
    g_assert_cmphex(value >> 16, ==, PLL_CNT_EXPECT);
    g_assert_cmphex(value & 0xffff, ==, XTAL_CNT_EXPECT);
    ac_write(qts, A_PLL_CAL_RESULT, 0);
    value = ac_read(qts, A_PLL_CAL_RESULT);
    g_assert_cmphex(value >> 16, ==, PLL_CNT_EXPECT);
    g_assert_cmphex(value & 0xffff, ==, XTAL_CNT_EXPECT);

    /*
     * PLL locked: the driver's do/while on UNLOCK terminates on the first
     * pass.
     */
    g_assert_cmphex(ac_read(qts, A_PLL_STAT) & PLL_STAT_UNLOCK, ==, 0);
    ac_write(qts, A_PLL_STAT, 0xffffffff);
    g_assert_cmphex(ac_read(qts, A_PLL_STAT), ==, 0);

    qtest_quit(qts);
}

static void test_irq_w1c(void)
{
    QTestState *qts = audcodec_qtest_start();
    uint32_t stat;
    int i;

    g_assert_cmphex(ac_read(qts, A_IRQ), ==, 0);

    /* Overflow the 16-entry DAC CH0 TX FIFO: DAC_CH0_APB_OF is set. */
    for (i = 0; i < 17; i++) {
        ac_write(qts, A_DAC_CH0_ENTRY, 0x1000 + i);
    }
    stat = ac_read(qts, A_IRQ);
    g_assert_cmphex(stat & IRQ_DAC_CH0_APB_OF, ==, IRQ_DAC_CH0_APB_OF);
    g_assert_cmphex(apb_cnt(ac_read(qts, A_APB_STAT), APB_DAC0_SHIFT), ==,
                    APB_CNT_MASK);

    /* Write-1-to-clear. */
    ac_write(qts, A_IRQ, IRQ_DAC_CH0_APB_OF);
    g_assert_cmphex(ac_read(qts, A_IRQ) & IRQ_DAC_CH0_APB_OF, ==, 0);

    /*
     * Reading an empty ADC channel FIFO raises ADC_CHn_APB_UF. CH1 has no
     * loopback data here (only DAC CH0 was written).
     */
    g_assert_cmphex(ac_read(qts, A_ADC_CH1_ENTRY), ==, 0);
    stat = ac_read(qts, A_IRQ);
    g_assert_cmphex(stat & IRQ_ADC_CH1_APB_UF, ==, IRQ_ADC_CH1_APB_UF);
    ac_write(qts, A_IRQ, IRQ_ADC_CH1_APB_UF);
    g_assert_cmphex(ac_read(qts, A_IRQ) & IRQ_ADC_CH1_APB_UF, ==, 0);

    /* IRQ_MSK is plain storage of the same bit positions. */
    ac_write(qts, A_IRQ_MSK, IRQ_DAC_CH0_APB_OF | IRQ_ADC_CH0_APB_UF);
    g_assert_cmphex(ac_read(qts, A_IRQ_MSK),
                    ==, IRQ_DAC_CH0_APB_OF | IRQ_ADC_CH0_APB_UF);
    ac_write(qts, A_IRQ_MSK, 0);

    qtest_quit(qts);
}

static void test_apb_fifo(void)
{
    QTestState *qts = audcodec_qtest_start();
    uint32_t stat;

    /* DAC CH0 push: APB_STAT counts and loopback into the ADC RX FIFO. */
    ac_write(qts, A_DAC_CH0_ENTRY, 0x11111111);
    ac_write(qts, A_DAC_CH0_ENTRY, 0x22222222);
    ac_write(qts, A_DAC_CH0_ENTRY, 0x33333333);

    stat = ac_read(qts, A_APB_STAT);
    g_assert_cmphex(apb_cnt(stat, APB_DAC0_SHIFT), ==, 3);
    g_assert_cmphex(apb_cnt(stat, APB_DAC1_SHIFT), ==, 0);
    g_assert_cmphex(apb_cnt(stat, APB_ADC0_SHIFT), ==, 3);
    g_assert_cmphex(apb_cnt(stat, APB_ADC1_SHIFT), ==, 0);

    /* Reading DAC entry returns the last value written. */
    g_assert_cmphex(ac_read(qts, A_DAC_CH0_ENTRY), ==, 0x33333333);

    /* ADC CH0 pop: FIFO order preserved, counts drop to zero. */
    g_assert_cmphex(ac_read(qts, A_ADC_CH0_ENTRY), ==, 0x11111111);
    g_assert_cmphex(ac_read(qts, A_ADC_CH0_ENTRY), ==, 0x22222222);
    g_assert_cmphex(ac_read(qts, A_ADC_CH0_ENTRY), ==, 0x33333333);
    stat = ac_read(qts, A_APB_STAT);
    g_assert_cmphex(apb_cnt(stat, APB_ADC0_SHIFT), ==, 0);
    g_assert_cmphex(apb_cnt(stat, APB_DAC0_SHIFT), ==, 3);

    /* Empty ADC FIFO: read returns 0 and raises the underflow flag. */
    g_assert_cmphex(ac_read(qts, A_ADC_CH0_ENTRY), ==, 0);
    g_assert_cmphex(ac_read(qts, A_IRQ) & IRQ_ADC_CH0_APB_UF,
                    ==, IRQ_ADC_CH0_APB_UF);
    ac_write(qts, A_IRQ, IRQ_ADC_CH0_APB_UF);

    /* Channel 1 has an independent FIFO pair. */
    ac_write(qts, A_DAC_CH1_ENTRY, 0xaaaaaaaa);
    ac_write(qts, A_DAC_CH1_ENTRY, 0xbbbbbbbb);
    stat = ac_read(qts, A_APB_STAT);
    g_assert_cmphex(apb_cnt(stat, APB_DAC1_SHIFT), ==, 2);
    g_assert_cmphex(apb_cnt(stat, APB_ADC1_SHIFT), ==, 2);
    g_assert_cmphex(ac_read(qts, A_ADC_CH1_ENTRY), ==, 0xaaaaaaaa);
    g_assert_cmphex(ac_read(qts, A_ADC_CH1_ENTRY), ==, 0xbbbbbbbb);

    qtest_quit(qts);
}

/*
 * Driver-like init sequence (codec_driver_init -> pll_calibration ->
 * configure -> start): every write/read the driver performs must complete and
 * the polled PLL status must be terminal, or the guest would hang.
 */
static void test_driver_like_init(void)
{
    QTestState *qts = audcodec_qtest_start();
    uint32_t value;
    int i;

    /* pll_turn_on() / refgen_init(): analog config is stored, not modelled. */
    ac_write(qts, A_BG_CFG0, 0x00002121);
    ac_write(qts, A_BG_CFG1, 0x00000000);
    ac_write(qts, A_BG_CFG2, 0x00000000);
    ac_write(qts, A_REFGEN_CFG, 0x00000107);
    ac_write(qts, A_PLL_CFG4, 0x00ffc3ff);
    ac_write(qts, A_PLL_CFG5, 0x1fffffff);
    ac_write(qts, A_PLL_CFG6, 0x03ffffff);

    /* pll_calibration(): LEN + EN toggles with DONE polling. */
    ac_write(qts, A_PLL_CFG0, 0x00800000);
    ac_write(qts, A_PLL_CFG2, 0x00004000); /* EN_LF_VCIN */
    ac_write(qts, A_PLL_CAL_CFG, PLL_CAL_LEN(2000));
    for (i = 0; i < 3; i++) {
        ac_write(qts, A_PLL_CAL_CFG,
                 (ac_read(qts, A_PLL_CAL_CFG) & 0xffff0000U) | PLL_CAL_EN);
        g_assert_cmphex(ac_read(qts, A_PLL_CAL_CFG) & PLL_CAL_DONE,
                        ==, PLL_CAL_DONE);
        value = ac_read(qts, A_PLL_CAL_RESULT);
        g_assert_cmphex(value >> 16, ==, PLL_CNT_EXPECT);
        ac_write(qts, A_PLL_CAL_CFG, ac_read(qts, A_PLL_CAL_CFG) & ~PLL_CAL_EN);
    }

    /* pll_update_freq(): lock check must read unlocked == 0. */
    ac_write(qts, A_PLL_CFG2, 0x00004000);
    ac_write(qts, A_PLL_CFG3, 0x00480005);
    g_assert_cmphex(ac_read(qts, A_PLL_STAT) & PLL_STAT_UNLOCK, ==, 0);

    /* codec_configure(): digital config readback. */
    ac_write(qts, A_CFG, CFG_DAC_ENABLE | CFG_ADC_ENABLE);
    ac_write(qts, A_DAC_CFG, 0x00000600);
    ac_write(qts, A_ADC_CFG, 0x00000100);
    ac_write(qts, A_DAC_CH0_CFG, 0x00000081); /* ENABLE | DMA_EN */
    ac_write(qts, A_ADC_CH0_CFG, 0x00000080); /* DMA_EN */
    g_assert_cmphex(ac_read(qts, A_CFG),
                    ==, CFG_DAC_ENABLE | CFG_ADC_ENABLE);
    g_assert_cmphex(ac_read(qts, A_DAC_CFG), ==, 0x00000600);
    g_assert_cmphex(ac_read(qts, A_ADC_CFG), ==, 0x00000100);
    g_assert_cmphex(ac_read(qts, A_DAC_CH0_CFG), ==, 0x00000081);
    g_assert_cmphex(ac_read(qts, A_ADC_CH0_CFG), ==, 0x00000080);

    /* codec_start(TX): DMA/CPU pushes data into the DAC entry FIFO. */
    for (i = 0; i < 4; i++) {
        ac_write(qts, A_DAC_CH0_ENTRY, 0x5a5a0000 + i);
    }
    g_assert_cmphex(apb_cnt(ac_read(qts, A_APB_STAT), APB_DAC0_SHIFT), ==, 4);

    qtest_quit(qts);
}

static void test_unimplemented_offsets(void)
{
    QTestState *qts = audcodec_qtest_start();

    ac_write(qts, 0x1c, 0xffffffff);
    g_assert_cmphex(ac_read(qts, 0x1c), ==, 0);
    ac_write(qts, 0xdc, 0xdeadbeef);
    g_assert_cmphex(ac_read(qts, 0xdc), ==, 0);
    ac_write(qts, 0x3fc, 0xffffffff);
    g_assert_cmphex(ac_read(qts, 0x3fc), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sf32lb52x/audcodec/reset", test_reset);
    qtest_add_func("/sf32lb52x/audcodec/config-readback",
                   test_config_readback);
    qtest_add_func("/sf32lb52x/audcodec/pll-handshake", test_pll_handshake);
    qtest_add_func("/sf32lb52x/audcodec/irq-w1c", test_irq_w1c);
    qtest_add_func("/sf32lb52x/audcodec/apb-fifo", test_apb_fifo);
    qtest_add_func("/sf32lb52x/audcodec/driver-like-init",
                   test_driver_like_init);
    qtest_add_func("/sf32lb52x/audcodec/unimplemented-offsets",
                   test_unimplemented_offsets);

    return g_test_run();
}
