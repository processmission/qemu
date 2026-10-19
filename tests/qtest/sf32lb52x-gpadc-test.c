/*
 * QTest for the SiFli SF32LB52X general purpose SAR ADC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "sf32lb52-devkit-lcd"

#define GPADC_BASE 0x50087000
#define GPADC_CFG1 0x00
#define GPADC_SLOT(n) (0x04 + 4 * (n))
#define GPADC_RDATA(n) (0x24 + 4 * (n))
#define GPADC_DMA_RDATA 0x34
#define GPADC_CTRL 0x38
#define GPADC_CTRL2 0x3c
#define GPADC_STATUS 0x40
#define GPADC_IRQ 0x44

#define CFG1_RESET 0x04714a44
#define SLOT_RESET 0x00020801
#define CTRL_RESET 0x00000130

#define SLOT_EN (1U << 0)
#define SLOT_PCHNL(ch) ((uint32_t)(ch) << 8)

#define CTRL_START (1U << 1)

#define STATUS_ADC_DONE (1U << 0)
#define STATUS_SLOT_DONE_MASK (0xffU << 1)
#define STATUS_CUR_SLOT_MASK (0x7U << 9)

#define IRQ_ICR (1U << 0)
#define IRQ_IMR (1U << 1)
#define IRQ_IRSR (1U << 2)

/* Model default conversion codes (see sf32lb52x_gpadc.c). */
#define CODE(ch) (0x100 + 0x80 * (ch))

static QTestState *gpadc_start(void)
{
    QTestState *qts = qtest_init("-machine " MACHINE " -serial null");

    qtest_irq_intercept_out_named(qts, "/machine/soc/gpadc", "sysbus-irq");
    return qts;
}

static uint32_t gpadc_read(QTestState *qts, uint64_t off)
{
    uint32_t value;

    qtest_memread(qts, GPADC_BASE + off, &value, sizeof(value));
    return value;
}

static void gpadc_write(QTestState *qts, uint64_t off, uint32_t value)
{
    qtest_memwrite(qts, GPADC_BASE + off, &value, sizeof(value));
}

static void gpadc_configure_slot(QTestState *qts, unsigned int slot,
                                 unsigned int channel)
{
    gpadc_write(qts, GPADC_SLOT(slot), SLOT_EN | SLOT_PCHNL(channel));
}

static void gpadc_disable_all_slots(QTestState *qts)
{
    unsigned int i;

    /*
     * SLOTn resets with SLOT_EN set (SVD reset 0x00020801); the driver
     * disables every slot during init before enabling the wanted ones.
     */
    for (i = 0; i < 8; i++) {
        gpadc_write(qts, GPADC_SLOT(i), SLOT_RESET & ~SLOT_EN);
    }
}

static void gpadc_start_conversion(QTestState *qts)
{
    gpadc_write(qts, GPADC_CTRL, CTRL_RESET | CTRL_START);
}

static void test_gpadc_reset(void)
{
    QTestState *qts = gpadc_start();

    g_assert_cmphex(gpadc_read(qts, GPADC_CFG1), ==, CFG1_RESET);
    g_assert_cmphex(gpadc_read(qts, GPADC_SLOT(0)), ==, SLOT_RESET);
    g_assert_cmphex(gpadc_read(qts, GPADC_SLOT(7)), ==, SLOT_RESET);
    g_assert_cmphex(gpadc_read(qts, GPADC_RDATA(0)), ==, 0);
    g_assert_cmphex(gpadc_read(qts, GPADC_CTRL), ==, CTRL_RESET);
    g_assert_cmphex(gpadc_read(qts, GPADC_CTRL2), ==, CTRL_RESET);
    g_assert_cmphex(gpadc_read(qts, GPADC_STATUS), ==, 0);
    g_assert_cmphex(gpadc_read(qts, GPADC_IRQ), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_gpadc_conversion_and_irq(void)
{
    QTestState *qts = gpadc_start();
    uint32_t status;

    gpadc_disable_all_slots(qts);
    gpadc_configure_slot(qts, 0, 0);
    gpadc_configure_slot(qts, 1, 1);
    gpadc_start_conversion(qts);

    /* START is self-clearing; the rest of CTRL keeps its reset value. */
    g_assert_cmphex(gpadc_read(qts, GPADC_CTRL), ==, CTRL_RESET);

    status = gpadc_read(qts, GPADC_STATUS);
    g_assert_cmphex(status & STATUS_ADC_DONE, ==, STATUS_ADC_DONE);
    g_assert_cmphex(status & STATUS_SLOT_DONE_MASK, ==, (1U << 1) | (1U << 2));
    g_assert_cmphex((status & STATUS_CUR_SLOT_MASK) >> 9, ==, 1);

    g_assert_cmphex(gpadc_read(qts, GPADC_RDATA(0)) & 0xfff, ==, CODE(0));
    g_assert_cmphex((gpadc_read(qts, GPADC_RDATA(0)) >> 16) & 0xfff, ==,
                    CODE(1));
    g_assert_cmphex(gpadc_read(qts, GPADC_IRQ) & IRQ_IRSR, ==, IRQ_IRSR);
    g_assert_true(qtest_get_irq(qts, 0));

    gpadc_write(qts, GPADC_IRQ, IRQ_ICR);
    g_assert_cmphex(gpadc_read(qts, GPADC_IRQ), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_gpadc_slot_mapping(void)
{
    QTestState *qts = gpadc_start();

    gpadc_disable_all_slots(qts);
    gpadc_configure_slot(qts, 2, 2);
    gpadc_start_conversion(qts);

    /* Even slots land in bits 11:0 of the RDATA register, slot 2 -> RDATA1. */
    g_assert_cmphex(gpadc_read(qts, GPADC_RDATA(1)) & 0xfff, ==, CODE(2));

    qtest_quit(qts);
}

static void test_gpadc_irq_mask(void)
{
    QTestState *qts = gpadc_start();

    gpadc_disable_all_slots(qts);
    gpadc_configure_slot(qts, 0, 0);
    gpadc_write(qts, GPADC_IRQ, IRQ_IMR);
    gpadc_start_conversion(qts);

    g_assert_cmphex(gpadc_read(qts, GPADC_IRQ) & IRQ_IRSR, ==, IRQ_IRSR);
    g_assert_false(qtest_get_irq(qts, 0));

    gpadc_write(qts, GPADC_IRQ, IRQ_ICR);
    gpadc_write(qts, GPADC_IRQ, 0);
    gpadc_start_conversion(qts);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_gpadc_read_only(void)
{
    QTestState *qts = gpadc_start();

    gpadc_write(qts, GPADC_RDATA(0), 0xdeadbeef);
    gpadc_write(qts, GPADC_STATUS, 0xff);
    gpadc_write(qts, GPADC_DMA_RDATA, 0xffffff);

    g_assert_cmphex(gpadc_read(qts, GPADC_RDATA(0)), ==, 0);
    g_assert_cmphex(gpadc_read(qts, GPADC_STATUS), ==, 0);
    g_assert_cmphex(gpadc_read(qts, GPADC_DMA_RDATA), ==, 0);

    qtest_quit(qts);
}

static void test_gpadc_bad_offset(void)
{
    QTestState *qts = gpadc_start();

    g_assert_cmphex(gpadc_read(qts, 0x800), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/sf32lb52x/gpadc/reset", test_gpadc_reset);
    g_test_add_func("/sf32lb52x/gpadc/conversion-irq",
                    test_gpadc_conversion_and_irq);
    g_test_add_func("/sf32lb52x/gpadc/slot-mapping",
                    test_gpadc_slot_mapping);
    g_test_add_func("/sf32lb52x/gpadc/irq-mask", test_gpadc_irq_mask);
    g_test_add_func("/sf32lb52x/gpadc/read-only", test_gpadc_read_only);
    g_test_add_func("/sf32lb52x/gpadc/bad-offset", test_gpadc_bad_offset);

    return g_test_run();
}
