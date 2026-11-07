/*
 * QTest for the SiFli SF32LB52X temperature sensor
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "sf32lb52-devkit-lcd"

#define TSEN_BASE 0x50089000
#define TSEN_CTRL 0x00
#define TSEN_RDATA 0x04
#define TSEN_IRQ 0x08

#define CTRL_RESET 0x04714a44
#define CTRL_PU (1U << 0)
#define CTRL_RSTB (1U << 1)
#define CTRL_RUN (1U << 2)
#define CTRL_RDY (1U << 10)

#define IRQ_ICR (1U << 0)
#define IRQ_IMR (1U << 1)
#define IRQ_IRSR (1U << 2)

/* 25 degrees C code derived from the Zephyr driver formula. */
#define RDATA_25C 1071

/* One model conversion period (100 Hz). */
#define CONV_PERIOD_NS (10 * 1000 * 1000)

static QTestState *tsen_start(void)
{
    return qtest_init("-machine " MACHINE " -serial null");
}

static uint32_t tsen_read(QTestState *qts, uint64_t off)
{
    uint32_t value;

    qtest_memread(qts, TSEN_BASE + off, &value, sizeof(value));
    return value;
}

static void tsen_write(QTestState *qts, uint64_t off, uint32_t value)
{
    qtest_memwrite(qts, TSEN_BASE + off, &value, sizeof(value));
}

static void test_tsen_reset(void)
{
    QTestState *qts = tsen_start();

    g_assert_cmphex(tsen_read(qts, TSEN_CTRL), ==, CTRL_RESET);
    g_assert_cmphex(tsen_read(qts, TSEN_RDATA), ==, 0);
    g_assert_cmphex(tsen_read(qts, TSEN_IRQ), ==, 0);

    qtest_quit(qts);
}

/*
 * Follow the Zephyr power-up sequence (clear RSTB, enable, power up, release
 * reset, run) and check the ready bit, the first conversion and the periodic
 * refresh.
 */
static void test_tsen_run_and_convert(void)
{
    QTestState *qts = tsen_start();
    uint32_t ctrl = CTRL_RESET;

    tsen_write(qts, TSEN_CTRL, ctrl & ~CTRL_RSTB);
    tsen_write(qts, TSEN_CTRL, ctrl | CTRL_PU);
    tsen_write(qts, TSEN_CTRL, ctrl | CTRL_PU | CTRL_RSTB);

    g_assert_cmphex(tsen_read(qts, TSEN_CTRL) & CTRL_RDY, ==, CTRL_RDY);
    g_assert_cmphex(tsen_read(qts, TSEN_CTRL) & CTRL_RUN, ==, CTRL_RUN);
    g_assert_cmphex(tsen_read(qts, TSEN_RDATA), ==, RDATA_25C);
    g_assert_cmphex(tsen_read(qts, TSEN_IRQ) & IRQ_IRSR, ==, IRQ_IRSR);

    /* The driver clears IRSR after reading RDATA. */
    tsen_write(qts, TSEN_IRQ, IRQ_ICR);
    g_assert_cmphex(tsen_read(qts, TSEN_IRQ), ==, 0);

    /* A later conversion refreshes the raw status. */
    qtest_clock_step(qts, CONV_PERIOD_NS);
    g_assert_cmphex(tsen_read(qts, TSEN_IRQ) & IRQ_IRSR, ==, IRQ_IRSR);
    g_assert_cmphex(tsen_read(qts, TSEN_RDATA), ==, RDATA_25C);

    qtest_quit(qts);
}

static void test_tsen_rdata_ro(void)
{
    QTestState *qts = tsen_start();

    tsen_write(qts, TSEN_RDATA, 0x123);
    g_assert_cmphex(tsen_read(qts, TSEN_RDATA), ==, 0);

    qtest_quit(qts);
}

static void test_tsen_bad_offset(void)
{
    QTestState *qts = tsen_start();

    g_assert_cmphex(tsen_read(qts, 0x800), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/sf32lb52x/tsen/reset", test_tsen_reset);
    g_test_add_func("/sf32lb52x/tsen/run-convert", test_tsen_run_and_convert);
    g_test_add_func("/sf32lb52x/tsen/rdata-ro", test_tsen_rdata_ro);
    g_test_add_func("/sf32lb52x/tsen/bad-offset", test_tsen_bad_offset);

    return g_test_run();
}
