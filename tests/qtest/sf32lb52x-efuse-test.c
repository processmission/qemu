/*
 * QTest for the SiFli SF32LB52X eFuse (OTP) controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "sf32lb52-devkit-lcd"

#define EFUSE_BASE 0x5000c000
#define EFUSE_CR 0x00
#define EFUSE_TIMR 0x04
#define EFUSE_SR 0x08
#define EFUSE_RSVDR 0x0c
#define EFUSE_PGM_DATA(n) (0x10 + 4 * (n))
#define EFUSE_BANK_DATA(bank, word) (0x30 + 0x20 * (bank) + 4 * (word))
#define EFUSE_ANACR 0xb0
#define EFUSE_DB_SEL 0xb4

#define CR_EN (1U << 0)
#define CR_MODE (1U << 1)
#define CR_BANK(bank) ((uint32_t)(bank) << 2)
#define SR_DONE (1U << 0)

static QTestState *efuse_start(void)
{
    return qtest_init("-machine " MACHINE " -serial null");
}

static uint32_t efuse_read(QTestState *qts, uint64_t off)
{
    uint32_t value;

    qtest_memread(qts, EFUSE_BASE + off, &value, sizeof(value));
    return value;
}

static void efuse_write(QTestState *qts, uint64_t off, uint32_t value)
{
    qtest_memwrite(qts, EFUSE_BASE + off, &value, sizeof(value));
}

/* Issue a read command for one bank the way otp_sifli_efuse.c does. */
static void efuse_read_bank(QTestState *qts, unsigned int bank)
{
    efuse_write(qts, EFUSE_CR, CR_BANK(bank));
    efuse_write(qts, EFUSE_CR, CR_BANK(bank) | CR_EN);
}

static void test_efuse_reset(void)
{
    QTestState *qts = efuse_start();

    g_assert_cmphex(efuse_read(qts, EFUSE_CR), ==, 0);
    g_assert_cmphex(efuse_read(qts, EFUSE_TIMR), ==, 0);
    g_assert_cmphex(efuse_read(qts, EFUSE_SR), ==, 0);
    g_assert_cmphex(efuse_read(qts, EFUSE_RSVDR), ==, 0);
    g_assert_cmphex(efuse_read(qts, EFUSE_PGM_DATA(0)), ==, 0);
    g_assert_cmphex(efuse_read(qts, EFUSE_BANK_DATA(0, 0)), ==, 0);
    g_assert_cmphex(efuse_read(qts, EFUSE_BANK_DATA(3, 7)), ==, 0);
    g_assert_cmphex(efuse_read(qts, EFUSE_ANACR), ==, 0);
    g_assert_cmphex(efuse_read(qts, EFUSE_DB_SEL), ==, 0);

    qtest_quit(qts);
}

static void test_efuse_read_protocol(void)
{
    QTestState *qts = efuse_start();

    /* EN self-clears, BANKSEL stays readable, DONE is asserted. */
    efuse_read_bank(qts, 2);
    g_assert_cmphex(efuse_read(qts, EFUSE_CR), ==, CR_BANK(2));
    g_assert_cmphex(efuse_read(qts, EFUSE_SR), ==, SR_DONE);

    /* The bank contents read back through the per-bank data window. */
    g_assert_cmphex(efuse_read(qts, EFUSE_BANK_DATA(2, 0)), ==, 0);
    g_assert_cmphex(efuse_read(qts, EFUSE_BANK_DATA(2, 7)), ==, 0);

    /* DONE is W1C. */
    efuse_write(qts, EFUSE_SR, SR_DONE);
    g_assert_cmphex(efuse_read(qts, EFUSE_SR), ==, 0);

    qtest_quit(qts);
}

static void test_efuse_bank0_read(void)
{
    QTestState *qts = efuse_start();

    efuse_read_bank(qts, 0);
    g_assert_cmphex(efuse_read(qts, EFUSE_CR), ==, 0);
    g_assert_cmphex(efuse_read(qts, EFUSE_SR), ==, SR_DONE);
    g_assert_cmphex(efuse_read(qts, EFUSE_BANK_DATA(0, 0)), ==, 0);

    efuse_write(qts, EFUSE_SR, SR_DONE);
    g_assert_cmphex(efuse_read(qts, EFUSE_SR), ==, 0);

    qtest_quit(qts);
}

static void test_efuse_bank_data_ro(void)
{
    QTestState *qts = efuse_start();

    efuse_write(qts, EFUSE_BANK_DATA(0, 0), 0xdeadbeef);
    efuse_write(qts, EFUSE_BANK_DATA(1, 3), 0x12345678);
    g_assert_cmphex(efuse_read(qts, EFUSE_BANK_DATA(0, 0)), ==, 0);
    g_assert_cmphex(efuse_read(qts, EFUSE_BANK_DATA(1, 3)), ==, 0);

    /* A read command must not disturb the shadowed bank data. */
    efuse_read_bank(qts, 0);
    g_assert_cmphex(efuse_read(qts, EFUSE_BANK_DATA(0, 0)), ==, 0);

    qtest_quit(qts);
}

static void test_efuse_program_data(void)
{
    QTestState *qts = efuse_start();

    efuse_write(qts, EFUSE_PGM_DATA(0), 0x12345678);
    g_assert_cmphex(efuse_read(qts, EFUSE_PGM_DATA(0)), ==, 0x12345678);

    /* Programming mode completes but does not change the bank contents. */
    efuse_write(qts, EFUSE_CR, CR_BANK(1) | CR_MODE | CR_EN);
    g_assert_cmphex(efuse_read(qts, EFUSE_CR), ==, CR_BANK(1) | CR_MODE);
    g_assert_cmphex(efuse_read(qts, EFUSE_SR), ==, SR_DONE);
    g_assert_cmphex(efuse_read(qts, EFUSE_BANK_DATA(1, 0)), ==, 0);

    efuse_write(qts, EFUSE_SR, SR_DONE);
    g_assert_cmphex(efuse_read(qts, EFUSE_SR), ==, 0);

    qtest_quit(qts);
}

static void test_efuse_rw_config(void)
{
    QTestState *qts = efuse_start();

    efuse_write(qts, EFUSE_TIMR, 0x00000400);
    efuse_write(qts, EFUSE_ANACR, 0x3);
    efuse_write(qts, EFUSE_DB_SEL, 0x55);
    g_assert_cmphex(efuse_read(qts, EFUSE_TIMR), ==, 0x00000400);
    g_assert_cmphex(efuse_read(qts, EFUSE_ANACR), ==, 0x3);
    g_assert_cmphex(efuse_read(qts, EFUSE_DB_SEL), ==, 0x55);

    qtest_quit(qts);
}

static void test_efuse_bad_offset(void)
{
    QTestState *qts = efuse_start();

    g_assert_cmphex(efuse_read(qts, 0xf0), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/sf32lb52x/efuse/reset", test_efuse_reset);
    g_test_add_func("/sf32lb52x/efuse/read-protocol",
                    test_efuse_read_protocol);
    g_test_add_func("/sf32lb52x/efuse/bank0-read", test_efuse_bank0_read);
    g_test_add_func("/sf32lb52x/efuse/bank-data-ro", test_efuse_bank_data_ro);
    g_test_add_func("/sf32lb52x/efuse/program-data",
                    test_efuse_program_data);
    g_test_add_func("/sf32lb52x/efuse/rw-config", test_efuse_rw_config);
    g_test_add_func("/sf32lb52x/efuse/bad-offset", test_efuse_bad_offset);

    return g_test_run();
}
