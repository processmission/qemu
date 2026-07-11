/*
 * QTest for the Rockchip RK3588 EVB machine model
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

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
#define RK3588_RKNN0_PC_BASE 0xfdab0000ULL
#define RK3588_RKNN0_CNA_BASE 0xfdab1000ULL
#define RK3588_RKNN0_CORE_BASE 0xfdab3000ULL
#define RK3588_RKNN1_PC_BASE 0xfdac0000ULL
#define RK3588_RKNN1_CNA_BASE 0xfdac1000ULL
#define RK3588_RKNN1_CORE_BASE 0xfdac3000ULL
#define RK3588_RKNN2_PC_BASE 0xfdad0000ULL
#define RK3588_RKNN2_CNA_BASE 0xfdad1000ULL
#define RK3588_RKNN2_CORE_BASE 0xfdad3000ULL
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
#define RK3588_RKNN0_SPI 110
#define RK3588_RKNN1_SPI 111
#define RK3588_RKNN2_SPI 112
#define RK3588_RKNN0_MMU_BASE 0xfdab9000ULL
#define RK3588_RKNN0_MMU1_BASE 0xfdaba000ULL
#define RK3588_RKNN1_MMU_BASE 0xfdaca000ULL
#define RK3588_RKNN2_MMU_BASE 0xfdada000ULL
#define RK3588_RKNN_TEST_DTE_ADDR (RK3588_RAM_BASE + 0x10000)
#define RK3588_RKNN_TEST_PTE_ADDR (RK3588_RAM_BASE + 0x11000)
#define RK3588_RKNN_TEST_REGCMD_ADDR (RK3588_RAM_BASE + 0x12000)
#define RK3588_RKNN_MATMUL_PTE_ADDR (RK3588_RAM_BASE + 0x20000)
#define RK3588_RKNN_MATMUL_DTE_ADDR (RK3588_RAM_BASE + 0x21000)
#define RK3588_RKNN_MATMUL_REGCMD_ADDR (RK3588_RAM_BASE + 0x22000)
#define RK3588_RKNN_MATMUL_INPUT_ADDR (RK3588_RAM_BASE + 0x23000)
#define RK3588_RKNN_MATMUL_WEIGHT_ADDR (RK3588_RAM_BASE + 0x24000)
#define RK3588_RKNN_MATMUL_OUTPUT_ADDR0 (RK3588_RAM_BASE + 0x25000)
#define RK3588_RKNN_MATMUL_OUTPUT_ADDR1 (RK3588_RAM_BASE + 0x27000)
#define RK3588_RKNN_MATMUL_REGCMD_IOVA 0x10000000U
#define RK3588_RKNN_MATMUL_INPUT_IOVA 0x10001000U
#define RK3588_RKNN_MATMUL_WEIGHT_IOVA 0x10002000U
#define RK3588_RKNN_MATMUL_OUTPUT_IOVA 0x10003800U
#define RK3588_RKNN_GOLDEN_TENSOR_ADDR (RK3588_RAM_BASE + 0x28000)
#define RK3588_RKNN_GOLDEN_REGCMD_IOVA 0x10000000U
#define RK3588_RKNN_GOLDEN_TENSOR_IOVA 0x068e6000U
#define RK3588_RKNN_GOLDEN_INPUT_IOVA 0x068e6340U
#define RK3588_RKNN_GOLDEN_WEIGHT_IOVA 0x068e6400U
#define RK3588_RKNN_GOLDEN_OUTPUT_IOVA 0x068e6c40U
#define RK3588_RKNN_MATMUL_M 32
#define RK3588_RKNN_MATMUL_K 64
#define RK3588_RKNN_MATMUL_N 32
#define RK3588_RKNN_MATMUL_COMMANDS 108
#define RK3588_SRST_A_RKNN1 250
#define RK3588_SRST_H_RKNN1 252
#define RK3588_SRST_A_RKNN2 254
#define RK3588_SRST_H_RKNN2 256
#define RK3588_SRST_A_RKNN0 272
#define RK3588_SRST_H_RKNN0 274
#define RK3588_GIC_QOM "/machine/gic"
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
#define RKNN_PC_VERSION 0x0000
#define RKNN_PC_VERSION_NUM 0x0004
#define RKNN_PC_OPERATION_ENABLE 0x0008
#define RKNN_PC_BASE_ADDRESS 0x0010
#define RKNN_PC_REGISTER_AMOUNTS 0x0014
#define RKNN_PC_INTERRUPT_MASK 0x0020
#define RKNN_PC_INTERRUPT_CLEAR 0x0024
#define RKNN_PC_INTERRUPT_STATUS 0x0028
#define RKNN_PC_INTERRUPT_RAW_STATUS 0x002c
#define RKNN_PC_TASK_CON 0x0030
#define RKNN_PC_TASK_DMA_BASE_ADDR 0x0034
#define RKNN_PC_TASK_STATUS 0x003c
#define RKNN_POINTER 0x0004
#define RKNN_PC_OPERATION_ENABLE_OP_EN 0x00000001
#define RKNN_DPU_INTERRUPT_BITS 0x00000300
#define RKNN_COMPLETE_DELAY_NS (100 * 1000)
#define RKNN_PC_VERSION_VALUE 0x00000100
#define RKNN_PC_VERSION_NUM_VALUE 0x00003588
#define RKNN_REGCMD_TARGET_CNA 0x0201ULL
#define RKNN_REGCMD_TARGET_CORE 0x0801ULL
#define RKNN_REGCMD_TARGET_DPU 0x1001ULL
#define RKNN_REGCMD_TARGET_DPU_RDMA 0x2001ULL
#define RKNN_REGCMD(target, value, reg) \
    (((target) << 48) | ((uint64_t)(uint32_t)(value) << 16) | (reg))
#define RKNN_CNA_DATA_SIZE0 0x1020ULL
#define RKNN_CNA_DATA_SIZE1 0x1024ULL
#define RKNN_CNA_FEATURE_DATA_ADDR 0x1070ULL
#define RKNN_CNA_DMA_CON1 0x107cULL
#define RKNN_CNA_DMA_CON2 0x1080ULL
#define RKNN_CNA_FC_DATA_SIZE0 0x1084ULL
#define RKNN_CNA_FC_DATA_SIZE1 0x1088ULL
#define RKNN_CNA_DCOMP_ADDR0 0x1110ULL
#define RKNN_CORE_MISC_CFG 0x3010ULL
#define RKNN_CORE_DATAOUT_SIZE_0 0x3014ULL
#define RKNN_CORE_DATAOUT_SIZE_1 0x3018ULL
#define RKNN_CORE_CLIP_TRUNCATE 0x301cULL
#define RKNN_DPU_FEATURE_MODE_CFG 0x400cULL
#define RKNN_DPU_DATA_FORMAT 0x4010ULL
#define RKNN_DPU_DST_BASE_ADDR 0x4020ULL
#define RKNN_DPU_DST_SURF_STRIDE 0x4024ULL
#define RKNN_DPU_DATA_CUBE_WIDTH 0x4030ULL
#define RKNN_DPU_DATA_CUBE_HEIGHT 0x4034ULL
#define RKNN_DPU_DATA_CUBE_CHANNEL 0x403cULL
#define RKNN_DPU_RDMA_DATA_CUBE_WIDTH 0x500cULL
#define RKNN_DPU_RDMA_DATA_CUBE_HEIGHT 0x5010ULL
#define RKNN_DPU_RDMA_DATA_CUBE_CHANNEL 0x5014ULL
#define RKNN_DPU_RDMA_SRC_BASE_ADDR 0x5018ULL
#define RKNN_DPU_RDMA_BS_BASE_ADDR 0x5020ULL
#define RKNN_DPU_RDMA_BN_BASE_ADDR 0x502cULL
#define RKNN_DPU_RDMA_ERDMA_CFG 0x5034ULL
#define RKNN_DPU_RDMA_EW_BASE_ADDR 0x5038ULL
#define RKNN_DPU_RDMA_EW_SURF_STRIDE 0x5040ULL
#define RKNN_DPU_RDMA_FEATURE_MODE_CFG 0x5044ULL
#define RK_IOMMU_DTE_ADDR 0x00
#define RK_IOMMU_STATUS 0x04
#define RK_IOMMU_COMMAND 0x08
#define RK_IOMMU_INT_MASK 0x1c
#define RK_IOMMU_STATUS_RESET 0x80000018
#define RK_IOMMU_STATUS_PAGING_ENABLED 0x00000001
#define RK_IOMMU_WINDOW_SIZE 0x100
#define RK_IOMMU_CMD_ENABLE_PAGING 0
#define RK_IOMMU_CMD_DISABLE_PAGING 1
#define RK_IOMMU_CMD_FORCE_RESET 6
#define RK_IOMMU_PTE_VALID 0x1
#define RK_IOMMU_PTE_READABLE 0x2
#define RK_IOMMU_PTE_WRITABLE 0x4
#define RK_IOMMU_PTE_RW (RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_READABLE | \
                         RK_IOMMU_PTE_WRITABLE)
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

static QTestState *rk3588_qtest_start_rknpu(void)
{
    return qtest_init("-machine " RK3588_EVB_MACHINE
                      ",rknpu=on -smp 1 -m 512M");
}

static QTestState *rk3588_qtest_start_rknpu_matmul(void)
{
    return qtest_init("-machine " RK3588_EVB_MACHINE
                      ",rknpu=on,rknpu-functional=on "
                      "-smp 1 -m 512M");
}

static QTestState *rk3588_qtest_start_rknpu_matmul_incoming(void)
{
    return qtest_init("-machine " RK3588_EVB_MACHINE
                      ",rknpu=on,rknpu-functional=on "
                      "-smp 1 -m 512M -incoming defer");
}

static QTestState *rk3588_qtest_start_rknpu_trace(const char *trace,
                                                 const char *event)
{
    return qtest_initf("-machine " RK3588_EVB_MACHINE
                       ",rknpu=on -smp 1 -m 512M "
                       "-trace enable=%s,file=%s", event, trace);
}

static char *rk3588_run_rknpu_trace_regcmd_event(const uint64_t *regcmd,
                                                 size_t regcmd_count,
                                                 const char *event)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *trace = NULL;
    char *contents = NULL;
    QTestState *qts;
    gsize len;
    int fd;

    g_assert_cmpuint(regcmd_count, >, 0);

    fd = g_file_open_tmp("rk3588-rknn-trace-XXXXXX", &trace, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    qts = rk3588_qtest_start_rknpu_trace(trace, event);

    qtest_writel(qts, RK3588_RKNN_TEST_DTE_ADDR,
                 RK3588_RKNN_TEST_PTE_ADDR | 1);
    qtest_writel(qts, RK3588_RKNN_TEST_PTE_ADDR,
                 RK3588_RKNN_TEST_REGCMD_ADDR | 1);
    for (size_t i = 0; i < regcmd_count; i++) {
        qtest_writeq(qts, RK3588_RKNN_TEST_REGCMD_ADDR +
                          i * sizeof(uint64_t), regcmd[i]);
    }

    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_DTE_ADDR,
                 RK3588_RKNN_TEST_DTE_ADDR);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_ENABLE_PAGING);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_BASE_ADDRESS, 0);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 regcmd_count - 1);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_OPERATION_ENABLE,
                 RKNN_PC_OPERATION_ENABLE_OP_EN);

    qtest_quit(qts);

    g_assert_true(g_file_get_contents(trace, &contents, &len, &error));
    g_assert_no_error(error);
    unlink(trace);

    return contents;
}

static char *rk3588_run_rknpu_trace_regcmd(const uint64_t *regcmd,
                                           size_t regcmd_count)
{
    return rk3588_run_rknpu_trace_regcmd_event(
        regcmd, regcmd_count, "rockchip_rknn_regcmd_*");
}

static uint64_t rk3588_rknn_regcmd(uint32_t target, uint32_t reg,
                                   uint32_t value)
{
    return cpu_to_le64(((uint64_t)target << 48) |
                       ((uint64_t)value << 16) | reg);
}

static void rk3588_rknn_make_matmul_regcmd(uint64_t commands[],
                                           bool supported)
{
    const uint32_t cna = 0x0201;
    const uint32_t core = 0x0801;
    const uint32_t dpu = 0x1001;
    unsigned int count = 0;

#define EMIT(_target, _reg, _value) \
    commands[count++] = rk3588_rknn_regcmd(_target, _reg, _value)
    EMIT(dpu, 0x4004, 0x0e);
    EMIT(cna, 0x100c, 0);
    EMIT(cna, 0x1010, (RK3588_RKNN_MATMUL_M + 1) << 4);
    EMIT(cna, 0x1014, 0x09);
    EMIT(cna, 0x1020, (1 << 16) |
         (supported ? RK3588_RKNN_MATMUL_M : RK3588_RKNN_MATMUL_M - 1));
    EMIT(cna, 0x1024, ((RK3588_RKNN_MATMUL_K - 1) << 16) |
         RK3588_RKNN_MATMUL_K);
    EMIT(cna, 0x1028, 1);
    EMIT(cna, 0x102c, RK3588_RKNN_MATMUL_M);
    EMIT(cna, 0x1030, RK3588_RKNN_MATMUL_N * RK3588_RKNN_MATMUL_K);
    EMIT(cna, 0x1034, RK3588_RKNN_MATMUL_K);
    EMIT(cna, 0x1038, (1 << 24) | (1 << 16) | RK3588_RKNN_MATMUL_N);
    EMIT(cna, 0x1040, (11 << 4) | 1);
    EMIT(cna, 0x1044, 1);
    EMIT(cna, 0x104c, 0x0b);
    for (uint32_t reg = 0x1050; reg <= 0x105c; reg += 4) {
        EMIT(cna, reg, 1 << 16);
    }
    EMIT(cna, 0x1060, 0);
    EMIT(cna, 0x1064, 0);
    EMIT(cna, 0x1068, 0);
    EMIT(cna, 0x1070, RK3588_RKNN_MATMUL_INPUT_IOVA);
    EMIT(cna, 0x1074, 0);
    EMIT(cna, 0x1078, 0x000f000f);
    EMIT(cna, 0x107c, 4);
    EMIT(cna, 0x1080, 28);
    EMIT(cna, 0x1084, (1 << 16) | RK3588_RKNN_MATMUL_M);
    EMIT(cna, 0x1088, RK3588_RKNN_MATMUL_K);
    EMIT(cna, 0x1100, 0);
    EMIT(cna, 0x1104, 0);
    EMIT(cna, 0x1110, RK3588_RKNN_MATMUL_WEIGHT_IOVA);
    for (uint32_t reg = 0x1140; reg < 0x1180; reg += 4) {
        EMIT(cna, reg, 0);
    }
    EMIT(cna, 0x1180, 0);
    EMIT(cna, 0x1184, 0);

    EMIT(core, 0x3010, 0);
    EMIT(core, 0x3014, (RK3588_RKNN_MATMUL_M - 1) << 16);
    EMIT(core, 0x3018, RK3588_RKNN_MATMUL_N - 1);
    EMIT(core, 0x301c, 0);
    EMIT(core, 0x3030, 0);

    EMIT(dpu, 0x400c, 0x1e4);
    EMIT(dpu, 0x4010, 4 << 29);
    EMIT(dpu, 0x4014, 0);
    EMIT(dpu, 0x4020, RK3588_RKNN_MATMUL_OUTPUT_IOVA);
    EMIT(dpu, 0x4024, RK3588_RKNN_MATMUL_M << 4);
    EMIT(dpu, 0x4030, 0);
    EMIT(dpu, 0x4034, RK3588_RKNN_MATMUL_M - 1);
    EMIT(dpu, 0x4038, 0);
    EMIT(dpu, 0x403c, ((RK3588_RKNN_MATMUL_N - 1) << 16) |
         (RK3588_RKNN_MATMUL_N - 1));
    EMIT(dpu, 0x4040, 0x53);
    EMIT(dpu, 0x4044, 0);
    EMIT(dpu, 0x4048, 0);
    EMIT(dpu, 0x404c, 0);
    EMIT(dpu, 0x4050, 0x7fe);
    EMIT(dpu, 0x4054, 0);
    EMIT(dpu, 0x4058, RK3588_RKNN_MATMUL_N - 1);
    EMIT(dpu, 0x405c, (RK3588_RKNN_MATMUL_M - 1) << 16);
    EMIT(dpu, 0x4060, 0x53);
    EMIT(dpu, 0x4064, 0);
    EMIT(dpu, 0x4068, 0);
    EMIT(dpu, 0x406c, 0);
    EMIT(dpu, 0x4070, 0x383);
    EMIT(dpu, 0x4074, 0);
    EMIT(dpu, 0x4078, 1);
    EMIT(dpu, 0x407c, 0);
    EMIT(dpu, 0x4080, 0);
    EMIT(dpu, 0x4084, 1);
    EMIT(dpu, 0x4088, 0);
    for (uint32_t reg = 0x4090; reg < 0x40b0; reg += 4) {
        EMIT(dpu, reg, 0);
    }
    EMIT(dpu, 0x40c0, (RK3588_RKNN_MATMUL_M * 8) << 4);
    EMIT(dpu, 0x40c4, 0);
    for (uint32_t reg = 0x4100; reg < 0x4130; reg += 4) {
        EMIT(dpu, reg, 0);
    }

    commands[count++] = 0;
    EMIT(0x0101, 0x0014, 0);
    EMIT(0x0041, 0, 0);
    EMIT(0x0081, 0x0008, 0x0d);
#undef EMIT
    g_assert_cmpuint(count, ==, RK3588_RKNN_MATMUL_COMMANDS);
}

static size_t rk3588_rknn_feature_index(unsigned int channels,
                                        unsigned int height,
                                        unsigned int atom,
                                        unsigned int channel,
                                        unsigned int row)
{
    return (channel / atom) * height * atom + atom * row + channel % atom;
}

static size_t rk3588_rknn_weight_index(unsigned int output,
                                       unsigned int channel)
{
    return (channel / 32 * 32) * 32 +
           (output / 32) * 32 * RK3588_RKNN_MATMUL_K + channel % 32 +
           (output % 32) * 32;
}

static void rk3588_rknn_prepare_matmul(QTestState *qts, bool supported,
                                       uint8_t sentinel)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    int8_t input[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_K] = { 0 };
    int8_t weights[RK3588_RKNN_MATMUL_N * RK3588_RKNN_MATMUL_K] = { 0 };
    uint8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4];

    rk3588_rknn_make_matmul_regcmd(commands, supported);
    for (unsigned int row = 0; row < RK3588_RKNN_MATMUL_M; row++) {
        for (unsigned int channel = 0; channel < RK3588_RKNN_MATMUL_K;
             channel++) {
            int8_t value = ((row * 37 + channel * 11 + 3) % 31) - 15;

            input[rk3588_rknn_feature_index(RK3588_RKNN_MATMUL_K,
                                            RK3588_RKNN_MATMUL_M, 16,
                                            channel, row)] = value;
        }
    }
    for (unsigned int out = 0; out < RK3588_RKNN_MATMUL_N; out++) {
        for (unsigned int channel = 0; channel < RK3588_RKNN_MATMUL_K;
             channel++) {
            weights[rk3588_rknn_weight_index(out, channel)] =
                ((out * 19 + channel * 7 + 5) % 29) - 14;
        }
    }
    memset(output, sentinel, sizeof(output));

    qtest_writel(qts, RK3588_RKNN_MATMUL_DTE_ADDR + 64 * 4,
                 RK3588_RKNN_MATMUL_PTE_ADDR | 1);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 0 * 4,
                 RK3588_RKNN_MATMUL_REGCMD_ADDR | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 1 * 4,
                 RK3588_RKNN_MATMUL_INPUT_ADDR | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 2 * 4,
                 RK3588_RKNN_MATMUL_WEIGHT_ADDR | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 3 * 4,
                 RK3588_RKNN_MATMUL_OUTPUT_ADDR0 | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 4 * 4,
                 RK3588_RKNN_MATMUL_OUTPUT_ADDR1 | RK_IOMMU_PTE_RW);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR, input, sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR, weights,
                   sizeof(weights));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800, output,
                   0x800);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR1, output + 0x800,
                   sizeof(output) - 0x800);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_DTE_ADDR,
                 RK3588_RKNN_MATMUL_DTE_ADDR);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_ENABLE_PAGING);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_BASE_ADDRESS,
                 RK3588_RKNN_MATMUL_REGCMD_IOVA);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 RK3588_RKNN_MATMUL_COMMANDS - 1);
}

static void rk3588_rknn_start_matmul(QTestState *qts)
{
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_OPERATION_ENABLE,
                 RKNN_PC_OPERATION_ENABLE_OP_EN);
}

static void rk3588_rknn_read_matmul_output(QTestState *qts, void *output,
                                           size_t length)
{
    g_assert_cmpuint(length, ==,
                     RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800, output,
                  0x800);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR1,
                  (uint8_t *)output + 0x800, length - 0x800);
}

static size_t rk3588_rknn_find_regcmd(const uint64_t commands[], size_t count,
                                      uint32_t target, uint32_t reg)
{
    for (size_t i = 0; i < count; i++) {
        uint64_t command = le64_to_cpu(commands[i]);

        if ((command >> 48) == target && (command & 0xffff) == reg) {
            return i;
        }
    }

    g_assert_not_reached();
}

static void rk3588_rknn_assert_matmul_output(QTestState *qts)
{
    uint32_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N];

    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int row = 0; row < RK3588_RKNN_MATMUL_M; row++) {
        for (unsigned int out = 0; out < RK3588_RKNN_MATMUL_N; out++) {
            int32_t expected = 0;
            size_t output_index = rk3588_rknn_feature_index(
                RK3588_RKNN_MATMUL_N, RK3588_RKNN_MATMUL_M, 4, out, row);

            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_MATMUL_K; channel++) {
                int8_t input =
                    ((row * 37 + channel * 11 + 3) % 31) - 15;
                int8_t weight =
                    ((out * 19 + channel * 7 + 5) % 29) - 14;

                expected += input * weight;
            }
            g_assert_cmpint((int32_t)le32_to_cpu(output[output_index]), ==,
                            expected);
        }
    }
}

static bool rk3588_qom_has_machine_child(QTestState *qts, const char *name)
{
    QDict *rsp;
    QList *children;
    QListEntry *entry;
    bool found = false;

    rsp = qtest_qmp(qts,
                    "{ 'execute': 'qom-list',"
                    "  'arguments': { 'path': '/machine' } }");
    g_assert(qdict_haskey(rsp, "return"));
    children = qdict_get_qlist(rsp, "return");

    QLIST_FOREACH_ENTRY(children, entry) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));

        if (!g_strcmp0(qdict_get_str(child, "name"), name)) {
            found = true;
            break;
        }
    }

    qobject_unref(rsp);
    return found;
}

static char *rk3588_create_dummy_kernel(void)
{
    g_autoptr(GError) error = NULL;
    char *path = NULL;
    int fd;

    fd = g_file_open_tmp("rk3588-kernel-XXXXXX", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, "\177ELF", 4), ==, 4);
    close(fd);

    return path;
}

static char *rk3588_fdtget(const char *fdtget, const char *dtb,
                           const char *type, const char *node,
                           const char *property)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *stderr_text = NULL;
    char *stdout_text = NULL;
    int status;
    char *argv[] = {
        (char *)fdtget, (char *)"-t", (char *)type, (char *)dtb,
        (char *)node, (char *)property, NULL,
    };

    g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                 &stdout_text, &stderr_text, &status, &error);
    g_assert_no_error(error);
    g_assert_cmpint(status, ==, 0);

    return stdout_text;
}

static void rk3588_assert_fdt_cells_consumed(const char *cells, int consumed)
{
    g_assert_cmpint(consumed, >, 0);

    for (const char *p = cells + consumed; *p; p++) {
        g_assert_true(g_ascii_isspace(*p));
    }
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

static void test_rk3588_rknpu_disabled_by_default(void)
{
    QTestState *qts = rk3588_qtest_start(1);

    g_assert_false(rk3588_qom_has_machine_child(qts, "rknn0"));
    g_assert_false(rk3588_qom_has_machine_child(qts, "rknn1"));
    g_assert_false(rk3588_qom_has_machine_child(qts, "rknn2"));

    qtest_quit(qts);
}

static void test_rk3588_rknpu_fdt(void)
{
    static const struct {
        const char *node;
        const char *iommu_node;
        uint64_t iommu_base0;
        uint64_t iommu_base1;
        unsigned int iommu_windows;
        unsigned int irq;
        unsigned int reset_a;
        unsigned int reset_h;
    } nodes[] = {
        {
            .node = "/npu@fdab0000",
            .iommu_node = "/iommu@fdab9000",
            .iommu_base0 = RK3588_RKNN0_MMU_BASE,
            .iommu_base1 = RK3588_RKNN0_MMU1_BASE,
            .iommu_windows = 2,
            .irq = RK3588_RKNN0_SPI,
            .reset_a = RK3588_SRST_A_RKNN0,
            .reset_h = RK3588_SRST_H_RKNN0,
        }, {
            .node = "/npu@fdac0000",
            .iommu_node = "/iommu@fdaca000",
            .iommu_base0 = RK3588_RKNN1_MMU_BASE,
            .iommu_windows = 1,
            .irq = RK3588_RKNN1_SPI,
            .reset_a = RK3588_SRST_A_RKNN1,
            .reset_h = RK3588_SRST_H_RKNN1,
        }, {
            .node = "/npu@fdad0000",
            .iommu_node = "/iommu@fdada000",
            .iommu_base0 = RK3588_RKNN2_MMU_BASE,
            .iommu_windows = 1,
            .irq = RK3588_RKNN2_SPI,
            .reset_a = RK3588_SRST_A_RKNN2,
            .reset_h = RK3588_SRST_H_RKNN2,
        },
    };
    const char *qemu = g_getenv("QTEST_QEMU_BINARY");
    g_autofree char *fdtget = g_find_program_in_path("fdtget");
    g_autofree char *kernel = NULL;
    g_autofree char *dtb = NULL;
    g_autofree char *machine = NULL;
    g_autoptr(GError) error = NULL;
    int fd;
    int status;
    char *argv[12];

    if (!qemu) {
        g_test_skip("QTEST_QEMU_BINARY not set");
        return;
    }
    if (!fdtget) {
        g_test_skip("fdtget not available");
        return;
    }

    kernel = rk3588_create_dummy_kernel();
    fd = g_file_open_tmp("rk3588-dtb-XXXXXX", &dtb, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    machine = g_strdup_printf(RK3588_EVB_MACHINE ",rknpu=on,dumpdtb=%s", dtb);
    argv[0] = (char *)qemu;
    argv[1] = (char *)"-machine";
    argv[2] = machine;
    argv[3] = (char *)"-smp";
    argv[4] = (char *)"1";
    argv[5] = (char *)"-m";
    argv[6] = (char *)"512M";
    argv[7] = (char *)"-kernel";
    argv[8] = kernel;
    argv[9] = (char *)"-display";
    argv[10] = (char *)"none";
    argv[11] = NULL;

    g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL,
                 &status, &error);
    g_assert_no_error(error);
    g_assert_cmpint(status, ==, 0);

    for (unsigned int i = 0; i < ARRAY_SIZE(nodes); i++) {
        g_autofree char *compatible =
            rk3588_fdtget(fdtget, dtb, "s", nodes[i].node, "compatible");
        g_autofree char *reg_names =
            rk3588_fdtget(fdtget, dtb, "s", nodes[i].node, "reg-names");
        g_autofree char *reset_names =
            rk3588_fdtget(fdtget, dtb, "s", nodes[i].node, "reset-names");
        g_autofree char *resets =
            rk3588_fdtget(fdtget, dtb, "u", nodes[i].node, "resets");
        g_autofree char *interrupts =
            rk3588_fdtget(fdtget, dtb, "u", nodes[i].node, "interrupts");
        g_autofree char *iommus =
            rk3588_fdtget(fdtget, dtb, "u", nodes[i].node, "iommus");
        g_autofree char *iommu_compatible =
            rk3588_fdtget(fdtget, dtb, "s", nodes[i].iommu_node,
                          "compatible");
        g_autofree char *iommu_phandle =
            rk3588_fdtget(fdtget, dtb, "u", nodes[i].iommu_node,
                          "phandle");
        g_autofree char *iommu_cells =
            rk3588_fdtget(fdtget, dtb, "u", nodes[i].iommu_node,
                          "#iommu-cells");
        g_autofree char *iommu_reg =
            rk3588_fdtget(fdtget, dtb, "u", nodes[i].iommu_node, "reg");
        g_autofree char *iommu_interrupts =
            rk3588_fdtget(fdtget, dtb, "u", nodes[i].iommu_node,
                          "interrupts");
        unsigned int phandle_a, reset_a, phandle_h, reset_h;
        unsigned int irq_type, irq, irq_flags, irq_cell;
        unsigned int npu_iommu_phandle, iommu_node_phandle;
        unsigned int iommu_cell_count;
        unsigned int reg[8];
        int consumed = 0;

        g_assert_nonnull(strstr(compatible, "rockchip,rk3588-rknn-core"));
        g_assert_nonnull(strstr(reg_names, "pc"));
        g_assert_nonnull(strstr(reg_names, "cna"));
        g_assert_nonnull(strstr(reg_names, "core"));
        g_assert_nonnull(strstr(reset_names, "srst_a"));
        g_assert_nonnull(strstr(reset_names, "srst_h"));

        g_assert_cmpint(sscanf(resets, "%u %u %u %u",
                               &phandle_a, &reset_a,
                               &phandle_h, &reset_h), ==, 4);
        g_assert_cmpuint(phandle_a, ==, phandle_h);
        g_assert_cmpuint(reset_a, ==, nodes[i].reset_a);
        g_assert_cmpuint(reset_h, ==, nodes[i].reset_h);

        g_assert_cmpint(sscanf(interrupts, "%u %u %u %u",
                               &irq_type, &irq,
                               &irq_flags, &irq_cell), ==, 4);
        g_assert_cmpuint(irq_type, ==, 0);
        g_assert_cmpuint(irq, ==, nodes[i].irq);
        g_assert_cmpuint(irq_flags, ==, 4);
        g_assert_cmpuint(irq_cell, ==, 0);

        g_assert_nonnull(strstr(iommu_compatible, "rockchip,rk3588-iommu"));
        g_assert_nonnull(strstr(iommu_compatible, "rockchip,rk3568-iommu"));
        g_assert_cmpint(sscanf(iommus, "%u", &npu_iommu_phandle), ==, 1);
        g_assert_cmpint(sscanf(iommu_phandle, "%u", &iommu_node_phandle), ==,
                        1);
        g_assert_cmpuint(npu_iommu_phandle, ==, iommu_node_phandle);
        g_assert_cmpint(sscanf(iommu_cells, "%u", &iommu_cell_count), ==, 1);
        g_assert_cmpuint(iommu_cell_count, ==, 0);

        g_assert_cmpint(sscanf(iommu_interrupts, "%u %u %u %u",
                               &irq_type, &irq,
                               &irq_flags, &irq_cell), ==, 4);
        g_assert_cmpuint(irq_type, ==, 0);
        g_assert_cmpuint(irq, ==, nodes[i].irq);
        g_assert_cmpuint(irq_flags, ==, 4);
        g_assert_cmpuint(irq_cell, ==, 0);

        if (nodes[i].iommu_windows == 2) {
            g_assert_cmpint(sscanf(iommu_reg,
                                   "%u %u %u %u %u %u %u %u %n",
                                   &reg[0], &reg[1], &reg[2], &reg[3],
                                   &reg[4], &reg[5], &reg[6], &reg[7],
                                   &consumed), ==,
                            8);
            rk3588_assert_fdt_cells_consumed(iommu_reg, consumed);
            g_assert_cmpuint(reg[4], ==, 0);
            g_assert_cmpuint(reg[5], ==, nodes[i].iommu_base1);
            g_assert_cmpuint(reg[6], ==, 0);
            g_assert_cmpuint(reg[7], ==, RK_IOMMU_WINDOW_SIZE);
        } else {
            g_assert_cmpint(sscanf(iommu_reg, "%u %u %u %u %n",
                                   &reg[0], &reg[1], &reg[2], &reg[3],
                                   &consumed), ==,
                            4);
            rk3588_assert_fdt_cells_consumed(iommu_reg, consumed);
        }
        g_assert_cmpuint(reg[0], ==, 0);
        g_assert_cmpuint(reg[1], ==, nodes[i].iommu_base0);
        g_assert_cmpuint(reg[2], ==, 0);
        g_assert_cmpuint(reg[3], ==, RK_IOMMU_WINDOW_SIZE);
    }

    unlink(dtb);
    unlink(kernel);
}

static void test_rk3588_rknpu_version_and_cores(void)
{
    static const uint64_t pc_bases[] = {
        RK3588_RKNN0_PC_BASE,
        RK3588_RKNN1_PC_BASE,
        RK3588_RKNN2_PC_BASE,
    };
    static const uint64_t cna_bases[] = {
        RK3588_RKNN0_CNA_BASE,
        RK3588_RKNN1_CNA_BASE,
        RK3588_RKNN2_CNA_BASE,
    };
    static const uint64_t core_bases[] = {
        RK3588_RKNN0_CORE_BASE,
        RK3588_RKNN1_CORE_BASE,
        RK3588_RKNN2_CORE_BASE,
    };
    QTestState *qts = rk3588_qtest_start_rknpu();

    g_assert_true(rk3588_qom_has_machine_child(qts, "rknn0"));
    g_assert_true(rk3588_qom_has_machine_child(qts, "rknn1"));
    g_assert_true(rk3588_qom_has_machine_child(qts, "rknn2"));

    for (unsigned int i = 0; i < ARRAY_SIZE(pc_bases); i++) {
        g_assert_cmphex(qtest_readl(qts, pc_bases[i] + RKNN_PC_VERSION), ==,
                        RKNN_PC_VERSION_VALUE);
        g_assert_cmphex(qtest_readl(qts,
                                    pc_bases[i] + RKNN_PC_VERSION_NUM), ==,
                        RKNN_PC_VERSION_NUM_VALUE);

        qtest_writel(qts, cna_bases[i] + RKNN_POINTER, 0x0001003f);
        qtest_writel(qts, core_bases[i] + RKNN_POINTER, 0x00010030);
        g_assert_cmphex(qtest_readl(qts, cna_bases[i] + RKNN_POINTER), ==,
                        0x0000000f);
        g_assert_cmphex(qtest_readl(qts, core_bases[i] + RKNN_POINTER), ==,
                        0x00000000);
        qtest_writel(qts, cna_bases[i] + RKNN_POINTER, 0xfffeffc0);
        g_assert_cmphex(qtest_readl(qts, cna_bases[i] + RKNN_POINTER), ==,
                        0x30000000);
    }

    qtest_quit(qts);
}

static void rk3588_assert_iommu_reset(QTestState *qts, uint64_t base)
{
    g_assert_cmphex(qtest_readl(qts, base + RK_IOMMU_STATUS), ==,
                    RK_IOMMU_STATUS_RESET);
    g_assert_cmphex(qtest_readl(qts, base + RK_IOMMU_INT_MASK), ==, 0);
}

static void test_rk3588_rknpu_iommu_mmio(void)
{
    static const uint64_t mmu_bases[] = {
        RK3588_RKNN0_MMU_BASE,
        RK3588_RKNN0_MMU1_BASE,
        RK3588_RKNN1_MMU_BASE,
        RK3588_RKNN2_MMU_BASE,
    };
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int i = 0; i < ARRAY_SIZE(mmu_bases); i++) {
        rk3588_assert_iommu_reset(qts, mmu_bases[i]);
    }

    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_DTE_ADDR,
                 0x12345000);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU_BASE +
                                RK_IOMMU_DTE_ADDR), ==, 0x12345000);

    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_ENABLE_PAGING);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU_BASE +
                                RK_IOMMU_STATUS) &
                    RK_IOMMU_STATUS_PAGING_ENABLED, ==,
                    RK_IOMMU_STATUS_PAGING_ENABLED);

    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_DISABLE_PAGING);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU_BASE +
                                RK_IOMMU_STATUS) &
                    RK_IOMMU_STATUS_PAGING_ENABLED, ==, 0);

    qtest_writel(qts, RK3588_RKNN0_MMU1_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_ENABLE_PAGING);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU1_BASE +
                                RK_IOMMU_STATUS) &
                    RK_IOMMU_STATUS_PAGING_ENABLED, ==,
                    RK_IOMMU_STATUS_PAGING_ENABLED);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU_BASE +
                                RK_IOMMU_STATUS) &
                    RK_IOMMU_STATUS_PAGING_ENABLED, ==, 0);

    qtest_writel(qts, RK3588_RKNN0_MMU1_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_FORCE_RESET);
    rk3588_assert_iommu_reset(qts, RK3588_RKNN0_MMU1_BASE);

    qtest_system_reset(qts);
    for (unsigned int i = 0; i < ARRAY_SIZE(mmu_bases); i++) {
        rk3588_assert_iommu_reset(qts, mmu_bases[i]);
    }

    qtest_quit(qts);
}

static void test_rk3588_rknpu_start_complete_irq(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    qtest_irq_intercept_in(qts, RK3588_GIC_QOM);

    qtest_writel(qts, RK3588_RKNN0_CNA_BASE + RKNN_POINTER, 0x00100000);
    qtest_writel(qts, RK3588_RKNN0_CORE_BASE + RKNN_POINTER, 0x00101000);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_BASE_ADDRESS,
                 0x00200000);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS, 7);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_MASK,
                 RKNN_DPU_INTERRUPT_BITS);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_CLEAR,
                 RKNN_DPU_INTERRUPT_BITS);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_TASK_CON, 0x00010001);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_TASK_DMA_BASE_ADDR, 0);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_OPERATION_ENABLE,
                 RKNN_PC_OPERATION_ENABLE_OP_EN);

    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_INTERRUPT_STATUS), ==, 0);
    g_assert_false(qtest_get_irq(qts, RK3588_RKNN0_SPI));

    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    RKNN_DPU_INTERRUPT_BITS);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_INTERRUPT_STATUS), ==,
                    RKNN_DPU_INTERRUPT_BITS);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==, 1);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_OPERATION_ENABLE) &
                    RKNN_PC_OPERATION_ENABLE_OP_EN, ==, 0);
    g_assert_true(qtest_get_irq(qts, RK3588_RKNN0_SPI));

    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_CLEAR,
                 RKNN_DPU_INTERRUPT_BITS);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_INTERRUPT_RAW_STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_INTERRUPT_STATUS), ==, 0);
    g_assert_false(qtest_get_irq(qts, RK3588_RKNN0_SPI));

    qtest_quit(qts);
}

static void test_rk3588_rknpu_matmul_disabled(void)
{
    uint8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0xa5);
    }
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==, 1);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_matmul_functional(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();

    qtest_irq_intercept_in(qts, RK3588_GIC_QOM);
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_MASK,
                 RKNN_DPU_INTERRUPT_BITS);
    rk3588_rknn_start_matmul(qts);

    g_assert_cmphex(qtest_readl(qts,
                               RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800), ==,
                    0xa5a5a5a5);
    g_assert_false(qtest_get_irq(qts, RK3588_RKNN0_SPI));
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_assert_matmul_output(qts);
    g_assert_true(qtest_get_irq(qts, RK3588_RKNN0_SPI));
    qtest_quit(qts);
}

static void rk3588_rknn_load_golden_file(const char *name, void **data,
                                         gsize *length)
{
    g_autofree char *path = g_build_filename(
        "tests", "data",
        "rk3588-rknpu-matmul-int8-m1-k64-n32", name, NULL);
    g_autoptr(GError) error = NULL;
    char *contents = NULL;

    if (!g_file_get_contents(path, &contents, length, &error)) {
        g_error("cannot load %s: %s", path, error->message);
    }
    *data = contents;
}

static void test_rk3588_rknpu_matmul_hardware_golden(void)
{
    g_autofree void *regcmd = NULL;
    g_autofree void *input = NULL;
    g_autofree void *weights = NULL;
    g_autofree void *expected = NULL;
    g_autofree void *actual = NULL;
    g_autofree uint8_t *sentinel = NULL;
    gsize regcmd_length;
    gsize input_length;
    gsize weights_length;
    gsize output_length;
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();

    rk3588_rknn_load_golden_file("regcmd.bin", &regcmd, &regcmd_length);
    rk3588_rknn_load_golden_file("input-npu-layout-i8.bin", &input,
                                 &input_length);
    rk3588_rknn_load_golden_file("weights-npu-layout-i8.bin", &weights,
                                 &weights_length);
    rk3588_rknn_load_golden_file("output-npu-layout-i32.bin", &expected,
                                 &output_length);
    g_assert_cmpuint(regcmd_length, ==, 108 * sizeof(uint64_t));
    g_assert_cmpuint(input_length, ==, 64);
    g_assert_cmpuint(weights_length, ==, 2048);
    g_assert_cmpuint(output_length, ==, 128);

    qtest_writel(qts, RK3588_RKNN_MATMUL_DTE_ADDR +
                 (RK3588_RKNN_GOLDEN_REGCMD_IOVA >> 22) * 4,
                 RK3588_RKNN_MATMUL_PTE_ADDR | RK_IOMMU_PTE_VALID);
    qtest_writel(qts, RK3588_RKNN_MATMUL_DTE_ADDR +
                 (RK3588_RKNN_GOLDEN_TENSOR_IOVA >> 22) * 4,
                 RK3588_RKNN_MATMUL_PTE_ADDR | RK_IOMMU_PTE_VALID);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR +
                 ((RK3588_RKNN_GOLDEN_REGCMD_IOVA >> 12) & 0x3ff) * 4,
                 RK3588_RKNN_MATMUL_REGCMD_ADDR | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR +
                 ((RK3588_RKNN_GOLDEN_TENSOR_IOVA >> 12) & 0x3ff) * 4,
                 RK3588_RKNN_GOLDEN_TENSOR_ADDR | RK_IOMMU_PTE_RW);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, regcmd,
                   regcmd_length);
    qtest_memwrite(qts, RK3588_RKNN_GOLDEN_TENSOR_ADDR +
                   (RK3588_RKNN_GOLDEN_INPUT_IOVA & 0xfff), input,
                   input_length);
    qtest_memwrite(qts, RK3588_RKNN_GOLDEN_TENSOR_ADDR +
                   (RK3588_RKNN_GOLDEN_WEIGHT_IOVA & 0xfff), weights,
                   weights_length);
    sentinel = g_malloc0(output_length);
    memset(sentinel, 0xa5, output_length);
    qtest_memwrite(qts, RK3588_RKNN_GOLDEN_TENSOR_ADDR +
                   (RK3588_RKNN_GOLDEN_OUTPUT_IOVA & 0xfff), sentinel,
                   output_length);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_DTE_ADDR,
                 RK3588_RKNN_MATMUL_DTE_ADDR);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_ENABLE_PAGING);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_BASE_ADDRESS,
                 RK3588_RKNN_GOLDEN_REGCMD_IOVA);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 regcmd_length / sizeof(uint64_t) - 1);
    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    actual = g_malloc(output_length);
    qtest_memread(qts, RK3588_RKNN_GOLDEN_TENSOR_ADDR +
                  (RK3588_RKNN_GOLDEN_OUTPUT_IOVA & 0xfff), actual,
                  output_length);
    g_assert_cmpmem(actual, output_length, expected, output_length);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_matmul_iommu_access(void)
{
    uint8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4];
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_DISABLE_PAGING);
    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0xa5);
    }
    qtest_quit(qts);

    qts = rk3588_qtest_start_rknpu_matmul();
    rk3588_rknn_prepare_matmul(qts, true, 0x5a);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 3 * 4,
                 RK3588_RKNN_MATMUL_OUTPUT_ADDR0 |
                 (RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_READABLE));
    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0x5a);
    }
    qtest_quit(qts);

    qts = rk3588_qtest_start_rknpu_matmul();
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 3 * 4,
                 RK3588_RKNN_MATMUL_OUTPUT_ADDR0 |
                 (RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_WRITABLE));
    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_assert_matmul_output(qts);
    qtest_quit(qts);

    qts = rk3588_qtest_start_rknpu_matmul();
    rk3588_rknn_prepare_matmul(qts, true, 0x3c);
    rk3588_rknn_start_matmul(qts);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 1 * 4,
                 RK3588_RKNN_MATMUL_INPUT_ADDR |
                 (RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_WRITABLE));
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0x3c);
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_matmul_functional_shape(void)
{
    enum { TEST_M = 3 };
    const unsigned int m = TEST_M;
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    int8_t input[TEST_M * RK3588_RKNN_MATMUL_K];
    uint32_t output[TEST_M * RK3588_RKNN_MATMUL_N];
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();
    size_t index;

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(commands, true);

#define PATCH(_target, _reg, _value) do {                            \
    index = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),  \
                                    (_target), (_reg));              \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    PATCH(0x0201, 0x1010, (m + 1) << 4);
    PATCH(0x0201, 0x1020, (1 << 16) | m);
    PATCH(0x0201, 0x102c, m);
    PATCH(0x0201, 0x1080,
          (uint32_t) ((int32_t) m - 4) & 0x0fffffff);
    PATCH(0x0201, 0x1084, (1 << 16) | m);
    PATCH(0x0801, 0x3014, (m - 1) << 16);
    PATCH(0x1001, 0x4024, m << 4);
    PATCH(0x1001, 0x4034, m - 1);
    PATCH(0x1001, 0x405c, (m - 1) << 16);
    PATCH(0x1001, 0x40c0, (m * 8) << 4);
#undef PATCH

    memset(input, 0, sizeof(input));
    for (unsigned int row = 0; row < m; row++) {
        for (unsigned int channel = 0;
             channel < RK3588_RKNN_MATMUL_K; channel++) {
            input[rk3588_rknn_feature_index(RK3588_RKNN_MATMUL_K,
                                             m, 16, channel, row)] =
                ((row * 37 + channel * 11 + 3) % 31) - 15;
        }
    }
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR, input,
                   sizeof(input));

    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));
    for (unsigned int row = 0; row < m; row++) {
        for (unsigned int out = 0; out < RK3588_RKNN_MATMUL_N; out++) {
            int32_t expected = 0;
            size_t output_index = rk3588_rknn_feature_index(
                RK3588_RKNN_MATMUL_N, m, 4, out, row);

            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_MATMUL_K; channel++) {
                int8_t input_value =
                    ((row * 37 + channel * 11 + 3) % 31) - 15;
                int8_t weight_value =
                    ((out * 19 + channel * 7 + 5) % 29) - 14;

                expected += input_value * weight_value;
            }
            g_assert_cmpint((int32_t)le32_to_cpu(output[output_index]), ==,
                            expected);
        }
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_matmul_partial_channels_unmodeled(void)
{
    enum {
        TEST_M = 4,
        TEST_K_VALID = 13,
        TEST_K_STORAGE = 16,
        TEST_N_VALID = 17,
        TEST_N_WEIGHT = 18,
        TEST_N_CUBE = 32,
    };
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    int8_t input[TEST_M * TEST_K_STORAGE];
    int8_t weights[TEST_N_CUBE * 32];
    uint32_t output[TEST_M * TEST_N_CUBE];
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();
    size_t index;

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(commands, true);

#define PATCH(_target, _reg, _value) do {                            \
    index = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),  \
                                    (_target), (_reg));              \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    PATCH(0x0201, 0x1010, (TEST_M + 1) << 4);
    PATCH(0x0201, 0x1020, (1 << 16) | TEST_M);
    PATCH(0x0201, 0x1024,
          ((TEST_K_VALID - 1) << 16) | TEST_K_STORAGE);
    PATCH(0x0201, 0x102c, TEST_M);
    PATCH(0x0201, 0x1030, TEST_N_WEIGHT * TEST_K_STORAGE);
    PATCH(0x0201, 0x1034, TEST_K_STORAGE);
    PATCH(0x0201, 0x1038,
          (1 << 24) | (1 << 16) | TEST_N_WEIGHT);
    PATCH(0x0201, 0x1080, 0);
    PATCH(0x0201, 0x1084, (1 << 16) | TEST_M);
    PATCH(0x0201, 0x1088, TEST_K_STORAGE);
    PATCH(0x0801, 0x3014, (TEST_M - 1) << 16);
    PATCH(0x0801, 0x3018, TEST_N_CUBE - 1);
    PATCH(0x1001, 0x4024, TEST_M << 4);
    PATCH(0x1001, 0x4034, TEST_M - 1);
    PATCH(0x1001, 0x403c,
          ((TEST_N_VALID - 1) << 16) | (TEST_N_CUBE - 1));
    PATCH(0x1001, 0x4058, TEST_N_CUBE - 1);
    PATCH(0x1001, 0x405c, (TEST_M - 1) << 16);
    PATCH(0x1001, 0x40c0, (TEST_M * 8) << 4);
#undef PATCH

    memset(input, 0, sizeof(input));
    memset(weights, 0, sizeof(weights));
    for (unsigned int row = 0; row < TEST_M; row++) {
        for (unsigned int channel = 0; channel < TEST_K_VALID; channel++) {
            input[rk3588_rknn_feature_index(TEST_K_STORAGE, TEST_M, 16,
                                             channel, row)] =
                ((row * 37 + channel * 11 + 3) % 31) - 15;
        }
    }
    for (unsigned int out = 0; out < TEST_N_VALID; out++) {
        for (unsigned int channel = 0; channel < TEST_K_VALID; channel++) {
            size_t weight_index =
                (channel / 32 * 32) * 32 +
                (out / 32) * 32 * 32 + channel % 32 +
                (out % 32) * 32;

            weights[weight_index] =
                ((out * 19 + channel * 7 + 5) % 29) - 14;
        }
    }
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR, input,
                   sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR, weights,
                   sizeof(weights));

    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));
    for (unsigned int value = 0; value < ARRAY_SIZE(output); value++) {
        g_assert_cmphex(le32_to_cpu(output[value]), ==, 0xa5a5a5a5);
    }
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==, 1);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_semantic_regcmd(void)
{
    uint64_t original[RK3588_RKNN_MATMUL_COMMANDS];
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS + 1];
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();
    size_t insert = RK3588_RKNN_MATMUL_COMMANDS - 4;

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(original, true);
    memcpy(commands, original, insert * sizeof(*commands));
    commands[insert] = rk3588_rknn_regcmd(0x0201, 0x100c, 0);
    memcpy(commands + insert + 1, original + insert,
           (ARRAY_SIZE(original) - insert) * sizeof(*commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 ARRAY_SIZE(commands) - 1);

    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_assert_matmul_output(qts);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_requires_presence(void)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    uint8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4];
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();
    size_t clip;

    rk3588_rknn_prepare_matmul(qts, true, 0x6b);
    rk3588_rknn_make_matmul_regcmd(commands, true);
    clip = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),
                                   0x0801, 0x301c);
    commands[clip] = rk3588_rknn_regcmd(0x0801, 0x3010, 0);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));

    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0x6b);
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_conv_con2_profile(void)
{
    static const uint32_t invalid_values[] = {
        (RK3588_RKNN_MATMUL_M + 2) << 4,
        1 << 16,
        ((RK3588_RKNN_MATMUL_M + 1) << 4) | (1 << 2),
        ((RK3588_RKNN_MATMUL_M + 1) << 4) | 1,
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(invalid_values); i++) {
        uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
        uint8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4];
        QTestState *qts = rk3588_qtest_start_rknpu_matmul();
        size_t conv_con2;

        rk3588_rknn_prepare_matmul(qts, true, 0x7c);
        rk3588_rknn_make_matmul_regcmd(commands, true);
        conv_con2 = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),
                                             0x0201, 0x1010);
        commands[conv_con2] = rk3588_rknn_regcmd(0x0201, 0x1010,
                                                  invalid_values[i]);
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                       sizeof(commands));
        rk3588_rknn_start_matmul(qts);
        qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
        rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
        for (unsigned int j = 0; j < ARRAY_SIZE(output); j++) {
            g_assert_cmphex(output[j], ==, 0x7c);
        }
        qtest_quit(qts);
    }
}

static void test_rk3588_rknpu_pipeline_dpu_bank1(void)
{
    uint64_t original[RK3588_RKNN_MATMUL_COMMANDS];
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS + 2];
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();
    size_t feature;

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(original, true);
    feature = rk3588_rknn_find_regcmd(original, ARRAY_SIZE(original),
                                      0x1001, 0x400c);
    memcpy(commands, original, feature * sizeof(*commands));
    commands[feature] = rk3588_rknn_regcmd(0x1001, 0x400c, 0);
    commands[feature + 1] = rk3588_rknn_regcmd(0x1001, 0x4004, 0x0f);
    memcpy(commands + feature + 2, original + feature,
           (ARRAY_SIZE(original) - feature) * sizeof(*commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 ARRAY_SIZE(commands) - 1);

    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_assert_matmul_output(qts);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_pointer_completion(void)
{
    uint64_t original[RK3588_RKNN_MATMUL_COMMANDS];
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS + 1];
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(original, true);
    commands[0] = rk3588_rknn_regcmd(0x0201, 0x1004, 0x0f);
    memcpy(commands + 1, original, sizeof(original));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 ARRAY_SIZE(commands) - 1);

    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_assert_matmul_output(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0x0001000f);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_pointer_mode0(void)
{
    uint64_t original[RK3588_RKNN_MATMUL_COMMANDS];
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS + 1];
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(original, true);
    commands[0] = rk3588_rknn_regcmd(0x0201, 0x1004, 0x07);
    memcpy(commands + 1, original, sizeof(original));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 ARRAY_SIZE(commands) - 1);

    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_assert_matmul_output(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0x00010007);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_executor_disabled(void)
{
    uint64_t original[RK3588_RKNN_MATMUL_COMMANDS];
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS + 1];
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(original, true);
    commands[0] = rk3588_rknn_regcmd(0x0201, 0x1004, 0x0f);
    memcpy(commands + 1, original, sizeof(original));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 ARRAY_SIZE(commands) - 1);
    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0x0001000f);

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(original, true);
    commands[0] = rk3588_rknn_regcmd(0x0201, 0x1004, 0x02);
    memcpy(commands + 1, original, sizeof(original));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 ARRAY_SIZE(commands) - 1);
    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_assert_matmul_output(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0x00010002);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_pointer_persistent_domain(void)
{
    uint64_t original[RK3588_RKNN_MATMUL_COMMANDS];
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS + 1];
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();

    rk3588_rknn_prepare_matmul(qts, false, 0xa5);
    rk3588_rknn_make_matmul_regcmd(original, false);
    commands[0] = rk3588_rknn_regcmd(0x0801, 0x3004, 0x0f);
    memcpy(commands + 1, original, sizeof(original));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 ARRAY_SIZE(commands) - 1);
    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CORE_BASE +
                                RKNN_POINTER), ==, 0x0001000f);

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(original, true);
    commands[0] = rk3588_rknn_regcmd(0x0201, 0x1004, 0x0f);
    memcpy(commands + 1, original, sizeof(original));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 ARRAY_SIZE(commands) - 1);
    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_assert_matmul_output(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0x0001000f);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CORE_BASE +
                                RKNN_POINTER), ==, 0x0000000f);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_task_dma_base(void)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    const uint32_t task_base = 0x10000000;
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();
    size_t input, weight, output;

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(commands, true);
    input = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),
                                    0x0201, 0x1070);
    weight = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),
                                     0x0201, 0x1110);
    output = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),
                                     0x1001, 0x4020);
    commands[input] = rk3588_rknn_regcmd(
        0x0201, 0x1070, RK3588_RKNN_MATMUL_INPUT_IOVA - task_base);
    commands[weight] = rk3588_rknn_regcmd(
        0x0201, 0x1110, RK3588_RKNN_MATMUL_WEIGHT_IOVA - task_base);
    commands[output] = rk3588_rknn_regcmd(
        0x1001, 0x4020, RK3588_RKNN_MATMUL_OUTPUT_IOVA - task_base);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_TASK_DMA_BASE_ADDR,
                 task_base);

    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_assert_matmul_output(qts);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_submission_snapshot(void)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();
    size_t shape;

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(commands, true);
    shape = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),
                                    0x0201, 0x1020);
    rk3588_rknn_start_matmul(qts);

    qtest_writeq(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR +
                      shape * sizeof(uint64_t),
                 le64_to_cpu(rk3588_rknn_regcmd(0x0201, 0x1020,
                                                (1 << 16) | 31)));
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_assert_matmul_output(qts);
    qtest_quit(qts);
}

#ifndef _WIN32
static void test_rk3588_rknpu_pipeline_migration_snapshot(void)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    g_autofree char *migration_socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *source;
    QTestState *destination;
    size_t shape;
    int fd;

    fd = g_file_open_tmp("rk3588-rknpu-migration-XXXXXX",
                         &migration_socket, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    unlink(migration_socket);
    uri = g_strdup_printf("unix:%s", migration_socket);

    source = rk3588_qtest_start_rknpu_matmul();
    rk3588_rknn_prepare_matmul(source, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(commands, true);
    shape = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),
                                    0x0201, 0x1020);
    rk3588_rknn_start_matmul(source);

    /* The migrated RAM is invalid; the device snapshot must be used. */
    qtest_writeq(source, RK3588_RKNN_MATMUL_REGCMD_ADDR +
                      shape * sizeof(uint64_t),
                 le64_to_cpu(rk3588_rknn_regcmd(0x0201, 0x1020,
                                                (1 << 16) | 31)));
    destination = rk3588_qtest_start_rknpu_matmul_incoming();
    qtest_qmp_assert_success(destination,
                             "{ 'execute': 'migrate-incoming',"
                             "  'arguments': { 'uri': %s } }", uri);
    qtest_qmp_assert_success(source,
                             "{ 'execute': 'migrate',"
                             "  'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(destination, "RESUME");
    qtest_clock_step(destination, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_assert_matmul_output(destination);
    qtest_quit(source);
    qtest_quit(destination);
}
#endif

static void test_rk3588_rknpu_matmul_unsupported(void)
{
    uint8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4];
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();

    rk3588_rknn_prepare_matmul(qts, false, 0x5a);
    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0x5a);
    }
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==, 1);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_matmul_reset_pending(void)
{
    uint8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4];
    QTestState *qts = rk3588_qtest_start_rknpu_matmul();

    rk3588_rknn_prepare_matmul(qts, true, 0x3c);
    rk3588_rknn_start_matmul(qts);
    qtest_system_reset(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0x3c);
    }
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==, 0);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_regcmd_ingest_trace(void)
{
    static const uint64_t regcmd[] = {
        /* CNA.CBUF_CON0 */
        0x0201000000001040ULL,
        /* CNA.DCOMP_REGNUM */
        0x0201000000001104ULL,
        /* CNA.DCOMP_CTRL */
        0x0201000000001100ULL,
        /* CNA.CONV_CON1 */
        0x020100000000100cULL,
        /* DPU.S_POINTER */
        0x10010000000e4004ULL,
        /* DPU_RDMA.RDMA_S_POINTER */
        0x20010000000e5004ULL,
        /* CNA.CONV_CON2 */
        0x0201000000341010ULL,
        /* CNA.CONV_CON3 */
        0x0201000000001014ULL,
        /* PPU.OPERATION_ENABLE */
        0x4001000000016008ULL,
        /* PPU_RDMA.RDMA_OPERATION_ENABLE */
        0x8001000000017008ULL,
    };
    g_autofree char *contents = NULL;

    contents = rk3588_run_rknpu_trace_regcmd(regcmd, ARRAY_SIZE(regcmd));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_ingest core=0 bank=0 "
                            "commands=10 ingested=10 pc=0 cna=6 "
                            "core_writes=0 dpu_writes=2 raw=0 unknown=0"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_ingest_ppu core=0 bank=0 "
                            "ppu=1 ppu_rdma=1"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_shadow_write core=0 "
                            "bank=0 index=4 domain=DPU rel=0x004 "
                            "value=0x0000000e"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_shadow_write core=0 "
                            "bank=0 index=5 domain=DPU_RDMA rel=0x004 "
                            "value=0x0000000e"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_shadow_write core=0 "
                            "bank=0 index=8 domain=PPU rel=0x008 "
                            "value=0x00000001"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_shadow_write core=0 "
                            "bank=0 index=9 domain=PPU_RDMA rel=0x008 "
                            "value=0x00000001"));
}

static void test_rk3588_rknpu_regcmd_ingest_ppu_event(void)
{
    static const uint64_t regcmd[] = {
        /* PPU.OPERATION_ENABLE */
        0x4001000000016008ULL,
        /* PPU_RDMA.RDMA_OPERATION_ENABLE */
        0x8001000000017008ULL,
    };
    g_autofree char *contents = NULL;

    contents = rk3588_run_rknpu_trace_regcmd_event(
        regcmd, ARRAY_SIZE(regcmd),
        "rockchip_rknn_regcmd_ingest_ppu");
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_ingest_ppu core=0 bank=0 "
                            "ppu=1 ppu_rdma=1"));
}

static void test_rk3588_rknpu_regcmd_sample_window_trace(void)
{
    uint64_t regcmd[20];
    g_autofree char *contents = NULL;

    for (unsigned int i = 0; i < ARRAY_SIZE(regcmd); i++) {
        regcmd[i] = (0x0201ULL << 48) | ((uint64_t)i << 16) | 0x1040;
    }

    contents = rk3588_run_rknpu_trace_regcmd(regcmd, ARRAY_SIZE(regcmd));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_ingest core=0 bank=0 "
                            "commands=20 ingested=20 pc=0 cna=20 "
                            "core_writes=0 dpu_writes=0 raw=0 unknown=0"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_shadow_write core=0 "
                            "bank=0 index=19 domain=CNA rel=0x040 "
                            "value=0x00000013"));
}

static void test_rk3588_rknpu_regcmd_summary_trace(void)
{
    static const uint64_t regcmd[] = {
        RKNN_REGCMD(RKNN_REGCMD_TARGET_CNA, 0x01230045,
                    RKNN_CNA_DATA_SIZE0),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_CNA, 0x00560789,
                    RKNN_CNA_DATA_SIZE1),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_CNA, 0x12345000,
                    RKNN_CNA_FEATURE_DATA_ADDR),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_CNA, 0x00001230,
                    RKNN_CNA_DMA_CON1),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_CNA, 0x00004560,
                    RKNN_CNA_DMA_CON2),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_CNA, 0x00330022,
                    RKNN_CNA_FC_DATA_SIZE0),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_CNA, 0x00000044,
                    RKNN_CNA_FC_DATA_SIZE1),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_CNA, 0x23456000,
                    RKNN_CNA_DCOMP_ADDR0),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_CORE, 0x00000003,
                    RKNN_CORE_MISC_CFG),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_CORE, 0x00550066,
                    RKNN_CORE_DATAOUT_SIZE_0),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_CORE, 0x00000077,
                    RKNN_CORE_DATAOUT_SIZE_1),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_CORE, 0x0000001f,
                    RKNN_CORE_CLIP_TRUNCATE),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU, 0x00001234,
                    RKNN_DPU_FEATURE_MODE_CFG),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU, 0x00005678,
                    RKNN_DPU_DATA_FORMAT),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU, 0x34567000,
                    RKNN_DPU_DST_BASE_ADDR),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU, 0x00001230,
                    RKNN_DPU_DST_SURF_STRIDE),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU, 0x00000010,
                    RKNN_DPU_DATA_CUBE_WIDTH),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU, 0x00000020,
                    RKNN_DPU_DATA_CUBE_HEIGHT),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU, 0x00310030,
                    RKNN_DPU_DATA_CUBE_CHANNEL),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU_RDMA, 0x00000011,
                    RKNN_DPU_RDMA_DATA_CUBE_WIDTH),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU_RDMA, 0x00000022,
                    RKNN_DPU_RDMA_DATA_CUBE_HEIGHT),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU_RDMA, 0x00000033,
                    RKNN_DPU_RDMA_DATA_CUBE_CHANNEL),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU_RDMA, 0x45678000,
                    RKNN_DPU_RDMA_SRC_BASE_ADDR),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU_RDMA, 0x56789000,
                    RKNN_DPU_RDMA_BS_BASE_ADDR),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU_RDMA, 0x6789a000,
                    RKNN_DPU_RDMA_BN_BASE_ADDR),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU_RDMA, 0x00002468,
                    RKNN_DPU_RDMA_ERDMA_CFG),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU_RDMA, 0x789ab000,
                    RKNN_DPU_RDMA_EW_BASE_ADDR),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU_RDMA, 0x00004560,
                    RKNN_DPU_RDMA_EW_SURF_STRIDE),
        RKNN_REGCMD(RKNN_REGCMD_TARGET_DPU_RDMA, 0x00013579,
                    RKNN_DPU_RDMA_FEATURE_MODE_CFG),
    };
    g_autofree char *contents = NULL;

    contents = rk3588_run_rknpu_trace_regcmd(regcmd, ARRAY_SIZE(regcmd));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_summary_cna_io core=0 "
                            "bank=0 "
                            "feature_addr=0x12345000 input=291x69x1929 "
                            "line_stride=4656 "
                            "surf_stride=17760 dcomp_addr=0x23456000"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_summary_cna_fc core=0 "
                            "bank=0 fc=51x34x68"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_summary_core core=0 bank=0 "
                            "misc_cfg=0x00000003 out=102x85x119 "
                            "clip_truncate=31"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_summary_dpu core=0 bank=0 "
                            "dst_addr=0x34567000 surf_stride=291 "
                            "cube=16x32x48 orig_channels=49 "
                            "feature_mode=0x00001234 "
                            "data_format=0x00005678"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_summary_rdma_io core=0 "
                            "bank=0 "
                            "src_addr=0x45678000 bs_addr=0x56789000 "
                            "bn_addr=0x6789a000 ew_addr=0x789ab000"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_summary_rdma_shape core=0 "
                            "bank=0 cube=17x34x51 ew_surf_stride=1110 "
                            "feature_mode=0x00013579 "
                            "erdma_cfg=0x00002468"));
}

static void test_rk3588_rknpu_regcmd_raw_unknown_trace(void)
{
    static const uint64_t regcmd[] = {
        0x0000000000000000ULL,
        0x0041000000000000ULL,
        0x0081000000010008ULL,
        0x9999123456787777ULL,
    };
    g_autofree char *contents = NULL;

    contents = rk3588_run_rknpu_trace_regcmd(regcmd, ARRAY_SIZE(regcmd));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_ingest core=0 bank=0 "
                            "commands=4 ingested=4 pc=0 cna=0 "
                            "core_writes=0 dpu_writes=0 raw=3 unknown=1"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_unhandled core=0 "
                            "bank=0 index=0 kind=zero target=0x0000 "
                            "reg=0x0000 value=0x00000000 "
                            "raw=0x0000000000000000"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_unhandled core=0 "
                            "bank=0 index=1 kind=pre-op-enable "
                            "target=0x0041 reg=0x0000 value=0x00000000 "
                            "raw=0x0041000000000000"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_unhandled core=0 "
                            "bank=0 index=2 kind=block-op-enable "
                            "target=0x0081 reg=0x0008 value=0x00000001 "
                            "raw=0x0081000000010008"));
    g_assert_nonnull(strstr(contents,
                            "rockchip_rknn_regcmd_unhandled core=0 "
                            "bank=0 index=3 kind=unknown target=0x9999 "
                            "reg=0x7777 value=0x12345678 "
                            "raw=0x9999123456787777"));
}

static void test_rk3588_rknpu_reset_state(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    qtest_writel(qts, RK3588_RKNN0_CNA_BASE + RKNN_POINTER, 0xffffffff);
    qtest_writel(qts, RK3588_RKNN0_CORE_BASE + RKNN_POINTER, 0xffffffff);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_MASK,
                 RKNN_DPU_INTERRUPT_BITS);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_OPERATION_ENABLE,
                 RKNN_PC_OPERATION_ENABLE_OP_EN);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    RKNN_DPU_INTERRUPT_BITS);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_VERSION), ==,
                    RKNN_PC_VERSION_VALUE);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_INTERRUPT_MASK), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_INTERRUPT_RAW_STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CORE_BASE +
                                RKNN_POINTER), ==, 0);

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
    if (!qtest_has_device("rockchip.rk3588-rknn-core")) {
        g_test_skip("rockchip.rk3588-rknn-core device not available");
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
    qtest_add_func("/rk3588/rknpu-disabled-by-default",
                   test_rk3588_rknpu_disabled_by_default);
    qtest_add_func("/rk3588/rknpu-fdt", test_rk3588_rknpu_fdt);
    qtest_add_func("/rk3588/rknpu-version-and-cores",
                   test_rk3588_rknpu_version_and_cores);
    qtest_add_func("/rk3588/rknpu-iommu-mmio",
                   test_rk3588_rknpu_iommu_mmio);
    qtest_add_func("/rk3588/rknpu-start-complete-irq",
                   test_rk3588_rknpu_start_complete_irq);
    qtest_add_func("/rk3588/rknpu-matmul-disabled",
                   test_rk3588_rknpu_matmul_disabled);
    qtest_add_func("/rk3588/rknpu-matmul-functional",
                   test_rk3588_rknpu_matmul_functional);
    qtest_add_func("/rk3588/rknpu-matmul-hardware-golden",
                   test_rk3588_rknpu_matmul_hardware_golden);
    qtest_add_func("/rk3588/rknpu-matmul-iommu-access",
                   test_rk3588_rknpu_matmul_iommu_access);
    qtest_add_func("/rk3588/rknpu-matmul-functional-shape",
                   test_rk3588_rknpu_matmul_functional_shape);
    qtest_add_func("/rk3588/rknpu-matmul-partial-channels-unmodeled",
                   test_rk3588_rknpu_matmul_partial_channels_unmodeled);
    qtest_add_func("/rk3588/rknpu-pipeline-semantic-regcmd",
                   test_rk3588_rknpu_pipeline_semantic_regcmd);
    qtest_add_func("/rk3588/rknpu-pipeline-requires-presence",
                   test_rk3588_rknpu_pipeline_requires_presence);
    qtest_add_func("/rk3588/rknpu-pipeline-conv-con2-profile",
                   test_rk3588_rknpu_pipeline_conv_con2_profile);
    qtest_add_func("/rk3588/rknpu-pipeline-dpu-bank1",
                   test_rk3588_rknpu_pipeline_dpu_bank1);
    qtest_add_func("/rk3588/rknpu-pipeline-pointer-completion",
                   test_rk3588_rknpu_pipeline_pointer_completion);
    qtest_add_func("/rk3588/rknpu-pipeline-pointer-mode0",
                   test_rk3588_rknpu_pipeline_pointer_mode0);
    qtest_add_func("/rk3588/rknpu-pipeline-executor-disabled",
                   test_rk3588_rknpu_pipeline_executor_disabled);
    qtest_add_func("/rk3588/rknpu-pipeline-pointer-persistent-domain",
                   test_rk3588_rknpu_pipeline_pointer_persistent_domain);
    qtest_add_func("/rk3588/rknpu-pipeline-task-dma-base",
                   test_rk3588_rknpu_pipeline_task_dma_base);
    qtest_add_func("/rk3588/rknpu-pipeline-submission-snapshot",
                   test_rk3588_rknpu_pipeline_submission_snapshot);
#ifndef _WIN32
    qtest_add_func("/rk3588/rknpu-pipeline-migration-snapshot",
                   test_rk3588_rknpu_pipeline_migration_snapshot);
#endif
    qtest_add_func("/rk3588/rknpu-matmul-unsupported",
                   test_rk3588_rknpu_matmul_unsupported);
    qtest_add_func("/rk3588/rknpu-matmul-reset-pending",
                   test_rk3588_rknpu_matmul_reset_pending);
    qtest_add_func("/rk3588/rknpu-regcmd-ingest-trace",
                   test_rk3588_rknpu_regcmd_ingest_trace);
    qtest_add_func("/rk3588/rknpu-regcmd-ingest-ppu-event",
                   test_rk3588_rknpu_regcmd_ingest_ppu_event);
    qtest_add_func("/rk3588/rknpu-regcmd-sample-window-trace",
                   test_rk3588_rknpu_regcmd_sample_window_trace);
    qtest_add_func("/rk3588/rknpu-regcmd-summary-trace",
                   test_rk3588_rknpu_regcmd_summary_trace);
    qtest_add_func("/rk3588/rknpu-regcmd-raw-unknown-trace",
                   test_rk3588_rknpu_regcmd_raw_unknown_trace);
    qtest_add_func("/rk3588/rknpu-reset-state",
                   test_rk3588_rknpu_reset_state);
    qtest_add_func("/rk3588-evb/zvm-ram", test_rk3588_zvm_ram);

    return g_test_run();
}
