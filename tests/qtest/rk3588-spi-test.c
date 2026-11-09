/*
 * QEMU ROCK 5B SPI controller tests
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"

#define ROCK5B_MACHINE_NAME     "rock-5b"
#define ROCK5BPLUS_MACHINE_NAME "rock-5b-plus"
#define ROCK5B_GIC_DIST_BASE    0xfe600000ULL

/* The three controllers routed to the 40-pin header. */
#define ROCK5B_SPI0_BASE        0xfeb00000ULL
#define ROCK5B_SPI1_BASE        0xfeb10000ULL
#define ROCK5B_SPI2_BASE        0xfeb20000ULL
#define ROCK5B_SPI3_BASE        0xfeb30000ULL

/* SPI interrupts, as wired in the RK3588 device tree. */
#define ROCK5B_SPI0_SPI         326
#define ROCK5B_SPI1_SPI         327
#define ROCK5B_SPI3_SPI         329

/* GIC SPI N is INTID 32+N on the distributor; GICD_ISPENDR shows the level. */
#define ROCK5B_GIC_SPI_INTID(spi)   ((spi) + 32)
#define ROCK5B_GIC_ISPENDR(intid) \
    (ROCK5B_GIC_DIST_BASE + 0x200 + 4 * ((intid) / 32))

/* Rockchip SPI register bank, see include/hw/ssi/rockchip_spi.h. */
enum {
    SPI_CTRLR0 = 0x00, SPI_CTRLR1 = 0x04, SPI_SSIENR = 0x08,
    SPI_SER = 0x0c, SPI_BAUDR = 0x10, SPI_TXFTLR = 0x14,
    SPI_RXFTLR = 0x18, SPI_TXFLR = 0x1c, SPI_RXFLR = 0x20,
    SPI_SR = 0x24, SPI_IPR = 0x28, SPI_IMR = 0x2c,
    SPI_ISR = 0x30, SPI_RISR = 0x34, SPI_ICR = 0x38,
    SPI_DMACR = 0x3c, SPI_VERSION = 0x48,
    SPI_TXDR = 0x400, SPI_RXDR = 0x800,
};

#define SPI_VERSION_RK3588     0x00110002
#define SPI_FIFO_DEPTH          64

#define SPI_SR_TF_FULL          BIT(1)
#define SPI_SR_TF_EMPTY         BIT(2)
#define SPI_SR_RF_EMPTY         BIT(3)
#define SPI_INT_TF_EMPTY        BIT(0)
#define SPI_INT_TF_OVER         BIT(1)
#define SPI_INT_RF_UNDER        BIT(2)

#define SPI_READ(base, reg)     qtest_readl(qts, (base) + (reg))
#define SPI_WRITE(base, reg, v) qtest_writel(qts, (base) + (reg), (v))

static QTestState *rock5b_spi_start(void)
{
    return qtest_init("-machine " ROCK5B_MACHINE_NAME
                      " -m 256M -serial null -display none");
}

static void test_spi_reset_values(void)
{
    static const uint64_t bases[] = {
        ROCK5B_SPI0_BASE, ROCK5B_SPI1_BASE, ROCK5B_SPI3_BASE,
    };
    QTestState *qts = rock5b_spi_start();

    /*
     * A unimplemented window reads back as zero everywhere, so VERSION is
     * what tells a modeled controller apart from a stub.
     */
    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        uint64_t base = bases[i];

        g_assert_cmphex(SPI_READ(base, SPI_VERSION), ==, SPI_VERSION_RK3588);
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
        g_assert_cmphex(SPI_READ(base, SPI_DMACR), ==, 0);
    }

    qtest_quit(qts);
}

static void test_spi_register_access(void)
{
    QTestState *qts = rock5b_spi_start();
    uint64_t base = ROCK5B_SPI0_BASE;

    SPI_WRITE(base, SPI_SSIENR, 1);
    g_assert_cmphex(SPI_READ(base, SPI_SSIENR), ==, 1);

    /* BAUDR keeps the even prescaler; SER only exposes the two chip selects. */
    SPI_WRITE(base, SPI_BAUDR, 0x1235);
    g_assert_cmphex(SPI_READ(base, SPI_BAUDR), ==, 0x1234);
    SPI_WRITE(base, SPI_SER, 0xff);
    g_assert_cmphex(SPI_READ(base, SPI_SER), ==, 3);

    /* Every controller owns its register state. */
    SPI_WRITE(ROCK5B_SPI1_BASE, SPI_CTRLR0, 0x00010007);
    SPI_WRITE(ROCK5B_SPI3_BASE, SPI_CTRLR0, 0x0003000f);
    g_assert_cmphex(SPI_READ(base, SPI_CTRLR0), ==, 0);
    g_assert_cmphex(SPI_READ(ROCK5B_SPI1_BASE, SPI_CTRLR0), ==, 0x00010007);
    g_assert_cmphex(SPI_READ(ROCK5B_SPI3_BASE, SPI_CTRLR0), ==, 0x0003000f);

    qtest_quit(qts);
}

static void test_spi_fifo_and_irq(void)
{
    QTestState *qts = rock5b_spi_start();
    uint64_t base = ROCK5B_SPI3_BASE;

    /* An empty TX FIFO raises TF_EMPTY once the interrupt is unmasked. */
    SPI_WRITE(base, SPI_IMR, SPI_INT_TF_EMPTY);
    g_assert_cmphex(SPI_READ(base, SPI_ISR), ==, SPI_INT_TF_EMPTY);

    /* Frames pushed into TXDR are visible in the FIFO level registers. */
    SPI_WRITE(base, SPI_TXDR, 0x1111);
    SPI_WRITE(base, SPI_TXDR, 0x2222);
    g_assert_cmphex(SPI_READ(base, SPI_TXFLR), ==, 2);
    g_assert_cmphex(SPI_READ(base, SPI_SR) & SPI_SR_TF_EMPTY, ==, 0);

    /* Reading an empty RX FIFO raises RF_UNDER, which ICR clears. */
    g_assert_cmphex(qtest_readl(qts, base + SPI_RXDR), ==, 0);
    g_assert_cmphex(SPI_READ(base, SPI_RISR) & SPI_INT_RF_UNDER, ==,
                    SPI_INT_RF_UNDER);
    SPI_WRITE(base, SPI_ICR, SPI_INT_RF_UNDER);
    g_assert_cmphex(SPI_READ(base, SPI_RISR) & SPI_INT_RF_UNDER, ==, 0);

    /* Overflowing the TX FIFO keeps the depth and flags TF_OVER. */
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

    qtest_quit(qts);
}

static void test_spi_interrupt_lines(void)
{
    static const struct {
        uint64_t base;
        unsigned int spi;
    } controllers[] = {
        { ROCK5B_SPI0_BASE, ROCK5B_SPI0_SPI },
        { ROCK5B_SPI1_BASE, ROCK5B_SPI1_SPI },
        { ROCK5B_SPI3_BASE, ROCK5B_SPI3_SPI },
    };
    QTestState *qts = rock5b_spi_start();

    for (unsigned i = 0; i < ARRAY_SIZE(controllers); i++) {
        uint64_t base = controllers[i].base;
        unsigned int intid = ROCK5B_GIC_SPI_INTID(controllers[i].spi);
        uint32_t mask = BIT(intid % 32);

        /* Masking the source keeps the line low. */
        g_assert_cmphex(SPI_READ(base, SPI_IMR), ==, 0);
        g_assert_cmphex(qtest_readl(qts, ROCK5B_GIC_ISPENDR(intid)) & mask,
                        ==, 0);

        /* TF_EMPTY is level based: unmask it and the GIC input goes high. */
        SPI_WRITE(base, SPI_IMR, SPI_INT_TF_EMPTY);
        g_assert_cmphex(qtest_readl(qts, ROCK5B_GIC_ISPENDR(intid)) & mask,
                        ==, mask);

        SPI_WRITE(base, SPI_IMR, 0);
        g_assert_cmphex(qtest_readl(qts, ROCK5B_GIC_ISPENDR(intid)) & mask,
                        ==, 0);
    }

    qtest_quit(qts);
}

static void test_spi_unimplemented(void)
{
    g_autofree char *log = NULL;
    const char *log_path = "rk3588-spi-unimplemented.log";
    QTestState *qts;

    unlink(log_path);
    qts = qtest_initf("-machine " ROCK5B_MACHINE_NAME
                      " -m 256M -serial null -display none"
                      " -d unimp -D \"%s\"", log_path);

    /* The header controllers are modeled, so no access is logged. */
    g_assert_cmphex(SPI_READ(ROCK5B_SPI0_BASE, SPI_VERSION), ==,
                    SPI_VERSION_RK3588);
    g_assert_cmphex(SPI_READ(ROCK5B_SPI1_BASE, SPI_VERSION), ==,
                    SPI_VERSION_RK3588);
    g_assert_cmphex(SPI_READ(ROCK5B_SPI3_BASE, SPI_VERSION), ==,
                    SPI_VERSION_RK3588);

    /* SPI2 still carries the PMIC on the legacy machines and stays a stub. */
    g_assert_cmphex(SPI_READ(ROCK5B_SPI2_BASE, SPI_VERSION), ==, 0);

    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_null(strstr(log, "rk3588.spi0"));
    g_assert_null(strstr(log, "rk3588.spi1"));
    g_assert_null(strstr(log, "rk3588.spi3"));
    g_assert_nonnull(strstr(log, "rk3588.spi2: unimplemented device"));
    unlink(log_path);
}

/*
 * Clock a JEDEC read-ID command out of @base and collect @count bytes from
 * the receive FIFO.  Both tests below share it so that the chip-select
 * polarity case can compare against a known-good transfer on the same
 * controller.
 */
static void spi_rdid(QTestState *qts, uint64_t base, uint32_t ctrlr0,
                     uint32_t *rx, unsigned int count)
{
    SPI_WRITE(base, SPI_CTRLR0, ctrlr0);
    SPI_WRITE(base, SPI_SER, 0x1);
    SPI_WRITE(base, SPI_BAUDR, 0x4);
    SPI_WRITE(base, SPI_SSIENR, 0x1);

    for (unsigned int i = 0; i < count; i++) {
        SPI_WRITE(base, SPI_TXDR, i == 0 ? 0x9f : 0x0);
    }

    /* Wire clocks are modelled on a timer, so let the bus drain. */
    qtest_clock_step(qts, 100000);

    g_assert_cmphex(SPI_READ(base, SPI_TXFLR), ==, 0);
    g_assert_cmphex(SPI_READ(base, SPI_RXFLR), ==, count);
    for (unsigned int i = 0; i < count; i++) {
        rx[i] = qtest_readl(qts, base + SPI_RXDR);
    }
}

static void test_spi_flash_id(void)
{
    /* The command byte is clocked out first, then the three ID bytes. */
    static const uint32_t rdid_response[4] = { 0x00, 0x20, 0xba, 0x19 };
    uint32_t rx[ARRAY_SIZE(rdid_response)];
    QTestState *qts;

    if (!qtest_has_device("n25q256a")) {
        g_test_skip("n25q256a is not built in");
        return;
    }

    qts = qtest_init("-machine " ROCK5B_MACHINE_NAME
                     " -m 256M -serial null -display none"
                     " -device n25q256a,bus=/spi0/spi/");

    /* 8-bit frames, TX and RX enabled, chip select 0, 50 MHz bus clock. */
    spi_rdid(qts, ROCK5B_SPI0_BASE, 0x1, rx, ARRAY_SIZE(rx));
    for (unsigned int i = 0; i < ARRAY_SIZE(rx); i++) {
        g_assert_cmphex(rx[i], ==, rdid_response[i]);
    }

    qtest_quit(qts);
}

static void test_spi_cs_polarity(void)
{
    uint32_t rx[3];
    QTestState *qts;

    if (!qtest_has_device("n25q256a")) {
        g_test_skip("n25q256a is not built in");
        return;
    }

    qts = qtest_init("-machine " ROCK5B_MACHINE_NAME
                     " -m 256M -serial null -display none"
                     " -device n25q256a,bus=/spi1/spi/");

    /* Default active-low select: the flash on SPI1 answers the command. */
    spi_rdid(qts, ROCK5B_SPI1_BASE, 0x1, rx, ARRAY_SIZE(rx));
    g_assert_cmphex(rx[0], ==, 0x00);
    g_assert_cmphex(rx[1], ==, 0x20);
    g_assert_cmphex(rx[2], ==, 0xba);

    qtest_quit(qts);

    /* CTRLR0 bit 23 inverts the select, so the flash must stay quiet. */
    qts = qtest_init("-machine " ROCK5B_MACHINE_NAME
                     " -m 256M -serial null -display none"
                     " -device n25q256a,bus=/spi1/spi/");

    spi_rdid(qts, ROCK5B_SPI1_BASE, 0x1 | BIT(23), rx, ARRAY_SIZE(rx));
    for (unsigned int i = 0; i < ARRAY_SIZE(rx); i++) {
        g_assert_cmphex(rx[i], ==, 0);
    }

    qtest_quit(qts);
}

static void test_spi_legacy_spi2(void)
{
    QTestState *qts;

    /*
     * The legacy machines create SPI2 to carry the RK806 PMIC, so the
     * refactored helper has to keep working for them.  They return before
     * the unimplemented-device stubs are registered, so SPI0, SPI1 and
     * SPI3 stay unmapped and read back as all ones.
     */
    if (!qtest_has_machine(ROCK5BPLUS_MACHINE_NAME)) {
        g_test_skip(ROCK5BPLUS_MACHINE_NAME " machine is not available");
        return;
    }

    qts = qtest_init("-machine " ROCK5BPLUS_MACHINE_NAME
                     " -m 256M -serial null -display none");

    g_assert_cmphex(SPI_READ(ROCK5B_SPI2_BASE, SPI_VERSION), ==,
                    SPI_VERSION_RK3588);
    g_assert_cmphex(SPI_READ(ROCK5B_SPI2_BASE, SPI_SR), ==,
                    SPI_SR_TF_EMPTY | SPI_SR_RF_EMPTY);
    g_assert_cmphex(SPI_READ(ROCK5B_SPI0_BASE, SPI_VERSION), ==, 0xffffffff);
    g_assert_cmphex(SPI_READ(ROCK5B_SPI1_BASE, SPI_VERSION), ==, 0xffffffff);
    g_assert_cmphex(SPI_READ(ROCK5B_SPI3_BASE, SPI_VERSION), ==, 0xffffffff);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(ROCK5B_MACHINE_NAME)) {
        g_test_skip(ROCK5B_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3588-spi/reset-values", test_spi_reset_values);
    qtest_add_func("/rk3588-spi/register-access", test_spi_register_access);
    qtest_add_func("/rk3588-spi/fifo-and-irq", test_spi_fifo_and_irq);
    qtest_add_func("/rk3588-spi/interrupt-lines",
                   test_spi_interrupt_lines);
    qtest_add_func("/rk3588-spi/unimplemented", test_spi_unimplemented);
    qtest_add_func("/rk3588-spi/flash-id", test_spi_flash_id);
    qtest_add_func("/rk3588-spi/cs-polarity", test_spi_cs_polarity);
    qtest_add_func("/rk3588-spi/legacy-spi2", test_spi_legacy_spi2);

    return g_test_run();
}
