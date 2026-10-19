/*
 * QTest for the SiFli SF32LB52X I2C controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Behaviour covered (SVD I2C1, vendor HAL i2c.h, Zephyr
 * drivers/i2c/i2c_sf32lb.c command sequence):
 *  - SVD reset values (SAR/LCR/WCR/RCCR/BMR) and control register masks;
 *  - self-clearing unit/bus reset bits;
 *  - NACK path for an absent target, sticky until the next START;
 *  - SR W1C event bits vs sticky NACK;
 *  - interrupt level from IER & SR, cleared by W1C;
 *  - DBR data path and FIFO push/pop with OF/UF flags;
 *  - positive ACK transaction (write then read) against an i2c-echo slave
 *    attached by the test to one controller instance;
 *  - unimplemented/reserved and out-of-window offsets.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "sf32lb52-devkit-lcd"
#define I2C_BASE0 0x5009c000U
#define I2C_STRIDE 0x1000U
#define NUM_I2C 4

#define REG_CR     0x00
#define REG_TCR    0x04
#define REG_IER    0x08
#define REG_SR     0x0c
#define REG_DBR    0x10
#define REG_SAR    0x14
#define REG_LCR    0x18
#define REG_WCR    0x1c
#define REG_RCCR   0x20
#define REG_BMR    0x24
#define REG_DNR    0x28
#define REG_RSVD1  0x2c
#define REG_FIFO   0x30

#define CR_SCLE    (1U << 3)
#define CR_IUE     (1U << 2)
#define CR_MSDE    (1U << 8)
#define CR_BRGRST  (1U << 29)
#define CR_RSTREQ  (1U << 30)
#define CR_UR      (1U << 31)

#define TCR_TB     (1U << 0)
#define TCR_START  (1U << 1)
#define TCR_STOP   (1U << 2)
#define TCR_NACK   (1U << 3)
#define TCR_MA     (1U << 4)

#define SR_RWM     (1U << 0)
#define SR_NACK    (1U << 1)
#define SR_UB      (1U << 2)
#define SR_TE      (1U << 6)
#define SR_RF      (1U << 7)
#define SR_MSD     (1U << 12)
#define SR_OF      (1U << 14)
#define SR_UF      (1U << 15)

#define IER_TEIE   (1U << 6)
#define IER_RFIE   (1U << 7)
#define IER_MSDIE  (1U << 12)

#define ECHO_ADDR  0x42
#define ECHO_DBR   ((ECHO_ADDR << 1) | 0)
#define ECHO_DBR_R ((ECHO_ADDR << 1) | 1)

static uint32_t i2c_readl(QTestState *qts, uint32_t base, uint32_t off)
{
    uint32_t value;

    qtest_memread(qts, base + off, &value, sizeof(value));
    return value;
}

static void i2c_writel(QTestState *qts, uint32_t base, uint32_t off,
                       uint32_t value)
{
    qtest_memwrite(qts, base + off, &value, sizeof(value));
}

static QTestState *i2c_qtest_start(const char *extra)
{
    return qtest_initf("-machine " MACHINE " -serial null %s", extra);
}

/* Probe every instance for an ACK at ECHO_ADDR; exactly one must ACK. */
static uint32_t i2c_find_acked_base(QTestState *qts)
{
    uint32_t acked = 0;
    unsigned int found = 0;
    int i;

    for (i = 0; i < NUM_I2C; i++) {
        uint32_t base = I2C_BASE0 + i * I2C_STRIDE;
        uint32_t sr;

        i2c_writel(qts, base, REG_CR, CR_IUE | CR_MSDE);
        i2c_writel(qts, base, REG_DBR, ECHO_DBR);
        i2c_writel(qts, base, REG_TCR, TCR_START | TCR_TB);
        sr = i2c_readl(qts, base, REG_SR);
        if ((sr & SR_TE) && !(sr & SR_NACK)) {
            acked = base;
            found++;
        }
        /* Abort the probe and release the bus: UR resets the unit. */
        i2c_writel(qts, base, REG_CR, CR_UR);
    }

    g_assert_cmpuint(found, ==, 1);
    return acked;
}

static void test_i2c_reset_values(void)
{
    QTestState *qts = i2c_qtest_start("");
    uint32_t base = I2C_BASE0;

    g_assert_cmphex(i2c_readl(qts, base, REG_CR), ==, 0);
    g_assert_cmphex(i2c_readl(qts, base, REG_TCR), ==, 0);
    g_assert_cmphex(i2c_readl(qts, base, REG_IER), ==, 0);
    g_assert_cmphex(i2c_readl(qts, base, REG_SR), ==, 0);
    g_assert_cmphex(i2c_readl(qts, base, REG_DBR), ==, 0);
    g_assert_cmphex(i2c_readl(qts, base, REG_DNR), ==, 0);
    g_assert_cmphex(i2c_readl(qts, base, REG_FIFO), ==, 0);
    /* SVD I2C1 resets. */
    g_assert_cmphex(i2c_readl(qts, base, REG_SAR), ==, 0x47);
    g_assert_cmphex(i2c_readl(qts, base, REG_LCR), ==, 0x081c72ed);
    g_assert_cmphex(i2c_readl(qts, base, REG_WCR), ==, 0x0a);
    g_assert_cmphex(i2c_readl(qts, base, REG_RCCR), ==, 0x09);
    g_assert_cmphex(i2c_readl(qts, base, REG_BMR), ==, 0x03);

    qtest_quit(qts);
}

static void test_i2c_control_selfclear(void)
{
    QTestState *qts = i2c_qtest_start("");
    uint32_t base = I2C_BASE0;
    uint32_t cr;

    i2c_writel(qts, base, REG_CR, 1 | CR_IUE | CR_SCLE | CR_MSDE);
    cr = i2c_readl(qts, base, REG_CR);
    g_assert_cmphex(cr & 0x7fff, ==, 1 | CR_IUE | CR_SCLE | CR_MSDE);

    /* BRGRST and RSTREQ are self-clearing pulses. */
    i2c_writel(qts, base, REG_CR, cr | CR_BRGRST | CR_RSTREQ);
    cr = i2c_readl(qts, base, REG_CR);
    g_assert_cmphex(cr & (CR_BRGRST | CR_RSTREQ), ==, 0);
    g_assert_cmphex(cr & (1 | CR_IUE | CR_SCLE | CR_MSDE), ==,
                    1 | CR_IUE | CR_SCLE | CR_MSDE);

    /* UR resets the whole unit back to the SVD reset values. */
    i2c_writel(qts, base, REG_SAR, 0x55);
    i2c_writel(qts, base, REG_CR, cr | CR_UR);
    g_assert_cmphex(i2c_readl(qts, base, REG_CR), ==, 0);
    g_assert_cmphex(i2c_readl(qts, base, REG_SAR), ==, 0x47);
    g_assert_cmphex(i2c_readl(qts, base, REG_LCR), ==, 0x081c72ed);
    g_assert_cmphex(i2c_readl(qts, base, REG_WCR), ==, 0x0a);
    g_assert_cmphex(i2c_readl(qts, base, REG_RCCR), ==, 0x09);

    /* Software-visible masks. */
    i2c_writel(qts, base, REG_SAR, 0xffffffff);
    g_assert_cmphex(i2c_readl(qts, base, REG_SAR), ==, 0x7f);
    i2c_writel(qts, base, REG_DNR, 0xffffffff);
    g_assert_cmphex(i2c_readl(qts, base, REG_DNR), ==, 0x1ff);
    i2c_writel(qts, base, REG_RCCR, 0xffffffff);
    g_assert_cmphex(i2c_readl(qts, base, REG_RCCR), ==, 0x0f);

    qtest_quit(qts);
}

static void test_i2c_nack_sticky_w1c(void)
{
    QTestState *qts = i2c_qtest_start("");
    uint32_t base = I2C_BASE0;
    uint32_t sr;

    i2c_writel(qts, base, REG_CR, CR_IUE | CR_MSDE);
    /* Zero-length stop transfer to an absent target. */
    i2c_writel(qts, base, REG_DBR, 0x50 << 1);
    i2c_writel(qts, base, REG_TCR, TCR_START | TCR_TB | TCR_MA | TCR_STOP);

    sr = i2c_readl(qts, base, REG_SR);
    g_assert_cmphex(sr & (SR_TE | SR_NACK | SR_MSD), ==,
                    SR_TE | SR_NACK | SR_MSD);
    g_assert_cmphex(sr & (SR_UB | SR_RWM), ==, 0);

    /* Writing SR back clears the W1C event bits but not the NACK status. */
    i2c_writel(qts, base, REG_SR, sr);
    sr = i2c_readl(qts, base, REG_SR);
    g_assert_cmphex(sr & (SR_TE | SR_MSD), ==, 0);
    g_assert_cmphex(sr & SR_NACK, ==, SR_NACK);

    /* NACK is not W1C: writing bit 1 back leaves it set. */
    i2c_writel(qts, base, REG_SR, SR_NACK);
    g_assert_cmphex(i2c_readl(qts, base, REG_SR) & SR_NACK, ==, SR_NACK);

    qtest_quit(qts);
}

static void test_i2c_irq_level(void)
{
    QTestState *qts = i2c_qtest_start("");
    uint32_t base = I2C_BASE0;
    uint32_t sr;

    qtest_irq_intercept_out_named(qts, "/machine/soc/i2c[0]", "sysbus-irq");
    g_assert_false(qtest_get_irq(qts, 0));

    i2c_writel(qts, base, REG_IER, IER_TEIE | IER_MSDIE);
    i2c_writel(qts, base, REG_CR, CR_IUE | CR_MSDE);
    i2c_writel(qts, base, REG_DBR, 0x50 << 1);
    i2c_writel(qts, base, REG_TCR, TCR_START | TCR_TB | TCR_MA | TCR_STOP);

    sr = i2c_readl(qts, base, REG_SR);
    g_assert_cmphex(sr & (SR_TE | SR_MSD | SR_NACK), ==,
                    SR_TE | SR_MSD | SR_NACK);
    g_assert_true(qtest_get_irq(qts, 0));

    i2c_writel(qts, base, REG_SR, SR_TE | SR_MSD);
    g_assert_false(qtest_get_irq(qts, 0));

    /* Masking the sources also drops the level. */
    i2c_writel(qts, base, REG_IER, 0);
    i2c_writel(qts, base, REG_DBR, 0x51 << 1);
    i2c_writel(qts, base, REG_TCR, TCR_START | TCR_TB);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_i2c_fifo_and_offsets(void)
{
    QTestState *qts = i2c_qtest_start("");
    uint32_t base = I2C_BASE0;

    i2c_writel(qts, base, REG_FIFO, 0x11);
    i2c_writel(qts, base, REG_FIFO, 0x22);
    g_assert_cmphex(i2c_readl(qts, base, REG_FIFO), ==, 0x11);
    g_assert_cmphex(i2c_readl(qts, base, REG_FIFO), ==, 0x22);

    /* Empty FIFO sets UF, which is W1C. */
    (void)i2c_readl(qts, base, REG_FIFO);
    g_assert_cmphex(i2c_readl(qts, base, REG_SR) & SR_UF, ==, SR_UF);
    i2c_writel(qts, base, REG_SR, SR_UF);
    g_assert_cmphex(i2c_readl(qts, base, REG_SR) & SR_UF, ==, 0);

    /* Overflow: 16 entries fit, the 17th sets OF. */
    for (int i = 0; i < 16; i++) {
        i2c_writel(qts, base, REG_FIFO, i);
    }
    i2c_writel(qts, base, REG_FIFO, 0x33);
    g_assert_cmphex(i2c_readl(qts, base, REG_SR) & SR_OF, ==, SR_OF);

    /* Reserved register and out-of-window accesses are logged, read as 0. */
    g_assert_cmphex(i2c_readl(qts, base, REG_RSVD1), ==, 0);
    g_assert_cmphex(i2c_readl(qts, base, 0x800), ==, 0);
    i2c_writel(qts, base, 0x800, 0xffffffff);

    qtest_quit(qts);
}

static void test_i2c_echo_zero_length_stop(void)
{
    QTestState *qts = i2c_qtest_start(
        "-device i2c-echo,address=0x42,bus=i2c");
    uint32_t base = i2c_find_acked_base(qts);
    uint32_t sr;

    i2c_writel(qts, base, REG_CR, CR_IUE | CR_MSDE);

    /* A failed address first: NACK must be reported. */
    i2c_writel(qts, base, REG_DBR, 0x11 << 1);
    i2c_writel(qts, base, REG_TCR, TCR_START | TCR_TB | TCR_MA | TCR_STOP);
    g_assert_cmphex(i2c_readl(qts, base, REG_SR) & SR_NACK, ==, SR_NACK);

    /* Then the ACKed target: the new START clears the previous NACK. */
    i2c_writel(qts, base, REG_DBR, ECHO_DBR);
    i2c_writel(qts, base, REG_TCR, TCR_START | TCR_TB | TCR_MA | TCR_STOP);
    sr = i2c_readl(qts, base, REG_SR);
    g_assert_cmphex(sr & SR_TE, ==, SR_TE);
    g_assert_cmphex(sr & SR_NACK, ==, 0);
    g_assert_cmphex(sr & SR_MSD, ==, SR_MSD);
    g_assert_cmphex(sr & SR_UB, ==, 0);

    qtest_quit(qts);
}

static void test_i2c_echo_write_then_read(void)
{
    QTestState *qts = i2c_qtest_start(
        "-device i2c-echo,address=0x42,bus=i2c");
    uint32_t base = i2c_find_acked_base(qts);
    uint32_t sr;

    i2c_writel(qts, base, REG_CR, CR_IUE | CR_MSDE);

    /* Write message: START + address write. */
    i2c_writel(qts, base, REG_DBR, ECHO_DBR);
    i2c_writel(qts, base, REG_TCR, TCR_START | TCR_TB);
    sr = i2c_readl(qts, base, REG_SR);
    g_assert_cmphex(sr & (SR_TE | SR_UB), ==, SR_TE | SR_UB);
    g_assert_cmphex(sr & (SR_NACK | SR_RWM), ==, 0);
    i2c_writel(qts, base, REG_SR, sr);

    /* One data byte then STOP. */
    i2c_writel(qts, base, REG_DBR, 0x5a);
    i2c_writel(qts, base, REG_TCR, TCR_TB | TCR_STOP);
    sr = i2c_readl(qts, base, REG_SR);
    g_assert_cmphex(sr & SR_TE, ==, SR_TE);
    g_assert_cmphex(sr & SR_MSD, ==, SR_MSD);
    g_assert_cmphex(sr & SR_UB, ==, 0);
    i2c_writel(qts, base, REG_SR, sr);

    /* Read message: START + address read. */
    i2c_writel(qts, base, REG_DBR, ECHO_DBR_R);
    i2c_writel(qts, base, REG_TCR, TCR_START | TCR_TB);
    sr = i2c_readl(qts, base, REG_SR);
    g_assert_cmphex(sr & (SR_TE | SR_UB | SR_RWM), ==,
                    SR_TE | SR_UB | SR_RWM);
    g_assert_cmphex(sr & SR_NACK, ==, 0);
    i2c_writel(qts, base, REG_SR, sr);

    /* Single byte read: NACK + STOP on the last byte. */
    i2c_writel(qts, base, REG_TCR, TCR_TB | TCR_NACK | TCR_STOP);
    sr = i2c_readl(qts, base, REG_SR);
    g_assert_cmphex(sr & SR_RF, ==, SR_RF);
    g_assert_cmphex(sr & SR_MSD, ==, SR_MSD);
    g_assert_cmphex(sr & SR_UB, ==, 0);
    /* i2c-echo returns the byte previously written to it. */
    g_assert_cmphex(i2c_readl(qts, base, REG_DBR), ==, 0x5a);
    i2c_writel(qts, base, REG_SR, SR_RF | SR_MSD);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sf32lb52x/i2c/reset", test_i2c_reset_values);
    qtest_add_func("/sf32lb52x/i2c/control-selfclear",
                   test_i2c_control_selfclear);
    qtest_add_func("/sf32lb52x/i2c/nack-sticky-w1c",
                   test_i2c_nack_sticky_w1c);
    qtest_add_func("/sf32lb52x/i2c/irq-level", test_i2c_irq_level);
    qtest_add_func("/sf32lb52x/i2c/fifo-offsets",
                   test_i2c_fifo_and_offsets);
    qtest_add_func("/sf32lb52x/i2c/echo-zero-length-stop",
                   test_i2c_echo_zero_length_stop);
    qtest_add_func("/sf32lb52x/i2c/echo-write-read",
                   test_i2c_echo_write_then_read);

    return g_test_run();
}
