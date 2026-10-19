/*
 * QTest for the SiFli SF32LB52X LCDC / MIPI-DBI controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Covers the SVD LCDC1 block and the vendor HAL LCD_IF_TypeDef: reset
 * values, DBI/SPI register programming, busy
 * bits that must stay idle for
 * the Zephyr driver's unbounded polls, command/data/read capture, IRQ EOF
 * status W1C and masking, COMMAND.RESET, and a driver-like init sequence that
 * terminates.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define MACHINE "sf32lb52-devkit-lcd"
#define LCDC_BASE 0x50008000
#define LCDC_QOM_PATH "/machine/soc/lcdc"

/* Register offsets (SVD LCDC1 dump). */
#define LCDC_COMMAND       0x00
#define LCDC_STATUS        0x04
#define LCDC_IRQ           0x08
#define LCDC_SETTING       0x0c
#define LCDC_CANVAS_TL     0x10
#define LCDC_LAYER0_CONF   0x1c
#define LCDC_LAYER0_SRC    0x2c
#define LCDC_LAYER0_CFG0   0x38
#define LCDC_LAYER0_CFG1   0x3c
#define LCDC_LAYER0_STAT   0x40
#define LCDC_LCD_CONF      0x80
#define LCDC_LCD_IF_CONF   0x84
#define LCDC_LCD_MEM       0x88
#define LCDC_LCD_SINGLE    0x90
#define LCDC_LCD_WR        0x94
#define LCDC_LCD_RD        0x98
#define LCDC_SPI_IF_CONF   0x9c
#define LCDC_TE_CONF       0xa0
#define LCDC_TE_CONF2      0xa4
#define LCDC_STAT0         0x110

/* COMMAND */
#define CMD_START (1U << 0)
#define CMD_RESET (1U << 1)

/* STATUS */
#define STATUS_LCD_BUSY (1U << 0)

/* IRQ */
#define IRQ_EOF      (1U << 0)
#define IRQ_EOF_RAW  (1U << 16)

/* SETTING */
#define SETTING_EOF_MASK    (1U << 0)
#define SETTING_AUTO_GATE   (1U << 8)

/* LCD_IF_CONF */
#define IF_CONF_TAS(n)    (((n) & 0x7U) << 0)
#define IF_CONF_TAH(n)    (((n) & 0x7U) << 3)
#define IF_CONF_LCD_RSTB  (1U << 23)

/* LCD_SINGLE */
#define SINGLE_TYPE    (1U << 0)
#define SINGLE_WR_TRIG (1U << 1)
#define SINGLE_RD_TRIG (1U << 2)
#define SINGLE_BUSY    (1U << 3)

/* SPI_IF_CONF */
#define SPI_RD_LEN(n)  (((n) & 0x3U) << 20)
#define SPI_WR_LEN(n)  (((n) & 0x3U) << 22)
#define SPI_CLK_AUTO_DIS (1U << 25)
#define SPI_CS_NO_IDLE   (1U << 26)
#define SPI_CS_AUTO_DIS  (1U << 27)
#define SPI_CLK_INIT     (1U << 30)
#define SPI_3LINE        (4U << 17)

/* LCD_CONF */
#define CONF_INTF_8080 (0U << 2)
#define CONF_TARGET0   (0U << 0)

#define LCDC_CFG0_RESET 0x01055982U
#define LCDC_CFG1_RESET 0x80023307U

static QTestState *lcdc_qtest_start(void)
{
    return qtest_init("-machine " MACHINE " -serial null");
}

static uint32_t lcdc_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, LCDC_BASE + offset);
}

static void lcdc_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, LCDC_BASE + offset, value);
}

static uint32_t lcdc_qom_get_uint32(QTestState *qts, const char *property)
{
    QDict *r = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                         "{ 'path': %s, 'property': %s } }",
                         LCDC_QOM_PATH, property);
    uint32_t value = qdict_get_uint(r, "return");

    qobject_unref(r);
    return value;
}

/* Mirrors the driver's unbounded wait_busy(); bounded here to fail fast. */
static void lcdc_assert_idle(QTestState *qts)
{
    unsigned i;

    for (i = 0; i < 1000; i++) {
        uint32_t status = lcdc_readl(qts, LCDC_STATUS);
        uint32_t single = lcdc_readl(qts, LCDC_LCD_SINGLE);

        g_assert_cmphex(status & STATUS_LCD_BUSY, ==, 0);
        g_assert_cmphex(single & SINGLE_BUSY, ==, 0);
        g_assert_cmphex(single & (SINGLE_WR_TRIG | SINGLE_RD_TRIG), ==, 0);
    }
}

static void test_reset_values(void)
{
    QTestState *qts = lcdc_qtest_start();

    g_assert_cmphex(lcdc_readl(qts, LCDC_COMMAND), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_STATUS), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_IRQ), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_SETTING), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_CANVAS_TL), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LAYER0_CONF), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LAYER0_SRC), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LAYER0_CFG0), ==, LCDC_CFG0_RESET);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LAYER0_CFG1), ==, LCDC_CFG1_RESET);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LAYER0_STAT), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_CONF), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_IF_CONF), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_MEM), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_SINGLE), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_WR), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_RD), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_SPI_IF_CONF), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_TE_CONF), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_TE_CONF2), ==, 0);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "capture-count"), ==, 0);

    qtest_quit(qts);
}

static void test_register_store_readback(void)
{
    QTestState *qts = lcdc_qtest_start();

    lcdc_writel(qts, LCDC_LCD_CONF, 0x001fffff);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_CONF), ==, 0x001fffff);
    lcdc_writel(qts, LCDC_LCD_IF_CONF, 0x03ffffff);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_IF_CONF), ==, 0x03ffffff);
    lcdc_writel(qts, LCDC_SPI_IF_CONF, 0x7fffffff);
    g_assert_cmphex(lcdc_readl(qts, LCDC_SPI_IF_CONF), ==, 0x7fffffff);
    lcdc_writel(qts, LCDC_TE_CONF, 0x001fffff);
    g_assert_cmphex(lcdc_readl(qts, LCDC_TE_CONF), ==, 0x001fffff);
    lcdc_writel(qts, LCDC_TE_CONF2, 0xdeadbeef);
    g_assert_cmphex(lcdc_readl(qts, LCDC_TE_CONF2), ==, 0xdeadbeef);
    lcdc_writel(qts, LCDC_SETTING, 0x07ff017f);
    g_assert_cmphex(lcdc_readl(qts, LCDC_SETTING), ==, 0x07ff017f);
    lcdc_writel(qts, LCDC_LCD_WR, 0xa5a5a5a5);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_WR), ==, 0xa5a5a5a5);
    lcdc_writel(qts, LCDC_LCD_RD, 0x5a5a5a5a);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_RD), ==, 0x5a5a5a5a);

    qtest_quit(qts);
}

static void test_busy_bits_never_stick(void)
{
    QTestState *qts = lcdc_qtest_start();

    g_assert_cmphex(lcdc_readl(qts, LCDC_STATUS), ==, 0);
    /* 8080 command then data, each followed by a driver-style busy poll. */
    lcdc_writel(qts, LCDC_LCD_WR, 0x2c);
    lcdc_writel(qts, LCDC_LCD_SINGLE, SINGLE_WR_TRIG);
    lcdc_assert_idle(qts);
    lcdc_writel(qts, LCDC_LCD_WR, 0xaa);
    lcdc_writel(qts, LCDC_LCD_SINGLE, SINGLE_WR_TRIG | SINGLE_TYPE);
    lcdc_assert_idle(qts);
    /* TYPE is the only LCD_SINGLE bit that reads back. */
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_SINGLE), ==, SINGLE_TYPE);

    qtest_quit(qts);
}

static void test_capture_sequence(void)
{
    QTestState *qts = lcdc_qtest_start();

    /* 8080 command. */
    lcdc_writel(qts, LCDC_LCD_WR, 0x2c);
    lcdc_writel(qts, LCDC_LCD_SINGLE, SINGLE_WR_TRIG);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "capture-count"), ==, 1);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "last-type"), ==, 0);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "last-data"), ==, 0x2c);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "last-len"), ==, 1);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "last-is-read"), ==, 0);

    /* 8080 data byte. */
    lcdc_writel(qts, LCDC_LCD_WR, 0xaa);
    lcdc_writel(qts, LCDC_LCD_SINGLE, SINGLE_WR_TRIG | SINGLE_TYPE);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "capture-count"), ==, 2);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "last-type"), ==, 1);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "last-data"), ==, 0xaa);

    /* SPI bulk write: WR_LEN=3 -> four bytes captured from one LCD_WR. */
    lcdc_writel(qts, LCDC_SPI_IF_CONF, SPI_WR_LEN(3));
    lcdc_writel(qts, LCDC_LCD_WR, 0x11223344);
    lcdc_writel(qts, LCDC_LCD_SINGLE, SINGLE_WR_TRIG | SINGLE_TYPE);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "capture-count"), ==, 3);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "last-len"), ==, 4);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "last-data"), ==, 0x11223344);

    qtest_quit(qts);
}

static void test_irq_eof_and_w1c(void)
{
    QTestState *qts = lcdc_qtest_start();
    uint32_t irq;

    /* Unmasked EOF: COMMAND.START completes the (unrendered) frame. */
    lcdc_writel(qts, LCDC_SETTING, SETTING_EOF_MASK);
    lcdc_writel(qts, LCDC_COMMAND, CMD_START);
    irq = lcdc_readl(qts, LCDC_IRQ);
    g_assert_cmphex(irq & IRQ_EOF, ==, IRQ_EOF);
    g_assert_cmphex(irq & IRQ_EOF_RAW, ==, IRQ_EOF_RAW);
    /* W1C via the masked bit. */
    lcdc_writel(qts, LCDC_IRQ, IRQ_EOF);
    g_assert_cmphex(lcdc_readl(qts, LCDC_IRQ), ==, 0);

    /* Masked EOF: raw status latches, masked status stays clear. */
    lcdc_writel(qts, LCDC_SETTING, 0);
    lcdc_writel(qts, LCDC_COMMAND, CMD_START);
    irq = lcdc_readl(qts, LCDC_IRQ);
    g_assert_cmphex(irq & IRQ_EOF, ==, 0);
    g_assert_cmphex(irq & IRQ_EOF_RAW, ==, IRQ_EOF_RAW);
    lcdc_writel(qts, LCDC_SETTING, SETTING_EOF_MASK);
    g_assert_cmphex(lcdc_readl(qts, LCDC_IRQ) & IRQ_EOF, ==, IRQ_EOF);
    /* W1C via the raw bit. */
    lcdc_writel(qts, LCDC_IRQ, IRQ_EOF_RAW);
    g_assert_cmphex(lcdc_readl(qts, LCDC_IRQ), ==, 0);

    qtest_quit(qts);
}

static void test_read_path(void)
{
    QTestState *qts = lcdc_qtest_start();

    lcdc_writel(qts, LCDC_LCD_RD, 0xdead);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_RD), ==, 0xdead);
    /* RD_LEN=1 -> two-byte single read; no panel -> deterministic 0. */
    lcdc_writel(qts, LCDC_SPI_IF_CONF, SPI_RD_LEN(1));
    lcdc_writel(qts, LCDC_LCD_SINGLE, SINGLE_RD_TRIG);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_RD), ==, 0);
    g_assert_cmphex(qtest_readb(qts, LCDC_BASE + LCDC_LCD_RD), ==, 0);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "read-count"), ==, 1);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "last-is-read"), ==, 1);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "last-len"), ==, 2);

    qtest_quit(qts);
}

static void test_command_reset(void)
{
    QTestState *qts = lcdc_qtest_start();

    lcdc_writel(qts, LCDC_LCD_CONF, 0x12345);
    lcdc_writel(qts, LCDC_TE_CONF, 0x1);
    lcdc_writel(qts, LCDC_LAYER0_SRC, 0x20000000);
    lcdc_writel(qts, LCDC_SETTING, SETTING_EOF_MASK);
    lcdc_writel(qts, LCDC_COMMAND, CMD_START);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "capture-count"), ==, 0);

    lcdc_writel(qts, LCDC_COMMAND, CMD_RESET);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_CONF), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_TE_CONF), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LAYER0_SRC), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_SETTING), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_IRQ), ==, 0);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LAYER0_CFG0), ==, LCDC_CFG0_RESET);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LAYER0_CFG1), ==, LCDC_CFG1_RESET);

    qtest_quit(qts);
}

static void test_driver_like_init_sequence(void)
{
    QTestState *qts = lcdc_qtest_start();
    unsigned i;

    /* mipi_dbi_sf32lb.c:548-572: init sets these bits (clock is SoC-side). */
    lcdc_writel(qts, LCDC_SETTING,
                lcdc_readl(qts, LCDC_SETTING) | SETTING_AUTO_GATE);
    lcdc_writel(qts, LCDC_LCD_IF_CONF,
                lcdc_readl(qts, LCDC_LCD_IF_CONF) | IF_CONF_LCD_RSTB);
    g_assert_cmphex(lcdc_readl(qts, LCDC_SETTING) & SETTING_AUTO_GATE, ==,
                    SETTING_AUTO_GATE);
    g_assert_cmphex(lcdc_readl(qts, LCDC_LCD_IF_CONF) & IF_CONF_LCD_RSTB, ==,
                    IF_CONF_LCD_RSTB);

    /* 8080 configure: TAS/TAH + interface select (spi driver 296-308). */
    lcdc_writel(qts, LCDC_LCD_CONF, CONF_INTF_8080 | CONF_TARGET0);
    lcdc_writel(qts, LCDC_LCD_IF_CONF,
                lcdc_readl(qts, LCDC_LCD_IF_CONF) | IF_CONF_TAS(1) |
                IF_CONF_TAH(1));

    /* SPI configure variant: CS/clock auto-disable, 3-line, /2 divider. */
    lcdc_writel(qts, LCDC_SPI_IF_CONF,
                SPI_CS_AUTO_DIS | SPI_CS_NO_IDLE | SPI_CLK_AUTO_DIS |
                SPI_CLK_INIT | SPI_3LINE | (2U << 6));
    g_assert_cmphex(lcdc_readl(qts, LCDC_SPI_IF_CONF) & (0x7U << 17), ==,
                    SPI_3LINE);

    /* TE configure (mipi_dbi_sf32lb.c:513-539). */
    lcdc_writel(qts, LCDC_TE_CONF2, 100);
    lcdc_writel(qts, LCDC_TE_CONF, 0x3);
    g_assert_cmphex(lcdc_readl(qts, LCDC_TE_CONF2), ==, 100);

    /* Driver-like 8080 command+data stream (e.g. sleep-out then display-on). */
    lcdc_writel(qts, LCDC_LCD_WR, 0x11);
    lcdc_writel(qts, LCDC_LCD_SINGLE, SINGLE_WR_TRIG);
    lcdc_assert_idle(qts);
    lcdc_writel(qts, LCDC_LCD_SINGLE, SINGLE_RD_TRIG);
    lcdc_assert_idle(qts);
    for (i = 0; i < 8; i++) {
        lcdc_writel(qts, LCDC_LCD_WR, i);
        lcdc_writel(qts, LCDC_LCD_SINGLE, SINGLE_WR_TRIG | SINGLE_TYPE);
        lcdc_assert_idle(qts);
    }
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "capture-count"), ==, 10);
    g_assert_cmpuint(lcdc_qom_get_uint32(qts, "read-count"), ==, 1);

    /* Clear the EOF latch as the driver error path does (spi_sf32lb.c:625). */
    lcdc_writel(qts, LCDC_SETTING, SETTING_EOF_MASK);
    lcdc_writel(qts, LCDC_COMMAND, CMD_START);
    g_assert_cmphex(lcdc_readl(qts, LCDC_IRQ) & IRQ_EOF, ==, IRQ_EOF);
    lcdc_writel(qts, LCDC_IRQ, IRQ_EOF);
    g_assert_cmphex(lcdc_readl(qts, LCDC_IRQ), ==, 0);

    qtest_quit(qts);
}

static void test_bad_offsets_and_ro(void)
{
    QTestState *qts = lcdc_qtest_start();

    /* Reserved/beyond-window accesses are guest errors, not hangs. */
    g_assert_cmphex(qtest_readl(qts, LCDC_BASE + 0x44), ==, 0);
    g_assert_cmphex(qtest_readl(qts, LCDC_BASE + 0xffc), ==, 0);
    qtest_writel(qts, LCDC_BASE + 0x44, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, LCDC_BASE + 0x44), ==, 0);
    /* STATUS and the debug status registers are read-only. */
    lcdc_writel(qts, LCDC_STATUS, 0xffffffff);
    g_assert_cmphex(lcdc_readl(qts, LCDC_STATUS), ==, 0);
    lcdc_writel(qts, LCDC_STAT0, 0xffffffff);
    g_assert_cmphex(lcdc_readl(qts, LCDC_STAT0), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sf32lb52x/lcdc/reset", test_reset_values);
    qtest_add_func("/sf32lb52x/lcdc/register-store",
                   test_register_store_readback);
    qtest_add_func("/sf32lb52x/lcdc/busy-bits", test_busy_bits_never_stick);
    qtest_add_func("/sf32lb52x/lcdc/capture-sequence",
                   test_capture_sequence);
    qtest_add_func("/sf32lb52x/lcdc/irq-eof-w1c", test_irq_eof_and_w1c);
    qtest_add_func("/sf32lb52x/lcdc/read-path", test_read_path);
    qtest_add_func("/sf32lb52x/lcdc/command-reset", test_command_reset);
    qtest_add_func("/sf32lb52x/lcdc/driver-init",
                   test_driver_like_init_sequence);
    qtest_add_func("/sf32lb52x/lcdc/bad-offs", test_bad_offsets_and_ro);

    return g_test_run();
}
