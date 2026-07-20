/*
 * QTest for the Rockchip RK3588 EVB machine model
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define RK3588_QTEST_RAM_SIZE 0x20000000ULL
#define RK3588_RAM_BASE 0x00200000ULL
#define RK3588_ATAGS_BASE 0x001fe000ULL
#define RK3588_ATAG_DDR_MEM 0x54410052
#define RK3588_ATAG_DDR_MEM_WORDS 48
#define RK3588_IRAM_LOG_BASE 0xff100000ULL
#define RK3588_PMUSRAM_SKIP_ADDR 0xff101764ULL
#define RK3588_AARCH64_RET 0xd65f03c0
#define RK3588_FIRMWARE_DEFAULT_REG 0xf7010000ULL
#define RK3588_DDR_CHANNEL_STATUS 0xf7010014ULL
#define RK3588_DDR_CHANNEL_STATUS_READY 0x1
#define RK3588_DDR_CHANNEL_CMD 0xf7010080ULL
#define RK3588_DDR_CHANNEL_CMD_START 0x80000000U
#define RK3588_DDR_CHANNEL_BUSY 0xf7010090ULL
#define RK3588_DDR_CHANNEL_BUSY_FLAG 0x1
#define RK3588_DDR_CHANNEL_GATE_STATUS 0xf7010514ULL
#define RK3588_DDR_CHANNEL_GATE_BUSY 0x1
#define RK3588_DDR_CHANNEL_GATE_CMD 0xf7010510ULL
#define RK3588_DDR_CHANNEL_GATE_ENABLE 0x20
#define RK3588_DDR_CHANNEL_GATE_CTRL 0xf7010c80ULL
#define RK3588_DDR_CHANNEL_PHY_STATUS 0xf7010b90ULL
#define RK3588_DDR_CHANNEL_PHY_BUSY 0x10000
#define RK3588_DDR_PHY_GATE_CTRL 0xfe0c00b0ULL
#define RK3588_DDR_PHY_GATE_ENABLE 0x20
#define RK3588_DDRPHY_CTRL 0xfd8d8154ULL
#define RK3588_DDRPHY_STATUS 0xfd8d8184ULL
#define RK3588_DDRPHY_STATUS_ACTIVE 0x3
#define RK3588_PCIE3X4_APB_BASE 0xfe150000ULL
#define RK3588_PCIE3X4_DBI_BASE 0xa40000000ULL
#define RK3588_PCIE3X4_CFG_BASE 0xf0000000ULL
#define RK3588_CRU_BASE 0xfd7c0000ULL
#define RK3588_CRU_SPL_PLL_STATUS 0xfd810018ULL
#define RK3588_CRU_SPL_PLL_LOCKED 0x8000
#define RK3588_PMU0_GRF_BASE 0xfd588000ULL
#define RK3588_PMU0_GRF_WARM_BOOT_MAGIC 0x0084
#define RK3588_PMU0_GRF_WARM_BOOT_MAGIC_VALUE 0x13579bdf
#define RK3588_PMU1_GRF_BASE 0xfd58a000ULL
#define RK3588_SYS_GRF_CORE_STATUS 0xfd58c38cULL
#define RK3588_SYS_GRF_CORE_STATUS_ALL 0xf0
#define RK3588_ATF_DDR_RUNTIME_ADDR 0x0008d000ULL
#define RK3588_ATF_DDR_GLOBAL_PTR 0x0008d0a8ULL
#define RK3588_ATF_TIMER_PTR 0x0008d0b0ULL
#define RK3588_ATF_TIMER_TABLE 0x0008d0b8ULL
#define RK3588_ATF_TIMER_COUNTER 0x00062054ULL
#define RK3588_ATF_DDR_DESCRIPTOR 0x0008fd20ULL
#define RK3588_ATF_DDR_CHANNEL_TABLE 0x0008fe00ULL
#define RK3588_ATF_DDR_CHANNEL_TABLE_OFFSET \
    (RK3588_ATF_DDR_CHANNEL_TABLE - RK3588_ATF_DDR_DESCRIPTOR)
#define RK3588_ATF_DDR_GLOBAL_BASE 0xfd000000ULL
#define RK3588_ATF_DDR_CHANNEL0_BASE 0xfd100000ULL
#define RK3588_ATF_DDR_CTRL_BUSY_MASK ((1U << 31) | (1U << 3))
#define RK3588_ATF_DDR_STATUS_HANDSHAKE 0x14
#define RK3588_ATF_DDR_STATUS_REQUEST 0x1
#define RK3588_ATF_DDR_STATUS_ACK (1U << 31)
#define RK3588_FIRMWARE_SCRATCH 0x00100000ULL
#define RK3588_GTIMER_HZ 24000000
#define RK3588_STIMER_BASE 0xfd8c8000ULL
#define RK3588_GMAC0_BASE 0xfe1b0000ULL
#define RK3588_GMAC1_BASE 0xfe1c0000ULL
#define RK3588_SCMI_SHMEM_BASE 0x0010f000ULL
#define RK3588_SDMMC_BASE 0xfe2c0000ULL
#define RK3588_SDHCI_BASE 0xfe2e0000ULL
#define RK3588_USB2_HOST0_EHCI_BASE 0xfc800000ULL
#define RK3588_USB2_HOST0_OHCI_BASE 0xfc840000ULL
#define RK3588_USB2_HOST1_EHCI_BASE 0xfc880000ULL
#define RK3588_USB2_HOST1_OHCI_BASE 0xfc8c0000ULL
#define RK3588_GICD_BASE 0xfe600000ULL
#define RK3588_GIC_ITS0_BASE 0xfe640000ULL
#define RK3588_GIC_ITS1_BASE 0xfe660000ULL
#define RK3588_GICR_BASE 0xfe680000ULL
#define RK3588_GPIO0_BASE 0xfd8a0000ULL
#define RK3588_UART2_BASE 0xfeb50000ULL
#define RK3588_ZVM_LOW_RAM_BASE 0x68000000ULL
#define RK3588_ZVM_SHARED_RAM_BASE 0xe7f00000ULL
#define RK3588_ZVM_NOTIFY_INFO_BASE 0xefd00000ULL
#define RK3588_ZVM_NOTIFY_BASE 0xefe00000ULL
#define RK3588_ZVM_HIGH_RAM_BASE 0x100000000ULL
#define RK3588_ZVM_HIGH_RAM_LAST 0x1ffffffffULL

#define GICD_TYPER 0x0004
#define GICR_TYPER 0x0008
#define GICR_TYPER_PLPIS 0x00000001
#define GITS_CTLR 0x0000
#define GITS_CTLR_QUIESCENT 0x80000000
#define GITS_TYPER 0x0008
#define GITS_TYPER_PHYSICAL 0x0000000000000001ULL
#define GITS_TYPER_IDBITS_MASK 0x0000000000001f00ULL
#define GITS_TYPER_IDBITS_15 0x0000000000000f00ULL
#define GITS_TYPER_DEVBITS_MASK 0x000000000003e000ULL
#define GITS_TYPER_DEVBITS_15 0x000000000001e000ULL
#define GITS_TYPER_CIDBITS_MASK 0x0000000f00000000ULL
#define GITS_TYPER_CIDBITS_15 0x0000000f00000000ULL
#define GITS_TYPER_CIL 0x0000001000000000ULL
#define GITS_PIDR0 0xffe0
#define GITS_PIDR2 0xffe8
#define GITS_PIDR0_ITS 0x94
#define GITS_PIDR2_GICV3 0x3b
#define SCMI_CHANNEL_STATUS 0x0004
#define SCMI_CHANNEL_FREE 0x00000001
#define SCMI_MSG_PAYLOAD 0x001c
#define DWC_PCIE_VENDOR_DEVICE 0x0000
#define DWC_PCIE_LTSSM_STATUS 0x300
#define DWC_PCIE_LTSSM_LINK_UP 0x00030011
#define DWMAC4_MAC_VERSION 0x0110
#define DWMAC4_SNPSVER_0x51 0x00000051
#define USB2_EHCI_CAPBASE 0x0000
#define USB2_EHCI_CAPLENGTH 0x20
#define USB2_EHCI_VERSION_1_0 0x0100
#define USB2_EHCI_HCSPARAMS 0x0004
#define USB2_EHCI_NPORTS 0x1
#define USB2_EHCI_USBCMD 0x0020
#define USB2_EHCI_USBSTS 0x0024
#define USB2_EHCI_PORTSC0 0x0064
#define USB2_EHCI_CMD_RESET 0x00000002
#define USB2_EHCI_CMD_RUN 0x00000001
#define USB2_EHCI_STS_HALT 0x00001000
#define USB2_EHCI_PORT_POWER 0x00001000
#define USB2_OHCI_REVISION 0x0000
#define USB2_OHCI_REVISION_1_0 0x10
#define USB2_OHCI_CMDSTATUS 0x0008
#define USB2_OHCI_ROOTHUB_A 0x0048
#define USB2_OHCI_PORTSTATUS0 0x0054
#define USB2_OHCI_HCR 0x00000001
#define USB2_OHCI_RH_A_NDP_MASK 0x000000ff
#define USB2_OHCI_RH_A_NDP1 0x00000001
#define USB2_OHCI_RH_A_NPS 0x00000200
#define USB2_OHCI_RH_A_NOCP 0x00001000
#define USB2_OHCI_RH_PS_PPS 0x00000100
#define DW_MMC_CTRL 0x0000
#define DW_MMC_CTRL_RESET 0x00000001
#define DW_MMC_FIFO_RESET 0x00000002
#define DW_MMC_DMA_RESET 0x00000004
#define DW_MMC_INT_ENABLE 0x00000010
#define DW_MMC_INTMASK 0x0024
#define DW_MMC_CMD 0x002c
#define DW_MMC_CMD_START 0x80000000
#define DW_MMC_MINTSTS 0x0040
#define DW_MMC_RINTSTS 0x0044
#define DW_MMC_INT_CMD_DONE 0x00000004
#define DW_MMC_STATUS 0x0048
#define DW_MMC_STATUS_FIFO_EMPTY 0x00000004
#define DW_MMC_FIFOTH 0x004c
#define DW_MMC_CDETECT 0x0050
#define DW_MMC_VERID 0x006c
#define DW_MMC_VERID_270A 0x0000270a
#define DW_MMC_HCON 0x0070
#define DW_MMC_RST_N 0x0078
#define DW_MMC_BMOD 0x0080
#define DW_MMC_BMOD_SWRESET 0x00000001
#define DW_MMC_BMOD_ENABLE 0x00000080
#define DW_MMC_DBADDR 0x0088
#define DW_MMC_IDSTS 0x008c
#define DW_MMC_IDINTEN 0x0090
#define DW_MMC_CDTHRCTL 0x0100
#define DW_MMC_ENABLE_SHIFT 0x0110
#define DW_MMC_RK_TIMING_CON0 0x0130
#define DW_MMC_LEGACY_DATA 0x0200
#define SDHCI_CAPABILITIES 0x0040
#define SDHCI_VENDOR_AREA1 0x00e8
#define DWCMSHC_VENDOR_AREA1 0x0500
#define DWCMSHC_VENDOR_AREA2 0x0800
#define DWCMSHC_EMMC_DLL_STATUS0 0x0840
#define DWCMSHC_EMMC_DLL_LOCKED 0x00000100
#define PMU1_GRF_OS_REG2 0x0208
#define ROCKCHIP_HIWORD_PIN0_SET 0x00010001
#define STIMER_CTRL 0x0004
#define STIMER_LOAD0 0x0014
#define STIMER_ENABLE 0x00000001
#define GPIO_SWPORT_DR_L 0x0000
#define GPIO_SWPORT_DDR_L 0x0008
#define GPIO_INT_EN_L 0x0010
#define GPIO_INT_MASK_L 0x0018
#define GPIO_INT_TYPE_L 0x0020
#define GPIO_INT_POLARITY_L 0x0028
#define GPIO_INT_STATUS_L 0x0050
#define GPIO_INT_RAWSTATUS_L 0x0058
#define GPIO_PORT_EOI_L 0x0060
#define GPIO_EXT_PORT_L 0x0070
#define GPIO_VERSION_ID 0x0078
#define GPIO_V2_ID 0x01000c2b
#define UART_LSR (5 << 2)
#define UART_LSR_THRE 0x20
#define UART_LSR_TEMT 0x40
/* Synopsys dw-apb-uart vendor register (sits above the 16550 core window). */
#define DW_UART_USR 0x7c
#define DW_UART_USR_BUSY 0x1
#define DW_UART_USR_TFNF 0x2
#define DW_UART_USR_TFE 0x4
#define RK3588_GPIO0_QOM "/machine/gpio0"
#define GPIO_PIN0 0x00000001U
#define GPIO_PIN0_WE 0x00010000U
#define GPIO_PIN0_SET (GPIO_PIN0_WE | GPIO_PIN0)
#define GPIO_PIN0_CLEAR GPIO_PIN0_WE
#define RK3588_EVB_MACHINE "rk3588-evb"

static QTestState *rk3588_qtest_start(unsigned int cpus)
{
    return qtest_initf("-machine " RK3588_EVB_MACHINE " -smp %u -m 512M",
                       cpus);
}

static QTestState *rk3588_qtest_start_zvm_ram(void)
{
    return qtest_init("-machine " RK3588_EVB_MACHINE
                      ",zvm-ram=on -smp 1 -m 512M");
}

static void test_rk3588_machine_creation(void)
{
    QTestState *qts = rk3588_qtest_start(1);
    uint64_t pattern = 0x1122334455667788ULL;
    uint32_t gicd_typer;
    uint8_t uart_lsr;

    qtest_writeq(qts, RK3588_RAM_BASE, pattern);
    g_assert_cmphex(qtest_readq(qts, RK3588_RAM_BASE), ==, pattern);
    qtest_writeb(qts, RK3588_IRAM_LOG_BASE, 0x5a);
    g_assert_cmphex(qtest_readb(qts, RK3588_IRAM_LOG_BASE), ==, 0x5a);
    g_assert_cmphex(qtest_readl(qts, RK3588_PMUSRAM_SKIP_ADDR), ==,
                    RK3588_AARCH64_RET);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATAGS_BASE), ==,
                    RK3588_ATAG_DDR_MEM_WORDS);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATAGS_BASE + 4), ==,
                    RK3588_ATAG_DDR_MEM);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATAGS_BASE + 8), ==, 1);
    g_assert_cmphex(qtest_readq(qts, RK3588_ATAGS_BASE + 16), ==, 0);
    g_assert_cmphex(qtest_readq(qts, RK3588_ATAGS_BASE + 24), ==,
                    RK3588_RAM_BASE + RK3588_QTEST_RAM_SIZE);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATAGS_BASE +
                                RK3588_ATAG_DDR_MEM_WORDS *
                                sizeof(uint32_t)), ==, 0);

    gicd_typer = qtest_readl(qts, RK3588_GICD_BASE + GICD_TYPER);
    g_assert_cmpuint(gicd_typer & 0x1f, >=, 0x10);

    uart_lsr = qtest_readb(qts, RK3588_UART2_BASE + UART_LSR);
    g_assert_cmphex(uart_lsr & (UART_LSR_THRE | UART_LSR_TEMT), ==,
                    UART_LSR_THRE | UART_LSR_TEMT);

    /*
     * dw-apb-uart USR (vendor range above the 0x20-byte 16550 core window)
     * must read BUSY clear and TX FIFO ready so Linux and firmware transmit
     * wait loops both terminate.
     */
    g_assert_cmphex(qtest_readl(qts, RK3588_UART2_BASE + DW_UART_USR) &
                    (DW_UART_USR_BUSY | DW_UART_USR_TFNF | DW_UART_USR_TFE),
                    ==, DW_UART_USR_TFNF | DW_UART_USR_TFE);

    qtest_system_reset(qts);
    uart_lsr = qtest_readb(qts, RK3588_UART2_BASE + UART_LSR);
    g_assert_cmphex(uart_lsr & (UART_LSR_THRE | UART_LSR_TEMT), ==,
                    UART_LSR_THRE | UART_LSR_TEMT);

    qtest_quit(qts);
}

static void test_rk3588_peripheral_mmio(void)
{
    QTestState *qts = rk3588_qtest_start(1);
    uint32_t pcie_id;
    uint32_t gmac_version;
    uint64_t sdhci_caps;

    /*
     * Root-port PCI vendor/device id lives in the DBI window (the DWC
     * core Type-0 cfg header), not the RK APB vendor window. designware.c
     * sets vendor=Synopsys(0x104C-ish) device=0xABCD for the root. The
     * 32-bit read at DBI+0 returns DEVICEID(15:0) | VENDORID(31:16) on
     * little-endian, so the assembled u32 is non-zero and non-0xffffffff.
     */
    pcie_id = qtest_readl(qts, RK3588_PCIE3X4_DBI_BASE +
                          DWC_PCIE_VENDOR_DEVICE);
    g_assert_cmphex(pcie_id, !=, 0);
    g_assert_cmphex(pcie_id, !=, UINT32_MAX);

    gmac_version = qtest_readl(qts, RK3588_GMAC0_BASE + DWMAC4_MAC_VERSION);
    g_assert_cmphex(gmac_version, ==, DWMAC4_SNPSVER_0x51);
    gmac_version = qtest_readl(qts, RK3588_GMAC1_BASE + DWMAC4_MAC_VERSION);
    g_assert_cmphex(gmac_version, ==, DWMAC4_SNPSVER_0x51);

    sdhci_caps = qtest_readl(qts, RK3588_SDHCI_BASE + SDHCI_CAPABILITIES);
    sdhci_caps |= (uint64_t)qtest_readl(qts, RK3588_SDHCI_BASE +
                                        SDHCI_CAPABILITIES + 4) << 32;
    g_assert_cmphex(sdhci_caps, !=, 0);
    g_assert_cmphex(sdhci_caps, !=, UINT64_MAX);

    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_VERSION_ID), ==,
                    GPIO_V2_ID);

    qtest_quit(qts);
}

static void test_rk3588_sdmmc_scmi(void)
{
    QTestState *qts = rk3588_qtest_start(1);
    uint32_t ctrl;

    /*
     * The SCMI shmem window is a tiny RAM-backed MMIO region. The SMC
     * transport itself is covered by guest-boot evidence, but qtest can still
     * pin the reset-visible channel state and the shmem read/write contract.
     */
    g_assert_cmphex(qtest_readl(qts, RK3588_SCMI_SHMEM_BASE +
                                SCMI_CHANNEL_STATUS), ==, SCMI_CHANNEL_FREE);
    qtest_writel(qts, RK3588_SCMI_SHMEM_BASE + SCMI_MSG_PAYLOAD,
                 0x5ca1c10c);
    g_assert_cmphex(qtest_readl(qts, RK3588_SCMI_SHMEM_BASE +
                                SCMI_MSG_PAYLOAD), ==, 0x5ca1c10c);

    /*
     * dw_mmc reset-visible contract: recent IP version exposes the 0x100 FIFO
     * data port, U-Boot can still use the legacy 0x200 data port, HCON selects
     * 32-bit IDMAC, RST_N is deasserted, and no qtest SD card is attached so
     * CDETECT bit0 reports absent.
     */
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_VERID), ==,
                    DW_MMC_VERID_270A);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_HCON), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_RST_N), ==, 1);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_CDETECT) & 1,
                    ==, 1);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_STATUS) &
                    DW_MMC_STATUS_FIFO_EMPTY, ==, DW_MMC_STATUS_FIFO_EMPTY);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_FIFOTH), ==,
                    0x00800000);

    qtest_writel(qts, RK3588_SDMMC_BASE + DW_MMC_CTRL,
                 DW_MMC_CTRL_RESET | DW_MMC_FIFO_RESET | DW_MMC_DMA_RESET);
    ctrl = qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_CTRL);
    g_assert_cmphex(ctrl & (DW_MMC_CTRL_RESET | DW_MMC_FIFO_RESET |
                            DW_MMC_DMA_RESET), ==, 0);

    qtest_writel(qts, RK3588_SDMMC_BASE + DW_MMC_BMOD,
                 DW_MMC_BMOD_ENABLE | DW_MMC_BMOD_SWRESET);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_BMOD), ==,
                    DW_MMC_BMOD_ENABLE);
    qtest_writel(qts, RK3588_SDMMC_BASE + DW_MMC_DBADDR, 0x00300000);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_DBADDR), ==,
                    0x00300000);
    qtest_writel(qts, RK3588_SDMMC_BASE + DW_MMC_IDINTEN, 0x00000303);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_IDINTEN), ==,
                    0x00000303);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_IDSTS), ==, 0);

    /*
     * A no-response CMD0 sets raw and masked CMD_DONE, then RINTSTS W1C clears
     * both raw and masked views. This pins the command side effect and W1C
     * status behavior without needing an attached card.
     */
    qtest_writel(qts, RK3588_SDMMC_BASE + DW_MMC_CTRL, DW_MMC_INT_ENABLE);
    qtest_writel(qts, RK3588_SDMMC_BASE + DW_MMC_INTMASK,
                 DW_MMC_INT_CMD_DONE);
    qtest_writel(qts, RK3588_SDMMC_BASE + DW_MMC_CMD, DW_MMC_CMD_START);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_CMD) &
                    DW_MMC_CMD_START, ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_RINTSTS) &
                    DW_MMC_INT_CMD_DONE, ==, DW_MMC_INT_CMD_DONE);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_MINTSTS) &
                    DW_MMC_INT_CMD_DONE, ==, DW_MMC_INT_CMD_DONE);
    qtest_writel(qts, RK3588_SDMMC_BASE + DW_MMC_RINTSTS,
                 DW_MMC_INT_CMD_DONE);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_RINTSTS) &
                    DW_MMC_INT_CMD_DONE, ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_MINTSTS) &
                    DW_MMC_INT_CMD_DONE, ==, 0);

    qtest_writel(qts, RK3588_SDMMC_BASE + DW_MMC_CDTHRCTL, 0x00100001);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_CDTHRCTL), ==,
                    0x00100001);
    qtest_writel(qts, RK3588_SDMMC_BASE + DW_MMC_LEGACY_DATA, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE +
                                DW_MMC_LEGACY_DATA), ==, 0);
    qtest_writel(qts, RK3588_SDMMC_BASE + DW_MMC_ENABLE_SHIFT, 1);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_ENABLE_SHIFT),
                    ==, 1);

    /* RK3588 does not use the internal phase registers; they are RAZ/WI. */
    qtest_writel(qts, RK3588_SDMMC_BASE + DW_MMC_RK_TIMING_CON0, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE +
                                DW_MMC_RK_TIMING_CON0), ==, 0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_SCMI_SHMEM_BASE +
                                SCMI_CHANNEL_STATUS), ==, SCMI_CHANNEL_FREE);
    g_assert_cmphex(qtest_readl(qts, RK3588_SCMI_SHMEM_BASE +
                                SCMI_MSG_PAYLOAD), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_VERID), ==,
                    DW_MMC_VERID_270A);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_RINTSTS), ==,
                    0);

    qtest_quit(qts);
}

static void test_rk3588_firmware_registers(void)
{
    QTestState *qts = rk3588_qtest_start(1);
    uint16_t vendor_area1;
    uint16_t vendor_area2;
    uint32_t sys_reg2;
    uint32_t dll_status;

    sys_reg2 = qtest_readl(qts, RK3588_PMU1_GRF_BASE + PMU1_GRF_OS_REG2);
    g_assert_cmphex(sys_reg2, !=, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_PMU0_GRF_BASE +
                                RK3588_PMU0_GRF_WARM_BOOT_MAGIC), ==,
                    RK3588_PMU0_GRF_WARM_BOOT_MAGIC_VALUE);
    g_assert_cmphex(qtest_readl(qts, RK3588_SYS_GRF_CORE_STATUS), ==,
                    RK3588_SYS_GRF_CORE_STATUS_ALL);
    g_assert_cmphex(qtest_readq(qts, RK3588_ATF_DDR_GLOBAL_PTR), ==,
                    RK3588_ATF_DDR_DESCRIPTOR);
    g_assert_cmphex(qtest_readq(qts, RK3588_ATF_TIMER_PTR), ==,
                    RK3588_ATF_TIMER_TABLE);
    g_assert_cmphex(qtest_readq(qts, RK3588_ATF_TIMER_TABLE), ==,
                    RK3588_ATF_TIMER_COUNTER);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATF_TIMER_TABLE + 0x8), ==,
                    1000000);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATF_TIMER_TABLE + 0xc), ==,
                    RK3588_GTIMER_HZ);
    g_assert_cmphex(qtest_readq(qts, RK3588_ATF_DDR_DESCRIPTOR), ==,
                    RK3588_ATF_DDR_GLOBAL_BASE);
    g_assert_cmphex(qtest_readq(qts, RK3588_ATF_DDR_DESCRIPTOR + 0x20), ==,
                    RK3588_ATF_DDR_CHANNEL_TABLE);
    g_assert_cmphex(qtest_readq(qts, RK3588_ATF_DDR_DESCRIPTOR +
                                RK3588_ATF_DDR_CHANNEL_TABLE_OFFSET), ==,
                    RK3588_ATF_DDR_CHANNEL0_BASE);
    qtest_writeq(qts, RK3588_ATF_DDR_DESCRIPTOR, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, RK3588_ATF_DDR_DESCRIPTOR), ==,
                    RK3588_ATF_DDR_GLOBAL_BASE);
    qtest_writel(qts, RK3588_FIRMWARE_SCRATCH, 0xa5a5f00d);
    g_assert_cmphex(qtest_readl(qts, RK3588_FIRMWARE_SCRATCH), ==,
                    0xa5a5f00d);
    qtest_writel(qts, RK3588_ATF_DDR_GLOBAL_BASE, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATF_DDR_GLOBAL_BASE) &
                    RK3588_ATF_DDR_CTRL_BUSY_MASK, ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATF_DDR_GLOBAL_BASE + 4), ==, 0);
    qtest_writel(qts, RK3588_ATF_DDR_CHANNEL0_BASE +
                 RK3588_ATF_DDR_STATUS_HANDSHAKE,
                 RK3588_ATF_DDR_STATUS_REQUEST);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATF_DDR_CHANNEL0_BASE +
                                RK3588_ATF_DDR_STATUS_HANDSHAKE) &
                    RK3588_ATF_DDR_STATUS_ACK, ==,
                    RK3588_ATF_DDR_STATUS_ACK);
    qtest_writel(qts, RK3588_ATF_DDR_CHANNEL0_BASE +
                 RK3588_ATF_DDR_STATUS_HANDSHAKE, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATF_DDR_CHANNEL0_BASE +
                                RK3588_ATF_DDR_STATUS_HANDSHAKE) &
                    RK3588_ATF_DDR_STATUS_ACK, ==, 0);

    g_assert_cmphex(qtest_readl(qts, RK3588_FIRMWARE_DEFAULT_REG), ==,
                    UINT32_MAX);
    qtest_writeb(qts, RK3588_FIRMWARE_DEFAULT_REG, 0xa5);
    g_assert_cmphex(qtest_readb(qts, RK3588_FIRMWARE_DEFAULT_REG), ==, 0xa5);
    g_assert_cmphex(qtest_readl(qts, RK3588_DDR_CHANNEL_STATUS) & 0x7, ==,
                    RK3588_DDR_CHANNEL_STATUS_READY);
    qtest_writel(qts, RK3588_DDR_CHANNEL_CMD, RK3588_DDR_CHANNEL_CMD_START);
    g_assert_cmphex(qtest_readl(qts, RK3588_DDR_CHANNEL_CMD) &
                    RK3588_DDR_CHANNEL_CMD_START, ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_DDR_CHANNEL_BUSY) &
                    RK3588_DDR_CHANNEL_BUSY_FLAG, ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_DDR_CHANNEL_GATE_STATUS) &
                    RK3588_DDR_CHANNEL_GATE_BUSY, ==, 0);
    qtest_writel(qts, RK3588_DDR_CHANNEL_GATE_CTRL, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_DDR_CHANNEL_GATE_STATUS) &
                    RK3588_DDR_CHANNEL_GATE_BUSY, ==, 0);
    qtest_writel(qts, RK3588_DDR_CHANNEL_GATE_CMD,
                 ~RK3588_DDR_CHANNEL_GATE_ENABLE);
    g_assert_cmphex(qtest_readl(qts, RK3588_DDR_CHANNEL_GATE_STATUS) &
                    RK3588_DDR_CHANNEL_GATE_BUSY, ==,
                    RK3588_DDR_CHANNEL_GATE_BUSY);
    qtest_writel(qts, RK3588_DDR_CHANNEL_GATE_CMD, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RK3588_DDR_CHANNEL_GATE_STATUS) &
                    RK3588_DDR_CHANNEL_GATE_BUSY, ==, 0);
    qtest_writel(qts, RK3588_DDR_PHY_GATE_CTRL, ~RK3588_DDR_PHY_GATE_ENABLE);
    g_assert_cmphex(qtest_readl(qts, RK3588_DDR_CHANNEL_GATE_STATUS) &
                    RK3588_DDR_CHANNEL_GATE_BUSY, ==,
                    RK3588_DDR_CHANNEL_GATE_BUSY);
    g_assert_cmphex(qtest_readl(qts, RK3588_DDR_CHANNEL_PHY_STATUS) &
                    RK3588_DDR_CHANNEL_PHY_BUSY, ==, 0);
    qtest_writel(qts, RK3588_DDRPHY_CTRL, 0x00030003);
    g_assert_cmphex(qtest_readl(qts, RK3588_DDRPHY_STATUS) &
                    RK3588_DDRPHY_STATUS_ACTIVE, ==,
                    RK3588_DDRPHY_STATUS_ACTIVE);
    qtest_writel(qts, RK3588_DDRPHY_CTRL, 0x00030000);
    g_assert_cmphex(qtest_readl(qts, RK3588_DDRPHY_STATUS) &
                    RK3588_DDRPHY_STATUS_ACTIVE, ==, 0);

    qtest_writel(qts, RK3588_PMU1_GRF_BASE, ROCKCHIP_HIWORD_PIN0_SET);
    g_assert_cmphex(qtest_readl(qts, RK3588_PMU1_GRF_BASE), ==, 1);

    qtest_writel(qts, RK3588_STIMER_BASE + STIMER_LOAD0, UINT32_MAX);
    qtest_writel(qts, RK3588_STIMER_BASE + STIMER_CTRL, STIMER_ENABLE);
    g_assert_cmphex(qtest_readl(qts, RK3588_STIMER_BASE + STIMER_CTRL), ==,
                    STIMER_ENABLE);

    vendor_area1 = qtest_readw(qts, RK3588_SDHCI_BASE + SDHCI_VENDOR_AREA1);
    vendor_area2 = qtest_readw(qts, RK3588_SDHCI_BASE +
                               SDHCI_VENDOR_AREA1 + 2);
    g_assert_cmphex(vendor_area1, ==, DWCMSHC_VENDOR_AREA1);
    g_assert_cmphex(vendor_area2, ==, DWCMSHC_VENDOR_AREA2);

    dll_status = qtest_readl(qts, RK3588_SDHCI_BASE +
                             DWCMSHC_EMMC_DLL_STATUS0);
    g_assert_cmphex(dll_status & DWCMSHC_EMMC_DLL_LOCKED, ==,
                    DWCMSHC_EMMC_DLL_LOCKED);

    qtest_quit(qts);
}

static void rk3588_assert_usb2_ehci(QTestState *qts, uint64_t base)
{
    uint32_t capbase = qtest_readl(qts, base + USB2_EHCI_CAPBASE);
    uint32_t hcsparams = qtest_readl(qts, base + USB2_EHCI_HCSPARAMS);

    g_assert_cmphex(capbase & 0xff, ==, USB2_EHCI_CAPLENGTH);
    g_assert_cmphex(capbase >> 16, ==, USB2_EHCI_VERSION_1_0);
    g_assert_cmphex(hcsparams & 0xf, ==, USB2_EHCI_NPORTS);
    g_assert_cmphex(qtest_readl(qts, base + USB2_EHCI_USBSTS) &
                    USB2_EHCI_STS_HALT, ==, USB2_EHCI_STS_HALT);
    g_assert_cmphex(qtest_readl(qts, base + USB2_EHCI_PORTSC0) &
                    USB2_EHCI_PORT_POWER, ==, USB2_EHCI_PORT_POWER);

    qtest_writel(qts, base + USB2_EHCI_USBCMD, USB2_EHCI_CMD_RESET);
    g_assert_cmphex(qtest_readl(qts, base + USB2_EHCI_USBCMD) &
                    USB2_EHCI_CMD_RESET, ==, 0);
    g_assert_cmphex(qtest_readl(qts, base + USB2_EHCI_USBSTS) &
                    USB2_EHCI_STS_HALT, ==, USB2_EHCI_STS_HALT);

    qtest_writel(qts, base + USB2_EHCI_USBCMD, USB2_EHCI_CMD_RUN);
    g_assert_cmphex(qtest_readl(qts, base + USB2_EHCI_USBCMD) &
                    USB2_EHCI_CMD_RUN, ==, USB2_EHCI_CMD_RUN);
    g_assert_cmphex(qtest_readl(qts, base + USB2_EHCI_USBSTS) &
                    USB2_EHCI_STS_HALT, ==, 0);
}

static void rk3588_assert_usb2_ohci(QTestState *qts, uint64_t base)
{
    uint32_t roothub_a;

    g_assert_cmphex(qtest_readl(qts, base + USB2_OHCI_REVISION) & 0xff, ==,
                    USB2_OHCI_REVISION_1_0);
    roothub_a = qtest_readl(qts, base + USB2_OHCI_ROOTHUB_A);
    g_assert_cmphex(roothub_a & USB2_OHCI_RH_A_NDP_MASK, ==,
                    USB2_OHCI_RH_A_NDP1);
    g_assert_cmphex(roothub_a & (USB2_OHCI_RH_A_NPS | USB2_OHCI_RH_A_NOCP),
                    ==, USB2_OHCI_RH_A_NPS | USB2_OHCI_RH_A_NOCP);
    g_assert_cmphex(qtest_readl(qts, base + USB2_OHCI_PORTSTATUS0) &
                    USB2_OHCI_RH_PS_PPS, ==, USB2_OHCI_RH_PS_PPS);

    qtest_writel(qts, base + USB2_OHCI_CMDSTATUS, USB2_OHCI_HCR);
    g_assert_cmphex(qtest_readl(qts, base + USB2_OHCI_CMDSTATUS) &
                    USB2_OHCI_HCR, ==, 0);
}

static void test_rk3588_usb2_host_firmware_windows(void)
{
    QTestState *qts = rk3588_qtest_start(1);

    rk3588_assert_usb2_ehci(qts, RK3588_USB2_HOST0_EHCI_BASE);
    rk3588_assert_usb2_ohci(qts, RK3588_USB2_HOST0_OHCI_BASE);
    rk3588_assert_usb2_ehci(qts, RK3588_USB2_HOST1_EHCI_BASE);
    rk3588_assert_usb2_ohci(qts, RK3588_USB2_HOST1_OHCI_BASE);

    qtest_system_reset(qts);
    rk3588_assert_usb2_ehci(qts, RK3588_USB2_HOST0_EHCI_BASE);
    rk3588_assert_usb2_ohci(qts, RK3588_USB2_HOST0_OHCI_BASE);

    qtest_quit(qts);
}

static void test_rk3588_its_lpi(void)
{
    QTestState *qts = rk3588_qtest_start(1);
    uint64_t typer;

    g_assert_cmphex(qtest_readq(qts, RK3588_GICR_BASE + GICR_TYPER) &
                    GICR_TYPER_PLPIS, ==, GICR_TYPER_PLPIS);

    g_assert_cmphex(qtest_readl(qts, RK3588_GIC_ITS0_BASE + GITS_CTLR), ==,
                    GITS_CTLR_QUIESCENT);
    g_assert_cmphex(qtest_readl(qts, RK3588_GIC_ITS1_BASE + GITS_CTLR), ==,
                    GITS_CTLR_QUIESCENT);

    g_assert_cmphex(qtest_readl(qts, RK3588_GIC_ITS0_BASE + GITS_PIDR0), ==,
                    GITS_PIDR0_ITS);
    g_assert_cmphex(qtest_readl(qts, RK3588_GIC_ITS1_BASE + GITS_PIDR0), ==,
                    GITS_PIDR0_ITS);
    g_assert_cmphex(qtest_readl(qts, RK3588_GIC_ITS0_BASE + GITS_PIDR2), ==,
                    GITS_PIDR2_GICV3);
    g_assert_cmphex(qtest_readl(qts, RK3588_GIC_ITS1_BASE + GITS_PIDR2), ==,
                    GITS_PIDR2_GICV3);

    typer = qtest_readq(qts, RK3588_GIC_ITS1_BASE + GITS_TYPER);
    g_assert_cmphex(typer & GITS_TYPER_PHYSICAL, ==, GITS_TYPER_PHYSICAL);
    g_assert_cmphex(typer & GITS_TYPER_IDBITS_MASK, ==,
                    GITS_TYPER_IDBITS_15);
    g_assert_cmphex(typer & GITS_TYPER_DEVBITS_MASK, ==,
                    GITS_TYPER_DEVBITS_15);
    g_assert_cmphex(typer & GITS_TYPER_CIDBITS_MASK, ==,
                    GITS_TYPER_CIDBITS_15);
    g_assert_cmphex(typer & GITS_TYPER_CIL, ==, GITS_TYPER_CIL);

    qtest_quit(qts);
}

/*
 * RK3588 PCIe-specific qtest coverage.
 *
 *   - APB LTSSM_STATUS pinned to 0x00030011 (substitution policy:
 *     bits 17:16 = 0b11 -> link up; bits 5:0 = 0x11 -> LTSSM L0).
 *     rockchip_pcie_link_up returns true on the first poll so
 *     dw_pcie_wait_for_link terminates immediately.
 *
 *   - DBI root-port vendor/device id is readable through the inherited
 *     designware 4 KiB sysbus mmio (DWC core Type-0 cfg header).
 *
 *   - APB vendor writes (PCIE_CLIENT_*) are dropped without aborting
 *     (D-15: AArch64 guest writes to unassigned MMIO abort). The driver
 *     writes GENERAL_CON, HOT_RESET_CTRL, INTR_MASK_LEGACY, POWER_CON.
 *
 *   - CRU PLL lock status at 0x600 reads 0xffffffff so clk-rk3588
 *     early init sees all PLLs locked.
 */
static void test_rk3588_pcie(void)
{
    QTestState *qts = rk3588_qtest_start(1);
    uint32_t ltssm;
    uint32_t pcie_id;

    /* APB LTSSM pinned link-up. */
    ltssm = qtest_readl(qts, RK3588_PCIE3X4_APB_BASE + DWC_PCIE_LTSSM_STATUS);
    g_assert_cmphex(ltssm, ==, DWC_PCIE_LTSSM_LINK_UP);

    /* APB PCIE_CLIENT_* writes are accepted without aborting. */
    qtest_writel(qts, RK3588_PCIE3X4_APB_BASE + 0x000, 0xffff0004);
    qtest_writel(qts, RK3588_PCIE3X4_APB_BASE + 0x180, 0xffff0010);
    qtest_writel(qts, RK3588_PCIE3X4_APB_BASE + 0x01c, 0x000f000f);
    g_assert_cmphex(qtest_readl(qts, RK3588_PCIE3X4_APB_BASE + 0x000), ==, 0);

    /* DBI root-port vendor/device id is readable. */
    pcie_id = qtest_readl(qts, RK3588_PCIE3X4_DBI_BASE +
                          DWC_PCIE_VENDOR_DEVICE);
    g_assert_cmphex(pcie_id, !=, 0);
    g_assert_cmphex(pcie_id, !=, UINT32_MAX);

    /* DBI writes (iATU viewport select) land without aborting. */
    qtest_writel(qts, RK3588_PCIE3X4_DBI_BASE + 0x900, 0x00000000);
    qtest_writel(qts, RK3588_PCIE3X4_DBI_BASE + 0x904, 0x00000004);

    /*
     * CRU PLL lock status MUST read 0xffffffff - clk-rk3588 PLL init
     * polls this bit very early; 0 here hangs boot before the console.
     */
    g_assert_cmphex(qtest_readl(qts, RK3588_CRU_BASE + 0x600), ==,
                    0xffffffff);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRU_SPL_PLL_STATUS) &
                    RK3588_CRU_SPL_PLL_LOCKED, ==,
                    RK3588_CRU_SPL_PLL_LOCKED);
    /* Other CRU offsets are RAZ. */
    g_assert_cmphex(qtest_readl(qts, RK3588_CRU_BASE + 0xa80), ==, 0);

    qtest_quit(qts);
}

static void test_rk3588_gpio_bank(void)
{
    QTestState *qts = rk3588_qtest_start(1);

    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_VERSION_ID), ==,
                    GPIO_V2_ID);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_EXT_PORT_L) &
                    GPIO_PIN0, ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_INT_STATUS_L) &
                    GPIO_PIN0, ==, 0);

    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_SWPORT_DDR_L, GPIO_PIN0_SET);
    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_SWPORT_DR_L, GPIO_PIN0_SET);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_EXT_PORT_L) &
                    GPIO_PIN0, ==, GPIO_PIN0);

    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_SWPORT_DR_L, GPIO_PIN0_CLEAR);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_EXT_PORT_L) &
                    GPIO_PIN0, ==, 0);

    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_SWPORT_DDR_L, GPIO_PIN0_CLEAR);
    qtest_set_irq_in(qts, RK3588_GPIO0_QOM, "unnamed-gpio-in", 0, 1);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_EXT_PORT_L) &
                    GPIO_PIN0, ==, GPIO_PIN0);

    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_INT_POLARITY_L,
                 GPIO_PIN0_SET);
    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_INT_MASK_L, GPIO_PIN0_SET);
    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_INT_EN_L, GPIO_PIN0_SET);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_INT_RAWSTATUS_L) &
                    GPIO_PIN0, ==, GPIO_PIN0);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_INT_STATUS_L) &
                    GPIO_PIN0, ==, 0);

    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_INT_MASK_L, GPIO_PIN0_CLEAR);
    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_SWPORT_DDR_L, GPIO_PIN0_SET);
    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_SWPORT_DR_L, GPIO_PIN0_SET);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_EXT_PORT_L) &
                    GPIO_PIN0, ==, GPIO_PIN0);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_INT_STATUS_L) &
                    GPIO_PIN0, ==, 0);

    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_SWPORT_DR_L, GPIO_PIN0_CLEAR);
    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_SWPORT_DDR_L, GPIO_PIN0_CLEAR);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_INT_STATUS_L) &
                    GPIO_PIN0, ==, GPIO_PIN0);

    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_INT_EN_L, GPIO_PIN0_CLEAR);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_INT_STATUS_L) &
                    GPIO_PIN0, ==, 0);

    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_INT_EN_L, GPIO_PIN0_SET);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_INT_STATUS_L) &
                    GPIO_PIN0, ==, GPIO_PIN0);

    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_PORT_EOI_L, GPIO_PIN0_SET);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_INT_STATUS_L) &
                    GPIO_PIN0, ==, GPIO_PIN0);

    qtest_set_irq_in(qts, RK3588_GPIO0_QOM, "unnamed-gpio-in", 0, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_INT_STATUS_L) &
                    GPIO_PIN0, ==, 0);

    qtest_writel(qts, RK3588_GPIO0_BASE + GPIO_PORT_EOI_L, GPIO_PIN0_SET);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_INT_STATUS_L) &
                    GPIO_PIN0, ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_EXT_PORT_L) &
                    GPIO_PIN0, ==, 0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_VERSION_ID), ==,
                    GPIO_V2_ID);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_EXT_PORT_L) &
                    GPIO_PIN0, ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_GPIO0_BASE + GPIO_INT_STATUS_L) &
                    GPIO_PIN0, ==, 0);

    qtest_quit(qts);
}

static void test_rk3588_smp_creation(void)
{
    QTestState *qts = rk3588_qtest_start(8);

    qtest_quit(qts);
}

static void test_rk3588_zvm_ram(void)
{
    QTestState *qts = rk3588_qtest_start_zvm_ram();

    qtest_writel(qts, RK3588_ZVM_LOW_RAM_BASE, 0x68000000);
    g_assert_cmphex(qtest_readl(qts, RK3588_ZVM_LOW_RAM_BASE), ==,
                    0x68000000);

    qtest_writel(qts, RK3588_ZVM_SHARED_RAM_BASE + 0x408, 0xe7f00408);
    g_assert_cmphex(qtest_readl(qts, RK3588_ZVM_SHARED_RAM_BASE + 0x408), ==,
                    0xe7f00408);

    qtest_writel(qts, RK3588_ZVM_NOTIFY_INFO_BASE, 0xefd00000);
    g_assert_cmphex(qtest_readl(qts, RK3588_ZVM_NOTIFY_INFO_BASE), ==,
                    0xefd00000);

    qtest_writel(qts, RK3588_ZVM_NOTIFY_BASE, 0xefe00000);
    g_assert_cmphex(qtest_readl(qts, RK3588_ZVM_NOTIFY_BASE), ==,
                    0xefe00000);

    qtest_writeq(qts, RK3588_ZVM_HIGH_RAM_BASE, 0x100000000ULL);
    g_assert_cmphex(qtest_readq(qts, RK3588_ZVM_HIGH_RAM_BASE), ==,
                    0x100000000ULL);

    qtest_writeb(qts, RK3588_ZVM_HIGH_RAM_LAST, 0x5a);
    g_assert_cmphex(qtest_readb(qts, RK3588_ZVM_HIGH_RAM_LAST), ==, 0x5a);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3588_EVB_MACHINE)) {
        g_test_skip(RK3588_EVB_MACHINE " machine not available");
        return 0;
    }
    if (!qtest_has_device("arm-gicv3")) {
        g_test_skip("arm-gicv3 device not available");
        return 0;
    }
    if (!qtest_has_device("rockchip-gpio")) {
        g_test_skip("rockchip-gpio device not available");
        return 0;
    }
    if (!qtest_has_device("arm-gicv3-its")) {
        g_test_skip("arm-gicv3-its device not available");
        return 0;
    }

    qtest_add_func("/rk3588-evb/machine-creation",
                   test_rk3588_machine_creation);
    qtest_add_func("/rk3588-evb/smp-creation", test_rk3588_smp_creation);
    qtest_add_func("/rk3588-evb/peripheral-mmio",
                   test_rk3588_peripheral_mmio);
    qtest_add_func("/rk3588-evb/sdmmc-scmi", test_rk3588_sdmmc_scmi);
    qtest_add_func("/rk3588-evb/firmware-registers",
                   test_rk3588_firmware_registers);
    qtest_add_func("/rk3588-evb/usb2-host-firmware-windows",
                   test_rk3588_usb2_host_firmware_windows);
    qtest_add_func("/rk3588-evb/its-lpi", test_rk3588_its_lpi);
    qtest_add_func("/rk3588-evb/pcie", test_rk3588_pcie);
    qtest_add_func("/rk3588-evb/gpio-bank", test_rk3588_gpio_bank);
    qtest_add_func("/rk3588-evb/zvm-ram", test_rk3588_zvm_ram);

    return g_test_run();
}
