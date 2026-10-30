/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register, interrupt and timer tests for the RK3566 VEPU540 encoder model.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "rk3566-test-common.h"

/* RK3566 VEPU540 register window at 0xfdf40000 and its interrupt line. */
#define RK3566_VENC_BASE 0xfdf40000ULL
#define RK3566_VENC_IRQ 140
#define RK3566_VENC_VERSION 0x000
#define RK3566_VENC_START 0x004
#define RK3566_VENC_CLEAR 0x008
#define RK3566_VENC_INT_EN 0x010
#define RK3566_VENC_INT_MASK 0x014
#define RK3566_VENC_INT_CLEAR 0x018
#define RK3566_VENC_INT_STATUS 0x01c
#define RK3566_VENC_RESOLUTION 0x030
#define RK3566_VENC_PICTURE 0x034
#define RK3566_VENC_SOURCE_FORMAT 0x044
#define RK3566_VENC_STREAM_TOP 0x14c
#define RK3566_VENC_STREAM_BOTTOM 0x150
#define RK3566_VENC_STREAM_START 0x158
#define RK3566_VENC_STREAM_LENGTH 0x210
#define RK3566_VENC_ENCODE_STATUS 0x228
#define RK3566_VENC_BUS_STATUS 0x238
#define RK3566_VENC_L2_ADDRESS 0x3f0
#define RK3566_VENC_L2_WRITE 0x3f4
#define RK3566_VENC_L2_READ 0x3f8
#define RK3566_VENC_CACHE_CLEAR 0x884

#define RK3566_VENC_IRQ_CLEAR (1u << 2)
#define RK3566_VENC_IRQ_READ_ERROR (1u << 7)
#define RK3566_VENC_IRQ_TIMEOUT (1u << 8)

#define RK3566_VENC_GIC_QOM "/machine/gic"

static uint32_t venc_read(QTestState *qts, uint32_t offset)
{
    return rk3566_mmio_read32_at(qts, RK3566_VENC_BASE, offset);
}

static void venc_write(QTestState *qts, uint32_t offset, uint32_t value)
{
    rk3566_mmio_write32_at(qts, RK3566_VENC_BASE, offset, value);
}

static bool rk3566_venc_available(QTestState *qts)
{
    return rk3566_qom_child_type_exists(qts, "/machine", "rk3566-rkvenc");
}

static void test_rk3566_venc_reset_values(void)
{
    QTestState *qts = rk3566_qtest_start();
    g_autofree char *venc = NULL;

    if (!rk3566_venc_available(qts)) {
        g_test_skip("RK3566 VENC device is not built "
                    "(libavcodec, libavutil, libswscale or x264 missing)");
        qtest_quit(qts);
        return;
    }

    venc = rk3566_qom_child_object_type(qts, "/machine", "venc");
    g_assert_cmpstr(venc, ==, "rk3566-rkvenc");

    /* VEPU5 defaults come from the register table. */
    g_assert_cmphex(venc_read(qts, RK3566_VENC_VERSION), ==, 0x500f1111);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_BUS_STATUS), ==, 0x7e);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_INT_STATUS), ==, 0);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_INT_EN), ==, 0);

    /* VERSION and the encoder-owned status words ignore writes. */
    venc_write(qts, RK3566_VENC_VERSION, 0xffffffff);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_VERSION), ==, 0x500f1111);
    venc_write(qts, RK3566_VENC_BUS_STATUS, 0xffffffff);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_BUS_STATUS), ==, 0x7e);
    venc_write(qts, RK3566_VENC_STREAM_LENGTH, 0x1234);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_STREAM_LENGTH), ==, 0);

    /* Unlisted words inside the encoder-owned range also ignore writes. */
    venc_write(qts, 0x220, 0xffffffff);
    g_assert_cmphex(venc_read(qts, 0x220), ==, 0);

    /* Writable control words store the full 32-bit value. */
    venc_write(qts, RK3566_VENC_RESOLUTION, 0x003f403f);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_RESOLUTION), ==, 0x003f403f);
    venc_write(qts, RK3566_VENC_PICTURE, 0x0000ff00);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_PICTURE), ==, 0x0000ff00);

    /* Unlisted words outside the encoder-owned range keep plain storage. */
    venc_write(qts, 0x00c, 0x5a5a5a5a);
    g_assert_cmphex(venc_read(qts, 0x00c), ==, 0x5a5a5a5a);

    qtest_quit(qts);
}

static void test_rk3566_venc_interrupts(void)
{
    QTestState *qts = rk3566_qtest_start();

    if (!rk3566_venc_available(qts)) {
        g_test_skip("RK3566 VENC device is not built "
                    "(libavcodec, libavutil, libswscale or x264 missing)");
        qtest_quit(qts);
        return;
    }

    qtest_irq_intercept_in(qts, RK3566_VENC_GIC_QOM);

    /* A stale start state reports a timeout in the status register. */
    venc_write(qts, RK3566_VENC_INT_EN, RK3566_VENC_IRQ_TIMEOUT);
    venc_write(qts, RK3566_VENC_START, 2u << 8);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_INT_STATUS) &
                    RK3566_VENC_IRQ_TIMEOUT, ==, RK3566_VENC_IRQ_TIMEOUT);
    g_assert_true(qtest_get_irq(qts, RK3566_VENC_IRQ));

    /* Writing 1 to INT_CLEAR clears the timeout bit. */
    venc_write(qts, RK3566_VENC_INT_CLEAR, RK3566_VENC_IRQ_TIMEOUT);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_INT_STATUS) &
                    RK3566_VENC_IRQ_TIMEOUT, ==, 0);
    g_assert_false(qtest_get_irq(qts, RK3566_VENC_IRQ));

    /* CLEAR bit 0 latches the clear interrupt, bit 1 drops the codecs. */
    venc_write(qts, RK3566_VENC_CLEAR, 0x3);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_INT_STATUS) &
                    RK3566_VENC_IRQ_CLEAR, ==, RK3566_VENC_IRQ_CLEAR);

    /* Writing zero to INT_STATUS clears all status bits. */
    venc_write(qts, RK3566_VENC_INT_STATUS, 0);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_INT_STATUS), ==, 0);
    g_assert_false(qtest_get_irq(qts, RK3566_VENC_IRQ));

    qtest_quit(qts);
}

static void test_rk3566_venc_l2_window(void)
{
    QTestState *qts = rk3566_qtest_start();

    if (!rk3566_venc_available(qts)) {
        g_test_skip("RK3566 VENC device is not built "
                    "(libavcodec, libavutil, libswscale or x264 missing)");
        qtest_quit(qts);
        return;
    }

    /* L2_WRITE is an index data port: the address word selects the slot. */
    venc_write(qts, RK3566_VENC_L2_ADDRESS, 4);
    venc_write(qts, RK3566_VENC_L2_WRITE, 0xdeadbeef);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_L2_WRITE), ==, 0);
    venc_write(qts, RK3566_VENC_L2_ADDRESS, 4);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_L2_READ), ==, 0xdeadbeef);

    /* The low address bit is ignored when the slot is selected. */
    venc_write(qts, RK3566_VENC_L2_ADDRESS, 5);
    venc_write(qts, RK3566_VENC_L2_WRITE, 0x11223344);
    venc_write(qts, RK3566_VENC_L2_ADDRESS, 4);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_L2_READ), ==, 0x11223344);

    /* Out-of-range slots read as zero and drop writes. */
    venc_write(qts, RK3566_VENC_L2_ADDRESS, 320 * 4);
    venc_write(qts, RK3566_VENC_L2_WRITE, 0x55);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_L2_READ), ==, 0);

    /* CACHE_CLEAR is a write-only trigger and always reads as zero. */
    venc_write(qts, RK3566_VENC_CACHE_CLEAR, 0xffffffff);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_CACHE_CLEAR), ==, 0);

    qtest_quit(qts);
}

static void test_rk3566_venc_start_timer(void)
{
    QTestState *qts = rk3566_qtest_start();

    if (!rk3566_venc_available(qts)) {
        g_test_skip("RK3566 VENC device is not built "
                    "(libavcodec, libavutil, libswscale or x264 missing)");
        qtest_quit(qts);
        return;
    }

    /*
     * Start a frame encode.  The default source stride is zero, so the
     * timer callback reports a read error instead of writing a stream.
     */
    venc_write(qts, RK3566_VENC_START, 1u << 8);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_ENCODE_STATUS), ==, 1);
    qtest_clock_step(qts, 100000);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_INT_STATUS) &
                    RK3566_VENC_IRQ_READ_ERROR, ==,
                    RK3566_VENC_IRQ_READ_ERROR);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_ENCODE_STATUS), ==, 0);
    g_assert_cmphex(venc_read(qts, RK3566_VENC_BUS_STATUS), ==, 0x7e);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566/venc-reset-values",
                   test_rk3566_venc_reset_values);
    qtest_add_func("/rk3566/venc-interrupts",
                   test_rk3566_venc_interrupts);
    qtest_add_func("/rk3566/venc-l2-window",
                   test_rk3566_venc_l2_window);
    qtest_add_func("/rk3566/venc-start-timer",
                   test_rk3566_venc_start_timer);

    return g_test_run();
}
