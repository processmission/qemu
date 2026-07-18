/*
 * QTest for the Radxa ROCK 5B+ machine
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define ROCK_5B_PLUS_MACHINE "rock-5b-plus"

#define RK3588_RAM_BASE 0x00200000ULL
#define RK3588_PMU1_GRF_BASE 0xfd58a000ULL
#define RK3588_PCIE3X4_DBI_BASE 0xa40000000ULL
#define RK3588_GMAC0_BASE 0xfe1b0000ULL
#define RK3588_GMAC1_BASE 0xfe1c0000ULL
#define RK3588_SDMMC_BASE 0xfe2c0000ULL
#define RK3588_SDHCI_BASE 0xfe2e0000ULL
#define RK3588_GICD_BASE 0xfe600000ULL
#define RK3588_UART2_BASE 0xfeb50000ULL

#define PMU1_GRF_OS_REG2 0x0208
#define PMU1_GRF_OS_REG3 0x020c
#define RK3588_DDRTYPE_LOW_SHIFT 13
#define RK3588_DDRTYPE_HIGH_SHIFT 12
#define RK3588_DDRTYPE_LOW_MASK 0x7
#define RK3588_DDRTYPE_HIGH_MASK 0x1
#define RK3588_LPDDR5 9

#define DWC_PCIE_VENDOR_DEVICE 0x0000
#define DWMAC4_MAC_VERSION 0x0110
#define DWMAC4_SNPSVER_0x51 0x00000051
#define DW_MMC_VERID 0x006c
#define DW_MMC_VERID_270A 0x0000270a
#define SDHCI_CAPABILITIES 0x0040
#define GICD_TYPER 0x0004
#define UART_LSR (5 << 2)
#define UART_LSR_THRE 0x20
#define UART_LSR_TEMT 0x40

static QTestState *rock_5b_plus_qtest_start(unsigned int cpus)
{
    return qtest_initf("-machine " ROCK_5B_PLUS_MACHINE
                       " -smp %u -m 512M", cpus);
}

static void test_rock_5b_plus_machine_creation(void)
{
    QTestState *qts = rock_5b_plus_qtest_start(1);
    uint64_t sdhci_caps;
    uint32_t pcie_id;
    uint32_t sys_reg2;
    uint32_t sys_reg3;

    qtest_writel(qts, RK3588_RAM_BASE, 0x5b5b3588);
    g_assert_cmphex(qtest_readl(qts, RK3588_RAM_BASE), ==, 0x5b5b3588);

    g_assert_cmphex(qtest_readb(qts, RK3588_UART2_BASE + UART_LSR) &
                    (UART_LSR_THRE | UART_LSR_TEMT), ==,
                    UART_LSR_THRE | UART_LSR_TEMT);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_VERID), ==,
                    DW_MMC_VERID_270A);

    sdhci_caps = qtest_readl(qts, RK3588_SDHCI_BASE + SDHCI_CAPABILITIES);
    sdhci_caps |= (uint64_t)qtest_readl(qts, RK3588_SDHCI_BASE +
                                        SDHCI_CAPABILITIES + 4) << 32;
    g_assert_cmphex(sdhci_caps, !=, 0);
    g_assert_cmphex(sdhci_caps, !=, UINT64_MAX);

    pcie_id = qtest_readl(qts, RK3588_PCIE3X4_DBI_BASE +
                          DWC_PCIE_VENDOR_DEVICE);
    g_assert_cmphex(pcie_id, !=, 0);
    g_assert_cmphex(pcie_id, !=, UINT32_MAX);

    g_assert_cmphex(qtest_readl(qts, RK3588_GICD_BASE + GICD_TYPER), !=, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_GICD_BASE + GICD_TYPER), !=,
                    UINT32_MAX);

    sys_reg2 = qtest_readl(qts, RK3588_PMU1_GRF_BASE + PMU1_GRF_OS_REG2);
    sys_reg3 = qtest_readl(qts, RK3588_PMU1_GRF_BASE + PMU1_GRF_OS_REG3);
    g_assert_cmphex((sys_reg2 >> RK3588_DDRTYPE_LOW_SHIFT) &
                    RK3588_DDRTYPE_LOW_MASK, ==,
                    RK3588_LPDDR5 & RK3588_DDRTYPE_LOW_MASK);
    g_assert_cmphex((sys_reg3 >> RK3588_DDRTYPE_HIGH_SHIFT) &
                    RK3588_DDRTYPE_HIGH_MASK, ==,
                    (RK3588_LPDDR5 >> 3) & RK3588_DDRTYPE_HIGH_MASK);

    /* ROCK 5B+ uses a PCIe RTL8125 NIC, not either RK3588 DWMAC. */
    g_assert_cmphex(qtest_readl(qts, RK3588_GMAC0_BASE +
                                DWMAC4_MAC_VERSION), !=,
                    DWMAC4_SNPSVER_0x51);
    g_assert_cmphex(qtest_readl(qts, RK3588_GMAC1_BASE +
                                DWMAC4_MAC_VERSION), !=,
                    DWMAC4_SNPSVER_0x51);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_VERID), ==,
                    DW_MMC_VERID_270A);

    qtest_quit(qts);
}

static void test_rock_5b_plus_smp_creation(void)
{
    QTestState *qts = rock_5b_plus_qtest_start(8);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(ROCK_5B_PLUS_MACHINE)) {
        g_test_skip(ROCK_5B_PLUS_MACHINE " machine not available");
        return 0;
    }

    qtest_add_func("/rock-5b-plus/machine-creation",
                   test_rock_5b_plus_machine_creation);
    qtest_add_func("/rock-5b-plus/smp-creation",
                   test_rock_5b_plus_smp_creation);

    return g_test_run();
}
