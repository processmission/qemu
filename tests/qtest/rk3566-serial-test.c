/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the Rockchip RK3566 UART clock gating and gated transmission.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "rk3566-test-common.h"

#define UART_THR          0x00
#define UART_LSR_THRE     (1u << 5)
#define UART_LSR_TEMT     (1u << 6)
#define UART_CLKGATE_VAL  0x6666

static QTestState *rk3566_serial_qtest_start(void)
{
    return qtest_initf("-machine " RK3566_MACHINE_NAME
                       " -m 512M -serial null");
}

static uint32_t uart2_lsr(QTestState *qts)
{
    return rk3566_mmio_read32_at(qts, RK3566_UART2_BASE,
                                 RK3566_DW8250_LSR);
}

static void test_uart_gated_transmit(void)
{
    QTestState *qts = rk3566_serial_qtest_start();
    uint32_t lsr;

    /*
     * Run the transmitter once with the functional clock enabled: the shift
     * register stays busy, so only THRE is reported.
     */
    rk3566_mmio_write32_at(qts, RK3566_UART2_BASE, UART_THR, 0x41);
    lsr = uart2_lsr(qts);
    g_assert_cmphex(lsr & UART_LSR_THRE, ==, UART_LSR_THRE);
    g_assert_cmphex(lsr & UART_LSR_TEMT, ==, 0);

    /*
     * The CRU clock gate registers take their write mask from the upper
     * half-word, so a plain write has no effect.
     */
    rk3566_mmio_write32_at(qts, RK3566_CRU_BASE,
                           RK3566_CRU_CLKGATE_UART2, UART_CLKGATE_VAL);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_CRU_BASE,
                                         RK3566_CRU_CLKGATE_UART2), ==, 0);

    /* Gate the UART2 functional clock. */
    rk3566_mmio_write32_at(qts, RK3566_CRU_BASE,
                           RK3566_CRU_CLKGATE_UART2,
                           RK3566_REG_WRITE_MASK(UART_CLKGATE_VAL));
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_CRU_BASE,
                                         RK3566_CRU_CLKGATE_UART2), ==,
                    UART_CLKGATE_VAL);

    /* Gating the clock completes the pending transfer. */
    lsr = uart2_lsr(qts);
    g_assert_cmphex(lsr & (UART_LSR_THRE | UART_LSR_TEMT), ==,
                    UART_LSR_THRE | UART_LSR_TEMT);

    /*
     * Writing THR while the clock is gated must not wedge the transmitter:
     * the fixed model hands the byte to the backend and reports the
     * transmitter as idle again, which is what early consoles poll for.
     */
    rk3566_mmio_write32_at(qts, RK3566_UART2_BASE, UART_THR, 0x42);
    lsr = uart2_lsr(qts);
    g_assert_cmphex(lsr & (UART_LSR_THRE | UART_LSR_TEMT), ==,
                    UART_LSR_THRE | UART_LSR_TEMT);

    /* A second write and poll behaves the same. */
    rk3566_mmio_write32_at(qts, RK3566_UART2_BASE, UART_THR, 0x43);
    lsr = uart2_lsr(qts);
    g_assert_cmphex(lsr & (UART_LSR_THRE | UART_LSR_TEMT), ==,
                    UART_LSR_THRE | UART_LSR_TEMT);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566/serial/gated-transmit",
                   test_uart_gated_transmit);

    return g_test_run();
}
