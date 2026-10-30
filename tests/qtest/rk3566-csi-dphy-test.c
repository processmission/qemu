/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the Rockchip RK3566 CSI D-PHY.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "rk3566-test-common.h"

#define DPHY_LANE_ENABLE  0x00
#define DPHY_STORAGE0     0x20
#define DPHY_STORAGE1     0x100
#define DPHY_LANE_RESET   0x1
#define DPHY_LANE_ENABLED 0x4c

static void test_dphy_registers(void)
{
    QTestState *qts = rk3566_qtest_start();
    g_autofree char *type;

    if (!rk3566_qtest_media_present(qts)) {
        g_test_skip("RK3566 media devices are not built "
                    "(libavcodec, libavutil, libswscale or x264 missing)");
        qtest_quit(qts);
        return;
    }

    type = rk3566_qom_child_object_type(qts, "/machine", "csi-dphy");
    g_assert_cmpstr(type, ==, "rk3566-csi-dphy");

    /* LANE_ENABLE resets to one and accepts arbitrary writes. */
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_DPHY_BASE,
                                         DPHY_LANE_ENABLE), ==,
                    DPHY_LANE_RESET);
    rk3566_mmio_write32_at(qts, RK3566_DPHY_BASE, DPHY_LANE_ENABLE,
                           DPHY_LANE_ENABLED);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_DPHY_BASE,
                                         DPHY_LANE_ENABLE), ==,
                    DPHY_LANE_ENABLED);
    rk3566_mmio_write32_at(qts, RK3566_DPHY_BASE, DPHY_LANE_ENABLE, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_DPHY_BASE,
                                         DPHY_LANE_ENABLE), ==, 0);

    /* Unlisted addresses fall back to plain storage. */
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_DPHY_BASE,
                                         DPHY_STORAGE1), ==, 0);
    rk3566_mmio_write32_at(qts, RK3566_DPHY_BASE, DPHY_STORAGE0,
                           0xdeadbeef);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_DPHY_BASE,
                                         DPHY_STORAGE0), ==, 0xdeadbeef);
    rk3566_mmio_write32_at(qts, RK3566_DPHY_BASE, DPHY_STORAGE1,
                           0x11223344);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_DPHY_BASE,
                                         DPHY_STORAGE1), ==, 0x11223344);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566/csi-dphy/registers", test_dphy_registers);

    return g_test_run();
}
