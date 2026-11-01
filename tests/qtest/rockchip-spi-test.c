/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tests for the Rockchip SPI controller on the MicroDuck beta machine.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"
#include "rk3566-test-common.h"

#define RK3566_SPI1_BASE 0xfe620000ULL
#define RK3566_SPI3_BASE 0xfe640000ULL

/* Register offsets from the MSPI controller. */
enum {
    SPI_CTRLR0 = 0x00,
    SPI_CTRLR1 = 0x04,
    SPI_SSIENR = 0x08,
    SPI_SER = 0x0c,
    SPI_BAUDR = 0x10,
    SPI_TXFTLR = 0x14,
    SPI_RXFTLR = 0x18,
    SPI_TXFLR = 0x1c,
    SPI_RXFLR = 0x20,
    SPI_SR = 0x24,
    SPI_IPR = 0x28,
    SPI_IMR = 0x2c,
    SPI_ISR = 0x30,
    SPI_RISR = 0x34,
    SPI_ICR = 0x38,
    SPI_DMACR = 0x3c,
    SPI_DMATDLR = 0x40,
    SPI_DMARDLR = 0x44,
    SPI_VERSION = 0x48,
    SPI_TXDR = 0x400,
    SPI_RXDR = 0x800,
};

#define SPI_FIFO_DEPTH 64
#define SPI_VERSION_RK3588 0x00110002

#define SPI_SR_BUSY BIT(0)
#define SPI_SR_TF_FULL BIT(1)
#define SPI_SR_TF_EMPTY BIT(2)
#define SPI_SR_RF_EMPTY BIT(3)
#define SPI_SR_RF_FULL BIT(4)

#define SPI_INT_TF_EMPTY BIT(0)
#define SPI_INT_TF_OVER BIT(1)
#define SPI_INT_RF_UNDER BIT(2)
#define SPI_INT_RF_OVER BIT(3)
#define SPI_INT_RF_FULL BIT(4)

#define SPI_READ(base, reg) rk3566_mmio_read32_at(qts, base, reg)
#define SPI_WRITE(base, reg, value) \
    rk3566_mmio_write32_at(qts, base, reg, value)

static void test_spi_reset_values(void)
{
    static const uint64_t bases[] = { RK3566_SPI1_BASE, RK3566_SPI3_BASE };
    QTestState *qts = rk3566_qtest_start();

    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        uint64_t base = bases[i];

        g_assert_cmphex(SPI_READ(base, SPI_CTRLR0), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_CTRLR1), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_SSIENR), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_SER), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_BAUDR), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_TXFTLR), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_RXFTLR), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_TXFLR), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_RXFLR), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_SR), ==,
                        SPI_SR_TF_EMPTY | SPI_SR_RF_EMPTY);
        g_assert_cmphex(SPI_READ(base, SPI_IPR), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_IMR), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_ISR), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_RISR), ==, SPI_INT_TF_EMPTY);
        g_assert_cmphex(SPI_READ(base, SPI_ICR), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_DMACR), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_DMATDLR), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_DMARDLR), ==, 0);
        g_assert_cmphex(SPI_READ(base, SPI_VERSION), ==,
                        SPI_VERSION_RK3588);
    }

    qtest_quit(qts);
}

static void test_spi_register_access(void)
{
    QTestState *qts = rk3566_qtest_start();
    uint64_t base = RK3566_SPI1_BASE;

    SPI_WRITE(base, SPI_SSIENR, 1);
    g_assert_cmphex(SPI_READ(base, SPI_SSIENR), ==, 1);

    SPI_WRITE(base, SPI_BAUDR, 0x1235);
    g_assert_cmphex(SPI_READ(base, SPI_BAUDR), ==, 0x1234);
    SPI_WRITE(base, SPI_BAUDR, 0xffff);
    g_assert_cmphex(SPI_READ(base, SPI_BAUDR), ==, 0xfffe);

    SPI_WRITE(base, SPI_IMR, 0x1f);
    g_assert_cmphex(SPI_READ(base, SPI_IMR), ==, 0x1f);

    SPI_WRITE(base, SPI_CTRLR1, 0x1ffff);
    g_assert_cmphex(SPI_READ(base, SPI_CTRLR1), ==, 0xffff);

    SPI_WRITE(base, SPI_SER, 0xff);
    g_assert_cmphex(SPI_READ(base, SPI_SER), ==, 3);

    SPI_WRITE(base, SPI_DMACR, 0xff);
    g_assert_cmphex(SPI_READ(base, SPI_DMACR), ==, 3);

    SPI_WRITE(base, SPI_TXFTLR, 0x100);
    g_assert_cmphex(SPI_READ(base, SPI_TXFTLR), ==, SPI_FIFO_DEPTH - 1);

    /* The two controllers keep independent register state. */
    SPI_WRITE(RK3566_SPI3_BASE, SPI_IMR, 2);
    g_assert_cmphex(SPI_READ(base, SPI_IMR), ==, 0x1f);
    g_assert_cmphex(SPI_READ(RK3566_SPI3_BASE, SPI_IMR), ==, 2);

    qtest_quit(qts);
}

static void test_spi_fifo_and_irq(void)
{
    QTestState *qts = rk3566_qtest_start();
    uint64_t base = RK3566_SPI1_BASE;

    /* An empty TX FIFO raises TF_EMPTY once the interrupt is unmasked. */
    SPI_WRITE(base, SPI_IMR, SPI_INT_TF_EMPTY);
    g_assert_cmphex(SPI_READ(base, SPI_ISR), ==, SPI_INT_TF_EMPTY);
    g_assert_cmphex(SPI_READ(base, SPI_RISR), ==, SPI_INT_TF_EMPTY);

    /* Frames pushed into TXDR are visible in the FIFO level registers. */
    SPI_WRITE(base, SPI_TXDR, 0x1111);
    SPI_WRITE(base, SPI_TXDR, 0x2222);
    SPI_WRITE(base, SPI_TXDR, 0x3333);
    g_assert_cmphex(SPI_READ(base, SPI_TXFLR), ==, 3);
    g_assert_cmphex(SPI_READ(base, SPI_RXFLR), ==, 0);
    g_assert_cmphex(SPI_READ(base, SPI_SR) & SPI_SR_TF_EMPTY, ==, 0);
    g_assert_cmphex(SPI_READ(base, SPI_ISR), ==, 0);
    g_assert_cmphex(SPI_READ(base, SPI_RISR), ==, 0);

    /* Reading an empty RX FIFO raises the RF_UNDER raw interrupt. */
    g_assert_cmphex(qtest_readl(qts, base + SPI_RXDR), ==, 0);
    g_assert_cmphex(SPI_READ(base, SPI_RISR) & SPI_INT_RF_UNDER, ==,
                    SPI_INT_RF_UNDER);
    SPI_WRITE(base, SPI_IMR, SPI_INT_RF_UNDER);
    g_assert_cmphex(SPI_READ(base, SPI_ISR), ==, SPI_INT_RF_UNDER);

    SPI_WRITE(base, SPI_ICR, SPI_INT_RF_UNDER);
    g_assert_cmphex(SPI_READ(base, SPI_RISR) & SPI_INT_RF_UNDER, ==, 0);
    g_assert_cmphex(SPI_READ(base, SPI_ISR), ==, 0);

    /* Filling the TX FIFO sets TF_FULL and raises TF_OVER on overflow. */
    for (unsigned i = 0; i < SPI_FIFO_DEPTH; i++) {
        SPI_WRITE(base, SPI_TXDR, i);
    }
    g_assert_cmphex(SPI_READ(base, SPI_TXFLR), ==, SPI_FIFO_DEPTH);
    g_assert_cmphex(SPI_READ(base, SPI_SR) & SPI_SR_TF_FULL, ==,
                    SPI_SR_TF_FULL);

    SPI_WRITE(base, SPI_TXDR, 0x5555);
    g_assert_cmphex(SPI_READ(base, SPI_TXFLR), ==, SPI_FIFO_DEPTH);
    g_assert_cmphex(SPI_READ(base, SPI_RISR) & SPI_INT_TF_OVER, ==,
                    SPI_INT_TF_OVER);

    SPI_WRITE(base, SPI_IMR, SPI_INT_TF_OVER);
    g_assert_cmphex(SPI_READ(base, SPI_ISR), ==, SPI_INT_TF_OVER);

    qtest_quit(qts);
}

static void test_spi_unimplemented(void)
{
    g_autofree char *log = NULL;
    const char *log_path = "rockchip-spi-unimplemented.log";
    QTestState *qts;

    unlink(log_path);
    qts = qtest_initf("-machine " RK3566_MACHINE_NAME
                      " -m 512M -d unimp -D \"%s\"", log_path);
    rk3566_mmio_write32_at(qts, RK3566_SPI1_BASE, 0x4c, 0xdeadbeef);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_SPI1_BASE, 0x4c), ==, 0);
    rk3566_mmio_write32_at(qts, RK3566_SPI1_BASE, SPI_SR, 0xffffffff);
    rk3566_mmio_write32_at(qts, RK3566_SPI1_BASE, SPI_RXDR, 0x1234);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log,
                            "rockchip-spi: write at 0x4c = 0xdeadbeef"));
    g_assert_nonnull(strstr(log,
                            "rockchip-spi: write at 0x24 = 0xffffffff"));
    g_assert_nonnull(strstr(log, "rockchip-spi: write at 0x800 = 0x1234"));
    unlink(log_path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rockchip-spi/reset-values", test_spi_reset_values);
    qtest_add_func("/rockchip-spi/register-access",
                   test_spi_register_access);
    qtest_add_func("/rockchip-spi/fifo-and-irq", test_spi_fifo_and_irq);
    qtest_add_func("/rockchip-spi/unimplemented", test_spi_unimplemented);

    return g_test_run();
}
