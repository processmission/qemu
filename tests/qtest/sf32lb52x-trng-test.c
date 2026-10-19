/*
 * QTest for the SiFli SF32LB52X true random number generator
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "sf32lb52-devkit-lcd"

#define TRNG_BASE      0x5000f000
#define TRNG_CTRL      0x00
#define TRNG_STAT      0x04
#define TRNG_ISR       0x0c
#define TRNG_RAND0     0x30
#define TRNG_CAL_CFG   0x50
#define TRNG_CAL_RES   0x54

#define CTRL_SEED_START (1U << 0)
#define CTRL_RAND_START (1U << 1)
#define STAT_SEED_VALID (1U << 1)
#define STAT_RAND_VALID (1U << 3)
#define CAL_ENABLE      (1U << 4)
#define CAL_DONE        (1U << 5)

static QTestState *trng_start(void)
{
    return qtest_init("-machine " MACHINE " -serial null");
}

static uint32_t trng_read(QTestState *qts, uint64_t off)
{
    uint32_t value;

    qtest_memread(qts, TRNG_BASE + off, &value, sizeof(value));
    return value;
}

static void trng_write(QTestState *qts, uint64_t off, uint32_t value)
{
    qtest_memwrite(qts, TRNG_BASE + off, &value, sizeof(value));
}

static void test_trng_reset(void)
{
    QTestState *qts = trng_start();

    g_assert_cmpuint(trng_read(qts, TRNG_CTRL), ==, 0);
    g_assert_cmpuint(trng_read(qts, TRNG_STAT), ==, 0);
    g_assert_cmpuint(trng_read(qts, TRNG_ISR), ==, 0x00070000);

    qtest_quit(qts);
}

static void test_trng_seed_and_random(void)
{
    QTestState *qts = trng_start();
    uint32_t first[8], second[8];
    bool differs = false;
    int i;

    trng_write(qts, TRNG_CTRL, CTRL_SEED_START);
    g_assert_cmpuint(trng_read(qts, TRNG_STAT) & STAT_SEED_VALID, ==,
                     STAT_SEED_VALID);

    trng_write(qts, TRNG_CTRL, CTRL_RAND_START);
    g_assert_cmpuint(trng_read(qts, TRNG_STAT) & STAT_RAND_VALID, ==,
                     STAT_RAND_VALID);

    qtest_memread(qts, TRNG_BASE + TRNG_RAND0, first, sizeof(first));
    for (i = 0; i < 8; i++) {
        g_assert_cmpuint(first[i], !=, 0);
    }
    for (i = 1; i < 8; i++) {
        if (first[i] != first[0]) {
            differs = true;
        }
    }
    g_assert_true(differs);

    /* A second request produces fresh data. */
    trng_write(qts, TRNG_CTRL, CTRL_RAND_START);
    qtest_memread(qts, TRNG_BASE + TRNG_RAND0, second, sizeof(second));
    g_assert_true(memcmp(first, second, sizeof(first)) != 0);

    qtest_quit(qts);
}

static void test_trng_calibration(void)
{
    QTestState *qts = trng_start();
    uint32_t cfg = 0x00001200;

    /*
     * The calibration counter flow is unimplemented: the request is stored
     * but no calibration result is invented.
     */
    trng_write(qts, TRNG_CAL_CFG, cfg);
    g_assert_cmpuint(trng_read(qts, TRNG_CAL_CFG), ==, cfg);
    g_assert_cmpuint(trng_read(qts, TRNG_CAL_RES), ==, 0);

    trng_write(qts, TRNG_CAL_CFG, cfg | CAL_ENABLE);
    g_assert_cmpuint(trng_read(qts, TRNG_CAL_CFG) & CAL_DONE, ==, 0);
    g_assert_cmpuint(trng_read(qts, TRNG_CAL_RES), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/sf32lb52x/trng/reset", test_trng_reset);
    g_test_add_func("/sf32lb52x/trng/seed-random", test_trng_seed_and_random);
    g_test_add_func("/sf32lb52x/trng/calibration", test_trng_calibration);

    return g_test_run();
}
