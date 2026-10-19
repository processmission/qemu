/*
 * QTest for the SiFli SF32LB52X MAILBOX controllers
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Covers the transport-level register model (SVD/HAL mailbox1.h and
 * mailbox2.h): per-block channel registers, ITR trigger, W1C ICR, derived
 * MISR, IER masking, level interrupt and EXR mutex state. The qtest
 * protocol allows intercepting only one device's IRQ per QEMU process,
 * so the LPSYS instance runs in a second QEMU.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "sf32lb52-devkit-lcd"

#define MAILBOX1_BASE 0x50082000
#define MAILBOX2_BASE 0x40002000

#define BLOCK_STRIDE 0x18
#define REG_IER 0x00
#define REG_ITR 0x04
#define REG_ICR 0x08
#define REG_ISR 0x0c
#define REG_MISR 0x10
#define REG_EXR 0x14

#define CHANNEL_MASK 0x0000ffffU
#define EXR_EX (1U << 31)

#define MAILBOX1_QOM_PATH "/machine/soc/mailbox[0]"
#define MAILBOX2_QOM_PATH "/machine/soc/mailbox[1]"

/* HPSYS instance: one intercepted IRQ line, index 0. */
static QTestState *mailbox_hpsys_start(void)
{
    QTestState *qts = qtest_init("-machine " MACHINE " -serial null");

    qtest_irq_intercept_out_named(qts, MAILBOX1_QOM_PATH, "sysbus-irq");
    return qts;
}

/* LPSYS instance in its own QEMU: one intercepted IRQ line, index 0. */
static QTestState *mailbox_lpsys_start(void)
{
    QTestState *qts = qtest_init("-machine " MACHINE " -serial null");

    qtest_irq_intercept_out_named(qts, MAILBOX2_QOM_PATH, "sysbus-irq");
    return qts;
}

static uint32_t mailbox_read(QTestState *qts, uint32_t base, unsigned int block,
                             unsigned int reg)
{
    uint32_t value;

    qtest_memread(qts, base + block * BLOCK_STRIDE + reg, &value,
                  sizeof(value));
    return value;
}

static void mailbox_write(QTestState *qts, uint32_t base, unsigned int block,
                          unsigned int reg, uint32_t value)
{
    qtest_memwrite(qts, base + block * BLOCK_STRIDE + reg, &value,
                   sizeof(value));
}

static void test_mailbox_reset(void)
{
    QTestState *qts = mailbox_hpsys_start();
    unsigned int block;
    static const unsigned int regs[] = {
        REG_IER, REG_ITR, REG_ICR, REG_ISR, REG_MISR, REG_EXR,
    };
    unsigned int i;

    for (block = 0; block < 4; block++) {
        for (i = 0; i < ARRAY_SIZE(regs); i++) {
            g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, block,
                                         regs[i]), ==, 0);
        }
    }
    for (block = 0; block < 2; block++) {
        for (i = 0; i < ARRAY_SIZE(regs); i++) {
            g_assert_cmphex(mailbox_read(qts, MAILBOX2_BASE, block,
                                         regs[i]), ==, 0);
        }
    }
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

/* IER/EXR are RW storage; ITR/ICR read as zero; MISR is ISR & IER. */
static void test_mailbox_channel_registers(void)
{
    QTestState *qts = mailbox_hpsys_start();

    mailbox_write(qts, MAILBOX1_BASE, 0, REG_IER, CHANNEL_MASK);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_IER), ==,
                    CHANNEL_MASK);

    mailbox_write(qts, MAILBOX1_BASE, 0, REG_EXR, EXR_EX | 0x5);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_EXR), ==,
                    EXR_EX | 0x5);

    /* ITR sets the matching ISR bit and is itself write-only. */
    mailbox_write(qts, MAILBOX1_BASE, 0, REG_ITR, 1U << 3);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_ITR), ==, 0);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_ISR), ==,
                    1U << 3);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_MISR), ==,
                    1U << 3);

    /* Masking hides the status from MISR but not from raw ISR. */
    mailbox_write(qts, MAILBOX1_BASE, 0, REG_IER, 0);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_ISR), ==,
                    1U << 3);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_MISR), ==, 0);

    /* ICR write-1 clears the status bit; ICR reads back zero. */
    mailbox_write(qts, MAILBOX1_BASE, 0, REG_ICR, 1U << 3);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_ICR), ==, 0);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_ISR), ==, 0);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_MISR), ==, 0);

    qtest_quit(qts);
}

/* IER unmask + ITR trigger drives the instance IRQ; ICR clears it. */
static void test_mailbox_irq_level(void)
{
    QTestState *qts = mailbox_hpsys_start();

    mailbox_write(qts, MAILBOX1_BASE, 0, REG_IER, 1U << 3);
    mailbox_write(qts, MAILBOX1_BASE, 0, REG_ITR, 1U << 3);
    g_assert_true(qtest_get_irq(qts, 0));

    mailbox_write(qts, MAILBOX1_BASE, 0, REG_ICR, 1U << 3);
    g_assert_false(qtest_get_irq(qts, 0));

    /* Masked trigger: raw status set, MISR and IRQ stay clear. */
    mailbox_write(qts, MAILBOX1_BASE, 0, REG_IER, 0);
    mailbox_write(qts, MAILBOX1_BASE, 0, REG_ITR, 1U << 3);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_ISR), ==,
                    1U << 3);
    g_assert_false(qtest_get_irq(qts, 0));
    mailbox_write(qts, MAILBOX1_BASE, 0, REG_ICR, 1U << 3);

    qtest_quit(qts);
}

/* The second instance has its own level interrupt (SoC IRQ80). */
static void test_mailbox_lpsys_irq(void)
{
    QTestState *qts = mailbox_lpsys_start();

    mailbox_write(qts, MAILBOX2_BASE, 1, REG_IER, 1U << 15);
    mailbox_write(qts, MAILBOX2_BASE, 1, REG_ITR, 1U << 15);
    g_assert_cmphex(mailbox_read(qts, MAILBOX2_BASE, 1, REG_MISR), ==,
                    1U << 15);
    g_assert_true(qtest_get_irq(qts, 0));

    mailbox_write(qts, MAILBOX2_BASE, 1, REG_ICR, 1U << 15);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

/* ICR is W1C: writing 0 leaves ISR bits untouched. */
static void test_mailbox_w1c(void)
{
    QTestState *qts = mailbox_hpsys_start();

    mailbox_write(qts, MAILBOX1_BASE, 0, REG_IER, CHANNEL_MASK);
    mailbox_write(qts, MAILBOX1_BASE, 0, REG_ITR, 0x0005);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_ISR), ==, 0x0005);

    mailbox_write(qts, MAILBOX1_BASE, 0, REG_ICR, 0);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_ISR), ==, 0x0005);

    mailbox_write(qts, MAILBOX1_BASE, 0, REG_ICR, 0x0001);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_ISR), ==, 0x0004);
    mailbox_write(qts, MAILBOX1_BASE, 0, REG_ICR, 0x0004);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_ISR), ==, 0);

    qtest_quit(qts);
}

/* Channels and blocks are independent. */
static void test_mailbox_channel_independence(void)
{
    QTestState *qts = mailbox_hpsys_start();

    mailbox_write(qts, MAILBOX1_BASE, 0, REG_IER, CHANNEL_MASK);
    mailbox_write(qts, MAILBOX1_BASE, 0, REG_ITR, (1U << 0) | (1U << 15));
    mailbox_write(qts, MAILBOX1_BASE, 1, REG_ITR, 1U << 0);

    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_ISR), ==,
                    (1U << 0) | (1U << 15));
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 1, REG_ISR), ==, 1U << 0);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 2, REG_ISR), ==, 0);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 3, REG_ISR), ==, 0);

    mailbox_write(qts, MAILBOX1_BASE, 0, REG_ICR, 1U << 0);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 0, REG_ISR), ==,
                    1U << 15);
    g_assert_cmphex(mailbox_read(qts, MAILBOX1_BASE, 1, REG_ISR), ==, 1U << 0);

    qtest_quit(qts);
}

/* Unimplemented offsets read as zero and are logged as guest errors. */
static void test_mailbox_bad_offset(void)
{
    QTestState *qts = mailbox_hpsys_start();
    uint32_t value;

    qtest_memread(qts, MAILBOX1_BASE + 0x100, &value, sizeof(value));
    g_assert_cmphex(value, ==, 0);
    qtest_memread(qts, MAILBOX2_BASE + 0x100, &value, sizeof(value));
    g_assert_cmphex(value, ==, 0);

    /* Offset past the implemented blocks (0x60/0x30) is not a register. */
    qtest_memread(qts, MAILBOX1_BASE + 0x60, &value, sizeof(value));
    g_assert_cmphex(value, ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/sf32lb52x/mailbox/reset", test_mailbox_reset);
    g_test_add_func("/sf32lb52x/mailbox/channel-registers",
                    test_mailbox_channel_registers);
    g_test_add_func("/sf32lb52x/mailbox/irq-level",
                    test_mailbox_irq_level);
    g_test_add_func("/sf32lb52x/mailbox/lpsys-irq",
                    test_mailbox_lpsys_irq);
    g_test_add_func("/sf32lb52x/mailbox/w1c", test_mailbox_w1c);
    g_test_add_func("/sf32lb52x/mailbox/channel-independence",
                    test_mailbox_channel_independence);
    g_test_add_func("/sf32lb52x/mailbox/bad-offset",
                    test_mailbox_bad_offset);

    return g_test_run();
}
