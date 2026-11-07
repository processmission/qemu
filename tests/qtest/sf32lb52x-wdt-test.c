/*
 * QTest for the SiFli SF32LB52X watchdog timer
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "sf32lb52-devkit-lcd"

#define WDT_BASE   0x50094000
#define WDT_CVR0   0x00
#define WDT_CVR1   0x04
#define WDT_CR     0x08
#define WDT_CCR    0x0c
#define WDT_SR     0x14
#define WDT_WP     0x18
#define WDT_FG     0x1c

#define CR_RESPONSE_MODE (1U << 4)
#define CCR_START  0x76
#define CCR_STOP   0x34
#define SR_INT     (1U << 0)
#define SR_ACTIVE  (1U << 1)
#define FG_RST_FG  (1U << 1)

static QTestState *wdt_start(void)
{
    QTestState *qts = qtest_init("-machine " MACHINE " -serial null");

    qtest_irq_intercept_out_named(qts, "/machine/soc/wdt", "sysbus-irq");
    return qts;
}

static uint32_t wdt_read(QTestState *qts, uint64_t off)
{
    uint32_t value;

    qtest_memread(qts, WDT_BASE + off, &value, sizeof(value));
    return value;
}

static void wdt_write(QTestState *qts, uint64_t off, uint32_t value)
{
    qtest_memwrite(qts, WDT_BASE + off, &value, sizeof(value));
}

static void test_wdt_reset(void)
{
    QTestState *qts = wdt_start();

    g_assert_cmpuint(wdt_read(qts, WDT_SR), ==, 0);
    g_assert_cmpuint(wdt_read(qts, WDT_CVR0), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

/*
 * Mode 1: the first timeout raises the interrupt, a start/feed clears it,
 * the CCR command value is readable back (driver contract) and stop clears
 * SR.ACTIVE.
 */
static void test_wdt_mode1_irq_and_feed(void)
{
    QTestState *qts = wdt_start();

    wdt_write(qts, WDT_CR, CR_RESPONSE_MODE);
    wdt_write(qts, WDT_CVR0, 100);
    wdt_write(qts, WDT_CCR, CCR_START);
    g_assert_cmpuint(wdt_read(qts, WDT_SR) & SR_ACTIVE, ==, SR_ACTIVE);
    g_assert_cmpuint(wdt_read(qts, WDT_CCR), ==, CCR_START);

    qtest_clock_step(qts, 5 * 1000 * 1000);
    g_assert_cmpuint(wdt_read(qts, WDT_SR) & SR_INT, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, 20 * 1000 * 1000);
    g_assert_cmpuint(wdt_read(qts, WDT_SR) & SR_INT, ==, SR_INT);
    g_assert_true(qtest_get_irq(qts, 0));

    /* Feed clears the pending interrupt. */
    wdt_write(qts, WDT_CCR, CCR_START);
    g_assert_cmpuint(wdt_read(qts, WDT_SR) & SR_INT, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    wdt_write(qts, WDT_CCR, CCR_STOP);
    g_assert_cmpuint(wdt_read(qts, WDT_SR) & SR_ACTIVE, ==, 0);
    g_assert_cmpuint(wdt_read(qts, WDT_CCR), ==, CCR_STOP);

    qtest_quit(qts);
}

/* Mode 0 (Zephyr default): a timeout resets the system. */
static void test_wdt_mode0_reset(void)
{
    QTestState *qts = wdt_start();

    wdt_write(qts, WDT_CR, 0);
    wdt_write(qts, WDT_CVR0, 10);
    wdt_write(qts, WDT_CCR, CCR_START);
    g_assert_cmpuint(wdt_read(qts, WDT_SR) & SR_ACTIVE, ==, SR_ACTIVE);

    qtest_clock_step(qts, 2 * 1000 * 1000);
    qtest_qmp_eventwait(qts, "RESET");

    /* Devices are reset, the watchdog reset cause survives. */
    g_assert_cmpuint(wdt_read(qts, WDT_SR), ==, 0);
    g_assert_cmpuint(wdt_read(qts, WDT_CCR), ==, 0);
    g_assert_cmpuint(wdt_read(qts, WDT_FG) & FG_RST_FG, ==, FG_RST_FG);

    qtest_quit(qts);
}

static void test_wdt_write_protect(void)
{
    QTestState *qts = wdt_start();

    wdt_write(qts, WDT_WP, 0x58ab99fc);
    g_assert_cmpuint(wdt_read(qts, WDT_WP) >> 31, ==, 1);

    /* Protected writes are ignored. */
    wdt_write(qts, WDT_CCR, CCR_START);
    g_assert_cmpuint(wdt_read(qts, WDT_SR) & SR_ACTIVE, ==, 0);

    wdt_write(qts, WDT_WP, 0x51ff8621);
    g_assert_cmpuint(wdt_read(qts, WDT_WP) >> 31, ==, 0);
    wdt_write(qts, WDT_CCR, CCR_START);
    g_assert_cmpuint(wdt_read(qts, WDT_SR) & SR_ACTIVE, ==, SR_ACTIVE);

    qtest_quit(qts);
}

/*
 * Mode 1, second stage: after the first timeout raises IRQ26, the second
 * timeout (CVR1) resets the system unless the watchdog is fed.
 */
static void test_wdt_mode1_second_stage_reset(void)
{
    QTestState *qts = wdt_start();

    wdt_write(qts, WDT_CR, CR_RESPONSE_MODE);
    wdt_write(qts, WDT_CVR0, 100);   /* 10 ms at 10 kHz */
    wdt_write(qts, WDT_CVR1, 100);   /* second stage: 10 ms */
    wdt_write(qts, WDT_CCR, CCR_START);

    /*
     * First stage expires at 10 ms, the second stage is then armed for
     * another 10 ms: stop between the two (15 ms) to observe the IRQ
     * before the reset happens.
     */
    qtest_clock_step(qts, 15 * 1000 * 1000);
    g_assert_cmpuint(wdt_read(qts, WDT_SR) & SR_INT, ==, SR_INT);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(wdt_read(qts, WDT_FG) & FG_RST_FG, ==, 0);

    /* Second stage expires without a feed: the system is reset. */
    qtest_clock_step(qts, 10 * 1000 * 1000);
    qtest_qmp_eventwait(qts, "RESET");

    g_assert_cmpuint(wdt_read(qts, WDT_SR), ==, 0);
    g_assert_cmpuint(wdt_read(qts, WDT_CCR), ==, 0);
    g_assert_cmpuint(wdt_read(qts, WDT_FG) & FG_RST_FG, ==, FG_RST_FG);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

/*
 * Mode 1 with CVR1 == 0: the second timer is never armed, so the first
 * timeout only raises the interrupt and the system is never reset.
 * Documented model behavior for the second-stage timeout.
 */
static void test_wdt_mode1_cvr1_zero_no_reset(void)
{
    QTestState *qts = wdt_start();

    wdt_write(qts, WDT_CR, CR_RESPONSE_MODE);
    wdt_write(qts, WDT_CVR0, 100);
    wdt_write(qts, WDT_CVR1, 0);
    wdt_write(qts, WDT_CCR, CCR_START);

    qtest_clock_step(qts, 20 * 1000 * 1000);
    g_assert_cmpuint(wdt_read(qts, WDT_SR) & SR_INT, ==, SR_INT);

    /* No reset happened: CCR/SR/FG still describe the running watchdog. */
    qtest_clock_step(qts, 200 * 1000 * 1000);
    g_assert_cmpuint(wdt_read(qts, WDT_CCR), ==, CCR_START);
    g_assert_cmpuint(wdt_read(qts, WDT_SR) & SR_ACTIVE, ==, SR_ACTIVE);
    g_assert_cmpuint(wdt_read(qts, WDT_FG) & FG_RST_FG, ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/sf32lb52x/wdt/reset", test_wdt_reset);
    g_test_add_func("/sf32lb52x/wdt/mode1-irq-feed",
                    test_wdt_mode1_irq_and_feed);
    g_test_add_func("/sf32lb52x/wdt/mode1-second-stage-reset",
                    test_wdt_mode1_second_stage_reset);
    g_test_add_func("/sf32lb52x/wdt/mode1-cvr1-zero-no-reset",
                    test_wdt_mode1_cvr1_zero_no_reset);
    g_test_add_func("/sf32lb52x/wdt/mode0-reset", test_wdt_mode0_reset);
    g_test_add_func("/sf32lb52x/wdt/write-protect",
                    test_wdt_write_protect);

    return g_test_run();
}
