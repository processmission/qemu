/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register tests for the Rockchip RK3566 RGA2 model.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "rk3566-test-common.h"

#define RK3566_RGA2_BASE          0xfdeb0000
#define RK3566_RGA2_IRQ           90

/* RGA2 control registers. */
#define RK3566_RGA2_SYS_CTRL      0x00
#define RK3566_RGA2_CMD_CTRL      0x04
#define RK3566_RGA2_CMD_BASE      0x08
#define RK3566_RGA2_STATUS        0x0c
#define RK3566_RGA2_INTERRUPT     0x10
#define RK3566_RGA2_VERSION       0x28
#define RK3566_RGA2_VERSION_VALUE 0x02000000
#define RK3566_RGA2_COMMAND       0x100

/* Interrupt status bits, their write-one-to-clear positions and masks. */
#define RK3566_RGA2_INT_ERROR      (1u << 0)
#define RK3566_RGA2_INT_DONE       ((1u << 2) | (1u << 3))
#define RK3566_RGA2_INT_ERROR_ACK  (1u << 4)
#define RK3566_RGA2_INT_DONE_ACK   ((1u << 6) | (1u << 7))
#define RK3566_RGA2_INT_ERROR_MASK (1u << 8)
#define RK3566_RGA2_INT_DONE_MASK  (1u << 10)

/* Command block and image buffers in guest DRAM. */
#define RK3566_RGA2_CMD_DRAM       0x1000
#define RK3566_RGA2_SRC_DRAM       0x2000
#define RK3566_RGA2_DST_DRAM       0x3000

/* Offsets inside the 32-word command block. */
#define RK3566_RGA2_CMD_SRC_BASE   0x08
#define RK3566_RGA2_CMD_SRC_VIR    0x18
#define RK3566_RGA2_CMD_SRC_ACT    0x1c
#define RK3566_RGA2_CMD_DST_BASE   0x3c
#define RK3566_RGA2_CMD_DST_VIR    0x48
#define RK3566_RGA2_CMD_DST_ACT    0x4c

/* A 4x4 RGBA image, 16 bytes per row. */
#define RK3566_RGA2_IMAGE_ACT      0x00030003
#define RK3566_RGA2_IMAGE_STRIDE   4

/*
 * Offsets outside the described register bank.  The device keeps the plain
 * storage fallback of the original model for these addresses.
 */
static const uint32_t rk3566_rga2_unlisted[] = {
    0x14, 0x2c, 0xfc, 0x180, 0x400, 0xffc,
};

static bool rk3566_rga2_start(QTestState **qts)
{
    *qts = rk3566_qtest_start();
    if (!rk3566_qtest_media_present(*qts)) {
        g_test_skip("RK3566 media devices are not built");
        qtest_quit(*qts);
        *qts = NULL;
        return false;
    }
    return true;
}

static void test_rk3566_rga2_reset_values(void)
{
    QTestState *qts;

    if (!rk3566_rga2_start(&qts)) {
        return;
    }

    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_SYS_CTRL), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_CMD_CTRL), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_CMD_BASE), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_STATUS), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_INTERRUPT), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_VERSION), ==,
                    RK3566_RGA2_VERSION_VALUE);

    /* CMD_BASE is plain storage. */
    rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE, RK3566_RGA2_CMD_BASE,
                           0x12345678);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_CMD_BASE), ==,
                    0x12345678);

    /* Bit 3 of SYS_CTRL is a self-clearing soft reset. */
    rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE, RK3566_RGA2_SYS_CTRL,
                           0x18);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_SYS_CTRL), ==, 0x10);

    /* VERSION and STATUS are read-only. */
    rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE, RK3566_RGA2_VERSION,
                           0xffffffff);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_VERSION), ==,
                    RK3566_RGA2_VERSION_VALUE);
    rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE, RK3566_RGA2_STATUS, 1);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_STATUS), ==, 0);

    /* The command block words are plain storage. */
    rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE, RK3566_RGA2_COMMAND,
                           0x55aa55aa);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_COMMAND), ==, 0x55aa55aa);

    /* Unlisted offsets keep their plain storage. */
    rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE, 0x20, 0xa5a5a5a5);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE, 0x20), ==,
                    0xa5a5a5a5);

    qtest_quit(qts);
}

static void test_rk3566_rga2_command_error_irq(void)
{
    QTestState *qts;

    if (!rk3566_rga2_start(&qts)) {
        return;
    }

    qtest_irq_intercept_in(qts, "/machine/gic");

    /* Unmask the error interrupt, then start a command with an empty block. */
    rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE, RK3566_RGA2_INTERRUPT,
                           RK3566_RGA2_INT_ERROR_MASK);
    rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE, RK3566_RGA2_CMD_BASE, 0);
    rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE, RK3566_RGA2_CMD_CTRL, 1);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_STATUS), ==, 1);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_SYS_CTRL) & 1, ==, 1);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_CMD_CTRL), ==, 1);

    /*
     * The command runs from a timer.  The zeroed block selects no valid
     * image, so the model reports the error interrupt and clears the busy
     * state.
     */
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_STATUS), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_CMD_CTRL), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_INTERRUPT), ==,
                    RK3566_RGA2_INT_ERROR_MASK | RK3566_RGA2_INT_ERROR);
    g_assert_true(qtest_get_irq(qts, RK3566_RGA2_IRQ));

    /* Writing bit 4 acknowledges the error bit and keeps the mask. */
    rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE, RK3566_RGA2_INTERRUPT,
                           RK3566_RGA2_INT_ERROR_ACK |
                           RK3566_RGA2_INT_ERROR_MASK);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_INTERRUPT), ==,
                    RK3566_RGA2_INT_ERROR_MASK);
    g_assert_false(qtest_get_irq(qts, RK3566_RGA2_IRQ));

    qtest_quit(qts);
}

static void test_rk3566_rga2_command_copy(void)
{
    static const uint32_t pixels[] = {
        0x04030201, 0x08070605, 0x0c0b0a09, 0x100f0e0d,
    };
    QTestState *qts;

    if (!rk3566_rga2_start(&qts)) {
        return;
    }

    qtest_irq_intercept_in(qts, "/machine/gic");

    /* Build a 4x4 RGBA to RGBA copy command in guest DRAM. */
    rk3566_mmio_write32(qts, RK3566_RGA2_CMD_DRAM +
                             RK3566_RGA2_CMD_SRC_BASE,
                        RK3566_RGA2_SRC_DRAM);
    rk3566_mmio_write32(qts, RK3566_RGA2_CMD_DRAM +
                             RK3566_RGA2_CMD_SRC_VIR,
                        RK3566_RGA2_IMAGE_STRIDE);
    rk3566_mmio_write32(qts, RK3566_RGA2_CMD_DRAM +
                             RK3566_RGA2_CMD_SRC_ACT,
                        RK3566_RGA2_IMAGE_ACT);
    rk3566_mmio_write32(qts, RK3566_RGA2_CMD_DRAM +
                             RK3566_RGA2_CMD_DST_BASE,
                        RK3566_RGA2_DST_DRAM);
    rk3566_mmio_write32(qts, RK3566_RGA2_CMD_DRAM +
                             RK3566_RGA2_CMD_DST_VIR,
                        RK3566_RGA2_IMAGE_STRIDE);
    rk3566_mmio_write32(qts, RK3566_RGA2_CMD_DRAM +
                             RK3566_RGA2_CMD_DST_ACT,
                        RK3566_RGA2_IMAGE_ACT);
    for (unsigned int i = 0; i < ARRAY_SIZE(pixels); i++) {
        rk3566_mmio_write32(qts, RK3566_RGA2_SRC_DRAM + i * 4, pixels[i]);
    }

    rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE, RK3566_RGA2_INTERRUPT,
                           RK3566_RGA2_INT_DONE_MASK);
    rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE, RK3566_RGA2_CMD_BASE,
                           RK3566_RGA2_CMD_DRAM);
    rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE, RK3566_RGA2_CMD_CTRL, 1);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_STATUS), ==, 1);

    qtest_clock_step(qts, 2000);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_STATUS), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_INTERRUPT), ==,
                    RK3566_RGA2_INT_DONE_MASK | RK3566_RGA2_INT_DONE);
    g_assert_true(qtest_get_irq(qts, RK3566_RGA2_IRQ));
    for (unsigned int i = 0; i < ARRAY_SIZE(pixels); i++) {
        g_assert_cmphex(rk3566_mmio_read32(qts,
                                          RK3566_RGA2_DST_DRAM + i * 4), ==,
                        pixels[i]);
    }

    /* Bits 6..7 acknowledge the completion bits. */
    rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE, RK3566_RGA2_INTERRUPT,
                           RK3566_RGA2_INT_DONE_ACK |
                           RK3566_RGA2_INT_DONE_MASK);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                        RK3566_RGA2_INTERRUPT), ==,
                    RK3566_RGA2_INT_DONE_MASK);
    g_assert_false(qtest_get_irq(qts, RK3566_RGA2_IRQ));

    qtest_quit(qts);
}

static void test_rk3566_rga2_storage_fallback(void)
{
    QTestState *qts;

    if (!rk3566_rga2_start(&qts)) {
        return;
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(rk3566_rga2_unlisted); i++) {
        rk3566_mmio_write32_at(qts, RK3566_RGA2_BASE,
                               rk3566_rga2_unlisted[i], 0xdeadbeef);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(rk3566_rga2_unlisted); i++) {
        g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_RGA2_BASE,
                                            rk3566_rga2_unlisted[i]), ==,
                        0xdeadbeef);
    }

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566/rga2-reset-values",
                   test_rk3566_rga2_reset_values);
    qtest_add_func("/rk3566/rga2-command-error-irq",
                   test_rk3566_rga2_command_error_irq);
    qtest_add_func("/rk3566/rga2-command-copy",
                   test_rk3566_rga2_command_copy);
    qtest_add_func("/rk3566/rga2-storage-fallback",
                   test_rk3566_rga2_storage_fallback);

    return g_test_run();
}
