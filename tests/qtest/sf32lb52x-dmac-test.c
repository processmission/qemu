/*
 * QTest for the SiFli SF32LB52X DMAC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Falsifiable coverage of the DMAC register behavior (SVD DMAC1, vendor HAL
 * dmac.h, Zephyr drivers/dma/dma_sf32lb.c):
 *
 *  - reset values of ISR/IFCR, all 8 channel banks, CSELR1/2 and DBGSEL;
 *  - RW readback with documented reserved-bit masks;
 *  - memory-to-memory copy of CNDTR items between CPAR and CM0AR, checked
 *    against the destination SRAM contents;
 *  - 8/16/32-bit item sizes and PINC/MINC increments;
 *  - DIR=1 (memory-to-peripheral) address mapping;
 *  - CNDTR decrement to 0, automatic EN clear, TCIF/GIF/HTIF flags;
 *  - per-channel level IRQ line, gated by TCIE/HTIE;
 *  - IFCR write-1-to-clear semantics and per-channel isolation;
 *  - CIRC reload of CNDTR/CPAR/CM0AR with EN kept;
 *  - request-gated (MEM2MEM=0) channels completing on enable;
 *  - unimplemented offsets inside the 4 KiB window read as zero.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "sf32lb52-devkit-lcd"
#define DMAC_BASE 0x50081000
#define DMAC_QOM_PATH "/machine/soc/dmac"

#define SRAM_BASE 0x20000000

#define CH_STRIDE 0x14
#define R_ISR 0x00
#define R_IFCR 0x04
#define R_CCR(ch) (0x08 + CH_STRIDE * (ch))
#define R_CNDTR(ch) (R_CCR(ch) + 0x04)
#define R_CPAR(ch) (R_CCR(ch) + 0x08)
#define R_CM0AR(ch) (R_CCR(ch) + 0x0c)
#define R_CBSR(ch) (R_CCR(ch) + 0x10)
#define R_CSELR1 0xa8
#define R_CSELR2 0xac
#define R_DBGSEL 0xb0

/* CCR fields */
#define CCR_EN (1U << 0)
#define CCR_TCIE (1U << 1)
#define CCR_HTIE (1U << 2)
#define CCR_TEIE (1U << 3)
#define CCR_DIR (1U << 4)
#define CCR_CIRC (1U << 5)
#define CCR_PINC (1U << 6)
#define CCR_MINC (1U << 7)
#define CCR_PSIZE(s) (((s) & 3U) << 8)
#define CCR_MSIZE(s) (((s) & 3U) << 10)
#define CCR_PL(p) (((p) & 3U) << 12)
#define CCR_MEM2MEM (1U << 14)

/* ISR/IFCR bits: 4 per channel, GIF/TCIF/HTIF/TEIF ascending */
#define ISR_GIF(ch) (1U << (4 * (ch) + 0))
#define ISR_TCIF(ch) (1U << (4 * (ch) + 1))
#define ISR_HTIF(ch) (1U << (4 * (ch) + 2))
#define ISR_TEIF(ch) (1U << (4 * (ch) + 3))
#define ISR_CH_MASK(ch) (0xFU << (4 * (ch)))

#define IFCR_CGIF(ch) ISR_GIF(ch)
#define IFCR_CTCIF(ch) ISR_TCIF(ch)
#define IFCR_CHTIF(ch) ISR_HTIF(ch)
#define IFCR_CTEIF(ch) ISR_TEIF(ch)

#define NUM_CHANNELS 8

static QTestState *dmac_qtest_start(void)
{
    QTestState *qts = qtest_init("-machine " MACHINE " -serial null");

    qtest_irq_intercept_out_named(qts, DMAC_QOM_PATH, "sysbus-irq");

    return qts;
}

static uint32_t dmac_read(QTestState *qts, unsigned int off)
{
    return qtest_readl(qts, DMAC_BASE + off);
}

static void dmac_write(QTestState *qts, unsigned int off, uint32_t value)
{
    qtest_writel(qts, DMAC_BASE + off, value);
}

static void dmac_config(QTestState *qts, unsigned int ch, uint32_t ccr,
                        uint32_t ndt, uint32_t cpar, uint32_t cm0ar)
{
    dmac_write(qts, R_CCR(ch), ccr);
    dmac_write(qts, R_CNDTR(ch), ndt);
    dmac_write(qts, R_CPAR(ch), cpar);
    dmac_write(qts, R_CM0AR(ch), cm0ar);
}

static bool dmac_wait_flags(QTestState *qts, unsigned int ch, uint32_t mask)
{
    int i;

    for (i = 0; i < 1000; i++) {
        if ((dmac_read(qts, R_ISR) & mask) == mask) {
            return true;
        }
    }

    return false;
}

static void test_reset(void)
{
    QTestState *qts = dmac_qtest_start();
    int ch;

    g_assert_cmphex(dmac_read(qts, R_ISR), ==, 0);
    g_assert_cmphex(dmac_read(qts, R_IFCR), ==, 0);

    for (ch = 0; ch < NUM_CHANNELS; ch++) {
        g_assert_cmphex(dmac_read(qts, R_CCR(ch)), ==, 0);
        g_assert_cmphex(dmac_read(qts, R_CNDTR(ch)), ==, 0);
        g_assert_cmphex(dmac_read(qts, R_CPAR(ch)), ==, 0);
        g_assert_cmphex(dmac_read(qts, R_CM0AR(ch)), ==, 0);
        g_assert_cmphex(dmac_read(qts, R_CBSR(ch)), ==, 0);
    }

    g_assert_cmphex(dmac_read(qts, R_CSELR1), ==, 0);
    g_assert_cmphex(dmac_read(qts, R_CSELR2), ==, 0);
    g_assert_cmphex(dmac_read(qts, R_DBGSEL), ==, 0);

    qtest_quit(qts);
}

static void test_registers(void)
{
    QTestState *qts = dmac_qtest_start();

    /* CCR: bits [14:0] writable, EN=0 configuration round-trips. */
    dmac_write(qts, R_CCR(0), 0x7ffe);
    g_assert_cmphex(dmac_read(qts, R_CCR(0)), ==, 0x7ffe);
    /* Reserved bits [31:15] are ignored and read back as zero. */
    dmac_write(qts, R_CCR(0), 0xffff7ffe);
    g_assert_cmphex(dmac_read(qts, R_CCR(0)), ==, 0x7ffe);

    /* CNDTR: NDT[15:0] only. */
    dmac_write(qts, R_CNDTR(0), 0x1234);
    g_assert_cmphex(dmac_read(qts, R_CNDTR(0)), ==, 0x1234);
    dmac_write(qts, R_CNDTR(0), 0xffffffff);
    g_assert_cmphex(dmac_read(qts, R_CNDTR(0)), ==, 0xffff);
    dmac_write(qts, R_CNDTR(0), 0x1234);

    /* CPAR/CM0AR are full 32-bit RW. */
    dmac_write(qts, R_CPAR(0), 0xffffffff);
    g_assert_cmphex(dmac_read(qts, R_CPAR(0)), ==, 0xffffffff);
    dmac_write(qts, R_CM0AR(0), 0xdeadbeef);
    g_assert_cmphex(dmac_read(qts, R_CM0AR(0)), ==, 0xdeadbeef);

    /* CBSR: BS[7:0] only. */
    dmac_write(qts, R_CBSR(0), 0x5a);
    g_assert_cmphex(dmac_read(qts, R_CBSR(0)), ==, 0x5a);
    dmac_write(qts, R_CBSR(0), 0xffffffff);
    g_assert_cmphex(dmac_read(qts, R_CBSR(0)), ==, 0xff);

    /* CSELR1/2: four 6-bit request selectors each. */
    dmac_write(qts, R_CSELR1, 0xffffffff);
    g_assert_cmphex(dmac_read(qts, R_CSELR1), ==, 0x3f3f3f3f);
    dmac_write(qts, R_CSELR1, 0x3f21150a);
    g_assert_cmphex(dmac_read(qts, R_CSELR1), ==, 0x3f21150a);
    dmac_write(qts, R_CSELR2, 0xffffffff);
    g_assert_cmphex(dmac_read(qts, R_CSELR2), ==, 0x3f3f3f3f);

    /* DBGSEL is a plain store-only register (vendor HAL layout). */
    dmac_write(qts, R_DBGSEL, 0xa5a5a5a5);
    g_assert_cmphex(dmac_read(qts, R_DBGSEL), ==, 0xa5a5a5a5);

    qtest_quit(qts);
}

static void test_mem2mem_word(void)
{
    QTestState *qts = dmac_qtest_start();
    static const uint32_t src[4] = {
        0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00
    };
    static const uint32_t pattern[4] = {
        0xeeeeeeee, 0xeeeeeeee, 0xeeeeeeee, 0xeeeeeeee
    };
    uint32_t dst[4];
    uint32_t ccr = CCR_MEM2MEM | CCR_MINC | CCR_PINC |
                   CCR_PSIZE(2) | CCR_MSIZE(2) | CCR_TCIE;
    uint32_t isr;
    int ch;

    qtest_memwrite(qts, SRAM_BASE + 0x000, src, sizeof(src));
    qtest_memwrite(qts, SRAM_BASE + 0x400, pattern, sizeof(pattern));

    dmac_write(qts, R_IFCR, 0xffffffff);
    dmac_config(qts, 0, ccr, 4, SRAM_BASE + 0x000, SRAM_BASE + 0x400);
    dmac_write(qts, R_CCR(0), ccr | CCR_EN);

    g_assert_true(dmac_wait_flags(qts, 0, ISR_TCIF(0)));

    qtest_memread(qts, SRAM_BASE + 0x400, dst, sizeof(dst));
    g_assert_cmpmem(dst, sizeof(dst), src, sizeof(src));
    g_assert_cmphex(dmac_read(qts, R_CNDTR(0)), ==, 0);
    g_assert_cmphex(dmac_read(qts, R_CCR(0)) & CCR_EN, ==, 0);

    isr = dmac_read(qts, R_ISR);
    g_assert_cmphex(isr & ISR_CH_MASK(0),
                    ==, ISR_GIF(0) | ISR_TCIF(0) | ISR_HTIF(0));
    g_assert_cmphex(isr & ISR_TEIF(0), ==, 0);

    g_assert_true(qtest_get_irq(qts, 0));
    for (ch = 1; ch < NUM_CHANNELS; ch++) {
        g_assert_false(qtest_get_irq(qts, ch));
    }

    /* W1C: clear TCIF only; HTIF and the sticky GIF stay set. */
    dmac_write(qts, R_IFCR, IFCR_CTCIF(0));
    isr = dmac_read(qts, R_ISR);
    g_assert_cmphex(isr & ISR_TCIF(0), ==, 0);
    g_assert_cmphex(isr & ISR_HTIF(0), ==, ISR_HTIF(0));
    g_assert_cmphex(isr & ISR_GIF(0), ==, ISR_GIF(0));
    g_assert_false(qtest_get_irq(qts, 0));

    /* CGIF clears all four flags of the channel. */
    dmac_write(qts, R_IFCR, IFCR_CGIF(0));
    g_assert_cmphex(dmac_read(qts, R_ISR) & ISR_CH_MASK(0), ==, 0);

    qtest_quit(qts);
}

static void test_datasize(void)
{
    QTestState *qts = dmac_qtest_start();
    uint8_t data[16];
    uint8_t copy[16];
    int i;

    for (i = 0; i < sizeof(data); i++) {
        data[i] = 0x80 + i;
    }
    memset(copy, 0x5a, sizeof(copy));

    qtest_memwrite(qts, SRAM_BASE + 0x000, data, sizeof(data));

    /* 8-bit items, 16 transfers. */
    qtest_memwrite(qts, SRAM_BASE + 0x040, copy, sizeof(copy));
    dmac_config(qts, 1, CCR_MEM2MEM | CCR_MINC | CCR_PINC |
                CCR_PSIZE(0) | CCR_MSIZE(0) | CCR_TCIE,
                sizeof(data), SRAM_BASE + 0x000, SRAM_BASE + 0x040);
    dmac_write(qts, R_CCR(1), dmac_read(qts, R_CCR(1)) | CCR_EN);
    g_assert_true(dmac_wait_flags(qts, 1, ISR_TCIF(1)));
    qtest_memread(qts, SRAM_BASE + 0x040, copy, sizeof(copy));
    g_assert_cmpmem(copy, sizeof(copy), data, sizeof(data));

    /* 16-bit items, 8 transfers. */
    memset(copy, 0x5a, sizeof(copy));
    qtest_memwrite(qts, SRAM_BASE + 0x080, copy, sizeof(copy));
    dmac_config(qts, 2, CCR_MEM2MEM | CCR_MINC | CCR_PINC |
                CCR_PSIZE(1) | CCR_MSIZE(1) | CCR_TCIE,
                sizeof(data) / 2, SRAM_BASE + 0x000, SRAM_BASE + 0x080);
    dmac_write(qts, R_CCR(2), dmac_read(qts, R_CCR(2)) | CCR_EN);
    g_assert_true(dmac_wait_flags(qts, 2, ISR_TCIF(2)));
    qtest_memread(qts, SRAM_BASE + 0x080, copy, sizeof(copy));
    g_assert_cmpmem(copy, sizeof(copy), data, sizeof(data));

    qtest_quit(qts);
}

static void test_direction_m2p(void)
{
    QTestState *qts = dmac_qtest_start();
    static const uint32_t src[4] = { 1, 2, 3, 4 };
    uint32_t dst[4];

    /*
     * MEMORY_TO_PERIPHERAL mapping: CPAR = destination, CM0AR = source,
     * DIR=1, PSIZE describes the peripheral (destination) side.
     */
    qtest_memwrite(qts, SRAM_BASE + 0x100, src, sizeof(src));
    memset(dst, 0, sizeof(dst));
    qtest_memwrite(qts, SRAM_BASE + 0x800, dst, sizeof(dst));

    /*
     * MINC advances the memory source; the fixed peripheral address only
     * receives the last item.
     */
    dmac_config(qts, 3, CCR_MEM2MEM | CCR_DIR | CCR_MINC |
                CCR_PSIZE(2) | CCR_MSIZE(2) | CCR_TCIE,
                4, SRAM_BASE + 0x800, SRAM_BASE + 0x100);
    dmac_write(qts, R_CCR(3), dmac_read(qts, R_CCR(3)) | CCR_EN);
    g_assert_true(dmac_wait_flags(qts, 3, ISR_TCIF(3)));

    qtest_memread(qts, SRAM_BASE + 0x800, dst, sizeof(dst));
    g_assert_cmpuint(dst[0], ==, src[3]);
    g_assert_cmpuint(dst[1], ==, 0);
    g_assert_cmpuint(dst[3], ==, 0);

    /* PINC advances the peripheral destination; MINC stays clear. */
    memset(dst, 0, sizeof(dst));
    qtest_memwrite(qts, SRAM_BASE + 0x900, dst, sizeof(dst));
    dmac_config(qts, 3, CCR_MEM2MEM | CCR_DIR | CCR_PINC |
                CCR_PSIZE(2) | CCR_MSIZE(2) | CCR_TCIE,
                4, SRAM_BASE + 0x900, SRAM_BASE + 0x100);
    dmac_write(qts, R_IFCR, IFCR_CGIF(3));
    dmac_write(qts, R_CCR(3), dmac_read(qts, R_CCR(3)) | CCR_EN);
    g_assert_true(dmac_wait_flags(qts, 3, ISR_TCIF(3)));

    qtest_memread(qts, SRAM_BASE + 0x900, dst, sizeof(dst));
    g_assert_cmpuint(dst[0], ==, src[0]);
    g_assert_cmpuint(dst[1], ==, src[0]);
    g_assert_cmpuint(dst[2], ==, src[0]);
    g_assert_cmpuint(dst[3], ==, src[0]);

    qtest_quit(qts);
}

static void test_circular(void)
{
    QTestState *qts = dmac_qtest_start();
    static const uint32_t src[4] = { 0xaa, 0xbb, 0xcc, 0xdd };
    static const uint32_t zero[4] = { 0, 0, 0, 0 };
    uint32_t ccr = CCR_MEM2MEM | CCR_CIRC | CCR_MINC | CCR_PINC |
                   CCR_PSIZE(2) | CCR_MSIZE(2) | CCR_TCIE;

    qtest_memwrite(qts, SRAM_BASE + 0x200, src, sizeof(src));
    qtest_memwrite(qts, SRAM_BASE + 0x240, zero, sizeof(zero));

    dmac_config(qts, 4, ccr, 4, SRAM_BASE + 0x200, SRAM_BASE + 0x240);
    dmac_write(qts, R_CCR(4), ccr | CCR_EN);
    g_assert_true(dmac_wait_flags(qts, 4, ISR_TCIF(4)));

    /* Circular pass reloads the programmed values and keeps EN set. */
    g_assert_cmphex(dmac_read(qts, R_CNDTR(4)), ==, 4);
    g_assert_cmphex(dmac_read(qts, R_CPAR(4)), ==, SRAM_BASE + 0x200);
    g_assert_cmphex(dmac_read(qts, R_CM0AR(4)), ==, SRAM_BASE + 0x240);
    g_assert_cmphex(dmac_read(qts, R_CCR(4)) & CCR_EN, ==, CCR_EN);
    g_assert_true(qtest_get_irq(qts, 4));

    /* Software stop: clearing EN stops further passes and clears the IRQ. */
    dmac_write(qts, R_CCR(4), dmac_read(qts, R_CCR(4)) & ~CCR_EN);
    dmac_write(qts, R_IFCR, IFCR_CGIF(4));
    g_assert_false(qtest_get_irq(qts, 4));
    g_assert_cmphex(dmac_read(qts, R_ISR) & ISR_CH_MASK(4), ==, 0);

    qtest_quit(qts);
}

static void test_htie_irq(void)
{
    QTestState *qts = dmac_qtest_start();
    static const uint32_t src[4] = { 1, 2, 3, 4 };
    uint32_t ccr = CCR_MEM2MEM | CCR_MINC | CCR_PINC |
                   CCR_PSIZE(2) | CCR_MSIZE(2) | CCR_HTIE;

    qtest_memwrite(qts, SRAM_BASE + 0x300, src, sizeof(src));
    dmac_config(qts, 5, ccr, 4, SRAM_BASE + 0x300, SRAM_BASE + 0x340);
    dmac_write(qts, R_CCR(5), ccr | CCR_EN);
    g_assert_true(dmac_wait_flags(qts, 5, ISR_TCIF(5)));

    g_assert_cmphex(dmac_read(qts, R_ISR) & ISR_HTIF(5), ==, ISR_HTIF(5));
    /* HTIE (not TCIE) is the enabled source, so the line is raised. */
    g_assert_true(qtest_get_irq(qts, 5));

    dmac_write(qts, R_IFCR, IFCR_CHTIF(5));
    g_assert_cmphex(dmac_read(qts, R_ISR) & ISR_HTIF(5), ==, 0);
    g_assert_cmphex(dmac_read(qts, R_ISR) & ISR_TCIF(5), ==, ISR_TCIF(5));
    g_assert_false(qtest_get_irq(qts, 5));

    dmac_write(qts, R_IFCR, IFCR_CGIF(5));
    qtest_quit(qts);
}

static void test_per_channel_irq(void)
{
    QTestState *qts = dmac_qtest_start();
    static const uint32_t src[4] = { 9, 8, 7, 6 };
    uint32_t ccr = CCR_MEM2MEM | CCR_MINC | CCR_PINC |
                   CCR_PSIZE(2) | CCR_MSIZE(2) | CCR_TCIE;

    qtest_memwrite(qts, SRAM_BASE + 0x380, src, sizeof(src));
    dmac_config(qts, 7, ccr, 4, SRAM_BASE + 0x380, SRAM_BASE + 0x3c0);
    dmac_write(qts, R_CCR(7), ccr | CCR_EN);
    g_assert_true(dmac_wait_flags(qts, 7, ISR_TCIF(7)));

    g_assert_true(qtest_get_irq(qts, 7));
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 6));

    dmac_write(qts, R_IFCR, IFCR_CGIF(7));
    g_assert_false(qtest_get_irq(qts, 7));

    qtest_quit(qts);
}

static void test_ifcr_isolation(void)
{
    QTestState *qts = dmac_qtest_start();
    static const uint32_t src[4] = { 0x10, 0x20, 0x30, 0x40 };
    uint32_t ccr = CCR_MEM2MEM | CCR_MINC | CCR_PINC |
                   CCR_PSIZE(2) | CCR_MSIZE(2) | CCR_TCIE;

    qtest_memwrite(qts, SRAM_BASE + 0x500, src, sizeof(src));

    dmac_config(qts, 0, ccr, 4, SRAM_BASE + 0x500, SRAM_BASE + 0x540);
    dmac_write(qts, R_CCR(0), ccr | CCR_EN);
    g_assert_true(dmac_wait_flags(qts, 0, ISR_TCIF(0)));

    dmac_config(qts, 1, ccr, 4, SRAM_BASE + 0x500, SRAM_BASE + 0x580);
    dmac_write(qts, R_CCR(1), ccr | CCR_EN);
    g_assert_true(dmac_wait_flags(qts, 1, ISR_TCIF(1)));

    /* Clearing channel 0 must not disturb channel 1. */
    dmac_write(qts, R_IFCR, IFCR_CGIF(0));
    g_assert_cmphex(dmac_read(qts, R_ISR) & ISR_CH_MASK(0), ==, 0);
    g_assert_cmphex(dmac_read(qts, R_ISR) & ISR_TCIF(1), ==, ISR_TCIF(1));
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    dmac_write(qts, R_IFCR, IFCR_CGIF(1));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

static void test_request_gated_on_enable(void)
{
    QTestState *qts = dmac_qtest_start();
    static const uint32_t src[4] = { 0x11223344, 0x55667788, 0x99aabbcc,
                                     0xddeeff00 };
    uint32_t dst[4] = { 0, 0, 0, 0 };
    uint32_t ccr = CCR_DIR | CCR_MINC | CCR_PINC |
                   CCR_PSIZE(2) | CCR_MSIZE(2) | CCR_TCIE;

    qtest_memwrite(qts, SRAM_BASE + 0x600, src, sizeof(src));
    qtest_memwrite(qts, SRAM_BASE + 0x640, dst, sizeof(dst));
    dmac_write(qts, R_IFCR, 0xffffffff);

    /*
     * MEM2MEM=0: request lines are not modeled, but the channel runs as
     * soon as EN is set with CPAR (destination) and CM0AR (source) as the
     * endpoints. This is the model the MPI QSPI flash driver relies on.
     */
    dmac_config(qts, 6, ccr, 4, SRAM_BASE + 0x640, SRAM_BASE + 0x600);
    dmac_write(qts, R_CCR(6), ccr | CCR_EN);

    g_assert_true(dmac_wait_flags(qts, 6, ISR_TCIF(6)));
    qtest_memread(qts, SRAM_BASE + 0x640, dst, sizeof(dst));
    g_assert_cmpmem(dst, sizeof(dst), src, sizeof(src));
    g_assert_cmphex(dmac_read(qts, R_CNDTR(6)), ==, 0);
    g_assert_cmphex(dmac_read(qts, R_CCR(6)) & CCR_EN, ==, 0);
    g_assert_true(qtest_get_irq(qts, 6));
    g_assert_cmphex(dmac_read(qts, R_ISR) & ISR_TEIF(6), ==, 0);

    dmac_write(qts, R_IFCR, IFCR_CGIF(6));
    g_assert_cmphex(dmac_read(qts, R_ISR) & ISR_CH_MASK(6), ==, 0);

    qtest_quit(qts);
}

static void test_unimplemented_offsets(void)
{
    QTestState *qts = dmac_qtest_start();

    g_assert_cmphex(dmac_read(qts, 0xb4), ==, 0);
    g_assert_cmphex(dmac_read(qts, 0xffc), ==, 0);
    dmac_write(qts, 0xb4, 0xdeadbeef);
    g_assert_cmphex(dmac_read(qts, 0xb4), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sf32lb52x/dmac/reset", test_reset);
    qtest_add_func("/sf32lb52x/dmac/registers", test_registers);
    qtest_add_func("/sf32lb52x/dmac/mem2mem-word", test_mem2mem_word);
    qtest_add_func("/sf32lb52x/dmac/datasize", test_datasize);
    qtest_add_func("/sf32lb52x/dmac/direction-m2p", test_direction_m2p);
    qtest_add_func("/sf32lb52x/dmac/circular", test_circular);
    qtest_add_func("/sf32lb52x/dmac/htie-irq", test_htie_irq);
    qtest_add_func("/sf32lb52x/dmac/per-channel-irq", test_per_channel_irq);
    qtest_add_func("/sf32lb52x/dmac/ifcr-isolation", test_ifcr_isolation);
    qtest_add_func("/sf32lb52x/dmac/request-gated-unimplemented",
                   test_request_gated_on_enable);
    qtest_add_func("/sf32lb52x/dmac/unimplemented-offsets",
                   test_unimplemented_offsets);

    return g_test_run();
}
