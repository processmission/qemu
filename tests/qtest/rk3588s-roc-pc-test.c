/*
 * QTest for the Firefly ROC-RK3588S-PC machine model
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define RK3588_RAM_BASE 0x00200000ULL
#define RK3588_SDMMC_BASE 0xfe2c0000ULL
#define RK3588_GMAC1_BASE 0xfe1c0000ULL
#define RK3588_ZVM_SHARED_RAM_BASE 0xe7f00000ULL
#define RK3588_ZVM_HIGH_RAM_BASE 0x100000000ULL

#define DW_MMC_VERID 0x006c
#define DW_MMC_VERID_270A 0x0000270a
#define DWMAC4_MAC_VERSION 0x0110
#define DWMAC4_SNPSVER_0x51 0x00000051

#define RK3588S_ROC_PC_MACHINE "rk3588s-roc-pc"

static QTestState *rk3588s_roc_pc_qtest_start(unsigned int cpus)
{
    return qtest_initf("-machine " RK3588S_ROC_PC_MACHINE
                       " -smp %u -m 512M", cpus);
}

static void test_rk3588s_roc_pc_machine_creation(void)
{
    QTestState *qts = rk3588s_roc_pc_qtest_start(1);

    qtest_writel(qts, RK3588_RAM_BASE, 0x3588);
    g_assert_cmphex(qtest_readl(qts, RK3588_RAM_BASE), ==, 0x3588);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_VERID), ==,
                    DW_MMC_VERID_270A);
    g_assert_cmphex(qtest_readl(qts, RK3588_GMAC1_BASE + DWMAC4_MAC_VERSION),
                    ==, DWMAC4_SNPSVER_0x51);

    /*
     * ROC-RK3588S-PC is the ZVM target board. Its machine default maps the
     * fixed ZVM guest/shared RAM windows that are real RAM on the 8 GiB board.
     */
    qtest_writel(qts, RK3588_ZVM_SHARED_RAM_BASE + 0x408, 0xe7f00408);
    g_assert_cmphex(qtest_readl(qts, RK3588_ZVM_SHARED_RAM_BASE + 0x408), ==,
                    0xe7f00408);
    qtest_writeq(qts, RK3588_ZVM_HIGH_RAM_BASE, 0x100000000ULL);
    g_assert_cmphex(qtest_readq(qts, RK3588_ZVM_HIGH_RAM_BASE), ==,
                    0x100000000ULL);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3588S_ROC_PC_MACHINE)) {
        g_test_skip(RK3588S_ROC_PC_MACHINE " machine not available");
        return 0;
    }

    qtest_add_func("/rk3588s-roc-pc/machine-creation",
                   test_rk3588s_roc_pc_machine_creation);

    return g_test_run();
}
