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
#define RK3588_CRYPTO_BASE 0xfe370000ULL
#define RK3588_PCIE3X4_DBI_BASE 0xa40000000ULL
#define RK3588_GMAC0_BASE 0xfe1b0000ULL
#define RK3588_GMAC1_BASE 0xfe1c0000ULL
#define RK3588_SDMMC_BASE 0xfe2c0000ULL
#define RK3588_SDHCI_BASE 0xfe2e0000ULL
#define RK3588_GICD_BASE 0xfe600000ULL
#define RK3588_SECURE_OTP_BASE 0xfe3a0000ULL
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
#define SECURE_OTP_DOUT 0x20
#define SECURE_OTP_INT_STATUS 0x84
#define SECURE_OTP_READ_DONE 0x2
#define CRYPTO_RST_CTL 0x004
#define CRYPTO_DMA_INT_ST 0x00c
#define CRYPTO_DMA_CTL 0x010
#define CRYPTO_DMA_LLI_ADDR 0x014
#define CRYPTO_FIFO_CTL 0x040
#define CRYPTO_HASH_CTL 0x048
#define CRYPTO_HASH_DOUT_0 0x3a0
#define CRYPTO_HASH_VALID 0x3e4
#define CRYPTO_WRITE_MASK(value) ((uint32_t)(value) << 16)
#define CRYPTO_DMA_SRC_ITEM_DONE 0x4
#define CRYPTO_DMA_LIST_ERR 0x20
#define CRYPTO_HASH_VALID_BIT 0x1
#define CRYPTO_FIFO_BYTESWAP 0x3
#define CRYPTO_HASH_SHA256_PAD_ENABLE 0x25
#define CRYPTO_LLI_USER_HASH_START_LAST 0x7
#define CRYPTO_LLI_DMA_LAST_SRC_DONE 0x401

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

static void test_rock_5b_plus_unfused_secure_otp(void)
{
    QTestState *qts = rock_5b_plus_qtest_start(1);

    g_assert_cmphex(qtest_readl(qts, RK3588_SECURE_OTP_BASE +
                                SECURE_OTP_INT_STATUS), ==,
                    SECURE_OTP_READ_DONE);
    g_assert_cmphex(qtest_readl(qts, RK3588_SECURE_OTP_BASE +
                                SECURE_OTP_DOUT), ==, 0);

    qtest_quit(qts);
}

static void test_rock_5b_plus_crypto_sha256(void)
{
    static const uint8_t input[] = { 'a', 'b', 'c' };
    static const uint32_t expected[] = {
        0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223,
        0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad,
    };
    const uint64_t input_addr = RK3588_RAM_BASE;
    const uint64_t lli_addr = RK3588_RAM_BASE + 0x1000;
    QTestState *qts = rock_5b_plus_qtest_start(1);
    uint32_t status = 0;

    qtest_memwrite(qts, input_addr, input, sizeof(input));
    qtest_writel(qts, lli_addr + 0x00, input_addr);
    qtest_writel(qts, lli_addr + 0x04, sizeof(input));
    qtest_writel(qts, lli_addr + 0x08, 0);
    qtest_writel(qts, lli_addr + 0x0c, 0);
    qtest_writel(qts, lli_addr + 0x10, CRYPTO_LLI_USER_HASH_START_LAST);
    qtest_writel(qts, lli_addr + 0x14, 0);
    qtest_writel(qts, lli_addr + 0x18, CRYPTO_LLI_DMA_LAST_SRC_DONE);
    qtest_writel(qts, lli_addr + 0x1c, 0);

    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_RST_CTL,
                 CRYPTO_WRITE_MASK(1) | 1);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_RST_CTL), ==, 0);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_FIFO_CTL,
                 CRYPTO_WRITE_MASK(CRYPTO_FIFO_BYTESWAP) |
                 CRYPTO_FIFO_BYTESWAP);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_HASH_CTL,
                 CRYPTO_WRITE_MASK(0xffff) |
                 CRYPTO_HASH_SHA256_PAD_ENABLE);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_HASH_CTL, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_HASH_CTL), ==,
                    CRYPTO_HASH_SHA256_PAD_ENABLE);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_LLI_ADDR, lli_addr);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_CTL,
                 CRYPTO_WRITE_MASK(1) | 1);

    for (unsigned int i = 0; i < 1000 && !status; i++) {
        qtest_clock_step(qts, 1);
        status = qtest_readl(qts, RK3588_CRYPTO_BASE +
                             CRYPTO_DMA_INT_ST);
    }

    g_assert_cmphex(status, ==, CRYPTO_DMA_SRC_ITEM_DONE);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_HASH_VALID), ==,
                    CRYPTO_HASH_VALID_BIT);
    for (unsigned int i = 0; i < ARRAY_SIZE(expected); i++) {
        g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                    CRYPTO_HASH_DOUT_0 + i * 4), ==,
                        expected[i]);
    }

    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_INT_ST,
                 CRYPTO_DMA_SRC_ITEM_DONE);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_HASH_VALID,
                 CRYPTO_HASH_VALID_BIT);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_DMA_INT_ST), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_HASH_VALID), ==, 0);

    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_CTL,
                 CRYPTO_WRITE_MASK(2) | 2);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_DMA_INT_ST), ==,
                    CRYPTO_DMA_LIST_ERR);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_INT_ST,
                 CRYPTO_DMA_LIST_ERR);

    qtest_writel(qts, lli_addr + 0x10, 0x6);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_CTL,
                 CRYPTO_WRITE_MASK(1) | 1);
    status = 0;
    for (unsigned int i = 0; i < 1000 && !status; i++) {
        qtest_clock_step(qts, 1);
        status = qtest_readl(qts, RK3588_CRYPTO_BASE +
                             CRYPTO_DMA_INT_ST);
    }
    g_assert_cmphex(status, ==, CRYPTO_DMA_LIST_ERR);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_INT_ST,
                 CRYPTO_DMA_LIST_ERR);

    qtest_writel(qts, lli_addr + 0x10,
                 CRYPTO_LLI_USER_HASH_START_LAST | 0x8);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_CTL,
                 CRYPTO_WRITE_MASK(1) | 1);
    status = 0;
    for (unsigned int i = 0; i < 1000 && !status; i++) {
        qtest_clock_step(qts, 1);
        status = qtest_readl(qts, RK3588_CRYPTO_BASE +
                             CRYPTO_DMA_INT_ST);
    }
    g_assert_cmphex(status, ==, CRYPTO_DMA_LIST_ERR);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_HASH_VALID), ==, 0);

    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_RST_CTL,
                 CRYPTO_WRITE_MASK(1) | 1);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_DMA_INT_ST), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_HASH_DOUT_0), ==, 0);

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
    qtest_add_func("/rock-5b-plus/unfused-secure-otp",
                   test_rock_5b_plus_unfused_secure_otp);
    qtest_add_func("/rock-5b-plus/crypto-sha256",
                   test_rock_5b_plus_crypto_sha256);

    return g_test_run();
}
