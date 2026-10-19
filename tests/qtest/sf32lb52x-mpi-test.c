/*
 * QTest for the SiFli SF32LB52X MPI2 QSPI-NOR controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "sf32lb52-devkit-lcd"
#define MPI_BASE 0x50042000

#define MPI_CR     0x00
#define MPI_DR     0x04
#define MPI_SR     0x10
#define MPI_SCR    0x14
#define MPI_CMDR1  0x18
#define MPI_AR1    0x1c
#define MPI_DLR1   0x24
#define MPI_CCR1   0x28
#define MPI_SMR    0x7c
#define MPI_SMKR   0x80

#define CR_EN      (1U << 0)
#define CR_CMD2E   (1U << 16)
#define CR_SME2    (1U << 18)
#define SR_TCF     (1U << 0)
#define SR_SMF     (1U << 3)
#define SCR_TCFC   (1U << 0)
#define SCR_SMFC   (1U << 3)

#define CCR_IMODE  (1U << 0)
#define CCR_ADMODE (1U << 3)
#define CCR_ADSIZE (2U << 6)   /* 3-byte address */
#define CCR_DMODE  (1U << 18)
#define CCR_FMODE  (1U << 21)

static QTestState *mpi_start(void)
{
    QTestState *qts = qtest_init("-machine " MACHINE " -serial null");

    qtest_writel(qts, MPI_BASE + MPI_CR, CR_EN);
    return qts;
}

static uint32_t mpi_readl(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, MPI_BASE + off);
}

static void mpi_writel(QTestState *qts, uint32_t off, uint32_t value)
{
    qtest_writel(qts, MPI_BASE + off, value);
}

static void mpi_cmd(QTestState *qts, uint32_t ccr, uint32_t addr,
                    uint32_t dlen, uint32_t cmd)
{
    mpi_writel(qts, MPI_CCR1, ccr);
    mpi_writel(qts, MPI_AR1, addr);
    mpi_writel(qts, MPI_DLR1, dlen);
    mpi_writel(qts, MPI_CMDR1, cmd);
    g_assert_cmphex(mpi_readl(qts, MPI_SR) & SR_TCF, ==, SR_TCF);
    mpi_writel(qts, MPI_SCR, SCR_TCFC);
}

static void test_mpi_reset(void)
{
    QTestState *qts = qtest_init("-machine " MACHINE " -serial null");

    g_assert_cmphex(mpi_readl(qts, MPI_CR), ==, 0);
    g_assert_cmphex(mpi_readl(qts, MPI_SR), ==, 0);

    qtest_quit(qts);
}

static void test_mpi_rdid(void)
{
    QTestState *qts = mpi_start();
    uint32_t id;

    mpi_cmd(qts, CCR_IMODE | CCR_DMODE, 0, 2, 0x9f);
    id = mpi_readl(qts, MPI_DR);
    /* PY25Q128HA: 0x85 0x20 0x18 little-endian in the FIFO. */
    g_assert_cmphex(id, ==, 0x00182085);

    qtest_quit(qts);
}

static void test_mpi_status_quad_enable(void)
{
    QTestState *qts = mpi_start();
    uint32_t sr;

    /* WREN sets WEL in status register 1. */
    mpi_cmd(qts, CCR_IMODE, 0, 0, 0x06);
    mpi_cmd(qts, CCR_IMODE | CCR_DMODE, 0, 0, 0x05);
    sr = mpi_readl(qts, MPI_DR) & 0xff;
    g_assert_cmphex(sr, ==, 0x02);

    /* WRSR with SR1=0x00, SR2=0x02 enables the quad bit. */
    mpi_writel(qts, MPI_DR, 0x00000200);
    mpi_cmd(qts, CCR_IMODE | CCR_DMODE | CCR_FMODE, 0, 1, 0x01);
    mpi_cmd(qts, CCR_IMODE | CCR_DMODE, 0, 0, 0x35);
    sr = mpi_readl(qts, MPI_DR) & 0xff;
    g_assert_cmphex(sr, ==, 0x02);

    qtest_quit(qts);
}

static void test_mpi_program_read_erase(void)
{
    QTestState *qts = mpi_start();
    const uint32_t addr = 0xf00000;
    const uint32_t xip_addr = 0x12000000 + addr;
    uint32_t data;

    /* Page program: "ABCD". */
    mpi_writel(qts, MPI_DR, 0x44434241);
    mpi_cmd(qts, CCR_IMODE | CCR_ADMODE | CCR_ADSIZE | CCR_DMODE | CCR_FMODE,
            addr, 3, 0x02);

    mpi_cmd(qts, CCR_IMODE | CCR_ADMODE | CCR_ADSIZE | CCR_DMODE,
            addr, 3, 0x03);
    data = mpi_readl(qts, MPI_DR);
    g_assert_cmphex(data, ==, 0x44434241);

    /* The CPU sees the programmed bytes through the XIP window. */
    qtest_memread(qts, xip_addr, &data, sizeof(data));
    g_assert_cmphex(data, ==, 0x44434241);

    /* Sector erase returns 0xff. */
    mpi_cmd(qts, CCR_IMODE, addr, 0, 0x20);
    mpi_cmd(qts, CCR_IMODE | CCR_ADMODE | CCR_ADSIZE | CCR_DMODE,
            addr, 3, 0x03);
    data = mpi_readl(qts, MPI_DR);
    g_assert_cmphex(data, ==, 0xffffffff);

    qtest_memread(qts, xip_addr, &data, sizeof(data));
    g_assert_cmphex(data, ==, 0xffffffff);

    qtest_quit(qts);
}

static void test_mpi_status_match(void)
{
    QTestState *qts = mpi_start();

    mpi_writel(qts, MPI_CR, CR_EN | CR_CMD2E | CR_SME2);
    mpi_writel(qts, MPI_SMR, 0x0);
    mpi_writel(qts, MPI_SMKR, 0x1);
    mpi_cmd(qts, CCR_IMODE | CCR_DMODE, 0, 0, 0x05);
    g_assert_cmphex(mpi_readl(qts, MPI_SR) & SR_SMF, ==, SR_SMF);
    mpi_writel(qts, MPI_SCR, SCR_SMFC);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/sf32lb52x/mpi/reset", test_mpi_reset);
    g_test_add_func("/sf32lb52x/mpi/rdid", test_mpi_rdid);
    g_test_add_func("/sf32lb52x/mpi/status-quad-enable",
                    test_mpi_status_quad_enable);
    g_test_add_func("/sf32lb52x/mpi/program-read-erase",
                    test_mpi_program_read_erase);
    g_test_add_func("/sf32lb52x/mpi/status-match", test_mpi_status_match);

    return g_test_run();
}
