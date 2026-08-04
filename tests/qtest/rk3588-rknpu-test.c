/*
 * QTest for the Rockchip RK3588 RKNPU accelerator
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qemu/bitops.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define RK3588_RAM_BASE 0x00200000ULL
#define RK3588_CRU_BASE 0xfd7c0000ULL
#define RK3588_CRU_SOFTRST_CON(n) (0xa00 + (n) * 4)
#define RK3588_RKNN0_PC_BASE 0xfdab0000ULL
#define RK3588_RKNN0_CNA_BASE 0xfdab1000ULL
#define RK3588_RKNN0_CORE_BASE 0xfdab3000ULL
#define RK3588_RKNN0_DPU_BASE 0xfdab4000ULL
#define RK3588_RKNN0_PPU_BASE 0xfdab6000ULL
#define RK3588_RKNN0_PPU_RDMA_BASE 0xfdab7000ULL
#define RK3588_RKNN0_GLOBAL_BASE 0xfdabf000ULL
#define RK3588_RKNN1_PC_BASE 0xfdac0000ULL
#define RK3588_RKNN1_CNA_BASE 0xfdac1000ULL
#define RK3588_RKNN1_CORE_BASE 0xfdac3000ULL
#define RK3588_RKNN1_DPU_BASE 0xfdac4000ULL
#define RK3588_RKNN1_PPU_BASE 0xfdac6000ULL
#define RK3588_RKNN1_PPU_RDMA_BASE 0xfdac7000ULL
#define RK3588_RKNN2_PC_BASE 0xfdad0000ULL
#define RK3588_RKNN2_CNA_BASE 0xfdad1000ULL
#define RK3588_RKNN2_CORE_BASE 0xfdad3000ULL
#define RK3588_RKNN2_DPU_BASE 0xfdad4000ULL
#define RK3588_RKNN2_PPU_BASE 0xfdad6000ULL
#define RK3588_RKNN2_PPU_RDMA_BASE 0xfdad7000ULL
#define RK3588_RKNN0_SPI 110
#define RK3588_RKNN1_SPI 111
#define RK3588_RKNN2_SPI 112
#define RK3588_RKNN0_QOM "/machine/rknn0"
#define RK3588_RKNN1_QOM "/machine/rknn1"
#define RK3588_RKNN2_QOM "/machine/rknn2"
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
#define RK3588_RKNN_MATMUL_RDMA_ADDR (RK3588_RAM_BASE + 0x29000)
#define RK3588_RKNN_CORE1_DTE_ADDR (RK3588_RAM_BASE + 0x90000)
#define RK3588_RKNN_CORE1_PTE_ADDR (RK3588_RAM_BASE + 0x91000)
#define RK3588_RKNN_CORE2_DTE_ADDR (RK3588_RAM_BASE + 0x94000)
#define RK3588_RKNN_CORE2_PTE_ADDR (RK3588_RAM_BASE + 0x95000)
#define RK3588_RKNN_MATMUL_REGCMD_IOVA 0x10000000U
#define RK3588_RKNN_MATMUL_INPUT_IOVA 0x10001000U
#define RK3588_RKNN_MATMUL_WEIGHT_IOVA 0x10002000U
#define RK3588_RKNN_MATMUL_OUTPUT_IOVA 0x10003800U
#define RK3588_RKNN_MATMUL_RDMA_IOVA 0x10005000U
#define RK3588_RKNN_PPU_INPUT_ADDR (RK3588_RAM_BASE + 0x30000)
#define RK3588_RKNN_PPU_OUTPUT_ADDR (RK3588_RAM_BASE + 0x40000)
#define RK3588_RKNN_PPU_INPUT_IOVA 0x10001000U
#define RK3588_RKNN_PPU_OUTPUT_IOVA 0x10010000U
#define RK3588_RKNN_PPU_COMMANDS 30
#define RK3588_RKNN_PPU_BUFFER_SIZE 0xc800
#define RK3588_RKNN_PPU_LINE_STRIDE 0x140
#define RK3588_RKNN_PPU_SURF_STRIDE 0x1900
#define RK3588_RKNN_PPU_SURFACES 8
#define RK3588_RKNN_PPU_BYTES_PER_PIXEL 16
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
#define RK3588_RKNN_DPU_RDMA_FP16_COMMANDS \
    (RK3588_RKNN_MATMUL_COMMANDS + 16)
#define RK3588_RKNN_DPU_RDMA_FP16_WIDTH 2
#define RK3588_RKNN_DPU_RDMA_FP16_HEIGHT 2
#define RK3588_RKNN_DPU_RDMA_FP16_CHANNELS 32
#define RK3588_RKNN_DPU_RDMA_FP16_VALID_CHANNELS 19
#define RK3588_RKNN_DPU_RDMA_FP16_INPUT_BYTES \
    (RK3588_RKNN_DPU_RDMA_FP16_WIDTH * \
     RK3588_RKNN_DPU_RDMA_FP16_HEIGHT * \
     RK3588_RKNN_DPU_RDMA_FP16_CHANNELS)
#define RK3588_RKNN_DPU_RDMA_FP16_SURFACE_BYTES \
    (sizeof(uint16_t) * RK3588_RKNN_DPU_RDMA_FP16_WIDTH * \
     RK3588_RKNN_DPU_RDMA_FP16_HEIGHT)
#define RK3588_RKNN_DPU_RDMA_FP16_OUTPUT_BYTES \
    (RK3588_RKNN_DPU_RDMA_FP16_INPUT_BYTES * sizeof(uint16_t))
#define RK3588_RKNN_DPU_LUT_ENTRIES 513
#define RK3588_RKNN_DEPTHWISE_WIDTH 8
#define RK3588_RKNN_DEPTHWISE_HEIGHT 8
#define RK3588_RKNN_DEPTHWISE_CHANNELS 32
#define RK3588_RKNN_DEPTHWISE_CUBE_CHANNELS 64
#define RK3588_RKNN_DEPTHWISE_KERNEL 3
#define RK3588_RKNN_DEPTHWISE_WEIGHT_BYTES 288
#define RK3588_RKNN_DEPTHWISE_OUTPUT_WORDS 4096
#define RK3588_RKNN_DEPTHWISE_OUTPUT_IOVA 0x1000c000U
#define RK3588_RKNN_DEPTHWISE_OUTPUT_ADDR (RK3588_RAM_BASE + 0x30000)
#define RK3588_RKNN_CONTROL_CHAIN_TASKS 120
#define RK3588_RKNN_CONTROL_CHAIN_LINK_COMMANDS 4
#define RK3588_RKNN_CONTROL_CHAIN_COMMANDS \
    ((RK3588_RKNN_CONTROL_CHAIN_TASKS - 1) * \
     RK3588_RKNN_CONTROL_CHAIN_LINK_COMMANDS + \
     RK3588_RKNN_MATMUL_COMMANDS)
#define RK3588_RKNN_SYNTH_CONV_ADDR (RK3588_RAM_BASE + 0x100000)
#define RK3588_RKNN_SYNTH_CONV_REGCMD_IOVA 0x10000000U
#define RK3588_RKNN_SYNTH_CONV_INPUT_IOVA 0x10010000U
#define RK3588_RKNN_SYNTH_CONV_WEIGHT_IOVA 0x10050000U
#define RK3588_RKNN_SYNTH_CONV_BS_IOVA 0x10051000U
#define RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA 0x10060000U
#define RK3588_RKNN_SYNTH_CONV_WIDTH 112
#define RK3588_RKNN_SYNTH_CONV_HEIGHT 12
#define RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS 32
#define RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS 64
#define RK3588_RKNN_SYNTH_CONV_ATOM 16
#define RK3588_RKNN_SYNTH_CONV_SURFACE_BYTES 200704
#define RK3588_RKNN_SYNTH_CONV_COMMANDS \
    (RK3588_RKNN_DPU_RDMA_FP16_COMMANDS + 2)
#define RK3588_RKNN_SYNTH_CONV_MAPPED_PAGES 256
#define RK3588_RKNN_SYNTH_CONV_INPUT_BYTES \
    (RK3588_RKNN_SYNTH_CONV_WIDTH * RK3588_RKNN_SYNTH_CONV_HEIGHT * \
     RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS)
#define RK3588_RKNN_SYNTH_CONV_WEIGHT_BYTES \
    (RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS * \
     RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS)
#define RK3588_RKNN_SYNTH_CONV_BS_BYTES \
    (RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS * 8)
#define RK3588_RKNN_SYNTH_CONV_OUTPUT_BYTES \
    (RK3588_RKNN_SYNTH_CONV_WIDTH * RK3588_RKNN_SYNTH_CONV_HEIGHT * \
     RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS)
#define RK3588_RKNN_RGB_CVT_INPUT_WIDTH 8
#define RK3588_RKNN_RGB_CVT_INPUT_HEIGHT 5
#define RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH 4
#define RK3588_RKNN_RGB_CVT_OUTPUT_HEIGHT 2
#define RK3588_RKNN_RGB_CVT_INPUT_CHANNELS 3
#define RK3588_RKNN_RGB_CVT_STORAGE_CHANNELS 16
#define RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS 32
#define RK3588_RKNN_RGB_CVT_KERNEL 3
#define RK3588_RKNN_RGB_CVT_SURFACE_STRIDE 0x1000
#define RK3588_RKNN_RGB_CVT_BS_IOVA 0x10052000U
#define RK3588_RKNN_RGB_CVT_BN_IOVA 0x10053000U
#define RK3588_RKNN_RGB_CVT_INPUT_BYTES \
    (RK3588_RKNN_RGB_CVT_INPUT_WIDTH * \
     RK3588_RKNN_RGB_CVT_INPUT_HEIGHT * \
     RK3588_RKNN_RGB_CVT_INPUT_CHANNELS)
#define RK3588_RKNN_RGB_CVT_WEIGHT_BYTES \
    (RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS * \
     RK3588_RKNN_RGB_CVT_KERNEL * RK3588_RKNN_RGB_CVT_KERNEL * \
     RK3588_RKNN_RGB_CVT_STORAGE_CHANNELS)
#define RK3588_RKNN_RGB_CVT_OUTPUT_SURFACE_BYTES \
    (RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH * \
     RK3588_RKNN_RGB_CVT_OUTPUT_HEIGHT * 16)
#define RK3588_RKNN_RGB_CVT_OUTPUT_BYTES \
    (RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH * \
     RK3588_RKNN_RGB_CVT_OUTPUT_HEIGHT * \
     RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS)
#define RK3588_RKNN_RGB_CVT_BS_BYTES \
    (RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS / 8 * 0x40)
#define RK3588_RKNN_RGB_CVT_CON0 0x00020404U
#define RK3588_RKNN_DEPTHWISE_INT8_WIDTH 4
#define RK3588_RKNN_DEPTHWISE_INT8_HEIGHT 3
#define RK3588_RKNN_DEPTHWISE_INT8_VALID_CHANNELS 32
#define RK3588_RKNN_DEPTHWISE_INT8_STORAGE_CHANNELS 64
#define RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE 0x1000
#define RK3588_RKNN_DEPTHWISE_INT8_BS_BYTES 0x200
#define RK3588_RKNN_DEPTHWISE_INT8_OUTPUT_BYTES \
    (4 * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE)
#define RK3588_SRST_A_RKNN1 250
#define RK3588_SRST_H_RKNN1 252
#define RK3588_SRST_A_RKNN2 254
#define RK3588_SRST_H_RKNN2 256
#define RK3588_SRST_A_RKNN0 272
#define RK3588_SRST_H_RKNN0 274
#define RK3588_VENDOR_SRST_A_RKNN0 486
#define RK3588_VENDOR_SRST_A_RKNN1 432
#define RK3588_VENDOR_SRST_A_RKNN2 448
#define RK3588_VENDOR_SRST_H_RKNN0 488
#define RK3588_VENDOR_SRST_H_RKNN1 434
#define RK3588_VENDOR_SRST_H_RKNN2 450
#define RK3588_GIC_QOM "/machine/gic"

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
#define RKNN_GLOBAL_OPERATION_ENABLE 0x0008
#define RKNN_POINTER 0x0004
#define RKNN_PC_OPERATION_ENABLE_OP_EN 0x00000001
#define RKNN_DPU_INTERRUPT_BITS 0x00000300
#define RKNN_DPU_BANK0_INTERRUPT 0x00000200
#define RKNN_DPU_BANK1_INTERRUPT 0x00000100
#define RKNN_PIPELINE_BANK0_INTERRUPT 0x000002aa
#define RKNN_PIPELINE_BANK1_INTERRUPT 0x00000155
#define RKNN_PC_INTERRUPT_VALID_BITS 0x0001ffff
#define RKNN_STAGE_RAW_STATUS_BITS 0xc0000000
#define RKNN_PPU_INTERRUPT_MASK 0x00000c00
#define RKNN_PPU_BANK0_INTERRUPT 0x00000400
#define RKNN_PPU_BANK1_INTERRUPT 0x00000800
#define RKNN_PPU_STATUS_SUCCESS 0x0000000c
#define RKNN_PPU_STATUS_FAULT 0x00000005
#define RKNN_DMA_READ_ERROR 0x00001000
#define RKNN_DMA_WRITE_ERROR 0x00002000
#define RKNN_TASK_STATUS_SUCCESS 0x0000f000
#define RKNN_PPU_TASK_STATUS 0x0000f000
#define RKNN_TASK_STATUS_FETCH_ERROR 0x0000a000
#define RKNN_COMPLETE_DELAY_NS (100 * 1000)
#define RKNN_PC_VERSION_VALUE 0x46495245
#define RKNN_PC_VERSION_NUM_VALUE 0x00000000
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
#define RKNN_DPU_OFFSET_PEND 0x4014ULL
#define RKNN_DPU_DST_BASE_ADDR 0x4020ULL
#define RKNN_DPU_DST_SURF_STRIDE 0x4024ULL
#define RKNN_DPU_DATA_CUBE_WIDTH 0x4030ULL
#define RKNN_DPU_DATA_CUBE_HEIGHT 0x4034ULL
#define RKNN_DPU_DATA_CUBE_NOTCH_ADDR 0x4038ULL
#define RKNN_DPU_DATA_CUBE_CHANNEL 0x403cULL
#define RKNN_DPU_RDMA_DATA_CUBE_WIDTH 0x500cULL
#define RKNN_DPU_RDMA_DATA_CUBE_HEIGHT 0x5010ULL
#define RKNN_DPU_RDMA_DATA_CUBE_CHANNEL 0x5014ULL
#define RKNN_DPU_RDMA_SRC_BASE_ADDR 0x5018ULL
#define RKNN_DPU_RDMA_BS_BASE_ADDR 0x5020ULL
#define RKNN_DPU_RDMA_NRDMA_CFG 0x5028ULL
#define RKNN_DPU_RDMA_BN_BASE_ADDR 0x502cULL
#define RKNN_DPU_RDMA_ERDMA_CFG 0x5034ULL
#define RKNN_DPU_RDMA_EW_BASE_ADDR 0x5038ULL
#define RKNN_DPU_RDMA_EW_SURF_STRIDE 0x5040ULL
#define RKNN_DPU_RDMA_FEATURE_MODE_CFG 0x5044ULL
#define RK_IOMMU_DTE_ADDR 0x00
#define RK_IOMMU_STATUS 0x04
#define RK_IOMMU_COMMAND 0x08
#define RK_IOMMU_PAGE_FAULT_ADDR 0x0c
#define RK_IOMMU_INT_RAWSTAT 0x14
#define RK_IOMMU_INT_CLEAR 0x18
#define RK_IOMMU_INT_MASK 0x1c
#define RK_IOMMU_INT_STATUS 0x20
#define RK_IOMMU_AUTO_GATING 0x24
#define RK_IOMMU_STATUS_RESET 0x80000018
#define RK_IOMMU_AUTO_GATING_RESET 0x00000001
#define RK_IOMMU_STATUS_PAGING_ENABLED 0x00000001
#define RK_IOMMU_STATUS_PAGE_FAULT_ACTIVE 0x00000002
#define RK_IOMMU_STATUS_PAGE_FAULT_IS_WRITE 0x00000020
#define RK_IOMMU_IRQ_PAGE_FAULT 0x00000001
#define RK_IOMMU_IRQ_BUS_ERROR 0x00000002
#define RK_IOMMU_WINDOW_SIZE 0x100
#define RK_IOMMU_CMD_ENABLE_PAGING 0
#define RK_IOMMU_CMD_DISABLE_PAGING 1
#define RK_IOMMU_CMD_PAGE_FAULT_DONE 5
#define RK_IOMMU_CMD_FORCE_RESET 6
#define RK_IOMMU_PTE_VALID 0x1
#define RK_IOMMU_PTE_READABLE 0x2
#define RK_IOMMU_PTE_WRITABLE 0x4
#define RK_IOMMU_PTE_RW (RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_READABLE | \
                         RK_IOMMU_PTE_WRITABLE)
#define RK3588_EVB_MACHINE "rk3588-evb"
#define RK3588S_ROC_PC_MACHINE "rk3588s-roc-pc"
#define ROCK_5B_PLUS_MACHINE "rock-5b-plus"

static uint32_t rk3588_rknn_read_pc(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, RK3588_RKNN0_PC_BASE + reg);
}

static void rk3588_rknn_assert_pc(QTestState *qts, uint32_t task_status,
                                  uint32_t raw_status);

static QTestState *rk3588_qtest_start(unsigned int cpus)
{
    return qtest_initf("-machine " RK3588_EVB_MACHINE " -smp %u -m 512M",
                       cpus);
}

static QTestState *rk3588_qtest_start_rknpu(void)
{
    return qtest_init("-machine " RK3588_EVB_MACHINE
                      ",rknpu=on -smp 1 -m 512M");
}

static void rk3588_rknpu_reset_fixture(QTestState *qts)
{
    qtest_system_reset(qts);
    rk3588_rknn_assert_pc(qts, 0, 0);
}

static QTestState *rk3588_qtest_start_rknpu_incoming(void)
{
    return qtest_init("-machine " RK3588_EVB_MACHINE
                      ",rknpu=on -smp 1 -m 512M -incoming defer");
}

#ifndef _WIN32
static QTestState *rk3588_rknn_migrate(QTestState *source,
                                       bool intercept_gic)
{
    g_autofree char *migration_socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *destination;
    int fd;

    fd = g_file_open_tmp("rk3588-rknpu-migration-XXXXXX",
                         &migration_socket, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    unlink(migration_socket);
    uri = g_strdup_printf("unix:%s", migration_socket);

    destination = rk3588_qtest_start_rknpu_incoming();
    if (intercept_gic) {
        qtest_irq_intercept_in(destination, RK3588_GIC_QOM);
    }
    qtest_qmp_assert_success(destination,
                             "{ 'execute': 'migrate-incoming',"
                             "  'arguments': { 'uri': %s } }", uri);
    qtest_qmp_assert_success(source,
                             "{ 'execute': 'migrate',"
                             "  'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(destination, "RESUME");

    return destination;
}
#endif

static uint32_t rk3588_rknn_register_amount(size_t command_count)
{
    g_assert_cmpuint(command_count, >, 0);

    return DIV_ROUND_UP(command_count, 2) - 1;
}

static uint64_t rk3588_rknn_regcmd(uint32_t target, uint32_t reg,
                                   uint32_t value)
{
    return cpu_to_le64(((uint64_t)target << 48) |
                       ((uint64_t)value << 16) | reg);
}

static void rk3588_rknn_make_matmul_regcmd(uint64_t commands[],
                                           bool modeled)
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
    EMIT(cna, 0x1020, (1 << 16) | RK3588_RKNN_MATMUL_M);
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

    EMIT(core, 0x3010, modeled ? 0 : 2);
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

static void rk3588_rknn_prepare_matmul(QTestState *qts, bool modeled,
                                       uint8_t sentinel)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    int8_t input[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_K] = { 0 };
    int8_t weights[RK3588_RKNN_MATMUL_N * RK3588_RKNN_MATMUL_K] = { 0 };
    uint8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4];

    rk3588_rknn_make_matmul_regcmd(commands, modeled);
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
                 rk3588_rknn_register_amount(RK3588_RKNN_MATMUL_COMMANDS));
}

static void rk3588_rknn_make_fp16_matmul_regcmd(uint64_t commands[],
                                                uint32_t output_precision)
{
    rk3588_rknn_make_matmul_regcmd(commands, true);

#define PATCH_FP16(_target, _reg, _value) do {                         \
    for (size_t index = 0; index < RK3588_RKNN_MATMUL_COMMANDS; index++) { \
        uint64_t command = le64_to_cpu(commands[index]);               \
        if ((command >> 48) == (_target) && (command & 0xffff) == (_reg)) { \
            commands[index] = rk3588_rknn_regcmd(                     \
                (_target), (_reg), (_value));                         \
            break;                                                     \
        }                                                               \
    }                                                                   \
} while (0)
    PATCH_FP16(0x0201, 0x100c, 0x120);
    PATCH_FP16(0x0201, 0x1010, 0x90);
    PATCH_FP16(0x0201, 0x1020, (1 << 16) | 8);
    PATCH_FP16(0x0201, 0x1024, (31 << 16) | 32);
    PATCH_FP16(0x0201, 0x102c, 8);
    PATCH_FP16(0x0201, 0x1030, 16 * 32 * 2);
    PATCH_FP16(0x0201, 0x1034, 32 * 2);
    PATCH_FP16(0x0201, 0x1038, (1 << 24) | (1 << 16) | 16);
    PATCH_FP16(0x0201, 0x107c, 4);
    PATCH_FP16(0x0201, 0x1080, 4);
    PATCH_FP16(0x0201, 0x1084, (1 << 16) | 8);
    PATCH_FP16(0x0201, 0x1088, 32);
    PATCH_FP16(0x0801, 0x3010, 0x201);
    PATCH_FP16(0x0801, 0x3014, 7 << 16);
    PATCH_FP16(0x0801, 0x3018, 15);
    PATCH_FP16(0x1001, 0x4010, (output_precision << 29) |
               (2 << 26) | 2);
    PATCH_FP16(0x1001, 0x4024, 8 << 4);
    PATCH_FP16(0x1001, 0x4034, 7);
    PATCH_FP16(0x1001, 0x403c, (15 << 16) | 15);
    PATCH_FP16(0x1001, 0x4050, 0x125);
    PATCH_FP16(0x1001, 0x4058, 15);
    PATCH_FP16(0x1001, 0x405c, 7 << 16);
    PATCH_FP16(0x1001, 0x4084, 1 | (output_precision == 2 ?
               (1U << 16) : 0));
    PATCH_FP16(0x1001, 0x40c0, 8 * 8 << 4);
#undef PATCH_FP16
}

static void rk3588_rknn_patch_regcmd(uint64_t commands[], size_t count,
                                     uint32_t target, uint32_t reg,
                                     uint32_t value);

static void rk3588_rknn_patch_fp16_regcmd(uint64_t commands[],
                                          uint32_t target, uint32_t reg,
                                          uint32_t value)
{
    rk3588_rknn_patch_regcmd(commands, RK3588_RKNN_MATMUL_COMMANDS,
                             target, reg, value);
}

static void rk3588_rknn_patch_regcmd(uint64_t commands[], size_t count,
                                     uint32_t target, uint32_t reg,
                                     uint32_t value)
{
    for (size_t index = 0; index < count; index++) {
        uint64_t command = le64_to_cpu(commands[index]);

        if ((command >> 48) == target && (command & 0xffff) == reg) {
            commands[index] = rk3588_rknn_regcmd(target, reg, value);
            return;
        }
    }
    g_assert_not_reached();
}

static void rk3588_rknn_make_dpu_rdma_fp16_regcmd(uint64_t commands[])
{
    uint64_t baseline[RK3588_RKNN_MATMUL_COMMANDS];
    unsigned int count = RK3588_RKNN_MATMUL_COMMANDS - 4;

    rk3588_rknn_make_matmul_regcmd(baseline, true);
    memcpy(commands, baseline, count * sizeof(*commands));

#define EMIT_RDMA(_reg, _value) \
    commands[count++] = rk3588_rknn_regcmd(0x2001, (_reg), (_value))
    EMIT_RDMA(0x5004, 0x0e);
    EMIT_RDMA(0x500c, RK3588_RKNN_DPU_RDMA_FP16_WIDTH - 1);
    EMIT_RDMA(0x5010, RK3588_RKNN_DPU_RDMA_FP16_HEIGHT - 1);
    EMIT_RDMA(0x5014, RK3588_RKNN_DPU_RDMA_FP16_CHANNELS - 1);
    EMIT_RDMA(0x5018, RK3588_RKNN_MATMUL_INPUT_IOVA);
    EMIT_RDMA(0x501c, 0);
    EMIT_RDMA(0x5020, 0);
    EMIT_RDMA(0x5028, 0);
    EMIT_RDMA(0x5034, 1);
    EMIT_RDMA(0x5038, 0);
    EMIT_RDMA(0x5040, 0);
    EMIT_RDMA(0x5044, 0x7801);
    EMIT_RDMA(0x5048, 0);
    EMIT_RDMA(0x504c, 0);
    EMIT_RDMA(0x5064, 0);
    EMIT_RDMA(0x5068, 0x01010101);
    EMIT_RDMA(0x506c, 0);
#undef EMIT_RDMA
    commands[count++] = baseline[RK3588_RKNN_MATMUL_COMMANDS - 3];
    commands[count++] = baseline[RK3588_RKNN_MATMUL_COMMANDS - 2];
    commands[count++] = rk3588_rknn_regcmd(
        0x0081, RKNN_PC_OPERATION_ENABLE, 0x18);
    g_assert_cmpuint(count, ==, RK3588_RKNN_DPU_RDMA_FP16_COMMANDS);

    rk3588_rknn_patch_fp16_regcmd(
        commands, RKNN_REGCMD_TARGET_DPU, RKNN_DPU_FEATURE_MODE_CFG, 0x1e5);
    rk3588_rknn_patch_fp16_regcmd(
        commands, RKNN_REGCMD_TARGET_DPU, RKNN_DPU_DATA_FORMAT, 2U << 29);
    rk3588_rknn_patch_fp16_regcmd(
        commands, RKNN_REGCMD_TARGET_DPU, RKNN_DPU_DST_BASE_ADDR,
        RK3588_RKNN_MATMUL_OUTPUT_IOVA);
    rk3588_rknn_patch_fp16_regcmd(
        commands, RKNN_REGCMD_TARGET_DPU, RKNN_DPU_DST_SURF_STRIDE,
        (RK3588_RKNN_DPU_RDMA_FP16_WIDTH *
         RK3588_RKNN_DPU_RDMA_FP16_HEIGHT) << 4);
    rk3588_rknn_patch_fp16_regcmd(
        commands, RKNN_REGCMD_TARGET_DPU, RKNN_DPU_DATA_CUBE_WIDTH,
        RK3588_RKNN_DPU_RDMA_FP16_WIDTH - 1);
    rk3588_rknn_patch_fp16_regcmd(
        commands, RKNN_REGCMD_TARGET_DPU, RKNN_DPU_DATA_CUBE_HEIGHT,
        RK3588_RKNN_DPU_RDMA_FP16_HEIGHT - 1);
    rk3588_rknn_patch_fp16_regcmd(
        commands, RKNN_REGCMD_TARGET_DPU, RKNN_DPU_DATA_CUBE_CHANNEL,
        ((RK3588_RKNN_DPU_RDMA_FP16_VALID_CHANNELS - 1) << 16) |
        (RK3588_RKNN_DPU_RDMA_FP16_CHANNELS - 1));
    rk3588_rknn_patch_fp16_regcmd(
        commands, RKNN_REGCMD_TARGET_DPU, 0x4050, 0x126);
    rk3588_rknn_patch_fp16_regcmd(
        commands, RKNN_REGCMD_TARGET_DPU, 0x4058,
        RK3588_RKNN_DPU_RDMA_FP16_CHANNELS - 1);
    rk3588_rknn_patch_fp16_regcmd(
        commands, RKNN_REGCMD_TARGET_DPU, 0x405c,
        ((RK3588_RKNN_DPU_RDMA_FP16_HEIGHT - 1) << 16) |
        (RK3588_RKNN_DPU_RDMA_FP16_WIDTH - 1));
    rk3588_rknn_patch_fp16_regcmd(
        commands, RKNN_REGCMD_TARGET_DPU, 0x4084, 0x00010001);
    rk3588_rknn_patch_fp16_regcmd(
        commands, RKNN_REGCMD_TARGET_DPU, 0x40c0,
         RK3588_RKNN_DPU_RDMA_FP16_SURFACE_BYTES << 4);
}

static size_t rk3588_rknn_make_dpu_rdma_int8_bypass_regcmd(
    uint64_t commands[])
{
    uint64_t baseline[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
    size_t output_count = 0;

    rk3588_rknn_make_dpu_rdma_fp16_regcmd(baseline);
    rk3588_rknn_patch_regcmd(
        baseline, ARRAY_SIZE(baseline), RKNN_REGCMD_TARGET_DPU,
        RKNN_DPU_DATA_FORMAT, 0);
    rk3588_rknn_patch_regcmd(
        baseline, ARRAY_SIZE(baseline), RKNN_REGCMD_TARGET_DPU,
        0x4040, 0x53);
    rk3588_rknn_patch_regcmd(
        baseline, ARRAY_SIZE(baseline), RKNN_REGCMD_TARGET_DPU,
        0x4050, 2);
    rk3588_rknn_patch_regcmd(
        baseline, ARRAY_SIZE(baseline), RKNN_REGCMD_TARGET_DPU,
        0x4054, 0);
    rk3588_rknn_patch_regcmd(
        baseline, ARRAY_SIZE(baseline), RKNN_REGCMD_TARGET_DPU,
        0x4060, 0x53);
    rk3588_rknn_patch_regcmd(
        baseline, ARRAY_SIZE(baseline), RKNN_REGCMD_TARGET_DPU,
        0x4070, 0x383);
    rk3588_rknn_patch_regcmd(
        baseline, ARRAY_SIZE(baseline), RKNN_REGCMD_TARGET_DPU,
        0x4084, 1);
    rk3588_rknn_patch_regcmd(
        baseline, ARRAY_SIZE(baseline), RKNN_REGCMD_TARGET_DPU,
        0x40c0, (RK3588_RKNN_DPU_RDMA_FP16_WIDTH *
                 RK3588_RKNN_DPU_RDMA_FP16_HEIGHT) << 4);

    for (size_t index = 0; index < ARRAY_SIZE(baseline); index++) {
        uint64_t command = le64_to_cpu(baseline[index]);
        uint32_t target = command >> 48;

        if (target == RKNN_REGCMD_TARGET_CNA ||
            target == RKNN_REGCMD_TARGET_CORE ||
            (target == RKNN_REGCMD_TARGET_DPU &&
             (command & 0xffff) == 0x40c4)) {
            continue;
        }
        commands[output_count++] = baseline[index];
    }
    return output_count;
}

static size_t rk3588_rknn_make_dpu_rdma_int8_unary_regcmd(
    uint64_t commands[])
{
    size_t count = rk3588_rknn_make_dpu_rdma_int8_bypass_regcmd(commands);

    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             0x4040, 0x00020050);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             0x4044, 3);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             0x4080, -2);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             0x4084, 4);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             0x4088, 2);
    return count;
}

static size_t rk3588_rknn_make_dpu_rdma_int8_binary_regcmd(
    uint64_t commands[])
{
    size_t count = rk3588_rknn_make_dpu_rdma_int8_bypass_regcmd(commands);

    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             RKNN_DPU_DATA_FORMAT, 0x20);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             0x4040, 0x00020040);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             0x4044, 2);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             0x4048, 0x00040200);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             0x4070, 0x904202c0);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             0x4074, 3);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             0x4078, 0x00010002);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU_RDMA,
                             0x5034, 0x40000004);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU_RDMA,
                             0x5038, RK3588_RKNN_MATMUL_RDMA_IOVA);
    rk3588_rknn_patch_regcmd(
        commands, count, RKNN_REGCMD_TARGET_DPU_RDMA, 0x5040,
        (RK3588_RKNN_DPU_RDMA_FP16_WIDTH *
         RK3588_RKNN_DPU_RDMA_FP16_HEIGHT) << 4);
    return count;
}

static size_t rk3588_rknn_make_dpu_rdma_int16_unpool_regcmd(
    uint64_t commands[], unsigned int output_height)
{
    enum {
        INPUT_WIDTH = 3,
        INPUT_CHANNELS = 8,
        OUTPUT_WIDTH = INPUT_WIDTH * 2,
        OUTPUT_CHANNELS = INPUT_CHANNELS * 2,
    };
    size_t count = rk3588_rknn_make_dpu_rdma_int8_bypass_regcmd(commands);

    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             RKNN_DPU_DATA_FORMAT, 1U << 26);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             RKNN_DPU_DST_SURF_STRIDE, OUTPUT_WIDTH << 4);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             RKNN_DPU_DATA_CUBE_WIDTH, OUTPUT_WIDTH - 1);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             RKNN_DPU_DATA_CUBE_HEIGHT, output_height - 1);
    rk3588_rknn_patch_regcmd(
        commands, count, RKNN_REGCMD_TARGET_DPU,
        RKNN_DPU_DATA_CUBE_CHANNEL,
        ((OUTPUT_CHANNELS - 1) << 16) | (OUTPUT_CHANNELS - 1));
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             0x4038, (OUTPUT_WIDTH << 16) | OUTPUT_WIDTH);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             0x4050, 0x126);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU,
                             0x4058, OUTPUT_CHANNELS - 1);
    rk3588_rknn_patch_regcmd(
        commands, count, RKNN_REGCMD_TARGET_DPU, 0x405c,
        ((output_height - 1) << 16) | (OUTPUT_WIDTH - 1));
    rk3588_rknn_patch_regcmd(
        commands, count, RKNN_REGCMD_TARGET_DPU, 0x40c0,
        OUTPUT_WIDTH * output_height * 32);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU_RDMA,
                             0x500c, INPUT_WIDTH - 1);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU_RDMA,
                             0x5010, output_height - 1);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU_RDMA,
                             0x5014, INPUT_CHANNELS - 1);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU_RDMA,
                             0x5044, 0xf801);
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU_RDMA,
                             0x5048, 0x1249);
    return count;
}

static void rk3588_rknn_make_dpu_rdma_fp16_bs_regcmd(uint64_t commands[])
{
    rk3588_rknn_make_dpu_rdma_fp16_regcmd(commands);
    rk3588_rknn_patch_regcmd(commands, RK3588_RKNN_DPU_RDMA_FP16_COMMANDS,
                             RKNN_REGCMD_TARGET_DPU, RKNN_DPU_DATA_FORMAT,
                             0x48000002);
    rk3588_rknn_patch_regcmd(commands, RK3588_RKNN_DPU_RDMA_FP16_COMMANDS,
                             RKNN_REGCMD_TARGET_DPU, 0x4050, 2);
    rk3588_rknn_patch_regcmd(commands, RK3588_RKNN_DPU_RDMA_FP16_COMMANDS,
                             RKNN_REGCMD_TARGET_DPU, 0x4054, 0);
    rk3588_rknn_patch_regcmd(commands, RK3588_RKNN_DPU_RDMA_FP16_COMMANDS,
                             RKNN_REGCMD_TARGET_DPU, 0x4040, 0x00020040);
    rk3588_rknn_patch_regcmd(commands, RK3588_RKNN_DPU_RDMA_FP16_COMMANDS,
                             RKNN_REGCMD_TARGET_DPU, 0x4044, 0x42500000);
    rk3588_rknn_patch_regcmd(commands, RK3588_RKNN_DPU_RDMA_FP16_COMMANDS,
                             RKNN_REGCMD_TARGET_DPU, 0x4048, 0x2ee10000);
    rk3588_rknn_patch_regcmd(commands, RK3588_RKNN_DPU_RDMA_FP16_COMMANDS,
                             RKNN_REGCMD_TARGET_DPU, 0x40c0,
                             (RK3588_RKNN_DPU_RDMA_FP16_WIDTH *
                              RK3588_RKNN_DPU_RDMA_FP16_HEIGHT) << 4);
    rk3588_rknn_patch_regcmd(commands, RK3588_RKNN_DPU_RDMA_FP16_COMMANDS,
                             RKNN_REGCMD_TARGET_DPU_RDMA, 0x5044, 0x17849);
}

static void rk3588_rknn_make_dpu_rdma_fp16_lut_regcmd(uint64_t commands[])
{
    const size_t count = RK3588_RKNN_DPU_RDMA_FP16_COMMANDS;

    rk3588_rknn_make_dpu_rdma_fp16_regcmd(commands);
#define PATCH_DPU(_reg, _value) \
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU, \
                            (_reg), (_value))
#define PATCH_RDMA(_reg, _value) \
    rk3588_rknn_patch_regcmd(commands, count, RKNN_REGCMD_TARGET_DPU_RDMA, \
                            (_reg), (_value))
    PATCH_DPU(0x400c, 0x000001e5);
    PATCH_DPU(0x4010, 0x48000002);
    PATCH_DPU(0x4024, 0x00000010);
    PATCH_DPU(0x4030, 0);
    PATCH_DPU(0x4034, 0);
    PATCH_DPU(0x403c, 0x00070007);
    PATCH_DPU(0x4040, 0x53);
    PATCH_DPU(0x4050, 2);
    PATCH_DPU(0x4054, 0);
    PATCH_DPU(0x4058, 7);
    PATCH_DPU(0x405c, 0);
    PATCH_DPU(0x4060, 0x00020040);
    PATCH_DPU(0x4064, 0x467ff000);
    PATCH_DPU(0x4068, 0x6a660000);
    PATCH_DPU(0x4070, 0x302);
    PATCH_DPU(0x4074, 0);
    PATCH_DPU(0x4078, 1);
    PATCH_DPU(0x4080, 1);
    PATCH_DPU(0x4084, 0x00010001);
    PATCH_DPU(0x4088, 0x0000f000);
    PATCH_DPU(0x40c0, 0x10);
    PATCH_DPU(0x4108, 0x68);
    PATCH_DPU(0x410c, 0x00050500);
    PATCH_DPU(0x4110, 0xffffc000);
    PATCH_DPU(0x4114, 0);
    PATCH_DPU(0x4118, 0);
    PATCH_DPU(0x411c, 0x00004000);
    PATCH_DPU(0x4120, 0);
    PATCH_DPU(0x4124, 0);
    PATCH_DPU(0x4128, 0);
    PATCH_DPU(0x412c, 0);
    PATCH_RDMA(0x500c, 0);
    PATCH_RDMA(0x5010, 0);
    PATCH_RDMA(0x5014, 7);
    PATCH_RDMA(0x5044, 0x00017849);
#undef PATCH_RDMA
#undef PATCH_DPU
}

static void rk3588_rknn_program_fp16_lut(QTestState *qts)
{
    static const struct {
        uint16_t index;
        uint16_t value;
    } table0[] = {
        { 0, 0x0001 }, { 204, 0x000b }, { 205, 0x000b },
    }, table1[] = {
        { 102, 0x0256 }, { 103, 0x025c },
        { 307, 0x114a }, { 308, 0x1175 },
        { 409, 0x2ed0 }, { 410, 0x2f45 },
        { 460, 0x4d08 }, { 461, 0x4dc9 },
        { 489, 0x663f }, { 490, 0x6740 },
        { 492, 0x6949 }, { 493, 0x6a52 },
        { 495, 0x6c6b }, { 496, 0x6d7b },
        { 499, 0x70bc }, { 500, 0x71d8 },
        { 502, 0x7416 }, { 503, 0x753a },
        { 505, 0x778a }, { 506, 0x78b6 },
        { 508, 0x7b18 }, { 509, 0x7c4d },
        { 511, 0x7ec1 }, { 512, 0x7fff },
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(table0); i++) {
        qtest_writel(qts, RK3588_RKNN0_DPU_BASE + 0x100,
                     0x00020000 | table0[i].index);
        qtest_writel(qts, RK3588_RKNN0_DPU_BASE + 0x104, table0[i].value);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(table1); i++) {
        qtest_writel(qts, RK3588_RKNN0_DPU_BASE + 0x100,
                     0x00030000 | table1[i].index);
        qtest_writel(qts, RK3588_RKNN0_DPU_BASE + 0x104, table1[i].value);
    }
}

static void rk3588_rknn_prepare_dpu_rdma_fp16_lut(
    QTestState *qts, const uint64_t commands[], const uint16_t input[])
{
    uint8_t output[16];

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    memset(output, 0xa5, sizeof(output));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   RK3588_RKNN_DPU_RDMA_FP16_COMMANDS * sizeof(*commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR, input, 16);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                   output, sizeof(output));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(
                     RK3588_RKNN_DPU_RDMA_FP16_COMMANDS));
}

static size_t rk3588_rknn_spatial_feature_index(
    unsigned int width, unsigned int height, unsigned int atom,
    unsigned int channel, unsigned int row, unsigned int column);

static void rk3588_rknn_make_dpu_rdma_fp16_input(int8_t input[])
{
    for (unsigned int row = 0;
         row < RK3588_RKNN_DPU_RDMA_FP16_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_DPU_RDMA_FP16_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_DPU_RDMA_FP16_CHANNELS; channel++) {
                size_t index = rk3588_rknn_spatial_feature_index(
                    RK3588_RKNN_DPU_RDMA_FP16_WIDTH,
                    RK3588_RKNN_DPU_RDMA_FP16_HEIGHT, 16,
                    channel, row, column);

                input[index] = ((row * 61 + column * 29 + channel * 7) %
                                255) - 127;
            }
        }
    }
}

static void rk3588_rknn_prepare_dpu_rdma_fp16(
    QTestState *qts, const uint64_t commands[], const int8_t input[],
    uint8_t sentinel)
{
    uint8_t output[RK3588_RKNN_DPU_RDMA_FP16_OUTPUT_BYTES];

    rk3588_rknn_prepare_matmul(qts, true, sentinel);
    memset(output, sentinel, sizeof(output));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   RK3588_RKNN_DPU_RDMA_FP16_COMMANDS * sizeof(*commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR, input,
                   RK3588_RKNN_DPU_RDMA_FP16_INPUT_BYTES);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                   output, sizeof(output));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(
                     RK3588_RKNN_DPU_RDMA_FP16_COMMANDS));
}

static void rk3588_rknn_prepare_dpu_rdma_fp16_bs(
    QTestState *qts, const uint64_t commands[], const uint16_t input[],
    uint8_t sentinel)
{
    uint8_t output[RK3588_RKNN_DPU_RDMA_FP16_OUTPUT_BYTES];

    rk3588_rknn_prepare_matmul(qts, true, sentinel);
    memset(output, sentinel, sizeof(output));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   RK3588_RKNN_DPU_RDMA_FP16_COMMANDS * sizeof(*commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR, input,
                   sizeof(uint16_t) * RK3588_RKNN_DPU_RDMA_FP16_WIDTH *
                   RK3588_RKNN_DPU_RDMA_FP16_HEIGHT *
                   RK3588_RKNN_DPU_RDMA_FP16_CHANNELS);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                   output, sizeof(output));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(
                     RK3588_RKNN_DPU_RDMA_FP16_COMMANDS));
}

static void rk3588_rknn_start_matmul(QTestState *qts);
static void rk3588_rknn_run_matmul(QTestState *qts);

static uint16_t rk3588_rknn_half_from_uint32(uint32_t value)
{
    unsigned int exponent = 31 - clz32(value);
    unsigned int mantissa;

    if (exponent <= 10) {
        mantissa = (value - (1U << exponent)) << (10 - exponent);
    } else {
        mantissa = (value >> (exponent - 10)) - 1024;
    }
    return ((exponent + 15) << 10) | mantissa;
}

static uint16_t rk3588_rknn_half_from_int8(int8_t value)
{
    if (!value) {
        return 0;
    }
    if (value < 0) {
        return 0x8000 | rk3588_rknn_half_from_uint32(-(int)value);
    }
    return rk3588_rknn_half_from_uint32(value);
}

static uint32_t rk3588_rknn_float32_from_uint32(uint32_t value)
{
    float float_value = value;
    uint32_t bits;

    memcpy(&bits, &float_value, sizeof(bits));
    return bits;
}

static void rk3588_rknn_prepare_fp16_matmul(
    QTestState *qts, const uint64_t commands[], const uint16_t input[],
    const uint16_t weights[])
{
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   RK3588_RKNN_MATMUL_COMMANDS * sizeof(*commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR, input,
                   8 * 32 * sizeof(*input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR, weights,
                   16 * 32 * sizeof(*weights));
}

static void rk3588_rknn_make_fp16_matmul_data(uint16_t input[],
                                              uint16_t weights[])
{
    static const uint16_t half_integers[] = {
        0x3c00, 0x4000, 0x4200, 0x4400, 0x4500, 0x4600, 0x4700, 0x4800,
        0x4880, 0x4900, 0x4980, 0x4a00, 0x4a80, 0x4b00, 0x4b80, 0x4c00,
    };

    memset(input, 0, 8 * 32 * sizeof(*input));
    memset(weights, 0, 16 * 32 * sizeof(*weights));
    for (unsigned int row = 0; row < 8; row++) {
        for (unsigned int channel = 0; channel < 32; channel++) {
            size_t index = (channel / 8) * 8 * 8 + row * 8 + channel % 8;

            input[index] = cpu_to_le16(half_integers[row]);
        }
    }
    for (unsigned int output_channel = 0; output_channel < 16;
         output_channel++) {
        for (unsigned int channel = 0; channel < 32; channel++) {
            weights[output_channel * 32 + channel] =
                cpu_to_le16(half_integers[output_channel]);
        }
    }
}

static void test_rk3588_rknpu_matmul_fp16(void)
{
    enum {
        FP16_HEIGHT = 8,
        FP16_INPUT_CHANNELS = 32,
        FP16_OUTPUT_CHANNELS = 16,
    };
    uint16_t input[FP16_HEIGHT * FP16_INPUT_CHANNELS] = { 0 };
    uint16_t weights[FP16_OUTPUT_CHANNELS * FP16_INPUT_CHANNELS] = { 0 };
    uint8_t output[FP16_HEIGHT * FP16_OUTPUT_CHANNELS *
                   sizeof(uint32_t)] = { 0 };

    rk3588_rknn_make_fp16_matmul_data(input, weights);

    for (size_t precision_index = 0; precision_index < 2; precision_index++) {
        const uint32_t output_precision = precision_index ? 2 : 5;
        uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
        QTestState *qts = rk3588_qtest_start_rknpu();
        size_t output_bytes = output_precision == 2 ? 256 : sizeof(output);

        rk3588_rknn_make_fp16_matmul_regcmd(commands, output_precision);
        rk3588_rknn_prepare_fp16_matmul(qts, commands, input, weights);
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                       output, sizeof(output));
        rk3588_rknn_run_matmul(qts);
        qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                      output, output_bytes);

        for (unsigned int row = 0; row < FP16_HEIGHT; row++) {
            for (unsigned int output_channel = 0;
                 output_channel < FP16_OUTPUT_CHANNELS; output_channel++) {
                uint32_t expected = (row + 1) * (output_channel + 1) * 32;
                unsigned int atom = output_precision == 2 ? 8 : 4;
                size_t index = (output_channel / atom) * FP16_HEIGHT * atom +
                               row * atom + output_channel % atom;

                if (output_precision == 2) {
                    g_assert_cmphex(lduw_le_p(output + index * 2), ==,
                                    rk3588_rknn_half_from_uint32(expected));
                } else {
                    g_assert_cmphex(ldl_le_p(output + index * 4), ==,
                                    rk3588_rknn_float32_from_uint32(expected));
                }
            }
        }
        g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                        RKNN_TASK_STATUS_SUCCESS);
        qtest_quit(qts);
    }
}

static void test_rk3588_rknpu_matmul_int8_quantized_planar(void)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    uint8_t input[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_K] = { 0 };
    int8_t weights[RK3588_RKNN_MATMUL_N * RK3588_RKNN_MATMUL_K];
    int8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_matmul_regcmd(commands, true);
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                             RKNN_REGCMD_TARGET_CNA, 0x104c, 0xa);
    for (uint32_t reg = 0x1050; reg <= 0x105c; reg += 4) {
        rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                 RKNN_REGCMD_TARGET_CNA, reg,
                                 reg == 0x1050 ? 0x00010001 : 0x00010000);
    }
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                             RKNN_REGCMD_TARGET_CNA, 0x1180, 0xffff);
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                             RKNN_REGCMD_TARGET_CORE, 0x3010, 1);
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                             RKNN_REGCMD_TARGET_DPU, 0x400c, 0x1e4);
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                             RKNN_REGCMD_TARGET_DPU, 0x4010, 0);
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                             RKNN_REGCMD_TARGET_DPU, 0x4050,
                             0x124);
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                             RKNN_REGCMD_TARGET_DPU, 0x4054, 1);
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                             RKNN_REGCMD_TARGET_DPU, 0x4080, -2);
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                             RKNN_REGCMD_TARGET_DPU, 0x4084, 1);
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                             RKNN_REGCMD_TARGET_DPU, 0x4088, 0);
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                             RKNN_REGCMD_TARGET_DPU, 0x40c0,
                             RK3588_RKNN_MATMUL_M * 32);
    memset(weights, 1, sizeof(weights));
    memset(output, 0xa5, sizeof(output));

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                   commands, sizeof(commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                   input, sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR,
                   weights, sizeof(weights));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                   output, sizeof(output));
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));

    for (unsigned int index = 0; index < ARRAY_SIZE(output); index++) {
        g_assert_cmpint(output[index], ==, 30);
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);

}

static void rk3588_test_rknpu_matmul_int8_out_cvt(bool qd_enabled)
{
    static const struct {
        int8_t input;
        int8_t weight;
        int32_t offset;
        uint16_t scale;
        uint8_t shift;
        int8_t expected;
    } cases[] = {
        { 1, 1, 0, 1, 0, 1 },
        { 2, 4, 0, 1, 2, 2 },
        { 1, 2, 10, 1, 0, 12 },
        { 3, 64, 0, 1, 0, 127 },
        { 1, 1, -200, 1, 0, -128 },
        { 1, 2, 0, 3, 1, 3 },
    };
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int test = 0; test < ARRAY_SIZE(cases); test++) {
        uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
        int8_t input[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_K] = { 0 };
        int8_t weights[RK3588_RKNN_MATMUL_N * RK3588_RKNN_MATMUL_K] = { 0 };
        int8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N];

        if (test) {
            rk3588_rknpu_reset_fixture(qts);
        }
        rk3588_rknn_prepare_matmul(qts, true, 0xa5);
        rk3588_rknn_make_matmul_regcmd(commands, true);
        rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                 RKNN_REGCMD_TARGET_CORE,
                                 RKNN_CORE_MISC_CFG, qd_enabled);
        rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                 RKNN_REGCMD_TARGET_DPU,
                                 RKNN_DPU_DATA_FORMAT, 0);
        rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                 RKNN_REGCMD_TARGET_DPU, 0x4050, 0x124);
        rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                 RKNN_REGCMD_TARGET_DPU, 0x4060, 0x52);
        rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                 RKNN_REGCMD_TARGET_DPU, 0x4080,
                                 cases[test].offset);
        rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                 RKNN_REGCMD_TARGET_DPU, 0x4084,
                                 cases[test].scale);
        rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                 RKNN_REGCMD_TARGET_DPU, 0x4088,
                                 cases[test].shift);
        rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                 RKNN_REGCMD_TARGET_DPU, 0x40c0,
                                 (RK3588_RKNN_MATMUL_M * 2) << 4);

        for (unsigned int row = 0; row < RK3588_RKNN_MATMUL_M; row++) {
            input[rk3588_rknn_feature_index(
                RK3588_RKNN_MATMUL_K, RK3588_RKNN_MATMUL_M, 16, 0,
                row)] = cases[test].input;
        }
        for (unsigned int channel = 0; channel < RK3588_RKNN_MATMUL_N;
             channel++) {
            weights[rk3588_rknn_weight_index(channel, 0)] =
                cases[test].weight;
        }
        memset(output, 0xa5, sizeof(output));

        qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                       commands, sizeof(commands));
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                       input, sizeof(input));
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR,
                       weights, sizeof(weights));
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                       output, sizeof(output));

        rk3588_rknn_run_matmul(qts);
        qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                      output, sizeof(output));

        for (unsigned int row = 0; row < RK3588_RKNN_MATMUL_M; row++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_MATMUL_N; channel++) {
                size_t index = rk3588_rknn_feature_index(
                    RK3588_RKNN_MATMUL_N, RK3588_RKNN_MATMUL_M, 16,
                    channel, row);

                g_assert_cmpint(output[index], ==, cases[test].expected);
            }
        }
        rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_SUCCESS,
                                 RKNN_PIPELINE_BANK1_INTERRUPT);
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_matmul_int8_out_cvt_qd_disabled(void)
{
    rk3588_test_rknpu_matmul_int8_out_cvt(false);
}

static void test_rk3588_rknpu_matmul_int8_out_cvt_qd_enabled(void)
{
    rk3588_test_rknpu_matmul_int8_out_cvt(true);
}

static size_t rk3588_rknn_fp16_weight_index(unsigned int output,
                                            unsigned int channel)
{
    return (((output / 16) * 2 + channel / 32) * 16 + output % 16) * 32 +
           channel % 32;
}

static void test_rk3588_rknpu_matmul_fp16_weight_groups(void)
{
    enum {
        HEIGHT = 8,
        INPUT_CHANNELS = 64,
        OUTPUT_CHANNELS = 17,
        OUTPUT_STORAGE_CHANNELS = 24,
        WEIGHT_STORAGE_OUTPUT_CHANNELS = 32,
    };
    uint16_t input[HEIGHT * INPUT_CHANNELS] = { 0 };
    uint16_t weights[WEIGHT_STORAGE_OUTPUT_CHANNELS * INPUT_CHANNELS] = { 0 };
    uint16_t output[HEIGHT * OUTPUT_STORAGE_CHANNELS] = { 0 };
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_fp16_matmul_regcmd(commands, 2);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x1024,
                                  (INPUT_CHANNELS - 1) << 16 | INPUT_CHANNELS);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x1030,
                                  WEIGHT_STORAGE_OUTPUT_CHANNELS *
                                  INPUT_CHANNELS * 2);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x1034,
                                  INPUT_CHANNELS * 2);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x1038,
                                  (1 << 24) | (1 << 16) | OUTPUT_CHANNELS);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x1088,
                                  INPUT_CHANNELS);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0801, 0x3018,
                                  OUTPUT_CHANNELS - 1);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x1001, 0x403c,
                                  ((OUTPUT_CHANNELS - 1) << 16) |
                                  (OUTPUT_CHANNELS - 1));
    rk3588_rknn_patch_fp16_regcmd(commands, 0x1001, 0x4058,
                                  OUTPUT_CHANNELS - 1);

    for (unsigned int row = 0; row < HEIGHT; row++) {
        for (unsigned int channel = 0; channel < INPUT_CHANNELS; channel++) {
            size_t index = rk3588_rknn_feature_index(
                INPUT_CHANNELS, HEIGHT, 8, channel, row);

            input[index] = cpu_to_le16(
                rk3588_rknn_half_from_uint32(row + 1));
        }
    }
    for (unsigned int output_channel = 0;
         output_channel < OUTPUT_CHANNELS; output_channel++) {
        for (unsigned int channel = 0; channel < INPUT_CHANNELS; channel++) {
            size_t index = rk3588_rknn_fp16_weight_index(
                output_channel, channel);

            weights[index] = cpu_to_le16(
                rk3588_rknn_half_from_uint32(output_channel + 1));
        }
    }

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                   commands, sizeof(commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                   input, sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR,
                   weights, sizeof(weights));
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));
    for (unsigned int row = 0; row < HEIGHT; row++) {
        for (unsigned int output_channel = 0;
             output_channel < OUTPUT_CHANNELS; output_channel++) {
            size_t index = (output_channel / 8) * HEIGHT * 8 + row * 8 +
                           output_channel % 8;
            uint32_t expected = (row + 1) * (output_channel + 1) *
                                INPUT_CHANNELS;
            g_assert_cmphex(lduw_le_p(output + index), ==,
                            rk3588_rknn_half_from_uint32(expected));
        }
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_matmul_fp16_control_mutations(void)
{
    static const struct {
        const char *name;
        uint32_t target;
        uint32_t reg;
        uint32_t value;
    } cases[] = {
        { "output-conversion", 0x1001, 0x4084, 1 },
        { "output-shift", 0x1001, 0x4088, 1 },
        { "output-type", 0x1001, 0x4088, 1U << 31 },
        { "output-round", 0x1001, 0x4088, 1U << 30 },
        { "output-stride", 0x1001, 0x4024, 0 },
        { "pad-value", 0x0201, 0x1184, 0x3c00 },
        { "cna-convolution-mode", 0x0201, 0x100c, 0x121 },
        { "core-process-precision", 0x0801, 0x3010, 0x101 },
    };
    uint16_t input[8 * 32];
    uint16_t weights[16 * 32];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_fp16_matmul_data(input, weights);
    for (unsigned int case_index = 0; case_index < ARRAY_SIZE(cases);
         case_index++) {
        uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];

        if (case_index) {
            rk3588_rknpu_reset_fixture(qts);
        }
        g_test_message("FP16 mutation: %s", cases[case_index].name);
        rk3588_rknn_make_fp16_matmul_regcmd(commands, 2);
        rk3588_rknn_patch_fp16_regcmd(
            commands, cases[case_index].target, cases[case_index].reg,
            cases[case_index].value);
        rk3588_rknn_prepare_fp16_matmul(qts, commands, input, weights);
        rk3588_rknn_run_matmul(qts);
        g_assert_cmphex(qtest_readb(
            qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800), ==, 0xa5);
        rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_fp16_compact_large_broadcast(void)
{
    enum {
        VALID_OUTPUTS = 1001,
        STORAGE_OUTPUTS = 1008,
    };
    const uint32_t output_iova = 0x10006000;
    const uint64_t output_addr = RK3588_RAM_BASE + 0x30000;
    uint16_t input[8] = { cpu_to_le16(0x63d4) };
    uint16_t weights[VALID_OUTPUTS * 8] = { 0 };
    uint16_t output[STORAGE_OUTPUTS];
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int output_channel = 0;
         output_channel < VALID_OUTPUTS; output_channel++) {
        weights[output_channel * 8] = cpu_to_le16(0x3c00);
    }
    memset(output, 0xa5, sizeof(output));
    rk3588_rknn_make_fp16_matmul_regcmd(commands, 2);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x100c,
                                  0x60008120);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x1020,
                                  (1U << 16) | 1);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x1024, 8);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x1028, 1);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x102c, 1);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x1030,
                                  sizeof(weights));
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x1034, 16);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x1038,
                                  (1U << 24) | (1U << 16) |
                                  VALID_OUTPUTS);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x104c, 0xb);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x107c, 1);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x1080, 0);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x1084,
                                  (1U << 16) | 1);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0201, 0x1088, 8);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0801, 0x3010, 0x200);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0801, 0x3014, 0);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x0801, 0x3018,
                                  STORAGE_OUTPUTS - 1);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x1001, 0x400c, 0x1e4);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x1001, 0x4020, output_iova);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x1001, 0x4024, 0);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x1001, 0x4030, 0);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x1001, 0x4034, 0);
    rk3588_rknn_patch_fp16_regcmd(
        commands, 0x1001, 0x403c,
        ((VALID_OUTPUTS - 1) << 16) | (STORAGE_OUTPUTS - 1));
    rk3588_rknn_patch_fp16_regcmd(commands, 0x1001, 0x4050, 0x126);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x1001, 0x4058,
                                  STORAGE_OUTPUTS - 1);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x1001, 0x405c, 0);
    rk3588_rknn_patch_fp16_regcmd(commands, 0x1001, 0x40c0, 0x20);

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    for (unsigned int page = 0; page < 4; page++) {
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + (2 + page) * 4,
                     (RK3588_RKNN_MATMUL_WEIGHT_ADDR + page * 0x1000) |
                     RK_IOMMU_PTE_RW);
    }
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 6 * 4,
                 output_addr | RK_IOMMU_PTE_RW);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                   commands, sizeof(commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                   input, sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR,
                   weights, sizeof(weights));
    qtest_memwrite(qts, output_addr, output, sizeof(output));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(
                     RK3588_RKNN_MATMUL_COMMANDS));
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, output_addr, output, sizeof(output));
    for (unsigned int index = 0; index < ARRAY_SIZE(output); index++) {
        g_assert_cmphex(le16_to_cpu(output[index]), ==,
                        index < VALID_OUTPUTS ? 0x63d4 : 0);
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_rdma_fp16_ew_data_mode(void)
{
    enum { CHANNELS = 32 };
    static const struct {
        uint32_t ew_cfg;
        uint32_t rdma_feature;
        uint32_t out_cvt_scale;
        uint32_t out_cvt_shift;
        uint16_t first_expected;
        uint16_t second_expected;
        bool notched;
    } cases[] = {
        { 0x108402c0, 0x00017849, 0x00010001, 0, 0x0000, 0x4000 },
        { 0x108303c0, 0x00017841, 0x00000001, 0, 0x3c00, 0x4000 },
        { 0x108202c0, 0x00017849, 0x00010001, 0, 0x4800, 0x4600,
          true },
    };

    for (unsigned int case_index = 0;
         case_index < ARRAY_SIZE(cases); case_index++) {
        uint16_t input[CHANNELS * 2] = { 0 };
        uint16_t ew[CHANNELS] = { 0 };
        uint16_t output[CHANNELS];
        uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
        QTestState *qts = rk3588_qtest_start_rknpu();

        for (unsigned int channel = 0; channel < CHANNELS; channel++) {
            unsigned int surface = channel / 8;
            unsigned int lane = channel % 8;
            unsigned int input_index = channel;

            if (cases[case_index].notched) {
                input_index = surface * 16 + lane;
            }

            input[input_index] = cpu_to_le16(0x4400);
            output[channel] = cpu_to_le16(0xa5a5);
        }
        for (unsigned int channel = 0; channel < CHANNELS / 2; channel++) {
            unsigned int surface = channel / 8;
            unsigned int lane = channel % 8;
            unsigned int ew_index = channel;

            if (cases[case_index].notched) {
                ew_index = surface * 16 + lane;
            }

            ew[ew_index] = cpu_to_le16(0x4000);
        }
        rk3588_rknn_make_dpu_rdma_fp16_lut_regcmd(commands);
#define PATCH_EW_MODE(_target, _reg, _value) \
        rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands), \
                                 (_target), (_reg), (_value))
        uint32_t surface_stride = 0x10;
        uint32_t surface_notch = 0;

        if (cases[case_index].notched) {
            surface_stride = 0x20;
            surface_notch = 0x10;
        }

        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU, 0x4020,
                      RK3588_RKNN_MATMUL_OUTPUT_IOVA);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU, 0x4024, 0x10);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU, 0x403c,
                      ((CHANNELS - 1) << 16) | (CHANNELS - 1));
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU, 0x4058, CHANNELS - 1);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU, 0x405c, 0);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU, 0x4060, 0x53);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU, 0x4070,
                      cases[case_index].ew_cfg);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU, 0x4080, 0);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU, 0x4084,
                      cases[case_index].out_cvt_scale);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU, 0x4088,
                      cases[case_index].out_cvt_shift);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU, 0x40c0, 0x10);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x500c, 0);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5010, 0);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5014, CHANNELS - 1);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5018,
                      RK3588_RKNN_MATMUL_INPUT_IOVA);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5034, 0x40000008);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5038,
                      RK3588_RKNN_MATMUL_WEIGHT_IOVA);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5040,
                      surface_stride);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5044,
                      cases[case_index].rdma_feature);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x504c,
                      surface_notch);
        PATCH_EW_MODE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x506c,
                      surface_notch);
#undef PATCH_EW_MODE
        rk3588_rknn_prepare_matmul(qts, true, 0xa5);
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                       commands, sizeof(commands));
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                       input, sizeof(input));
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR,
                       ew, sizeof(ew));
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                       output, sizeof(output));
        qtest_writel(qts, RK3588_RKNN0_PC_BASE +
                     RKNN_PC_REGISTER_AMOUNTS,
                     rk3588_rknn_register_amount(
                         RK3588_RKNN_DPU_RDMA_FP16_COMMANDS));
        rk3588_rknn_run_matmul(qts);
        qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                      output, sizeof(output));

        for (unsigned int channel = 0; channel < CHANNELS; channel++) {
            uint16_t expected = cases[case_index].second_expected;

            if (!((channel / 8) & 1)) {
                expected = cases[case_index].first_expected;
            }
            g_assert_cmphex(le16_to_cpu(output[channel]), ==, expected);
        }
        g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                        RKNN_TASK_STATUS_SUCCESS);
        qtest_quit(qts);
    }
}

static void test_rk3588_rknpu_dpu_rdma_fp16_ew_controls(void)
{
    static const struct {
        uint32_t target;
        uint32_t reg;
        uint32_t value;
        bool spatial2;
    } cases[] = {
        { RKNN_REGCMD_TARGET_DPU, 0x4070, 0x008502c0 },
        { RKNN_REGCMD_TARGET_DPU_RDMA, 0x5044, 0x00017848 },
        { RKNN_REGCMD_TARGET_DPU_RDMA, 0x5034, 0x4 },
        { RKNN_REGCMD_TARGET_DPU, 0x4074, 1 },
        { RKNN_REGCMD_TARGET_DPU, 0x4078, 2 },
        { RKNN_REGCMD_TARGET_DPU, 0x4034, 1U << 22 },
        { RKNN_REGCMD_TARGET_DPU, 0x4050, 3 },
        { RKNN_REGCMD_TARGET_DPU, 0x4054, 1 },
        { RKNN_REGCMD_TARGET_DPU, 0x40c0, 0x20 },
        { RKNN_REGCMD_TARGET_DPU, 0x40c0, 0x10, true },
    };
    static const uint16_t input[] = {
        0x3c00, 0x4000, 0x4200, 0x4400,
        0x4500, 0x4600, 0x4700, 0x4800,
    };
    uint16_t operand[8];
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int channel = 0; channel < ARRAY_SIZE(operand); channel++) {
        operand[channel] = cpu_to_le16(0x3e00);
    }

    for (unsigned int case_index = 0; case_index < ARRAY_SIZE(cases);
         case_index++) {
        uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
        uint8_t output[16];

        if (case_index) {
            rk3588_rknpu_reset_fixture(qts);
        }
        rk3588_rknn_make_dpu_rdma_fp16_lut_regcmd(commands);
#define PATCH_EW_BASE(_target, _reg, _value) \
        rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands), \
                                 (_target), (_reg), (_value))
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x4020,
                      RK3588_RKNN_MATMUL_OUTPUT_IOVA);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x4024, 0x10);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x403c,
                      (7U << 16) | 15);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x4058, 15);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x405c, 0);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x4060, 0x53);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x4070, 0x008402c0);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x4074, 0);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x4078, 1);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x4080, 0);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x4084, 0x00010001);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x4088, 0);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x40c0, 0x10);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x500c, 0);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5010, 0);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5014, 15);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5018,
                      RK3588_RKNN_MATMUL_INPUT_IOVA);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5034, 8);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5038,
                      RK3588_RKNN_MATMUL_OUTPUT_IOVA + 0x810);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5040, 0x10);
        PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5044, 0x00017849);
        if (cases[case_index].spatial2) {
            PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x4024, 0x20);
            PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x4030, 1);
            PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x405c, 1);
            PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU, 0x40c0, 0x20);
            PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x500c, 1);
            PATCH_EW_BASE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5040, 0x20);
        }
#undef PATCH_EW_BASE
        rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                 cases[case_index].target,
                                 cases[case_index].reg,
                                 cases[case_index].value);
        rk3588_rknn_prepare_matmul(qts, true, 0xa5);
        memset(output, 0xa5, sizeof(output));
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                       commands, sizeof(commands));
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                       input, sizeof(input));
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                       output, sizeof(output));
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR1 + 0x10,
                       operand, sizeof(operand));
        qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                     rk3588_rknn_register_amount(
                         RK3588_RKNN_DPU_RDMA_FP16_COMMANDS));
        rk3588_rknn_run_matmul(qts);
        qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                      output, sizeof(output));
        for (unsigned int index = 0; index < ARRAY_SIZE(output); index++) {
            g_assert_cmphex(output[index], ==, 0xa5);
        }
        g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                        RKNN_TASK_STATUS_FETCH_ERROR | 1);
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_rdma_fp16_divide(void)
{
    enum {
        VALID_CHANNELS = 1001,
        STORAGE_CHANNELS = 1008,
    };
    static const struct {
        uint16_t input;
        uint16_t operand;
        uint16_t expected;
    } boundary[] = {
        { 0x1ae0, 0x45bb, 0x10cc },
        { 0x1d50, 0x45bb, 0x136a },
        { 0x2760, 0x3d18, 0x25ca },
        { 0x1560, 0x3d18, 0x1438 },
        { 0x0f80, 0x3d19, 0x0de2 },
        { 0x1140, 0x3d19, 0x101e },
    };
    uint16_t input[STORAGE_CHANNELS];
    uint16_t operand[STORAGE_CHANNELS];
    uint16_t output[STORAGE_CHANNELS];
    uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int channel = 0; channel < STORAGE_CHANNELS; channel++) {
        if (channel < ARRAY_SIZE(boundary)) {
            input[channel] = cpu_to_le16(boundary[channel].input);
            operand[channel] = cpu_to_le16(boundary[channel].operand);
        } else {
            input[channel] = cpu_to_le16(0x4000);
            operand[channel] = cpu_to_le16(
                channel & 1 ? 0x4000 : 0x3c00);
        }
        output[channel] = cpu_to_le16(0xa5a5);
    }
    rk3588_rknn_make_dpu_rdma_fp16_lut_regcmd(commands);
#define PATCH_DIV(_target, _reg, _value) \
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands), (_target), \
                             (_reg), (_value))
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU, 0x4020,
              RK3588_RKNN_MATMUL_OUTPUT_IOVA);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU, 0x4024, 0x10);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU, 0x403c,
              ((VALID_CHANNELS - 1) << 16) | (STORAGE_CHANNELS - 1));
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU, 0x4058, STORAGE_CHANNELS - 1);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU, 0x405c, 0);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU, 0x4060, 0x53);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU, 0x4070, 0x008303c0);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU, 0x4074, 0x12345678);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU, 0x4078, 0x87654321);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU, 0x407c, 0x3c00);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU, 0x4080, 0);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU, 0x4084, 1);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU, 0x4088, 0);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU, 0x40c0, 0x10);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU_RDMA, 0x500c, 0);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5010, 0);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5014,
              STORAGE_CHANNELS - 1);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5018,
              RK3588_RKNN_MATMUL_INPUT_IOVA);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5034, 8);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5038,
              RK3588_RKNN_MATMUL_WEIGHT_IOVA);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5040, 0x10);
    PATCH_DIV(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5044, 0x00017841);
#undef PATCH_DIV
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                   commands, sizeof(commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                   input, sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR,
                   operand, sizeof(operand));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                   output, sizeof(output));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(
                     RK3588_RKNN_DPU_RDMA_FP16_COMMANDS));
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));
    for (unsigned int channel = 0; channel < ARRAY_SIZE(output); channel++) {
        uint16_t expected;

        if (channel >= VALID_CHANNELS) {
            expected = 0;
        } else if (channel < ARRAY_SIZE(boundary)) {
            expected = boundary[channel].expected;
        } else {
            expected = channel & 1 ? 0x3c00 : 0x4000;
        }

        g_assert_cmphex(le16_to_cpu(output[channel]), ==, expected);
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

enum Rk3588RKNNRaw16ReshapeLayout {
    RK3588_RKNN_RAW16_RESHAPE_CHANNEL_MAJOR,
    RK3588_RKNN_RAW16_RESHAPE_WDMA_TILED,
    RK3588_RKNN_RAW16_RESHAPE_SPATIAL_MAJOR,
};

static void rk3588_rknn_make_dpu_rdma_raw16_reshape_regcmd(
    uint64_t commands[], unsigned int width, unsigned int height,
    unsigned int channels, unsigned int wdma_width,
    unsigned int wdma_height, enum Rk3588RKNNRaw16ReshapeLayout layout,
    bool invalid_size_c)
{
    unsigned int spatial = width * height;
    unsigned int surfaces = DIV_ROUND_UP(channels, 8);
    unsigned int surface_length = spatial * surfaces;
    unsigned int wdma_size_c =
        surface_length / (wdma_width * wdma_height * 8) - 1;
    bool wdma_tiled = layout == RK3588_RKNN_RAW16_RESHAPE_WDMA_TILED;
    bool spatial_major = layout == RK3588_RKNN_RAW16_RESHAPE_SPATIAL_MAJOR;

    wdma_size_c += invalid_size_c;

    rk3588_rknn_make_dpu_rdma_fp16_lut_regcmd(commands);
#define PATCH_RESHAPE(_target, _reg, _value) \
    rk3588_rknn_patch_regcmd( \
        commands, RK3588_RKNN_DPU_RDMA_FP16_COMMANDS, (_target), \
        (_reg), (_value))
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x400c,
                  0x1e5 | (surface_length << 9));
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4010, 0x24000001);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4014, 0);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4020,
                  RK3588_RKNN_MATMUL_OUTPUT_IOVA);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4024,
                  (spatial_major ? wdma_size_c + 1 : wdma_width) << 4);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4030, width - 1);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4034, height - 1);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4038,
                  spatial_major ?
                      (((wdma_size_c + 1) * 8 - wdma_width) * 0x10001) :
                  wdma_tiled ? 7 | (7 << 16) : wdma_width == 1 ?
                      ((channels - 1) << 16) | (channels - 1) : 0);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x403c,
                  ((channels - 1) << 16) | (channels - 1));
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4040, 0x00040053);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4050, 0x080007fe);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4054, 0);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4058,
                  (layout == RK3588_RKNN_RAW16_RESHAPE_CHANNEL_MAJOR) << 27 |
                  (wdma_size_c << 16) | (channels - 1));
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x405c,
                  ((wdma_height - 1) << 16) | (wdma_width - 1));
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4060, 0x00020053);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4070, 0x00840383);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4074, 0);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4078, 1);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4080, 0);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4084, 1);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4088, 0);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x40c0,
                  wdma_width *
                  (spatial_major ? 1 :
                   (wdma_tiled ? wdma_height : 1) * 8) * 16);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x500c, width - 1);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5010, height - 1);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5014, channels - 1);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5018,
                  RK3588_RKNN_MATMUL_INPUT_IOVA);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x501c, 0);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5034, 1);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5040, 0);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5044, 0xf821);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5048, 0);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x504c, 0);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5064, 0);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x506c, 0);
#undef PATCH_RESHAPE
}

static void rk3588_rknn_test_dpu_rdma_raw16_reshape(
    unsigned int width, unsigned int height, unsigned int channels,
    unsigned int wdma_width, unsigned int wdma_height,
    enum Rk3588RKNNRaw16ReshapeLayout layout,
    bool invalid_wdma_product)
{
    size_t values = width * height * channels;
    g_autofree uint16_t *input = g_new(uint16_t, values);
    g_autofree uint16_t *output = g_new(uint16_t, values);
    size_t output_bytes = values * sizeof(*output);
    size_t output_first_page = MIN(output_bytes, 0x800);
    uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int row = 0; row < height; row++) {
        for (unsigned int column = 0; column < width; column++) {
            for (unsigned int channel = 0; channel < channels; channel++) {
                size_t index = rk3588_rknn_spatial_feature_index(
                    width, height, 8, channel, row, column);

                input[index] = cpu_to_le16(
                    (row * width + column) * channels + channel + 1);
            }
        }
    }
    memset(output, 0xa5, values * sizeof(*output));
    rk3588_rknn_make_dpu_rdma_raw16_reshape_regcmd(
        commands, width, height, channels, wdma_width,
        wdma_height, layout, invalid_wdma_product);
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                   commands, sizeof(commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                   input, values * sizeof(*input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                   output, output_first_page);
    if (output_bytes > output_first_page) {
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR1,
                       (uint8_t *)output + output_first_page,
                       output_bytes - output_first_page);
    }
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(
                     RK3588_RKNN_DPU_RDMA_FP16_COMMANDS));
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, output_first_page);
    if (output_bytes > output_first_page) {
        qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR1,
                      (uint8_t *)output + output_first_page,
                      output_bytes - output_first_page);
    }

    if (invalid_wdma_product) {
        for (unsigned int index = 0; index < values; index++) {
            g_assert_cmphex(le16_to_cpu(output[index]), ==, 0xa5a5);
        }
        g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                        RKNN_TASK_STATUS_FETCH_ERROR | 1);
    } else {
        for (unsigned int index = 0; index < values; index++) {
            g_assert_cmphex(le16_to_cpu(output[index]), ==,
                            le16_to_cpu(input[index]));
        }
        g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                        RKNN_TASK_STATUS_SUCCESS);
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_rdma_raw16_reshape(void)
{
    g_test_message("raw16 reshape: channel-major WDMA 1x2x16");
    rk3588_rknn_test_dpu_rdma_raw16_reshape(1, 16, 16, 1, 2,
        RK3588_RKNN_RAW16_RESHAPE_CHANNEL_MAJOR, false);
    g_test_message("raw16 reshape: tiled WDMA 1x2x16");
    rk3588_rknn_test_dpu_rdma_raw16_reshape(1, 16, 16, 1, 2,
        RK3588_RKNN_RAW16_RESHAPE_WDMA_TILED, false);
    g_test_message("raw16 reshape: spatial-major WDMA 1x2x8");
    rk3588_rknn_test_dpu_rdma_raw16_reshape(16, 16, 8, 1, 2,
        RK3588_RKNN_RAW16_RESHAPE_SPATIAL_MAJOR, false);
    g_test_message("raw16 reshape: WDMA 7x1x16");
    rk3588_rknn_test_dpu_rdma_raw16_reshape(8, 7, 16, 7, 1,
        RK3588_RKNN_RAW16_RESHAPE_CHANNEL_MAJOR, false);
    g_test_message("raw16 reshape: invalid WDMA product");
    rk3588_rknn_test_dpu_rdma_raw16_reshape(1, 16, 16, 1, 2,
        RK3588_RKNN_RAW16_RESHAPE_CHANNEL_MAJOR, true);
}

static void rk3588_rknn_make_dpu_rdma_raw16_compact_u8_regcmd(
    uint64_t commands[])
{
    enum {
        WIDTH = 4,
        HEIGHT = 3,
        CHANNELS = 8,
        SPATIAL = WIDTH * HEIGHT,
    };

    rk3588_rknn_make_dpu_rdma_fp16_lut_regcmd(commands);
#define PATCH_COMPACT_U8(_target, _reg, _value)                      \
    rk3588_rknn_patch_regcmd(                                        \
        commands, RK3588_RKNN_DPU_RDMA_FP16_COMMANDS,                \
        (_target), (_reg), (_value))
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x400c,
                     0x1e5 | (1U << 25) | (2U << 26) |
                     (DIV_ROUND_UP(SPATIAL, 16) << 9));
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4010, 0x24000001);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4014, 0);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4020,
                     RK3588_RKNN_MATMUL_OUTPUT_IOVA);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4024, 0);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4030, WIDTH - 1);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4034, HEIGHT - 1);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x403c,
                     ((CHANNELS - 1) << 16) | (CHANNELS - 1));
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4040, 0x53);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4050, 2);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4058, CHANNELS - 1);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x405c,
                     ((HEIGHT - 1) << 16) | (WIDTH - 1));
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4060, 0x53);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4070, 0x383);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4074, 0);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4078, 1);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4080, 0);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4084, 1);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x4088, 0);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU, 0x40c0, 0);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU_RDMA, 0x500c, WIDTH - 1);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5010, HEIGHT - 1);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5014, CHANNELS - 1);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5018,
                     RK3588_RKNN_MATMUL_RDMA_IOVA);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU_RDMA, 0x501c, 0);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5034, 1);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5038, 0);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5040, 0);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5044, 0xf821);
    PATCH_COMPACT_U8(RKNN_REGCMD_TARGET_DPU_RDMA, 0x506c, 0);
#undef PATCH_COMPACT_U8
}

static void test_rk3588_rknpu_dpu_rdma_raw16_compact_u8(void)
{
    enum {
        WIDTH = 4,
        HEIGHT = 3,
        CHANNELS = 8,
        SPATIAL = WIDTH * HEIGHT,
    };
    uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
    uint16_t input[SPATIAL * CHANNELS];
    uint8_t output[SPATIAL + 1];
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int spatial = 0; spatial < SPATIAL; spatial++) {
        for (unsigned int lane = 0; lane < CHANNELS; lane++) {
            uint16_t value = lane ? 0x8000 + lane * 0x100 + spatial :
                (spatial == 0 ? 0x1234 : 0x0100 + spatial * 17);

            input[spatial * CHANNELS + lane] = cpu_to_le16(value);
        }
    }
    rk3588_rknn_make_dpu_rdma_raw16_compact_u8_regcmd(commands);
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                   commands, sizeof(commands));
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 5 * 4,
                 RK3588_RKNN_MATMUL_RDMA_ADDR | RK_IOMMU_PTE_RW);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_RDMA_ADDR,
                   input, sizeof(input));
    qtest_memset(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                 0xa5, sizeof(output));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(ARRAY_SIZE(commands)));
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));
    for (unsigned int spatial = 0; spatial < SPATIAL; spatial++) {
        g_assert_cmphex(output[spatial], ==,
                        le16_to_cpu(input[spatial * CHANNELS]) & 0xff);
    }
    g_assert_cmphex(output[SPATIAL], ==, 0xa5);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void rk3588_rknn_start_matmul(QTestState *qts)
{
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_TASK_CON, 0x00003001);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_OPERATION_ENABLE,
                 RKNN_PC_OPERATION_ENABLE_OP_EN);
}

static void rk3588_cru_rknpu_reset_pulse(QTestState *qts,
                                          uint32_t offset, uint32_t bit)
{
    qtest_writel(qts, RK3588_CRU_BASE + offset, (bit << 16) | bit);
    qtest_writel(qts, RK3588_CRU_BASE + offset, bit << 16);
}

static void rk3588_rknn_wait_idle(QTestState *qts)
{
    gint64 deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    while (qtest_qom_get_bool(qts, "/machine/rknn0",
                              "x-execution-active")) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        qtest_clock_step(qts, 1);
        g_usleep(100);
    }
}

static void rk3588_rknn_wait_core_idle(QTestState *qts, const char *path)
{
    gint64 deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    while (qtest_qom_get_bool(qts, path, "x-execution-active")) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        qtest_clock_step(qts, 1);
        g_usleep(100);
    }
}

static void rk3588_rknn_run_matmul(QTestState *qts)
{
    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
}

static void rk3588_rknn_assert_pc(QTestState *qts, uint32_t task_status,
                                  uint32_t raw_status)
{
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    task_status);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    raw_status);
}

static void test_rk3588_rknpu_dpu_rdma_int8_to_fp16(void)
{
    uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
    int8_t input[RK3588_RKNN_DPU_RDMA_FP16_INPUT_BYTES];
    uint8_t output[RK3588_RKNN_DPU_RDMA_FP16_OUTPUT_BYTES];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_dpu_rdma_fp16_regcmd(commands);
    rk3588_rknn_patch_regcmd(
        commands, ARRAY_SIZE(commands), RKNN_REGCMD_TARGET_DPU,
        0x4040, 0x00040053);
    rk3588_rknn_patch_regcmd(
        commands, ARRAY_SIZE(commands), RKNN_REGCMD_TARGET_DPU,
        0x4060, 0x00020053);
    rk3588_rknn_patch_regcmd(
        commands, ARRAY_SIZE(commands), RKNN_REGCMD_TARGET_DPU,
        0x4070, 0x00840383);
    rk3588_rknn_patch_regcmd(
        commands, ARRAY_SIZE(commands), RKNN_REGCMD_TARGET_DPU_RDMA,
        0x5040, 0x1230);
    rk3588_rknn_patch_regcmd(
        commands, ARRAY_SIZE(commands), RKNN_REGCMD_TARGET_DPU_RDMA,
        0x506c, 0x4560);
    rk3588_rknn_make_dpu_rdma_fp16_input(input);
    rk3588_rknn_prepare_dpu_rdma_fp16(qts, commands, input, 0xa5);
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));
    for (unsigned int row = 0;
         row < RK3588_RKNN_DPU_RDMA_FP16_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_DPU_RDMA_FP16_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_DPU_RDMA_FP16_CHANNELS; channel++) {
                size_t input_index = rk3588_rknn_spatial_feature_index(
                    RK3588_RKNN_DPU_RDMA_FP16_WIDTH,
                    RK3588_RKNN_DPU_RDMA_FP16_HEIGHT, 16,
                    channel, row, column);
                size_t output_index = rk3588_rknn_spatial_feature_index(
                    RK3588_RKNN_DPU_RDMA_FP16_WIDTH,
                    RK3588_RKNN_DPU_RDMA_FP16_HEIGHT, 8,
                    channel, row, column);
                uint16_t expected;

                if (channel < RK3588_RKNN_DPU_RDMA_FP16_VALID_CHANNELS) {
                    expected = rk3588_rknn_half_from_int8(
                        input[input_index]);
                } else if (channel < ROUND_UP(
                               RK3588_RKNN_DPU_RDMA_FP16_VALID_CHANNELS,
                               8)) {
                    expected = 0;
                } else {
                    expected = 0xa5a5;
                }

                g_assert_cmphex(lduw_le_p(output + output_index * 2), ==,
                                expected);
            }
        }
    }
    rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_SUCCESS,
                             RKNN_DPU_INTERRUPT_BITS);

    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_rdma_int8_unary(void)
{
    enum {
        INPUT_BYTES = RK3588_RKNN_DPU_RDMA_FP16_INPUT_BYTES,
    };
    uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
    int8_t input[INPUT_BYTES];
    int8_t output[INPUT_BYTES];
    size_t command_count =
        rk3588_rknn_make_dpu_rdma_int8_unary_regcmd(commands);
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int index = 0; index < ARRAY_SIZE(input); index++) {
        input[index] = index * 37 + 11;
    }
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                   commands, command_count * sizeof(*commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                   input, sizeof(input));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(command_count));
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));
    for (unsigned int row = 0;
         row < RK3588_RKNN_DPU_RDMA_FP16_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_DPU_RDMA_FP16_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_DPU_RDMA_FP16_CHANNELS; channel++) {
                size_t index = rk3588_rknn_spatial_feature_index(
                    RK3588_RKNN_DPU_RDMA_FP16_WIDTH,
                    RK3588_RKNN_DPU_RDMA_FP16_HEIGHT, 16,
                    channel, row, column);
                int expected = channel <
                    RK3588_RKNN_DPU_RDMA_FP16_VALID_CHANNELS ?
                    CLAMP(input[index] + 1, INT8_MIN, INT8_MAX) : 0;

                g_assert_cmpint(output[index], ==, expected);
            }
        }
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void rk3588_rknn_make_dpu_rdma_int8_reshape_regcmd(
    uint64_t commands[], unsigned int input_width,
    unsigned int input_height, unsigned int output_width,
    unsigned int output_height, unsigned int channels)
{
    unsigned int surfaces = channels / 16;
    unsigned int input_line_notch = input_width * (surfaces - 1);
    int input_surface_notch =
        -(input_width * input_height +
          input_line_notch * (input_height - 1) - input_width);
    size_t command_count =
        rk3588_rknn_make_dpu_rdma_int8_bypass_regcmd(commands);

#define PATCH_RESHAPE(_target, _reg, _value) \
    rk3588_rknn_patch_regcmd(commands, command_count, (_target), (_reg), \
                             (_value))
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, RKNN_DPU_OFFSET_PEND, 7);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, RKNN_DPU_DST_SURF_STRIDE, 0x10);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, RKNN_DPU_DATA_CUBE_WIDTH,
                  output_width - 1);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, RKNN_DPU_DATA_CUBE_HEIGHT,
                  output_height - 1);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4038,
                  (output_width * (surfaces - 1)) * 0x10001);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, RKNN_DPU_DATA_CUBE_CHANNEL,
                  ((channels - 1) << 16) | (channels - 1));
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x4058,
                  ((surfaces - 1) << 16) | (channels - 1));
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x405c,
                  ((output_height - 1) << 16) | (output_width - 1));
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU, 0x40c0, output_width * 16);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x500c, input_width - 1);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5010, input_height - 1);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5014, channels - 1);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x5048,
                  input_line_notch << 19);
    PATCH_RESHAPE(RKNN_REGCMD_TARGET_DPU_RDMA, 0x504c,
                  ((uint32_t)input_surface_notch & 0x0fffffff) << 4);
#undef PATCH_RESHAPE
}

static void test_rk3588_rknpu_dpu_rdma_int8_spatial_reshape(void)
{
    enum {
        INPUT_WIDTH = 2,
        INPUT_HEIGHT = 4,
        OUTPUT_WIDTH = 4,
        OUTPUT_HEIGHT = 2,
        CHANNELS = 32,
        SURFACES = CHANNELS / 16,
        INPUT_LINE_NOTCH = INPUT_WIDTH * (SURFACES - 1),
        INPUT_SURFACE_NOTCH =
            -(INPUT_WIDTH * INPUT_HEIGHT +
              INPUT_LINE_NOTCH * (INPUT_HEIGHT - 1) - INPUT_WIDTH),
        VALUES = INPUT_WIDTH * INPUT_HEIGHT * CHANNELS,
    };
    uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
    int8_t input[VALUES];
    int8_t output[VALUES];
    size_t command_count = RK3588_RKNN_DPU_RDMA_FP16_COMMANDS;
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_dpu_rdma_int8_reshape_regcmd(
        commands, INPUT_WIDTH, INPUT_HEIGHT, OUTPUT_WIDTH, OUTPUT_HEIGHT,
        CHANNELS);

    memset(input, 0, sizeof(input));
    memset(output, 0xa5, sizeof(output));
    for (unsigned int row = 0; row < INPUT_HEIGHT; row++) {
        for (unsigned int surface = 0; surface < SURFACES; surface++) {
            for (unsigned int column = 0; column < INPUT_WIDTH; column++) {
                for (unsigned int lane = 0; lane < 16; lane++) {
                    unsigned int channel = surface * 16 + lane;
                    size_t index =
                        ((row * SURFACES + surface) * INPUT_WIDTH + column) *
                        16 + lane;

                    input[index] = row * 31 + column * 7 + channel;
                }
            }
        }
    }

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                   commands, command_count * sizeof(*commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                   input, sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                   output, sizeof(output));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(command_count));
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);

    for (unsigned int row = 0; row < OUTPUT_HEIGHT; row++) {
        for (unsigned int surface = 0; surface < SURFACES; surface++) {
            for (unsigned int column = 0; column < OUTPUT_WIDTH; column++) {
                unsigned int spatial = row * OUTPUT_WIDTH + column;
                unsigned int input_row = spatial / INPUT_WIDTH;
                unsigned int input_column = spatial % INPUT_WIDTH;

                for (unsigned int lane = 0; lane < 16; lane++) {
                    size_t input_index =
                        ((input_row * SURFACES + surface) * INPUT_WIDTH +
                         input_column) * 16 + lane;
                    size_t output_index =
                        ((row * SURFACES + surface) * OUTPUT_WIDTH + column) *
                        16 + lane;

                    g_assert_cmphex((uint8_t)output[output_index], ==,
                                    (uint8_t)input[input_index]);
                }
            }
        }
    }
    {
        static const struct {
            const char *name;
            uint32_t target;
            uint32_t reg;
            uint32_t value;
        } cases[] = {
            { "source-line-notch", RKNN_REGCMD_TARGET_DPU_RDMA, 0x5048,
              (INPUT_LINE_NOTCH + 1) << 19 },
            { "source-surface-notch", RKNN_REGCMD_TARGET_DPU_RDMA, 0x504c,
              ((uint32_t)(INPUT_SURFACE_NOTCH + 1) & 0x0fffffff) << 4 },
            { "wdma-surfaces", RKNN_REGCMD_TARGET_DPU, 0x4058,
              CHANNELS - 1 },
            { "output-notch", RKNN_REGCMD_TARGET_DPU, 0x4038,
              (OUTPUT_WIDTH * (SURFACES - 1) + 1) * 0x10001 },
            { "surface-add", RKNN_REGCMD_TARGET_DPU, 0x40c0,
              OUTPUT_WIDTH * 16 + 16 },
        };

        for (unsigned int index = 0; index < ARRAY_SIZE(cases); index++) {
            uint64_t mutated[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];

            rk3588_rknpu_reset_fixture(qts);
            g_test_message("DPU-RDMA INT8 reshape mutation: %s",
                           cases[index].name);
            rk3588_rknn_make_dpu_rdma_int8_reshape_regcmd(
                mutated, INPUT_WIDTH, INPUT_HEIGHT, OUTPUT_WIDTH,
                OUTPUT_HEIGHT, CHANNELS);
            rk3588_rknn_patch_regcmd(
                mutated, ARRAY_SIZE(mutated), cases[index].target,
                cases[index].reg, cases[index].value);
            rk3588_rknn_prepare_matmul(qts, true, 0xa5);
            qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                           mutated, sizeof(mutated));
            qtest_writel(qts, RK3588_RKNN0_PC_BASE +
                         RKNN_PC_REGISTER_AMOUNTS,
                         rk3588_rknn_register_amount(ARRAY_SIZE(mutated)));
            rk3588_rknn_run_matmul(qts);
            g_assert_cmphex(qtest_readb(
                qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800), ==, 0xa5);
            rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
        }
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_rdma_int8_binary(void)
{
    enum {
        INPUT_BYTES = RK3588_RKNN_DPU_RDMA_FP16_INPUT_BYTES,
        EW_SURFACE_ATOMS = RK3588_RKNN_DPU_RDMA_FP16_WIDTH *
                           RK3588_RKNN_DPU_RDMA_FP16_HEIGHT,
        EW_SURFACE_BYTES = EW_SURFACE_ATOMS * 16,
        EW_SURFACE_STRIDE_ATOMS = EW_SURFACE_ATOMS * 2,
        EW_SURFACE_STRIDE_BYTES = EW_SURFACE_STRIDE_ATOMS * 16,
        EW_STORAGE_BYTES = EW_SURFACE_STRIDE_BYTES + EW_SURFACE_BYTES,
        EW_GAPPED_STRIDE_BYTES = 0x3000,
        EW_GAPPED_STRIDE_ATOMS = EW_GAPPED_STRIDE_BYTES / 16,
        EW_GAPPED_PTE_INDEX = 8,
    };
    uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
    int8_t input[INPUT_BYTES];
    int8_t ew_input[INPUT_BYTES];
    int8_t ew_storage[EW_STORAGE_BYTES];
    int8_t output[INPUT_BYTES];
    size_t command_count =
        rk3588_rknn_make_dpu_rdma_int8_binary_regcmd(commands);
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_patch_regcmd(
        commands, command_count, RKNN_REGCMD_TARGET_DPU_RDMA, 0x5040,
        EW_SURFACE_STRIDE_ATOMS << 4);
    rk3588_rknn_patch_regcmd(
        commands, command_count, RKNN_REGCMD_TARGET_DPU_RDMA, 0x506c,
        (EW_SURFACE_STRIDE_ATOMS - EW_SURFACE_ATOMS) << 4);
    memset(ew_storage, 0xa5, sizeof(ew_storage));
    for (unsigned int index = 0; index < ARRAY_SIZE(input); index++) {
        input[index] = index * 29 + 7;
        ew_input[index] = index * 43 - 91;
    }
    memcpy(ew_storage, ew_input, EW_SURFACE_BYTES);
    memcpy(ew_storage + EW_SURFACE_STRIDE_BYTES,
           ew_input + EW_SURFACE_BYTES, EW_SURFACE_BYTES);
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 5 * 4,
                 RK3588_RKNN_MATMUL_RDMA_ADDR | RK_IOMMU_PTE_RW);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                   commands, command_count * sizeof(*commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                   input, sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_RDMA_ADDR,
                   ew_storage, sizeof(ew_storage));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(command_count));
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));
    for (unsigned int row = 0;
         row < RK3588_RKNN_DPU_RDMA_FP16_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_DPU_RDMA_FP16_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_DPU_RDMA_FP16_CHANNELS; channel++) {
                size_t index = rk3588_rknn_spatial_feature_index(
                    RK3588_RKNN_DPU_RDMA_FP16_WIDTH,
                    RK3588_RKNN_DPU_RDMA_FP16_HEIGHT, 16,
                    channel, row, column);
                int expected = channel <
                    RK3588_RKNN_DPU_RDMA_FP16_VALID_CHANNELS ?
                    CLAMP(input[index] + ew_input[index] + 5,
                          INT8_MIN, INT8_MAX) : 0;

                g_assert_cmpint(output[index], ==, expected);
            }
        }
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);

    rk3588_rknn_patch_regcmd(
        commands, command_count, RKNN_REGCMD_TARGET_DPU_RDMA, 0x5040,
        EW_GAPPED_STRIDE_ATOMS << 4);
    rk3588_rknn_patch_regcmd(
        commands, command_count, RKNN_REGCMD_TARGET_DPU_RDMA, 0x506c,
        (EW_GAPPED_STRIDE_ATOMS - EW_SURFACE_ATOMS) << 4);
    qts = rk3588_qtest_start_rknpu();
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 5 * 4,
                 RK3588_RKNN_MATMUL_RDMA_ADDR | RK_IOMMU_PTE_RW);
    qtest_writel(qts,
                 RK3588_RKNN_MATMUL_PTE_ADDR + EW_GAPPED_PTE_INDEX * 4,
                 (RK3588_RKNN_MATMUL_RDMA_ADDR + 0x1000) |
                 RK_IOMMU_PTE_RW);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                   commands, command_count * sizeof(*commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                   input, sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_RDMA_ADDR,
                   ew_input, EW_SURFACE_BYTES);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_RDMA_ADDR + 0x1000,
                   ew_input + EW_SURFACE_BYTES, EW_SURFACE_BYTES);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(command_count));
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));
    for (unsigned int row = 0;
         row < RK3588_RKNN_DPU_RDMA_FP16_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_DPU_RDMA_FP16_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_DPU_RDMA_FP16_CHANNELS; channel++) {
                size_t index = rk3588_rknn_spatial_feature_index(
                    RK3588_RKNN_DPU_RDMA_FP16_WIDTH,
                    RK3588_RKNN_DPU_RDMA_FP16_HEIGHT, 16,
                    channel, row, column);
                int expected = channel <
                    RK3588_RKNN_DPU_RDMA_FP16_VALID_CHANNELS ?
                    CLAMP(input[index] + ew_input[index] + 5,
                          INT8_MIN, INT8_MAX) : 0;

                g_assert_cmpint(output[index], ==, expected);
            }
        }
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);

    qts = rk3588_qtest_start_rknpu();
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 5 * 4,
                 RK3588_RKNN_MATMUL_RDMA_ADDR |
                 (RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_WRITABLE));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                   commands, command_count * sizeof(*commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                   input, sizeof(input));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(command_count));
    rk3588_rknn_run_matmul(qts);
    g_assert_cmphex(qtest_readb(
        qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800), ==, 0xa5);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    RKNN_DPU_INTERRUPT_BITS | RKNN_DMA_READ_ERROR);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_rdma_int8_pipeline_controls(void)
{
    static const struct {
        const char *name;
        bool unary;
        uint32_t target;
        uint32_t reg;
        uint32_t value;
    } cases[] = {
        { "bs-alu-source", true, RKNN_REGCMD_TARGET_DPU,
          0x4040, 0x00020150 },
        { "bs-mul-source", false, RKNN_REGCMD_TARGET_DPU,
          0x4048, 0x00040201 },
        { "ew-binary", false, RKNN_REGCMD_TARGET_DPU,
          0x4070, 0x905202c0 },
        { "erdma-mode", false, RKNN_REGCMD_TARGET_DPU_RDMA,
          0x5034, 0x4000000c },
        { "ew-stride", false, RKNN_REGCMD_TARGET_DPU_RDMA,
          0x5040, 0x50 },
    };
    int8_t input[RK3588_RKNN_DPU_RDMA_FP16_INPUT_BYTES] = {};
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int index = 0; index < ARRAY_SIZE(cases); index++) {
        uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
        size_t command_count = cases[index].unary
            ? rk3588_rknn_make_dpu_rdma_int8_unary_regcmd(commands)
            : rk3588_rknn_make_dpu_rdma_int8_binary_regcmd(commands);

        if (index) {
            rk3588_rknpu_reset_fixture(qts);
        }
        g_test_message("DPU-RDMA INT8 pipeline mutation: %s",
                       cases[index].name);
        rk3588_rknn_patch_regcmd(commands, command_count,
                                 cases[index].target, cases[index].reg,
                                 cases[index].value);
        rk3588_rknn_prepare_matmul(qts, true, 0xa5);
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 5 * 4,
                     RK3588_RKNN_MATMUL_RDMA_ADDR | RK_IOMMU_PTE_RW);
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                       commands, command_count * sizeof(*commands));
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                       input, sizeof(input));
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_RDMA_ADDR,
                       input, sizeof(input));
        qtest_writel(qts, RK3588_RKNN0_PC_BASE +
                     RKNN_PC_REGISTER_AMOUNTS,
                     rk3588_rknn_register_amount(command_count));
        rk3588_rknn_run_matmul(qts);
        g_assert_cmphex(qtest_readb(
            qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800), ==, 0xa5);
        rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_rdma_int16_unpool_odd_height(void)
{
    enum {
        INPUT_WIDTH = 3,
        INPUT_HEIGHT = 3,
        INPUT_BYTES = INPUT_WIDTH * INPUT_HEIGHT * 16,
        OUTPUT_WIDTH = INPUT_WIDTH * 2,
        OUTPUT_HEIGHT = INPUT_HEIGHT * 2,
        OUTPUT_BYTES = OUTPUT_WIDTH * OUTPUT_HEIGHT * 16,
    };
    uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
    uint8_t input[INPUT_BYTES];
    uint8_t output[OUTPUT_BYTES];
    size_t command_count =
        rk3588_rknn_make_dpu_rdma_int16_unpool_regcmd(commands, INPUT_HEIGHT);
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int index = 0; index < ARRAY_SIZE(input); index++) {
        input[index] = index * 29 + 7;
    }
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                   commands, command_count * sizeof(*commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                   input, sizeof(input));
    qtest_memset(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                 0xa5, sizeof(output));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(command_count));
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));
    for (unsigned int row = 0; row < OUTPUT_HEIGHT; row++) {
        for (unsigned int column = 0; column < OUTPUT_WIDTH; column++) {
            for (unsigned int channel = 0; channel < 16; channel++) {
                size_t output_index =
                    (row * OUTPUT_WIDTH + column) * 16 + channel;
                size_t input_index =
                    ((row / 2) * INPUT_WIDTH + column / 2) * 16 + channel;

                g_assert_cmphex(output[output_index], ==,
                                input[input_index]);
            }
        }
    }
    rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_SUCCESS,
                             RKNN_DPU_INTERRUPT_BITS);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_rdma_int16_unpool_controls(void)
{
    static const struct {
        const char *name;
        uint32_t target;
        uint32_t reg;
        uint32_t value;
    } cases[] = {
        { "unpool-config", RKNN_REGCMD_TARGET_DPU_RDMA, 0x5048, 0x1248 },
        { "rdma-width", RKNN_REGCMD_TARGET_DPU_RDMA, 0x500c, 3 },
        { "output-notch", RKNN_REGCMD_TARGET_DPU, 0x4038, 0x00060005 },
        { "input-format", RKNN_REGCMD_TARGET_DPU,
          RKNN_DPU_DATA_FORMAT, 0 },
    };
    uint8_t input[3 * 16] = {};
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int index = 0; index < ARRAY_SIZE(cases); index++) {
        uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
        size_t command_count =
            rk3588_rknn_make_dpu_rdma_int16_unpool_regcmd(commands, 2);

        if (index) {
            rk3588_rknpu_reset_fixture(qts);
        }
        g_test_message("DPU-RDMA INT16 unpool mutation: %s",
                       cases[index].name);
        rk3588_rknn_patch_regcmd(commands, command_count,
                                 cases[index].target, cases[index].reg,
                                 cases[index].value);
        rk3588_rknn_prepare_matmul(qts, true, 0xa5);
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                       commands, command_count * sizeof(*commands));
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                       input, sizeof(input));
        qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                     rk3588_rknn_register_amount(command_count));
        rk3588_rknn_run_matmul(qts);
        g_assert_cmphex(qtest_readb(
            qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800), ==, 0xa5);
        rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
    }
    qtest_quit(qts);
}

static void rk3588_rknn_make_dpu_rdma_fp16_bs_input(uint16_t input[])
{
    static const uint16_t values[] = {
        0x0000, 0x3c00, 0xbc00, 0x4000,
        0xc000, 0x3555, 0x7bff, 0xfbff,
    };

    for (unsigned int row = 0;
         row < RK3588_RKNN_DPU_RDMA_FP16_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_DPU_RDMA_FP16_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_DPU_RDMA_FP16_CHANNELS; channel++) {
                size_t index = rk3588_rknn_spatial_feature_index(
                    RK3588_RKNN_DPU_RDMA_FP16_WIDTH,
                    RK3588_RKNN_DPU_RDMA_FP16_HEIGHT, 8,
                    channel, row, column);

                input[index] = values[channel % ARRAY_SIZE(values)];
            }
        }
    }
}

static void test_rk3588_rknpu_dpu_rdma_fp16_bs(void)
{
    static const struct {
        const char *name;
        uint32_t bs_cfg;
        uint32_t relux_cmp;
        uint16_t expected[8];
        bool bypass_controls;
    } cases[] = {
        { "normal", 0x00020040, 0,
          { 0x4597, 0x45b2, 0x457b, 0x45ce,
            0x4560, 0x45a0, 0x6ee2, 0xeedf } },
        { "subtract", 0x00040040, 0,
          { 0xc597, 0xc57b, 0xc5b2, 0xc560,
            0xc5ce, 0xc58e, 0x6edf, 0xeee2 } },
        { "relu", 0x00020000, 0,
          { 0x4597, 0x45b2, 0x457b, 0x45ce,
            0x4560, 0x45a0, 0x6ee2, 0x0000 } },
        { "alu-bypass", 0x00020042, 0,
          { 0x0000, 0x2ee1, 0xaee1, 0x32e1,
            0xb2e1, 0x2896, 0x6ee0, 0xeee0 } },
        { "mul-bypass", 0x00020050, 0,
          { 0x5280, 0x52a0, 0x5260, 0x52c0,
            0x5240, 0x528b, 0x7c00, 0xfbfd } },
        { "relu-x", 0x00000092, 0x3f800000,
          { 0x0000, 0x3c00, 0x0000, 0x3c00,
            0x0000, 0x3555, 0x3c00, 0x0000 } },
        { "stage-bypass", 0x00020041, 0,
          { 0x0000, 0x3c00, 0xbc00, 0x4000,
            0xc000, 0x3555, 0x7bff, 0xfbff }, true },
    };
    uint16_t input[RK3588_RKNN_DPU_RDMA_FP16_WIDTH *
                   RK3588_RKNN_DPU_RDMA_FP16_HEIGHT *
                   RK3588_RKNN_DPU_RDMA_FP16_CHANNELS];
    uint16_t output[ARRAY_SIZE(input)];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_dpu_rdma_fp16_bs_input(input);
    for (unsigned int test = 0; test < ARRAY_SIZE(cases); test++) {
        uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];

        if (test) {
            rk3588_rknpu_reset_fixture(qts);
        }
        g_test_message("DPU-RDMA FP16 BS vector: %s", cases[test].name);
        rk3588_rknn_make_dpu_rdma_fp16_bs_regcmd(commands);
        rk3588_rknn_patch_regcmd(
            commands, ARRAY_SIZE(commands), RKNN_REGCMD_TARGET_DPU,
            0x4040, cases[test].bs_cfg);
        if (cases[test].relux_cmp) {
            rk3588_rknn_patch_regcmd(
                commands, ARRAY_SIZE(commands), RKNN_REGCMD_TARGET_DPU,
                0x404c, cases[test].relux_cmp);
        }
        if (cases[test].bypass_controls) {
            rk3588_rknn_patch_regcmd(
                commands, ARRAY_SIZE(commands), RKNN_REGCMD_TARGET_DPU,
                0x4060, 0x00040053);
            rk3588_rknn_patch_regcmd(
                commands, ARRAY_SIZE(commands), RKNN_REGCMD_TARGET_DPU,
                0x4070, 0x00840383);
            rk3588_rknn_patch_regcmd(
                commands, ARRAY_SIZE(commands),
                RKNN_REGCMD_TARGET_DPU_RDMA, 0x5040, 0x1230);
            rk3588_rknn_patch_regcmd(
                commands, ARRAY_SIZE(commands),
                RKNN_REGCMD_TARGET_DPU_RDMA, 0x506c, 0x4560);
        }
        rk3588_rknn_prepare_dpu_rdma_fp16_bs(qts, commands, input, 0xa5);
        rk3588_rknn_run_matmul(qts);
        qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                      output, sizeof(output));

        for (unsigned int row = 0;
             row < RK3588_RKNN_DPU_RDMA_FP16_HEIGHT; row++) {
            for (unsigned int column = 0;
                 column < RK3588_RKNN_DPU_RDMA_FP16_WIDTH; column++) {
                for (unsigned int channel = 0;
                     channel < RK3588_RKNN_DPU_RDMA_FP16_CHANNELS;
                     channel++) {
                    size_t index = rk3588_rknn_spatial_feature_index(
                        RK3588_RKNN_DPU_RDMA_FP16_WIDTH,
                        RK3588_RKNN_DPU_RDMA_FP16_HEIGHT, 8,
                        channel, row, column);
                    uint16_t expected;

                    if (channel < RK3588_RKNN_DPU_RDMA_FP16_VALID_CHANNELS) {
                        expected = cases[test].expected[channel % 8];
                    } else if (channel < ROUND_UP(
                                   RK3588_RKNN_DPU_RDMA_FP16_VALID_CHANNELS,
                                   8)) {
                        expected = 0;
                    } else {
                        expected = 0xa5a5;
                    }

                    g_assert_cmphex(le16_to_cpu(output[index]), ==, expected);
                }
            }
        }
        g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                        RKNN_TASK_STATUS_SUCCESS);
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_rdma_fp16_lut(void)
{
    static const uint16_t input[] = {
        0x0000, 0xb800, 0xbc00, 0xc000,
        0xc400, 0xc800, 0xcc00, 0xd000,
    };
    static const uint16_t expected[] = {
        0x3bfe, 0x38d9, 0x35e1, 0x3054,
        0x24b2, 0x0e00, 0x0400, 0x0400,
    };
    uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
    uint16_t output[ARRAY_SIZE(expected)];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_dpu_rdma_fp16_lut_regcmd(commands);
    rk3588_rknn_program_fp16_lut(qts);
    rk3588_rknn_prepare_dpu_rdma_fp16_lut(qts, commands, input);
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(expected); i++) {
        g_assert_cmphex(le16_to_cpu(output[i]), ==, expected[i]);
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_rdma_fp16_lut_controls(void)
{
    static const struct {
        uint32_t target;
        uint32_t reg;
        uint32_t value;
    } cases[] = {
        { RKNN_REGCMD_TARGET_DPU, 0x4060, 0x00020041 },
        { RKNN_REGCMD_TARGET_DPU, 0x4068, 0x6a660001 },
        { RKNN_REGCMD_TARGET_DPU, 0x4068, 0x6a660002 },
        { RKNN_REGCMD_TARGET_DPU, 0x4068, 0x6a660100 },
        { RKNN_REGCMD_TARGET_DPU, 0x4088, 0x4000f000 },
        { RKNN_REGCMD_TARGET_DPU, 0x4088, 0x0000e000 },
        { RKNN_REGCMD_TARGET_DPU, 0x4024, 0 },
        { RKNN_REGCMD_TARGET_DPU, 0x4108, 0x69 },
        { RKNN_REGCMD_TARGET_DPU, 0x410c, 0x00050400 },
        { RKNN_REGCMD_TARGET_DPU_RDMA, 0x5044, 0x00017841 },
    };
    uint16_t input[8] = { 0 };
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int index = 0; index < ARRAY_SIZE(cases); index++) {
        uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];

        if (index) {
            rk3588_rknpu_reset_fixture(qts);
        }
        rk3588_rknn_make_dpu_rdma_fp16_lut_regcmd(commands);
        rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                 cases[index].target, cases[index].reg,
                                 cases[index].value);
        rk3588_rknn_program_fp16_lut(qts);
        rk3588_rknn_prepare_dpu_rdma_fp16_lut(qts, commands, input);
        rk3588_rknn_run_matmul(qts);
        g_assert_cmphex(qtest_readb(
            qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800), ==, 0xa5);
        rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_rdma_int8_to_fp16_iommu(void)
{
    uint64_t commands[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];
    int8_t input[RK3588_RKNN_DPU_RDMA_FP16_INPUT_BYTES];
    uint8_t output[RK3588_RKNN_DPU_RDMA_FP16_OUTPUT_BYTES];
    QTestState *qts;

    rk3588_rknn_make_dpu_rdma_fp16_regcmd(commands);
    rk3588_rknn_make_dpu_rdma_fp16_input(input);

    qts = rk3588_qtest_start_rknpu();
    rk3588_rknn_prepare_dpu_rdma_fp16(qts, commands, input, 0xa5);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 1 * 4,
                 RK3588_RKNN_MATMUL_INPUT_ADDR |
                 (RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_READABLE));
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 3 * 4,
                 RK3588_RKNN_MATMUL_OUTPUT_ADDR0 |
                 (RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_WRITABLE));
    rk3588_rknn_run_matmul(qts);
    g_assert_cmphex(qtest_readw(
        qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800), ==,
        rk3588_rknn_half_from_int8(input[0]));
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    RKNN_DPU_INTERRUPT_BITS);
    qtest_quit(qts);

    qts = rk3588_qtest_start_rknpu();
    rk3588_rknn_prepare_dpu_rdma_fp16(qts, commands, input, 0xa5);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 1 * 4,
                 RK3588_RKNN_MATMUL_INPUT_ADDR |
                 (RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_WRITABLE));
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));
    for (unsigned int index = 0; index < ARRAY_SIZE(output); index++) {
        g_assert_cmphex(output[index], ==, 0xa5);
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    RKNN_DPU_INTERRUPT_BITS | RKNN_DMA_READ_ERROR);
    qtest_quit(qts);

    qts = rk3588_qtest_start_rknpu();
    rk3588_rknn_prepare_dpu_rdma_fp16(qts, commands, input, 0xa5);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 3 * 4,
                 RK3588_RKNN_MATMUL_OUTPUT_ADDR0 |
                 (RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_READABLE));
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  output, sizeof(output));
    for (unsigned int index = 0; index < ARRAY_SIZE(output); index++) {
        g_assert_cmphex(output[index], ==, 0xa5);
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    RKNN_DPU_INTERRUPT_BITS | RKNN_DMA_WRITE_ERROR);
    qtest_quit(qts);
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

static int32_t rk3588_rknn_matmul_expected(unsigned int row,
                                            unsigned int out)
{
    int32_t expected = 0;

    for (unsigned int channel = 0;
         channel < RK3588_RKNN_MATMUL_K; channel++) {
        int8_t input = ((row * 37 + channel * 11 + 3) % 31) - 15;
        int8_t weight = ((out * 19 + channel * 7 + 5) % 29) - 14;

        expected += input * weight;
    }
    return expected;
}

static void rk3588_rknn_assert_matmul_output_data(const uint32_t output[])
{
    for (unsigned int row = 0; row < RK3588_RKNN_MATMUL_M; row++) {
        for (unsigned int out = 0; out < RK3588_RKNN_MATMUL_N; out++) {
            size_t output_index = rk3588_rknn_feature_index(
                RK3588_RKNN_MATMUL_N, RK3588_RKNN_MATMUL_M, 4, out, row);

            g_assert_cmpint((int32_t)le32_to_cpu(output[output_index]), ==,
                            rk3588_rknn_matmul_expected(row, out));
        }
    }
}

static void rk3588_rknn_assert_matmul_output(QTestState *qts)
{
    uint32_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N];

    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    rk3588_rknn_assert_matmul_output_data(output);
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

static char *rk3588_dump_rknpu_dtb(const char *machine_type)
{
    const char *qemu = g_getenv("QTEST_QEMU_BINARY");
    g_autofree char *kernel = rk3588_create_dummy_kernel();
    g_autofree char *dtb = NULL;
    g_autofree char *machine = NULL;
    g_autoptr(GError) error = NULL;
    int fd;
    int status;
    char *argv[12];

    g_assert_nonnull(qemu);
    fd = g_file_open_tmp("rk3588-dtb-XXXXXX", &dtb, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    machine = g_strdup_printf("%s,rknpu=on,dumpdtb=%s", machine_type, dtb);
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
    unlink(kernel);

    return g_steal_pointer(&dtb);
}

static bool rk3588_fdt_has_node(const char *fdtget, const char *dtb,
                                const char *node)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *stdout_text = NULL;
    g_autofree char *stderr_text = NULL;
    int status;
    char *argv[] = {
        (char *)fdtget, (char *)"-l", (char *)dtb, (char *)node, NULL,
    };

    g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                 &stdout_text, &stderr_text, &status, &error);
    g_assert_no_error(error);

    return status == 0;
}

static GArray *rk3588_parse_fdt_cells(const char *cells)
{
    g_auto(GStrv) tokens = g_strsplit_set(cells, " \t\r\n", -1);
    GArray *values = g_array_new(false, false, sizeof(uint32_t));

    for (unsigned int i = 0; tokens[i]; i++) {
        char *end = NULL;
        uint64_t value;
        uint32_t cell;

        if (!tokens[i][0]) {
            continue;
        }
        errno = 0;
        value = g_ascii_strtoull(tokens[i], &end, 10);
        g_assert_cmpint(errno, ==, 0);
        g_assert_true(end != tokens[i] && !*end);
        g_assert_cmpuint(value, <=, UINT32_MAX);
        cell = value;

        g_array_append_val(values, cell);
    }

    return values;
}

static GArray *rk3588_fdtget_cells(const char *fdtget, const char *dtb,
                                   const char *node, const char *property)
{
    g_autofree char *text = rk3588_fdtget(fdtget, dtb, "u", node,
                                          property);

    return rk3588_parse_fdt_cells(text);
}

static void rk3588_assert_fdt_string(const char *fdtget, const char *dtb,
                                     const char *node, const char *property,
                                     const char *expected)
{
    g_autofree char *value = rk3588_fdtget(fdtget, dtb, "s", node,
                                           property);

    g_assert_cmpstr(g_strstrip(value), ==, expected);
}

static void rk3588_assert_fdt_contains(const char *fdtget, const char *dtb,
                                       const char *node,
                                       const char *property,
                                       const char *expected)
{
    g_autofree char *value = rk3588_fdtget(fdtget, dtb, "s", node,
                                           property);

    g_assert_nonnull(strstr(value, expected));
}

static void rk3588_rknn_prepare_multitask(QTestState *qts,
                                          uint64_t first[],
                                          uint64_t second[],
                                          uint64_t second_output_address);
static void rk3588_rknn_step_tasks(QTestState *qts,
                                   unsigned int task_count);

static void test_rk3588_rknpu_pipeline_three_task_chain(void)
{
    uint64_t first[RK3588_RKNN_MATMUL_COMMANDS];
    uint64_t second[RK3588_RKNN_MATMUL_COMMANDS];
    uint64_t third[RK3588_RKNN_MATMUL_COMMANDS];
    uint8_t first_output[RK3588_RKNN_MATMUL_M *
                         RK3588_RKNN_MATMUL_N * 4];
    uint8_t third_output[sizeof(first_output)];
    const uint32_t third_regcmd_iova =
        RK3588_RKNN_MATMUL_REGCMD_IOVA + 0x8000;
    const uint32_t third_output_iova =
        RK3588_RKNN_MATMUL_REGCMD_IOVA + 0x9800;
    const uint64_t second_output_address = RK3588_RAM_BASE + 0x2a000;
    const uint64_t third_regcmd_address = RK3588_RAM_BASE + 0x2d000;
    const uint64_t third_output_address = RK3588_RAM_BASE + 0x2e000;
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t output;

    memset(third_output, 0x5a, sizeof(third_output));
    rk3588_rknn_prepare_multitask(qts, first, second,
                                  second_output_address);
    rk3588_rknn_make_matmul_regcmd(third, true);
    second[ARRAY_SIZE(second) - 4] = rk3588_rknn_regcmd(
        0x0101, RKNN_PC_BASE_ADDRESS, third_regcmd_iova);
    second[ARRAY_SIZE(second) - 3] = rk3588_rknn_regcmd(
        0x0101, RKNN_PC_REGISTER_AMOUNTS,
        rk3588_rknn_register_amount(ARRAY_SIZE(third)));
    output = rk3588_rknn_find_regcmd(third, ARRAY_SIZE(third),
                                     0x1001, 0x4020);
    third[output] = rk3588_rknn_regcmd(0x1001, 0x4020,
                                       third_output_iova);
    qtest_memwrite(qts, RK3588_RAM_BASE + 0x29000, second,
                   sizeof(second));
    qtest_memwrite(qts, third_regcmd_address, third, sizeof(third));
    qtest_memwrite(qts, third_output_address + 0x800, third_output,
                   sizeof(third_output));
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 8 * 4,
                 third_regcmd_address | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 9 * 4,
                 third_output_address | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 10 * 4,
                 (third_output_address + 0x1000) | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN0_CNA_BASE + RKNN_POINTER, 0x0e);
    qtest_writel(qts, RK3588_RKNN0_CORE_BASE + RKNN_POINTER, 0x0e);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_TASK_CON, 0x00003003);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_OPERATION_ENABLE,
                 RKNN_PC_OPERATION_ENABLE_OP_EN);
    rk3588_rknn_step_tasks(qts, 3);

    rk3588_rknn_read_matmul_output(qts, first_output, sizeof(first_output));
    qtest_memread(qts, third_output_address + 0x800, third_output,
                  sizeof(third_output));
    g_assert_cmpmem(third_output, sizeof(third_output),
                    first_output, sizeof(first_output));
    rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_SUCCESS,
                             RKNN_PIPELINE_BANK1_INTERRUPT);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0x0001000e);
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

static void test_rk3588s_roc_pc_rknpu_fdt(void)
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
    g_autofree char *fdtget = g_find_program_in_path("fdtget");
    g_autofree char *dtb = NULL;

    if (!qtest_has_machine(RK3588S_ROC_PC_MACHINE)) {
        g_test_skip(RK3588S_ROC_PC_MACHINE " machine not available");
        return;
    }
    if (!g_getenv("QTEST_QEMU_BINARY")) {
        g_test_skip("QTEST_QEMU_BINARY not set");
        return;
    }
    if (!fdtget) {
        g_test_skip("fdtget not available");
        return;
    }

    dtb = rk3588_dump_rknpu_dtb(RK3588S_ROC_PC_MACHINE);

    for (unsigned int i = 0; i < ARRAY_SIZE(nodes); i++) {
        g_autoptr(GArray) resets = rk3588_fdtget_cells(
            fdtget, dtb, nodes[i].node, "resets");
        g_autoptr(GArray) interrupts = rk3588_fdtget_cells(
            fdtget, dtb, nodes[i].node, "interrupts");
        g_autoptr(GArray) iommus = rk3588_fdtget_cells(
            fdtget, dtb, nodes[i].node, "iommus");
        g_autoptr(GArray) iommu_phandle = rk3588_fdtget_cells(
            fdtget, dtb, nodes[i].iommu_node, "phandle");
        g_autoptr(GArray) iommu_cells = rk3588_fdtget_cells(
            fdtget, dtb, nodes[i].iommu_node, "#iommu-cells");
        g_autoptr(GArray) iommu_reg = rk3588_fdtget_cells(
            fdtget, dtb, nodes[i].iommu_node, "reg");
        g_autoptr(GArray) iommu_interrupts = rk3588_fdtget_cells(
            fdtget, dtb, nodes[i].iommu_node, "interrupts");
        GArray *irq_arrays[] = { interrupts, iommu_interrupts };

        rk3588_assert_fdt_contains(fdtget, dtb, nodes[i].node,
                                   "compatible", "rockchip,rk3588-rknn-core");
        rk3588_assert_fdt_contains(fdtget, dtb, nodes[i].node,
                                   "reg-names", "pc");
        rk3588_assert_fdt_contains(fdtget, dtb, nodes[i].node,
                                   "reg-names", "cna");
        rk3588_assert_fdt_contains(fdtget, dtb, nodes[i].node,
                                   "reg-names", "core");
        rk3588_assert_fdt_contains(fdtget, dtb, nodes[i].node,
                                   "reset-names", "srst_a");
        rk3588_assert_fdt_contains(fdtget, dtb, nodes[i].node,
                                   "reset-names", "srst_h");
        rk3588_assert_fdt_contains(fdtget, dtb, nodes[i].iommu_node,
                                   "compatible", "rockchip,rk3588-iommu");
        rk3588_assert_fdt_contains(fdtget, dtb, nodes[i].iommu_node,
                                   "compatible", "rockchip,rk3568-iommu");

        g_assert_cmpuint(resets->len, ==, 4);
        g_assert_cmpuint(g_array_index(resets, uint32_t, 0), ==,
                         g_array_index(resets, uint32_t, 2));
        g_assert_cmpuint(g_array_index(resets, uint32_t, 1), ==,
                         nodes[i].reset_a);
        g_assert_cmpuint(g_array_index(resets, uint32_t, 3), ==,
                         nodes[i].reset_h);
        for (unsigned int irq_index = 0;
             irq_index < ARRAY_SIZE(irq_arrays); irq_index++) {
            GArray *irq = irq_arrays[irq_index];

            g_assert_cmpuint(irq->len, ==, 4);
            g_assert_cmpuint(g_array_index(irq, uint32_t, 0), ==, 0);
            g_assert_cmpuint(g_array_index(irq, uint32_t, 1), ==,
                             nodes[i].irq);
            g_assert_cmpuint(g_array_index(irq, uint32_t, 2), ==, 4);
            g_assert_cmpuint(g_array_index(irq, uint32_t, 3), ==, 0);
        }
        g_assert_cmpuint(iommus->len, ==, 1);
        g_assert_cmpuint(iommu_phandle->len, ==, 1);
        g_assert_cmpuint(g_array_index(iommus, uint32_t, 0), ==,
                         g_array_index(iommu_phandle, uint32_t, 0));
        g_assert_cmpuint(iommu_cells->len, ==, 1);
        g_assert_cmpuint(g_array_index(iommu_cells, uint32_t, 0), ==, 0);
        g_assert_cmpuint(iommu_reg->len, ==, nodes[i].iommu_windows * 4);
        for (unsigned int window = 0; window < nodes[i].iommu_windows;
             window++) {
            uint64_t expected_base = window ? nodes[i].iommu_base1 :
                                              nodes[i].iommu_base0;

            g_assert_cmpuint(g_array_index(iommu_reg, uint32_t,
                                           window * 4), ==, 0);
            g_assert_cmpuint(g_array_index(iommu_reg, uint32_t,
                                           window * 4 + 1), ==,
                             expected_base);
            g_assert_cmpuint(g_array_index(iommu_reg, uint32_t,
                                           window * 4 + 2), ==, 0);
            g_assert_cmpuint(g_array_index(iommu_reg, uint32_t,
                                           window * 4 + 3), ==,
                             RK_IOMMU_WINDOW_SIZE);
        }
    }

    unlink(dtb);
}

static void rk3588_test_rknpu_aggregate_fdt(const char *machine_type)
{
    static const uint32_t core_base[] = {
        RK3588_RKNN0_PC_BASE,
        RK3588_RKNN1_PC_BASE,
        RK3588_RKNN2_PC_BASE,
    };
    static const uint32_t iommu_base[] = {
        RK3588_RKNN0_MMU_BASE,
        RK3588_RKNN0_MMU1_BASE,
        RK3588_RKNN1_MMU_BASE,
        RK3588_RKNN2_MMU_BASE,
    };
    static const uint32_t irq[] = {
        RK3588_RKNN0_SPI,
        RK3588_RKNN1_SPI,
        RK3588_RKNN2_SPI,
    };
    static const uint32_t reset[] = {
        RK3588_VENDOR_SRST_A_RKNN0,
        RK3588_VENDOR_SRST_A_RKNN1,
        RK3588_VENDOR_SRST_A_RKNN2,
        RK3588_VENDOR_SRST_H_RKNN0,
        RK3588_VENDOR_SRST_H_RKNN1,
        RK3588_VENDOR_SRST_H_RKNN2,
    };
    const char *npu = "/npu@fdab0000";
    const char *iommu = "/iommu@fdab9000";
    g_autofree char *fdtget = g_find_program_in_path("fdtget");
    g_autofree char *dtb = NULL;
    g_autoptr(GArray) npu_reg = NULL;
    g_autoptr(GArray) npu_interrupts = NULL;
    g_autoptr(GArray) npu_clocks = NULL;
    g_autoptr(GArray) npu_resets = NULL;
    g_autoptr(GArray) npu_iommus = NULL;
    g_autoptr(GArray) iommu_reg = NULL;
    g_autoptr(GArray) iommu_interrupts = NULL;
    g_autoptr(GArray) iommu_clocks = NULL;
    g_autoptr(GArray) iommu_cells = NULL;
    g_autoptr(GArray) iommu_phandle = NULL;
    g_autoptr(GArray) clock_provider = NULL;
    g_autoptr(GArray) reset_provider = NULL;

    if (!g_getenv("QTEST_QEMU_BINARY")) {
        g_test_skip("QTEST_QEMU_BINARY not set");
        return;
    }
    if (!fdtget) {
        g_test_skip("fdtget not available");
        return;
    }

    dtb = rk3588_dump_rknpu_dtb(machine_type);
    g_assert_true(rk3588_fdt_has_node(fdtget, dtb, npu));
    g_assert_false(rk3588_fdt_has_node(fdtget, dtb, "/npu@fdac0000"));
    g_assert_false(rk3588_fdt_has_node(fdtget, dtb, "/npu@fdad0000"));
    g_assert_true(rk3588_fdt_has_node(fdtget, dtb, iommu));
    g_assert_false(rk3588_fdt_has_node(fdtget, dtb, "/iommu@fdaca000"));
    g_assert_false(rk3588_fdt_has_node(fdtget, dtb, "/iommu@fdada000"));

    rk3588_assert_fdt_string(fdtget, dtb, npu, "compatible",
                             "rockchip,rk3588-rknpu");
    rk3588_assert_fdt_string(fdtget, dtb, npu, "interrupt-names",
                             "npu0_irq npu1_irq npu2_irq");
    rk3588_assert_fdt_string(
        fdtget, dtb, npu, "clock-names",
        "clk_npu aclk0 aclk1 aclk2 hclk0 hclk1 hclk2 pclk");
    rk3588_assert_fdt_string(
        fdtget, dtb, npu, "reset-names",
        "srst_a0 srst_a1 srst_a2 srst_h0 srst_h1 srst_h2");
    rk3588_assert_fdt_string(fdtget, dtb, npu, "status", "okay");
    rk3588_assert_fdt_string(fdtget, dtb, iommu, "compatible",
                             "rockchip,iommu-v2");
    rk3588_assert_fdt_string(fdtget, dtb, iommu, "interrupt-names",
                             "npu0_mmu npu1_mmu npu2_mmu");
    rk3588_assert_fdt_string(
        fdtget, dtb, iommu, "clock-names",
        "aclk0 aclk1 aclk2 iface0 iface1 iface2");
    rk3588_assert_fdt_string(fdtget, dtb, iommu, "status", "okay");

    npu_reg = rk3588_fdtget_cells(fdtget, dtb, npu, "reg");
    npu_interrupts = rk3588_fdtget_cells(fdtget, dtb, npu, "interrupts");
    npu_clocks = rk3588_fdtget_cells(fdtget, dtb, npu, "clocks");
    npu_resets = rk3588_fdtget_cells(fdtget, dtb, npu, "resets");
    npu_iommus = rk3588_fdtget_cells(fdtget, dtb, npu, "iommus");
    iommu_reg = rk3588_fdtget_cells(fdtget, dtb, iommu, "reg");
    iommu_interrupts = rk3588_fdtget_cells(
        fdtget, dtb, iommu, "interrupts");
    iommu_clocks = rk3588_fdtget_cells(fdtget, dtb, iommu, "clocks");
    iommu_cells = rk3588_fdtget_cells(
        fdtget, dtb, iommu, "#iommu-cells");
    iommu_phandle = rk3588_fdtget_cells(fdtget, dtb, iommu, "phandle");
    clock_provider = rk3588_fdtget_cells(
        fdtget, dtb, "/xin24m", "phandle");
    reset_provider = rk3588_fdtget_cells(
        fdtget, dtb, "/clock-reset-controller@fd7c0000", "phandle");

    g_assert_cmpuint(npu_reg->len, ==, 12);
    for (unsigned int i = 0; i < ARRAY_SIZE(core_base); i++) {
        g_assert_cmpuint(g_array_index(npu_reg, uint32_t, i * 4), ==, 0);
        g_assert_cmpuint(g_array_index(npu_reg, uint32_t, i * 4 + 1), ==,
                         core_base[i]);
        g_assert_cmpuint(g_array_index(npu_reg, uint32_t, i * 4 + 2), ==, 0);
        g_assert_cmpuint(g_array_index(npu_reg, uint32_t, i * 4 + 3), ==,
                         0x10000);
    }
    g_assert_cmpuint(npu_interrupts->len, ==, 12);
    g_assert_cmpuint(iommu_interrupts->len, ==, 12);
    for (unsigned int i = 0; i < ARRAY_SIZE(irq); i++) {
        g_assert_cmpuint(g_array_index(npu_interrupts, uint32_t, i * 4), ==,
                         0);
        g_assert_cmpuint(g_array_index(npu_interrupts, uint32_t, i * 4 + 1),
                         ==, irq[i]);
        g_assert_cmpuint(g_array_index(npu_interrupts, uint32_t, i * 4 + 2),
                         ==, 4);
        g_assert_cmpuint(g_array_index(npu_interrupts, uint32_t, i * 4 + 3),
                         ==, 0);
        for (unsigned int cell = 0; cell < 4; cell++) {
            g_assert_cmpuint(
                g_array_index(iommu_interrupts, uint32_t, i * 4 + cell), ==,
                g_array_index(npu_interrupts, uint32_t, i * 4 + cell));
        }
    }
    g_assert_cmpuint(npu_clocks->len, ==, 8);
    for (unsigned int i = 1; i < npu_clocks->len; i++) {
        g_assert_cmpuint(g_array_index(npu_clocks, uint32_t, i), ==,
                         g_array_index(npu_clocks, uint32_t, 0));
    }
    g_assert_cmpuint(clock_provider->len, ==, 1);
    g_assert_cmpuint(g_array_index(npu_clocks, uint32_t, 0), ==,
                     g_array_index(clock_provider, uint32_t, 0));
    g_assert_cmpuint(npu_resets->len, ==, 12);
    for (unsigned int i = 0; i < ARRAY_SIZE(reset); i++) {
        g_assert_cmpuint(g_array_index(npu_resets, uint32_t, i * 2), ==,
                         g_array_index(npu_resets, uint32_t, 0));
        g_assert_cmpuint(g_array_index(npu_resets, uint32_t, i * 2 + 1), ==,
                         reset[i]);
    }
    g_assert_cmpuint(reset_provider->len, ==, 1);
    g_assert_cmpuint(g_array_index(npu_resets, uint32_t, 0), ==,
                     g_array_index(reset_provider, uint32_t, 0));

    g_assert_cmpuint(iommu_reg->len, ==, 16);
    for (unsigned int i = 0; i < ARRAY_SIZE(iommu_base); i++) {
        g_assert_cmpuint(g_array_index(iommu_reg, uint32_t, i * 4), ==, 0);
        g_assert_cmpuint(g_array_index(iommu_reg, uint32_t, i * 4 + 1), ==,
                         iommu_base[i]);
        g_assert_cmpuint(g_array_index(iommu_reg, uint32_t, i * 4 + 2), ==,
                         0);
        g_assert_cmpuint(g_array_index(iommu_reg, uint32_t, i * 4 + 3), ==,
                         RK_IOMMU_WINDOW_SIZE);
    }
    g_assert_cmpuint(iommu_clocks->len, ==, 6);
    for (unsigned int i = 1; i < iommu_clocks->len; i++) {
        g_assert_cmpuint(g_array_index(iommu_clocks, uint32_t, i), ==,
                         g_array_index(iommu_clocks, uint32_t, 0));
    }
    g_assert_cmpuint(g_array_index(iommu_clocks, uint32_t, 0), ==,
                     g_array_index(clock_provider, uint32_t, 0));
    g_assert_cmpuint(iommu_cells->len, ==, 1);
    g_assert_cmpuint(g_array_index(iommu_cells, uint32_t, 0), ==, 0);
    g_assert_cmpuint(iommu_phandle->len, ==, 1);
    g_assert_cmpuint(npu_iommus->len, ==, 1);
    g_assert_cmpuint(g_array_index(npu_iommus, uint32_t, 0), ==,
                     g_array_index(iommu_phandle, uint32_t, 0));

    unlink(dtb);
}

static void test_rk3588_evb_rknpu_fdt(void)
{
    rk3588_test_rknpu_aggregate_fdt(RK3588_EVB_MACHINE);
}

static void test_rock_5b_plus_rknpu_fdt(void)
{
    if (!qtest_has_machine(ROCK_5B_PLUS_MACHINE)) {
        g_test_skip(ROCK_5B_PLUS_MACHINE " machine not available");
        return;
    }

    rk3588_test_rknpu_aggregate_fdt(ROCK_5B_PLUS_MACHINE);
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
                        (i << 28) | 0x0000000f);
        g_assert_cmphex(qtest_readl(qts, core_bases[i] + RKNN_POINTER), ==,
                        i << 28);
        qtest_writel(qts, cna_bases[i] + RKNN_POINTER, 0xfffeffc0);
        g_assert_cmphex(qtest_readl(qts, cna_bases[i] + RKNN_POINTER), ==,
                        i << 28);
    }

    qtest_quit(qts);
}

static void test_rk3588_rknpu_ppu_windows_all_cores(void)
{
    static const uint64_t ppu_bases[] = {
        RK3588_RKNN0_PPU_BASE,
        RK3588_RKNN1_PPU_BASE,
        RK3588_RKNN2_PPU_BASE,
    };
    static const uint64_t ppu_rdma_bases[] = {
        RK3588_RKNN0_PPU_RDMA_BASE,
        RK3588_RKNN1_PPU_RDMA_BASE,
        RK3588_RKNN2_PPU_RDMA_BASE,
    };
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int i = 0; i < ARRAY_SIZE(ppu_bases); i++) {
        g_assert_cmphex(qtest_readl(qts, ppu_bases[i]), ==, 0);
        g_assert_cmphex(qtest_readl(qts, ppu_rdma_bases[i]), ==, 0);
        qtest_writel(qts, ppu_bases[i], UINT32_MAX);
        qtest_writel(qts, ppu_rdma_bases[i], UINT32_MAX);
        g_assert_cmphex(qtest_readl(qts, ppu_bases[i]), ==, 0);
        g_assert_cmphex(qtest_readl(qts, ppu_rdma_bases[i]), ==, 0);
        qtest_writel(qts, ppu_bases[i] + RKNN_POINTER, UINT32_MAX);
        qtest_writel(qts, ppu_rdma_bases[i] + RKNN_POINTER, UINT32_MAX);
        g_assert_cmphex(qtest_readl(qts, ppu_bases[i] + RKNN_POINTER), ==,
                        0x3000000f);
        g_assert_cmphex(qtest_readl(qts,
                                    ppu_rdma_bases[i] + RKNN_POINTER), ==,
                        0x3000000f);
    }

    qtest_system_reset(qts);
    for (unsigned int i = 0; i < ARRAY_SIZE(ppu_bases); i++) {
        g_assert_cmphex(qtest_readl(qts, ppu_bases[i]), ==, 0);
        g_assert_cmphex(qtest_readl(qts, ppu_bases[i] + RKNN_POINTER), ==, 0);
        g_assert_cmphex(qtest_readl(qts, ppu_rdma_bases[i]), ==, 0);
        g_assert_cmphex(qtest_readl(qts,
                                    ppu_rdma_bases[i] + RKNN_POINTER), ==, 0);
    }

    qtest_quit(qts);
}

static void rk3588_assert_iommu_reset(QTestState *qts, uint64_t base)
{
    g_assert_cmphex(qtest_readl(qts, base + RK_IOMMU_DTE_ADDR), ==, 0);
    g_assert_cmphex(qtest_readl(qts, base + RK_IOMMU_STATUS), ==,
                    RK_IOMMU_STATUS_RESET);
    g_assert_cmphex(qtest_readl(qts, base + RK_IOMMU_INT_MASK), ==, 0);
    g_assert_cmphex(qtest_readl(qts, base + RK_IOMMU_AUTO_GATING), ==,
                    RK_IOMMU_AUTO_GATING_RESET);
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

    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_AUTO_GATING,
                 0x12345678);
    qtest_writel(qts, RK3588_RKNN0_MMU1_BASE + RK_IOMMU_DTE_ADDR,
                 0x23456000);
    qtest_writel(qts, RK3588_RKNN0_MMU1_BASE + RK_IOMMU_INT_MASK,
                 RK_IOMMU_IRQ_PAGE_FAULT | RK_IOMMU_IRQ_BUS_ERROR);
    qtest_writel(qts, RK3588_RKNN0_MMU1_BASE + RK_IOMMU_AUTO_GATING,
                 0x80000000);
    qtest_writel(qts, RK3588_RKNN0_MMU1_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_FORCE_RESET);
    rk3588_assert_iommu_reset(qts, RK3588_RKNN0_MMU1_BASE);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU_BASE +
                                RK_IOMMU_AUTO_GATING), ==, 0x12345678);

    qtest_system_reset(qts);
    for (unsigned int i = 0; i < ARRAY_SIZE(mmu_bases); i++) {
        rk3588_assert_iommu_reset(qts, mmu_bases[i]);
    }

    qtest_quit(qts);
}

static uint32_t rk3588_iommu_v2_desc(uint64_t phys, uint32_t flags)
{
    return (phys & 0xfffff000) |
           ((phys >> 32) & 0xf) << 8 |
           ((phys >> 36) & 0xf) << 4 |
           flags;
}

static void test_rk3588_rknpu_iommu_v2_high_address(void)
{
    const uint64_t high_phys = UINT64_C(1) << 32;
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    QTestState *qts = qtest_initf(
        "-machine " RK3588_EVB_MACHINE
        ",rknpu=on,zvm-ram=on -smp 1 -m 512M");

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(commands, true);
    qtest_memwrite(qts, high_phys, commands, sizeof(commands));
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR,
                 rk3588_iommu_v2_desc(
                     high_phys,
                     RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_READABLE));
    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_assert_matmul_output(qts);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void rk3588_assert_iommu_fault(QTestState *qts, uint64_t base,
                                      uint32_t iova, uint32_t irq,
                                      bool write)
{
    uint32_t status = qtest_readl(qts, base + RK_IOMMU_STATUS);

    g_assert_cmphex(qtest_readl(qts, base + RK_IOMMU_PAGE_FAULT_ADDR), ==,
                    iova);
    g_assert_cmphex(qtest_readl(qts, base + RK_IOMMU_INT_RAWSTAT), ==, irq);
    g_assert_cmphex(qtest_readl(qts, base + RK_IOMMU_INT_STATUS), ==, irq);
    if (irq == RK_IOMMU_IRQ_PAGE_FAULT) {
        g_assert_cmphex(status & RK_IOMMU_STATUS_PAGE_FAULT_ACTIVE, ==,
                        RK_IOMMU_STATUS_PAGE_FAULT_ACTIVE);
        g_assert_cmphex(status & RK_IOMMU_STATUS_PAGE_FAULT_IS_WRITE, ==,
                        write ? RK_IOMMU_STATUS_PAGE_FAULT_IS_WRITE : 0);
    }
}

static void test_rk3588_rknpu_iommu_fault_irq(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    qtest_irq_intercept_in(qts, RK3588_GIC_QOM);
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_MASK,
                 RKNN_DPU_INTERRUPT_BITS);
    rk3588_rknn_run_matmul(qts);
    g_assert_true(qtest_get_irq(qts, RK3588_RKNN0_SPI));

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_INT_MASK,
                 RK_IOMMU_IRQ_PAGE_FAULT | RK_IOMMU_IRQ_BUS_ERROR);
    qtest_writel(qts, RK3588_RKNN_MATMUL_DTE_ADDR + 64 * 4, 0);
    rk3588_rknn_start_matmul(qts);
    rk3588_assert_iommu_fault(qts, RK3588_RKNN0_MMU_BASE,
                              RK3588_RKNN_MATMUL_REGCMD_IOVA,
                              RK_IOMMU_IRQ_PAGE_FAULT, false);
    g_assert_true(qtest_get_irq(qts, RK3588_RKNN0_SPI));
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_PAGE_FAULT_DONE);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU_BASE +
                                RK_IOMMU_STATUS) &
                    (RK_IOMMU_STATUS_PAGE_FAULT_ACTIVE |
                     RK_IOMMU_STATUS_PAGE_FAULT_IS_WRITE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU_BASE +
                                RK_IOMMU_INT_RAWSTAT), ==,
                    RK_IOMMU_IRQ_PAGE_FAULT);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU_BASE +
                                RK_IOMMU_INT_STATUS), ==,
                    RK_IOMMU_IRQ_PAGE_FAULT);
    g_assert_true(qtest_get_irq(qts, RK3588_RKNN0_SPI));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_CLEAR,
                 UINT32_MAX);
    g_assert_true(qtest_get_irq(qts, RK3588_RKNN0_SPI));
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_INT_CLEAR,
                 RK_IOMMU_IRQ_PAGE_FAULT);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU_BASE +
                                RK_IOMMU_INT_RAWSTAT), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU_BASE +
                                RK_IOMMU_INT_STATUS), ==, 0);
    g_assert_false(qtest_get_irq(qts, RK3588_RKNN0_SPI));
    qtest_quit(qts);

    qts = rk3588_qtest_start_rknpu();
    qtest_irq_intercept_in(qts, RK3588_GIC_QOM);
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_MASK, 0);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_INT_MASK,
                 RK_IOMMU_IRQ_PAGE_FAULT);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 3 * 4,
                 RK3588_RKNN_MATMUL_OUTPUT_ADDR0 |
                 (RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_READABLE));
    rk3588_rknn_run_matmul(qts);
    rk3588_assert_iommu_fault(qts, RK3588_RKNN0_MMU_BASE,
                              RK3588_RKNN_MATMUL_REGCMD_IOVA + 0x3800,
                              RK_IOMMU_IRQ_PAGE_FAULT, true);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_INT_CLEAR,
                 RK_IOMMU_IRQ_PAGE_FAULT);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU_BASE +
                                RK_IOMMU_INT_STATUS), ==, 0);
    g_assert_false(qtest_get_irq(qts, RK3588_RKNN0_SPI));
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU_BASE +
                                RK_IOMMU_STATUS) &
                    RK_IOMMU_STATUS_PAGE_FAULT_ACTIVE, ==,
                    RK_IOMMU_STATUS_PAGE_FAULT_ACTIVE);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_PAGE_FAULT_DONE);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU_BASE +
                                RK_IOMMU_STATUS) &
                    RK_IOMMU_STATUS_PAGE_FAULT_ACTIVE, ==, 0);
    qtest_quit(qts);

    qts = rk3588_qtest_start_rknpu();
    qtest_irq_intercept_in(qts, RK3588_GIC_QOM);
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_DTE_ADDR,
                 0xfffff000);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_INT_MASK,
                 RK_IOMMU_IRQ_BUS_ERROR);
    rk3588_rknn_start_matmul(qts);
    rk3588_assert_iommu_fault(qts, RK3588_RKNN0_MMU_BASE,
                              RK3588_RKNN_MATMUL_REGCMD_IOVA,
                              RK_IOMMU_IRQ_BUS_ERROR, false);
    g_assert_true(qtest_get_irq(qts, RK3588_RKNN0_SPI));
    qtest_quit(qts);
}

static void test_rk3588_rknpu_iommu_permission_fault_bank(void)
{
    const uint64_t mmu1_dte_addr = RK3588_RAM_BASE + 0x2b000;
    const uint64_t mmu1_pte_addr = RK3588_RAM_BASE + 0x2c000;
    QTestState *qts = rk3588_qtest_start_rknpu();

    qtest_irq_intercept_in(qts, RK3588_GIC_QOM);
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_writel(qts, RK3588_RKNN_MATMUL_DTE_ADDR + 64 * 4, 0);

    qtest_memset(qts, mmu1_dte_addr, 0, 4096);
    qtest_memset(qts, mmu1_pte_addr, 0, 4096);
    qtest_writel(qts, mmu1_dte_addr + 64 * 4,
                 mmu1_pte_addr | RK_IOMMU_PTE_VALID);
    qtest_writel(qts, mmu1_pte_addr + 0 * 4,
                 RK3588_RKNN_MATMUL_REGCMD_ADDR | RK_IOMMU_PTE_RW);
    qtest_writel(qts, mmu1_pte_addr + 1 * 4,
                 RK3588_RKNN_MATMUL_INPUT_ADDR | RK_IOMMU_PTE_RW);
    qtest_writel(qts, mmu1_pte_addr + 2 * 4,
                 RK3588_RKNN_MATMUL_WEIGHT_ADDR | RK_IOMMU_PTE_RW);
    qtest_writel(qts, mmu1_pte_addr + 3 * 4,
                 RK3588_RKNN_MATMUL_OUTPUT_ADDR0 |
                 RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_READABLE);
    qtest_writel(qts, mmu1_pte_addr + 4 * 4,
                 RK3588_RKNN_MATMUL_OUTPUT_ADDR1 |
                 RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_READABLE);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_INT_MASK,
                 RK_IOMMU_IRQ_PAGE_FAULT);
    qtest_writel(qts, RK3588_RKNN0_MMU1_BASE + RK_IOMMU_DTE_ADDR,
                 mmu1_dte_addr);
    qtest_writel(qts, RK3588_RKNN0_MMU1_BASE + RK_IOMMU_INT_MASK,
                 RK_IOMMU_IRQ_PAGE_FAULT);
    qtest_writel(qts, RK3588_RKNN0_MMU1_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_ENABLE_PAGING);

    rk3588_rknn_run_matmul(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_MMU_BASE +
                                RK_IOMMU_INT_RAWSTAT), ==, 0);
    rk3588_assert_iommu_fault(qts, RK3588_RKNN0_MMU1_BASE,
                              RK3588_RKNN_MATMUL_OUTPUT_IOVA,
                              RK_IOMMU_IRQ_PAGE_FAULT, true);
    g_assert_true(qtest_get_irq(qts, RK3588_RKNN0_SPI));
    qtest_quit(qts);
}

static void test_rk3588_rknpu_start_fetch_error(void)
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

    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_STATUS), ==, 0);
    g_assert_false(qtest_get_irq(qts, RK3588_RKNN0_SPI));

    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    g_assert_cmphex(rk3588_rknn_read_pc(
                        qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==, 0);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_STATUS), ==, 0);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_FETCH_ERROR);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_OPERATION_ENABLE) &
                    RKNN_PC_OPERATION_ENABLE_OP_EN, ==, 0);
    g_assert_false(qtest_get_irq(qts, RK3588_RKNN0_SPI));

    qtest_quit(qts);
}

static void test_rk3588_rknpu_matmul(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    qtest_irq_intercept_in(qts, RK3588_GIC_QOM);
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_MASK,
                 RKNN_DPU_INTERRUPT_BITS);
    rk3588_rknn_start_matmul(qts);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_BASE_ADDRESS,
                 0x20000000);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS, 0);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_OPERATION_ENABLE,
                 RKNN_PC_OPERATION_ENABLE_OP_EN);

    g_assert_cmphex(qtest_readl(qts,
                               RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800), ==,
                    0xa5a5a5a5);
    g_assert_false(qtest_get_irq(qts, RK3588_RKNN0_SPI));
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
    rk3588_rknn_assert_matmul_output(qts);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    RKNN_PIPELINE_BANK1_INTERRUPT);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_STATUS), ==,
                    RKNN_DPU_BANK1_INTERRUPT);
    g_assert_true(qtest_get_irq(qts, RK3588_RKNN0_SPI));

    rk3588_rknn_prepare_matmul(qts, true, 0x5a);
    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_assert_matmul_output(qts);
    qtest_quit(qts);
}

enum {
    RK3588_RKNN_LONG_HEIGHT = 2047,
    RK3588_RKNN_LONG_INPUT_CHANNELS = 1024,
    RK3588_RKNN_LONG_OUTPUT_CHANNELS = 64,
    RK3588_RKNN_LONG_INPUT_BYTES =
        2048 * RK3588_RKNN_LONG_INPUT_CHANNELS,
    RK3588_RKNN_LONG_WEIGHT_BYTES =
        RK3588_RKNN_LONG_INPUT_CHANNELS * RK3588_RKNN_LONG_OUTPUT_CHANNELS,
    RK3588_RKNN_LONG_OUTPUT_BYTES =
        RK3588_RKNN_LONG_HEIGHT * RK3588_RKNN_LONG_OUTPUT_CHANNELS *
        sizeof(uint32_t),
    RK3588_RKNN_LONG_INPUT_PAGES = RK3588_RKNN_LONG_INPUT_BYTES / 4096,
    RK3588_RKNN_LONG_WEIGHT_PAGES = RK3588_RKNN_LONG_WEIGHT_BYTES / 4096,
    RK3588_RKNN_LONG_OUTPUT_PAGES =
        DIV_ROUND_UP(RK3588_RKNN_LONG_OUTPUT_BYTES, 4096),
};

static const uint32_t rk3588_rknn_long_input_iova = 0x10010000;
static const uint32_t rk3588_rknn_long_weight_iova = 0x10210000;
static const uint32_t rk3588_rknn_long_output_iova = 0x10220000;
static const uint64_t rk3588_rknn_long_input_addr =
    RK3588_RAM_BASE + 0x400000;
static const uint64_t rk3588_rknn_long_weight_addr =
    RK3588_RAM_BASE + 0x600000;
static const uint64_t rk3588_rknn_long_output_addr =
    RK3588_RAM_BASE + 0x700000;
static const uint64_t rk3588_rknn_core1_long_output_addr =
    RK3588_RAM_BASE + 0x800000;
static const uint64_t rk3588_rknn_core2_long_output_addr =
    RK3588_RAM_BASE + 0x900000;

static void rk3588_rknn_prepare_long_execution(QTestState *qts,
                                                uint8_t sentinel,
                                                unsigned int core_count)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    g_autofree uint8_t *input = g_malloc0(RK3588_RKNN_LONG_INPUT_BYTES);
    g_autofree uint8_t *weights = g_malloc0(RK3588_RKNN_LONG_WEIGHT_BYTES);
    unsigned int co_work_mode;

    rk3588_rknn_prepare_matmul(qts, true, sentinel);
    rk3588_rknn_make_matmul_regcmd(commands, true);
#define PATCH_RESPONSIVE(_target, _reg, _value) \
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands), \
                             (_target), (_reg), (_value))
    g_assert_cmpuint(core_count, >=, 1);
    g_assert_cmpuint(core_count, <=, 3);
    co_work_mode = core_count == 2 ? 4 : core_count == 3 ? 5 : 0;
    PATCH_RESPONSIVE(0x0201, 0x1010,
                     (co_work_mode << 28) |
                     ((RK3588_RKNN_LONG_HEIGHT + 1) << 4));
    PATCH_RESPONSIVE(0x0201, 0x1020,
                     (1 << 16) | RK3588_RKNN_LONG_HEIGHT);
    PATCH_RESPONSIVE(0x0201, 0x1024,
                     ((RK3588_RKNN_LONG_INPUT_CHANNELS - 1) << 16) |
                     RK3588_RKNN_LONG_INPUT_CHANNELS);
    PATCH_RESPONSIVE(0x0201, 0x102c, RK3588_RKNN_LONG_HEIGHT);
    PATCH_RESPONSIVE(0x0201, 0x1030, RK3588_RKNN_LONG_WEIGHT_BYTES);
    PATCH_RESPONSIVE(0x0201, 0x1034, RK3588_RKNN_LONG_INPUT_CHANNELS);
    PATCH_RESPONSIVE(0x0201, 0x1038,
                     (1 << 24) | (1 << 16) |
                     RK3588_RKNN_LONG_OUTPUT_CHANNELS);
    PATCH_RESPONSIVE(0x0201, 0x1070, rk3588_rknn_long_input_iova);
    PATCH_RESPONSIVE(0x0201, 0x1080, RK3588_RKNN_LONG_HEIGHT - 4);
    PATCH_RESPONSIVE(0x0201, 0x1084,
                     (1 << 16) | RK3588_RKNN_LONG_HEIGHT);
    PATCH_RESPONSIVE(0x0201, 0x1088, RK3588_RKNN_LONG_INPUT_CHANNELS);
    PATCH_RESPONSIVE(0x0201, 0x1110, rk3588_rknn_long_weight_iova);
    PATCH_RESPONSIVE(0x0801, 0x3014,
                     (RK3588_RKNN_LONG_HEIGHT - 1) << 16);
    PATCH_RESPONSIVE(0x0801, 0x3018,
                     RK3588_RKNN_LONG_OUTPUT_CHANNELS - 1);
    PATCH_RESPONSIVE(0x1001, 0x4020, rk3588_rknn_long_output_iova);
    PATCH_RESPONSIVE(0x1001, 0x4024, RK3588_RKNN_LONG_HEIGHT << 4);
    PATCH_RESPONSIVE(0x1001, 0x4034, RK3588_RKNN_LONG_HEIGHT - 1);
    PATCH_RESPONSIVE(0x1001, 0x403c,
                     ((RK3588_RKNN_LONG_OUTPUT_CHANNELS - 1) << 16) |
                     (RK3588_RKNN_LONG_OUTPUT_CHANNELS - 1));
    PATCH_RESPONSIVE(0x1001, 0x4058,
                     RK3588_RKNN_LONG_OUTPUT_CHANNELS - 1);
    PATCH_RESPONSIVE(0x1001, 0x405c,
                     (RK3588_RKNN_LONG_HEIGHT - 1) << 16);
    PATCH_RESPONSIVE(0x1001, 0x4070, 0x302);
    PATCH_RESPONSIVE(0x1001, 0x40c0,
                     (RK3588_RKNN_LONG_HEIGHT * 8) << 4);
    PATCH_RESPONSIVE(0x1001, 0x4108, 0x68);
    PATCH_RESPONSIVE(0x1001, 0x410c, 0x00050500);
    PATCH_RESPONSIVE(0x1001, 0x4110, 0xffffc000);
    PATCH_RESPONSIVE(0x1001, 0x4114, 0);
    PATCH_RESPONSIVE(0x1001, 0x4118, 0);
    PATCH_RESPONSIVE(0x1001, 0x411c, 0x00004000);
    PATCH_RESPONSIVE(0x1001, 0x4120, 0);
    PATCH_RESPONSIVE(0x1001, 0x4124, 0);
    PATCH_RESPONSIVE(0x1001, 0x4128, 0);
    PATCH_RESPONSIVE(0x1001, 0x412c, 0);
#undef PATCH_RESPONSIVE

    for (unsigned int page = 0; page < RK3588_RKNN_LONG_INPUT_PAGES; page++) {
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + (16 + page) * 4,
                     (rk3588_rknn_long_input_addr + page * 4096) |
                     RK_IOMMU_PTE_RW);
    }
    for (unsigned int page = 0; page < RK3588_RKNN_LONG_WEIGHT_PAGES;
         page++) {
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR +
                          (528 + page) * 4,
                     (rk3588_rknn_long_weight_addr + page * 4096) |
                     RK_IOMMU_PTE_RW);
    }
    for (unsigned int page = 0; page < RK3588_RKNN_LONG_OUTPUT_PAGES;
         page++) {
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR +
                          (544 + page) * 4,
                     (rk3588_rknn_long_output_addr + page * 4096) |
                     RK_IOMMU_PTE_RW);
    }
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                   commands, sizeof(commands));
    qtest_memwrite(qts, rk3588_rknn_long_input_addr, input,
                   RK3588_RKNN_LONG_INPUT_BYTES);
    qtest_memwrite(qts, rk3588_rknn_long_weight_addr, weights,
                   RK3588_RKNN_LONG_WEIGHT_BYTES);
    qtest_memset(qts, rk3588_rknn_long_output_addr, sentinel,
                 RK3588_RKNN_LONG_OUTPUT_PAGES * 4096);
    qtest_writel(qts, RK3588_RKNN0_DPU_BASE + 0x100, 0x00030000);
    qtest_writel(qts, RK3588_RKNN0_DPU_BASE + 0x104, 0x1234);
}

static void rk3588_rknn_assert_long_execution_output(QTestState *qts)
{
    g_assert_cmphex(qtest_readl(qts, rk3588_rknn_long_output_addr), ==,
                    0x1234);
    g_assert_cmphex(qtest_readl(qts, rk3588_rknn_long_output_addr +
                                RK3588_RKNN_LONG_OUTPUT_BYTES / 2), ==,
                    0x1234);
    g_assert_cmphex(qtest_readl(qts, rk3588_rknn_long_output_addr +
                                RK3588_RKNN_LONG_OUTPUT_BYTES - 4), ==,
                    0x1234);
    g_assert_cmphex(qtest_readl(qts, rk3588_rknn_long_output_addr +
                                RK3588_RKNN_LONG_OUTPUT_BYTES), ==,
                    0xa5a5a5a5);
}

static uint64_t rk3588_rknn_peer_long_output_addr(unsigned int core)
{
    g_assert_cmpuint(core, >=, 1);
    g_assert_cmpuint(core, <=, 2);

    return core == 1 ? rk3588_rknn_core1_long_output_addr :
                       rk3588_rknn_core2_long_output_addr;
}

static void rk3588_rknn_prepare_peer_long_execution(QTestState *qts,
                                                     unsigned int core)
{
    static const uint64_t dte_addr[] = {
        0, RK3588_RKNN_CORE1_DTE_ADDR, RK3588_RKNN_CORE2_DTE_ADDR,
    };
    static const uint64_t pte_addr[] = {
        0, RK3588_RKNN_CORE1_PTE_ADDR, RK3588_RKNN_CORE2_PTE_ADDR,
    };
    static const uint64_t mmu_base[] = {
        0, RK3588_RKNN1_MMU_BASE, RK3588_RKNN2_MMU_BASE,
    };
    static const uint64_t pc_base[] = {
        0, RK3588_RKNN1_PC_BASE, RK3588_RKNN2_PC_BASE,
    };
    static const uint64_t dpu_base[] = {
        0, RK3588_RKNN1_DPU_BASE, RK3588_RKNN2_DPU_BASE,
    };
    uint64_t output_addr = rk3588_rknn_peer_long_output_addr(core);

    qtest_writel(qts, dte_addr[core] + 64 * 4, pte_addr[core] | 1);
    qtest_writel(qts, pte_addr[core],
                 RK3588_RKNN_MATMUL_REGCMD_ADDR | RK_IOMMU_PTE_RW);
    for (unsigned int page = 0; page < RK3588_RKNN_LONG_INPUT_PAGES; page++) {
        qtest_writel(qts, pte_addr[core] + (16 + page) * 4,
                     (rk3588_rknn_long_input_addr + page * 4096) |
                     RK_IOMMU_PTE_RW);
    }
    for (unsigned int page = 0; page < RK3588_RKNN_LONG_WEIGHT_PAGES;
         page++) {
        qtest_writel(qts, pte_addr[core] + (528 + page) * 4,
                     (rk3588_rknn_long_weight_addr + page * 4096) |
                     RK_IOMMU_PTE_RW);
    }
    for (unsigned int page = 0; page < RK3588_RKNN_LONG_OUTPUT_PAGES;
         page++) {
        qtest_writel(qts, pte_addr[core] + (544 + page) * 4,
                     (output_addr + page * 4096) |
                     RK_IOMMU_PTE_RW);
    }
    qtest_memset(qts, output_addr, 0xa5,
                 RK3588_RKNN_LONG_OUTPUT_PAGES * 4096);
    qtest_writel(qts, mmu_base[core] + RK_IOMMU_DTE_ADDR, dte_addr[core]);
    qtest_writel(qts, mmu_base[core] + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_ENABLE_PAGING);
    qtest_writel(qts, pc_base[core] + RKNN_PC_BASE_ADDRESS,
                 RK3588_RKNN_MATMUL_REGCMD_IOVA);
    qtest_writel(qts, pc_base[core] + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(RK3588_RKNN_MATMUL_COMMANDS));
    qtest_writel(qts, dpu_base[core] + 0x100, 0x00030000);
    qtest_writel(qts, dpu_base[core] + 0x104, 0x1234);
}

static void rk3588_rknn_assert_peer_long_execution_output(QTestState *qts,
                                                          unsigned int core)
{
    uint64_t output_addr = rk3588_rknn_peer_long_output_addr(core);

    g_assert_cmphex(qtest_readl(qts, output_addr), ==,
                    0x1234);
    g_assert_cmphex(qtest_readl(qts, output_addr +
                               RK3588_RKNN_LONG_OUTPUT_BYTES / 2), ==,
                    0x1234);
    g_assert_cmphex(qtest_readl(qts, output_addr +
                               RK3588_RKNN_LONG_OUTPUT_BYTES - 4), ==,
                    0x1234);
    g_assert_cmphex(qtest_readl(qts, output_addr +
                               RK3588_RKNN_LONG_OUTPUT_BYTES), ==,
                    0xa5a5a5a5);
}

static void rk3588_rknn_wait_long_execution_output(QTestState *qts,
                                                    uint64_t output_addr)
{
    gint64 deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    while (qtest_readl(qts, output_addr +
                       RK3588_RKNN_LONG_OUTPUT_BYTES - 4) != 0x1234) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        qtest_clock_step(qts, 1);
        g_usleep(100);
    }
    for (unsigned int i = 0; i < 100; i++) {
        qtest_clock_step(qts, 1);
        g_usleep(100);
    }
}

static void rk3588_rknn_start_peer(QTestState *qts, unsigned int core)
{
    static const uint64_t pc_base[] = {
        0, RK3588_RKNN1_PC_BASE, RK3588_RKNN2_PC_BASE,
    };

    g_assert_cmpuint(core, >=, 1);
    g_assert_cmpuint(core, <=, 2);
    qtest_writel(qts, pc_base[core] + RKNN_PC_TASK_CON, 0x00003001);
    qtest_writel(qts, pc_base[core] + RKNN_PC_OPERATION_ENABLE,
                 RKNN_PC_OPERATION_ENABLE_OP_EN);
}

static void test_rk3588_rknpu_multicore_delayed_start_parallel(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_long_execution(qts, 0xa5, 1);
    rk3588_rknn_prepare_peer_long_execution(qts, 1);
    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    g_assert_true(qtest_qom_get_bool(qts, RK3588_RKNN0_QOM,
                                     "x-execution-active"));

    rk3588_rknn_start_peer(qts, 1);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    g_assert_true(qtest_qom_get_bool(qts, RK3588_RKNN0_QOM,
                                     "x-execution-active"));
    g_assert_true(qtest_qom_get_bool(qts, RK3588_RKNN1_QOM,
                                     "x-execution-active"));
    rk3588_rknn_wait_core_idle(qts, RK3588_RKNN0_QOM);
    rk3588_rknn_wait_core_idle(qts, RK3588_RKNN1_QOM);
    rk3588_rknn_assert_long_execution_output(qts);
    rk3588_rknn_assert_peer_long_execution_output(qts, 1);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN1_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_two_core_rendezvous(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_long_execution(qts, 0xa5, 2);
    rk3588_rknn_prepare_peer_long_execution(qts, 1);
    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_long_execution_output(qts,
                                           rk3588_rknn_long_output_addr);
    g_assert_true(qtest_qom_get_bool(qts, RK3588_RKNN0_QOM,
                                     "x-execution-active"));
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==, 0);

    rk3588_rknn_start_peer(qts, 1);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_core_idle(qts, RK3588_RKNN0_QOM);
    rk3588_rknn_wait_core_idle(qts, RK3588_RKNN1_QOM);
    rk3588_rknn_assert_long_execution_output(qts);
    rk3588_rknn_assert_peer_long_execution_output(qts, 1);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN1_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_three_core_rendezvous(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_long_execution(qts, 0xa5, 3);
    rk3588_rknn_prepare_peer_long_execution(qts, 1);
    rk3588_rknn_prepare_peer_long_execution(qts, 2);
    rk3588_rknn_start_matmul(qts);
    rk3588_rknn_start_peer(qts, 1);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_long_execution_output(qts,
                                           rk3588_rknn_long_output_addr);
    rk3588_rknn_wait_long_execution_output(
        qts, rk3588_rknn_peer_long_output_addr(1));
    g_assert_true(qtest_qom_get_bool(qts, RK3588_RKNN0_QOM,
                                     "x-execution-active"));
    g_assert_true(qtest_qom_get_bool(qts, RK3588_RKNN1_QOM,
                                     "x-execution-active"));
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN1_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==, 0);

    rk3588_rknn_start_peer(qts, 2);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_core_idle(qts, RK3588_RKNN0_QOM);
    rk3588_rknn_wait_core_idle(qts, RK3588_RKNN1_QOM);
    rk3588_rknn_wait_core_idle(qts, RK3588_RKNN2_QOM);
    rk3588_rknn_assert_long_execution_output(qts);
    rk3588_rknn_assert_peer_long_execution_output(qts, 1);
    rk3588_rknn_assert_peer_long_execution_output(qts, 2);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_execution_responsive(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_long_execution(qts, 0xa5, 1);

    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    g_assert_true(qtest_qom_get_bool(qts, RK3588_RKNN0_QOM,
                                     "x-execution-active"));
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_OPERATION_ENABLE) &
                    RKNN_PC_OPERATION_ENABLE_OP_EN, ==,
                    RKNN_PC_OPERATION_ENABLE_OP_EN);
    qtest_qmp_assert_success(qts, "{ 'execute': 'query-status' }");
    g_assert_true(qtest_qom_get_bool(qts, RK3588_RKNN0_QOM,
                                     "x-execution-active"));
    qtest_writel(qts, RK3588_RKNN0_DPU_BASE + 0x100, 0x00030000);
    qtest_writel(qts, RK3588_RKNN0_DPU_BASE + 0x104, 0x5678);

    rk3588_rknn_wait_idle(qts);
    rk3588_rknn_assert_long_execution_output(qts);
    rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_SUCCESS,
                          RKNN_PIPELINE_BANK1_INTERRUPT);

    qtest_memset(qts, rk3588_rknn_long_output_addr, 0xa5,
                 RK3588_RKNN_LONG_OUTPUT_PAGES * 4096);
    rk3588_rknn_start_matmul(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    g_assert_true(qtest_qom_get_bool(qts, RK3588_RKNN0_QOM,
                                     "x-execution-active"));
    rk3588_cru_rknpu_reset_pulse(
        qts, RK3588_CRU_SOFTRST_CON(30), BIT(6));
    g_assert_false(qtest_qom_get_bool(qts, RK3588_RKNN0_QOM,
                                      "x-execution-active"));
    rk3588_rknn_assert_pc(qts, 0, 0);
    qtest_quit(qts);
}

static void rk3588_rknn_write_slave_commands(QTestState *qts,
                                             const uint64_t commands[],
                                             size_t count,
                                             uint32_t pointer)
{
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_BASE_ADDRESS, 1);
    qtest_writel(qts, RK3588_RKNN0_CNA_BASE + RKNN_POINTER, pointer);
    qtest_writel(qts, RK3588_RKNN0_CORE_BASE + RKNN_POINTER, pointer);
    for (unsigned int i = 0; i < count; i++) {
        uint64_t command = le64_to_cpu(commands[i]);
        uint32_t target = command >> 48;
        uint32_t reg = command & 0xffff;
        uint32_t value = command >> 16;

        if (target == 0x0201 || target == 0x0801 || target == 0x1001) {
            if (reg == 0x4004) {
                value = pointer;
            }
            qtest_writel(qts, RK3588_RKNN0_PC_BASE + reg, value);
        }
    }
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_MASK,
                 RKNN_DPU_INTERRUPT_BITS);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_CLEAR,
                 UINT32_MAX);
    qtest_writel(qts, RK3588_RKNN0_GLOBAL_BASE +
                 RKNN_GLOBAL_OPERATION_ENABLE, 0x0d);
    qtest_writel(qts, RK3588_RKNN0_GLOBAL_BASE +
                 RKNN_GLOBAL_OPERATION_ENABLE, 0);
}

static void rk3588_rknn_write_slave_matmul(QTestState *qts, uint32_t pointer)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];

    rk3588_rknn_make_matmul_regcmd(commands, true);
    rk3588_rknn_write_slave_commands(qts, commands, ARRAY_SIZE(commands),
                                     pointer);
}

static void test_rk3588_rknpu_matmul_slave(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    qtest_irq_intercept_in(qts, RK3588_GIC_QOM);
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_write_slave_matmul(qts, 0x0e);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
    rk3588_rknn_assert_matmul_output(qts);
    rk3588_rknn_assert_pc(qts, 0x5000, RKNN_PIPELINE_BANK1_INTERRUPT);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_STATUS), ==,
                    RKNN_DPU_BANK1_INTERRUPT);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0x0001000e);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CORE_BASE +
                                RKNN_POINTER), ==, 0x0001000e);
    g_assert_true(qtest_get_irq(qts, RK3588_RKNN0_SPI));

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_write_slave_matmul(qts, 0x0f);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
    rk3588_rknn_assert_matmul_output(qts);
    rk3588_rknn_assert_pc(qts, 0x5000, RKNN_PIPELINE_BANK0_INTERRUPT);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_STATUS), ==,
                    RKNN_DPU_BANK0_INTERRUPT);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0x0000000f);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CORE_BASE +
                                RKNN_POINTER), ==, 0x0000000f);
    g_assert_true(qtest_get_irq(qts, RK3588_RKNN0_SPI));
    qtest_quit(qts);
}

typedef enum RK3588RKNNSynthConvBuffer {
    RK3588_RKNN_SYNTH_CONV_REGCMD,
    RK3588_RKNN_SYNTH_CONV_INPUT,
    RK3588_RKNN_SYNTH_CONV_WEIGHT,
    RK3588_RKNN_SYNTH_CONV_BS,
    RK3588_RKNN_SYNTH_CONV_OUTPUT,
} RK3588RKNNSynthConvBuffer;

typedef struct RK3588RKNNSynthConvFixture {
    uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
    int8_t input[RK3588_RKNN_SYNTH_CONV_INPUT_BYTES];
    int8_t weights[RK3588_RKNN_SYNTH_CONV_WEIGHT_BYTES];
    uint8_t bs[RK3588_RKNN_SYNTH_CONV_BS_BYTES];
    int8_t output[RK3588_RKNN_SYNTH_CONV_OUTPUT_BYTES];
} RK3588RKNNSynthConvFixture;

static size_t rk3588_rknn_synth_conv_feature_index(unsigned int channel,
                                                    unsigned int row,
                                                    unsigned int column)
{
    return (channel / RK3588_RKNN_SYNTH_CONV_ATOM) *
           RK3588_RKNN_SYNTH_CONV_HEIGHT *
           RK3588_RKNN_SYNTH_CONV_WIDTH *
           RK3588_RKNN_SYNTH_CONV_ATOM +
           (row * RK3588_RKNN_SYNTH_CONV_WIDTH + column) *
           RK3588_RKNN_SYNTH_CONV_ATOM +
           channel % RK3588_RKNN_SYNTH_CONV_ATOM;
}

static void rk3588_rknn_make_synth_conv_regcmd(uint64_t commands[])
{
    uint64_t baseline[RK3588_RKNN_DPU_RDMA_FP16_COMMANDS];

    rk3588_rknn_make_dpu_rdma_fp16_regcmd(baseline);
    memcpy(commands, baseline, (ARRAY_SIZE(baseline) - 3) *
                               sizeof(*commands));
    commands[ARRAY_SIZE(baseline) - 3] =
        rk3588_rknn_regcmd(0x2001, 0x502c, 0);
    commands[ARRAY_SIZE(baseline) - 2] =
        baseline[ARRAY_SIZE(baseline) - 3];
    commands[ARRAY_SIZE(baseline) - 1] =
        rk3588_rknn_regcmd(0x0201, 0x1060, 0);
    memcpy(commands + ARRAY_SIZE(baseline),
           baseline + ARRAY_SIZE(baseline) - 2,
           2 * sizeof(*commands));

#define PATCH_SYNTH_CONV(_target, _reg, _value) do {                 \
    rk3588_rknn_patch_regcmd(                                        \
        commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,                   \
        (_target), (_reg), (_value));                                \
} while (0)
    PATCH_SYNTH_CONV(0x0201, 0x100c, 0);
    PATCH_SYNTH_CONV(0x0201, 0x1010,
                     (RK3588_RKNN_SYNTH_CONV_HEIGHT + 1) << 4);
    PATCH_SYNTH_CONV(0x0201, 0x1014, 0x09);
    PATCH_SYNTH_CONV(0x0201, 0x1020,
                     (RK3588_RKNN_SYNTH_CONV_WIDTH << 16) |
                     RK3588_RKNN_SYNTH_CONV_HEIGHT);
    PATCH_SYNTH_CONV(0x0201, 0x1024,
                     ((RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS - 1) << 16) |
                     RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS);
    PATCH_SYNTH_CONV(0x0201, 0x1028, RK3588_RKNN_SYNTH_CONV_WIDTH);
    PATCH_SYNTH_CONV(0x0201, 0x102c,
                     RK3588_RKNN_SYNTH_CONV_WIDTH *
                     RK3588_RKNN_SYNTH_CONV_HEIGHT);
    PATCH_SYNTH_CONV(0x0201, 0x1030,
                     RK3588_RKNN_SYNTH_CONV_WEIGHT_BYTES);
    PATCH_SYNTH_CONV(0x0201, 0x1034,
                     RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS);
    PATCH_SYNTH_CONV(0x0201, 0x1038,
                     (1U << 24) | (1U << 16) |
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS);
    PATCH_SYNTH_CONV(0x0201, 0x1040, 0x201b);
    PATCH_SYNTH_CONV(0x0201, 0x1044, 0x38);
    PATCH_SYNTH_CONV(0x0201, 0x104c, 0x0b);
    for (uint32_t reg = 0x1050; reg <= 0x105c; reg += 4) {
        PATCH_SYNTH_CONV(0x0201, reg, 1U << 16);
    }
    PATCH_SYNTH_CONV(0x0201, 0x1070,
                     RK3588_RKNN_SYNTH_CONV_INPUT_IOVA);
    PATCH_SYNTH_CONV(0x0201, 0x1078, 0x000f000f);
    PATCH_SYNTH_CONV(0x0201, 0x107c,
                     RK3588_RKNN_SYNTH_CONV_WIDTH * 4);
    PATCH_SYNTH_CONV(0x0201, 0x1080,
                     RK3588_RKNN_SYNTH_CONV_SURFACE_BYTES / 16 -
                     RK3588_RKNN_SYNTH_CONV_WIDTH * 4);
    PATCH_SYNTH_CONV(0x0201, 0x1084,
                     (RK3588_RKNN_SYNTH_CONV_WIDTH << 16) |
                     RK3588_RKNN_SYNTH_CONV_HEIGHT);
    PATCH_SYNTH_CONV(0x0201, 0x1088,
                     RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS);
    PATCH_SYNTH_CONV(0x0201, 0x1110,
                     RK3588_RKNN_SYNTH_CONV_WEIGHT_IOVA);
    PATCH_SYNTH_CONV(0x0201, 0x1180, 0);
    PATCH_SYNTH_CONV(0x0201, 0x1184, UINT32_C(0xffffff80));

    PATCH_SYNTH_CONV(0x0801, 0x3010, 1);
    PATCH_SYNTH_CONV(0x0801, 0x3014,
                     ((RK3588_RKNN_SYNTH_CONV_HEIGHT - 1) << 16) |
                     (RK3588_RKNN_SYNTH_CONV_WIDTH - 1));
    PATCH_SYNTH_CONV(0x0801, 0x3018,
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS - 1);
    PATCH_SYNTH_CONV(0x0801, 0x301c, 0);

    PATCH_SYNTH_CONV(0x1001, 0x400c, 0x1e4);
    PATCH_SYNTH_CONV(0x1001, 0x4010, 0xe0);
    PATCH_SYNTH_CONV(0x1001, 0x4020,
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA);
    PATCH_SYNTH_CONV(0x1001, 0x4024,
                     RK3588_RKNN_SYNTH_CONV_SURFACE_BYTES);
    PATCH_SYNTH_CONV(0x1001, 0x4030,
                     RK3588_RKNN_SYNTH_CONV_WIDTH - 1);
    PATCH_SYNTH_CONV(0x1001, 0x4034,
                     RK3588_RKNN_SYNTH_CONV_HEIGHT - 1);
    PATCH_SYNTH_CONV(0x1001, 0x403c,
                     ((RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS - 1) << 16) |
                     (RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS - 1));
    PATCH_SYNTH_CONV(0x1001, 0x4040, 0x20140);
    PATCH_SYNTH_CONV(0x1001, 0x4048, 0xe01);
    PATCH_SYNTH_CONV(0x1001, 0x4050, 0x125);
    PATCH_SYNTH_CONV(0x1001, 0x4054, 0);
    PATCH_SYNTH_CONV(0x1001, 0x4058,
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS - 1);
    PATCH_SYNTH_CONV(0x1001, 0x405c,
                     ((RK3588_RKNN_SYNTH_CONV_HEIGHT - 1) << 16) |
                     (RK3588_RKNN_SYNTH_CONV_WIDTH - 1));
    PATCH_SYNTH_CONV(0x1001, 0x4060, 0x92);
    PATCH_SYNTH_CONV(0x1001, 0x406c, 0x2faf);
    PATCH_SYNTH_CONV(0x1001, 0x4070, 0x383);
    PATCH_SYNTH_CONV(0x1001, 0x4074, 0);
    PATCH_SYNTH_CONV(0x1001, 0x4078, 1);
    PATCH_SYNTH_CONV(0x1001, 0x4080, 0);
    PATCH_SYNTH_CONV(0x1001, 0x4084, 1);
    PATCH_SYNTH_CONV(0x1001, 0x4088, 0);
    PATCH_SYNTH_CONV(0x1001, 0x40c0, 0x62000);

    PATCH_SYNTH_CONV(0x2001, 0x500c,
                     RK3588_RKNN_SYNTH_CONV_WIDTH - 1);
    PATCH_SYNTH_CONV(0x2001, 0x5010,
                     RK3588_RKNN_SYNTH_CONV_HEIGHT - 1);
    PATCH_SYNTH_CONV(0x2001, 0x5014,
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS - 1);
    PATCH_SYNTH_CONV(0x2001, 0x5018, 0);
    PATCH_SYNTH_CONV(0x2001, 0x501c, 0xe);
    PATCH_SYNTH_CONV(0x2001, 0x5020,
                     RK3588_RKNN_SYNTH_CONV_BS_IOVA);
    PATCH_SYNTH_CONV(0x2001, 0x5028, 0);
    PATCH_SYNTH_CONV(0x2001, 0x502c, 0);
    PATCH_SYNTH_CONV(0x2001, 0x5034, 1);
    PATCH_SYNTH_CONV(0x2001, 0x5038, 0);
    PATCH_SYNTH_CONV(0x2001, 0x5040, 0);
    PATCH_SYNTH_CONV(0x2001, 0x5044, 0x7810);
    PATCH_SYNTH_CONV(0x2001, 0x5048, 0);
    PATCH_SYNTH_CONV(0x2001, 0x504c, 0);
    PATCH_SYNTH_CONV(0x2001, 0x5064, 0);
    PATCH_SYNTH_CONV(0x2001, 0x5068, 0x01010101);
    PATCH_SYNTH_CONV(0x2001, 0x506c, 0);
    PATCH_SYNTH_CONV(0x0081, RKNN_PC_OPERATION_ENABLE, 0x1d);
#undef PATCH_SYNTH_CONV
}

static void rk3588_rknn_init_synth_conv_fixture(
    RK3588RKNNSynthConvFixture *fixture)
{
    rk3588_rknn_make_synth_conv_regcmd(fixture->commands);
    for (unsigned int row = 0;
         row < RK3588_RKNN_SYNTH_CONV_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_SYNTH_CONV_WIDTH; column++) {
            int qd_sum = 0;

            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS;
                 channel++) {
                size_t index = rk3588_rknn_synth_conv_feature_index(
                    channel, row, column);
                int8_t value = ((row * 5 + column * 3 + channel) % 7) - 3;

                fixture->input[index] = value;
                qd_sum += value;
            }
            for (unsigned int output = 0;
                 output < RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS;
                 output++) {
                unsigned int group = output / 8;
                unsigned int lane = output % 8;
                int32_t alu = (int32_t)(output % 3) - 1;
                uint16_t cpend = output & 1;
                int value = fixture->input[
                    rk3588_rknn_synth_conv_feature_index(
                        output % RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS,
                        row, column)];
                size_t output_index = rk3588_rknn_synth_conv_feature_index(
                    output, row, column);

                value += cpend * qd_sum + alu;
                fixture->output[output_index] =
                    CLAMP(value, 0, INT8_MAX);
                stl_le_p(fixture->bs + group * 0x40 + lane * 4, alu);
                stw_le_p(fixture->bs + group * 0x40 + 0x20 + lane * 2,
                         cpend);
                stw_le_p(fixture->bs + group * 0x40 + 0x30 + lane * 2,
                         1 << 14);
            }
        }
    }
    for (unsigned int output = 0;
         output < RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS; output++) {
        fixture->weights[
            output * RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS +
            output % RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS] = 1;
    }
}

static void rk3588_rknn_make_synth_conv_buffer(
    RK3588RKNNSynthConvBuffer kind, void **data, gsize *length)
{
    g_autofree RK3588RKNNSynthConvFixture *fixture =
        g_new0(RK3588RKNNSynthConvFixture, 1);
    const void *source;

    rk3588_rknn_init_synth_conv_fixture(fixture);
    switch (kind) {
    case RK3588_RKNN_SYNTH_CONV_REGCMD:
        source = fixture->commands;
        *length = sizeof(fixture->commands);
        break;
    case RK3588_RKNN_SYNTH_CONV_INPUT:
        source = fixture->input;
        *length = sizeof(fixture->input);
        break;
    case RK3588_RKNN_SYNTH_CONV_WEIGHT:
        source = fixture->weights;
        *length = sizeof(fixture->weights);
        break;
    case RK3588_RKNN_SYNTH_CONV_BS:
        source = fixture->bs;
        *length = sizeof(fixture->bs);
        break;
    case RK3588_RKNN_SYNTH_CONV_OUTPUT:
        source = fixture->output;
        *length = sizeof(fixture->output);
        break;
    default:
        g_assert_not_reached();
    }
    *data = g_memdup2(source, *length);
}

static uint64_t rk3588_rknn_synth_conv_addr(uint32_t iova)
{
    return RK3588_RKNN_SYNTH_CONV_ADDR +
           iova - RK3588_RKNN_SYNTH_CONV_REGCMD_IOVA;
}

static uint32_t rk3588_rknn_regcmd_value(const uint64_t commands[],
                                         size_t command_count,
                                         uint32_t target, uint32_t reg);

static void rk3588_rknn_prepare_synth_conv(
    QTestState *qts, uint64_t *commands, size_t command_count,
    const uint8_t *input, size_t input_length,
    const uint8_t *weights, size_t weights_length,
    const uint8_t *bs, size_t bs_length, uint32_t height,
    uint32_t brdma_cfg)
{
    const size_t input_compact_surface_bytes =
        RK3588_RKNN_SYNTH_CONV_WIDTH *
        height *
        RK3588_RKNN_SYNTH_CONV_ATOM;
    const size_t input_storage_channels =
        input_length / (RK3588_RKNN_SYNTH_CONV_WIDTH * height);
    uint32_t input_surface_delta;
    int32_t signed_input_surface_delta;
    size_t input_dma_surface_bytes;
    size_t index;

    g_assert_cmpuint(height, >, 0);
#define PATCH(_target, _reg, _value) do {                              \
    index = rk3588_rknn_find_regcmd(commands, command_count,            \
                                    (_target), (_reg));                 \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    PATCH(0x0201, 0x1070, RK3588_RKNN_SYNTH_CONV_INPUT_IOVA);
    PATCH(0x0201, 0x1110, RK3588_RKNN_SYNTH_CONV_WEIGHT_IOVA);
    PATCH(0x0201, 0x1020,
          (RK3588_RKNN_SYNTH_CONV_WIDTH << 16) | height);
    PATCH(0x0201, 0x1010, (height + 1) << 4);
    PATCH(0x0201, 0x102c,
          RK3588_RKNN_SYNTH_CONV_WIDTH * height);
    PATCH(0x0201, 0x1084,
          (RK3588_RKNN_SYNTH_CONV_WIDTH << 16) | height);
    PATCH(0x0801, 0x3014,
          ((height - 1) << 16) |
          (RK3588_RKNN_SYNTH_CONV_WIDTH - 1));
    PATCH(0x1001, 0x4020, RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA);
    PATCH(0x1001, 0x4034, height - 1);
    PATCH(0x1001, 0x405c,
          ((height - 1) << 16) |
          (RK3588_RKNN_SYNTH_CONV_WIDTH - 1));
    PATCH(0x2001, 0x501c, brdma_cfg);
    PATCH(0x2001, 0x5020, RK3588_RKNN_SYNTH_CONV_BS_IOVA);
    PATCH(0x2001, 0x5010, height - 1);
#undef PATCH

    input_surface_delta = rk3588_rknn_regcmd_value(
        commands, command_count, 0x0201, 0x1080);
    signed_input_surface_delta = (int32_t)(input_surface_delta << 4) >> 4;
    input_dma_surface_bytes =
        (signed_input_surface_delta + rk3588_rknn_regcmd_value(
             commands, command_count, 0x0201, 0x107c)) *
        RK3588_RKNN_SYNTH_CONV_ATOM;

    g_assert_cmpuint(input_storage_channels, >, 0);
    g_assert_cmpuint(input_storage_channels %
                     RK3588_RKNN_SYNTH_CONV_ATOM, ==, 0);
    g_assert_cmpuint(input_length, ==,
                     RK3588_RKNN_SYNTH_CONV_WIDTH * height *
                     input_storage_channels);
    g_assert_cmpuint(weights_length, ==,
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS *
                     input_storage_channels);
    g_assert_cmpuint(bs_length, ==, 64 * 8);
    qtest_writel(qts, RK3588_RKNN_MATMUL_DTE_ADDR + 64 * 4,
                 RK3588_RKNN_MATMUL_PTE_ADDR | RK_IOMMU_PTE_VALID);
    for (unsigned int page = 0;
        page < RK3588_RKNN_SYNTH_CONV_MAPPED_PAGES; page++) {
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + page * 4,
                     (RK3588_RKNN_SYNTH_CONV_ADDR + page * 0x1000) |
                     RK_IOMMU_PTE_RW);
    }
    qtest_memwrite(qts, RK3588_RKNN_SYNTH_CONV_ADDR, commands,
                   command_count * sizeof(*commands));
    for (unsigned int surface = 0;
         surface < input_storage_channels /
                   RK3588_RKNN_SYNTH_CONV_ATOM; surface++) {
        qtest_memwrite(
            qts,
            rk3588_rknn_synth_conv_addr(
                RK3588_RKNN_SYNTH_CONV_INPUT_IOVA) +
                surface * input_dma_surface_bytes,
            input + surface * input_compact_surface_bytes,
            input_compact_surface_bytes);
    }
    qtest_memwrite(qts, rk3588_rknn_synth_conv_addr(
                       RK3588_RKNN_SYNTH_CONV_WEIGHT_IOVA),
                   weights, weights_length);
    qtest_memwrite(qts, rk3588_rknn_synth_conv_addr(
                       RK3588_RKNN_SYNTH_CONV_BS_IOVA),
                   bs, bs_length);
    qtest_memset(qts, rk3588_rknn_synth_conv_addr(
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA),
                 0xa5,
                 RK3588_RKNN_SYNTH_CONV_MAPPED_PAGES * 0x1000 -
                 (RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA -
                  RK3588_RKNN_SYNTH_CONV_REGCMD_IOVA));
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_DTE_ADDR,
                 RK3588_RKNN_MATMUL_DTE_ADDR);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_ENABLE_PAGING);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_BASE_ADDRESS,
                 RK3588_RKNN_SYNTH_CONV_REGCMD_IOVA);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(command_count));
}

static void rk3588_rknn_read_synth_conv_output(
    QTestState *qts, uint8_t *output, size_t output_length, uint32_t height)
{
    const size_t surface_bytes =
        RK3588_RKNN_SYNTH_CONV_WIDTH *
        height *
        RK3588_RKNN_SYNTH_CONV_ATOM;

    g_assert_cmpuint(output_length, ==,
                     RK3588_RKNN_SYNTH_CONV_WIDTH *
                     height *
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS);
    for (unsigned int surface = 0;
         surface < RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS /
                   RK3588_RKNN_SYNTH_CONV_ATOM; surface++) {
        qtest_memread(
            qts,
            rk3588_rknn_synth_conv_addr(
                RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA) +
                surface * RK3588_RKNN_SYNTH_CONV_SURFACE_BYTES,
            output + surface * surface_bytes, surface_bytes);
        if (surface + 1 <
            RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS /
            RK3588_RKNN_SYNTH_CONV_ATOM) {
            g_assert_cmphex(qtest_readb(
                qts,
                rk3588_rknn_synth_conv_addr(
                    RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA) +
                    surface * RK3588_RKNN_SYNTH_CONV_SURFACE_BYTES +
                    surface_bytes), ==, 0xa5);
        }
    }
}

static size_t rk3588_rknn_rgb_cvt_weight_index(unsigned int output,
                                                unsigned int kernel,
                                                unsigned int channel)
{
    return (kernel * RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS + output) *
           RK3588_RKNN_RGB_CVT_STORAGE_CHANNELS + channel;
}

static void rk3588_rknn_patch_all_regcmd(uint64_t commands[], size_t count,
                                         uint32_t target, uint32_t reg,
                                         uint32_t value)
{
    bool found = false;

    for (size_t index = 0; index < count; index++) {
        uint64_t command = le64_to_cpu(commands[index]);

        if ((command >> 48) == target && (command & 0xffff) == reg) {
            commands[index] = rk3588_rknn_regcmd(target, reg, value);
            found = true;
        }
    }
    g_assert_true(found);
}

static uint32_t rk3588_rknn_regcmd_value(const uint64_t commands[],
                                         size_t count, uint32_t target,
                                         uint32_t reg)
{
    bool found = false;
    uint32_t value = 0;

    for (size_t index = 0; index < count; index++) {
        uint64_t command = le64_to_cpu(commands[index]);

        if ((command >> 48) == target && (command & 0xffff) == reg) {
            value = (command >> 16) & UINT32_MAX;
            found = true;
        }
    }
    g_assert_true(found);
    return value;
}

static int64_t rk3588_rknn_rgb_cvt_round_shift(int64_t value,
                                                unsigned int shift,
                                                bool ties_away)
{
    bool negative = value < 0;
    uint64_t magnitude = negative ? -value : value;
    uint64_t quotient;
    uint64_t remainder;
    uint64_t half;

    if (!shift) {
        return value;
    }
    quotient = magnitude >> shift;
    remainder = magnitude & ((UINT64_C(1) << shift) - 1);
    half = UINT64_C(1) << (shift - 1);
    if (remainder > half ||
        (remainder == half && (ties_away || (quotient & 1)))) {
        quotient++;
    }
    return negative ? -(int64_t)quotient : quotient;
}

static int8_t rk3588_rknn_rgb_cvt_convert(
    const uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS],
    unsigned int channel, uint8_t raw)
{
    uint32_t cfg = rk3588_rknn_regcmd_value(
        commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,
        0x0201, 0x104c);
    uint32_t channel_cfg = rk3588_rknn_regcmd_value(
        commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,
        0x0201, 0x1050 + channel * 4);
    int input = cfg & (1U << 3) ? (int8_t)raw : raw;
    int16_t offset = channel_cfg;
    int16_t scale = channel_cfg >> 16;
    unsigned int shift = (cfg >> (4 + channel * 6)) & 0x3f;
    int64_t value = (input + offset) * scale;

    value = rk3588_rknn_rgb_cvt_round_shift(
        value, shift, cfg & (1U << 2));
    return CLAMP(value, INT8_MIN, INT8_MAX);
}

static size_t rk3588_rknn_rgb_cvt_output_index(unsigned int channel,
                                                unsigned int row,
                                                unsigned int column)
{
    return channel / 16 * RK3588_RKNN_RGB_CVT_OUTPUT_SURFACE_BYTES +
           (row * RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH + column) * 16 +
           channel % 16;
}

static void rk3588_rknn_make_rgb_cvt_data(
    uint8_t input[RK3588_RKNN_RGB_CVT_INPUT_BYTES],
    int8_t weights[RK3588_RKNN_RGB_CVT_WEIGHT_BYTES],
    uint8_t bs[RK3588_RKNN_RGB_CVT_BS_BYTES])
{
    memset(weights, 0, RK3588_RKNN_RGB_CVT_WEIGHT_BYTES);
    memset(bs, 0, RK3588_RKNN_RGB_CVT_BS_BYTES);
    for (unsigned int row = 0;
         row < RK3588_RKNN_RGB_CVT_INPUT_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_RGB_CVT_INPUT_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_RGB_CVT_INPUT_CHANNELS; channel++) {
                size_t index =
                    (row * RK3588_RKNN_RGB_CVT_INPUT_WIDTH + column) *
                    RK3588_RKNN_RGB_CVT_INPUT_CHANNELS + channel;

                input[index] = 124 +
                    (row * 7 + column * 2 + channel * 3) % 9;
            }
        }
    }
    for (unsigned int output = 0;
         output < RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS; output++) {
        unsigned int group = output / 8;
        unsigned int lane = output % 8;
        uint32_t alu = cpu_to_le32((output * 3) % 7);
        uint16_t cpend = cpu_to_le16(output % 3 == 0);
        uint16_t multiplier = cpu_to_le16(output % 2 + 1);

        memcpy(bs + group * 0x40 + lane * 4, &alu, sizeof(alu));
        memcpy(bs + group * 0x40 + 0x20 + lane * 2,
               &cpend, sizeof(cpend));
        memcpy(bs + group * 0x40 + 0x30 + lane * 2,
               &multiplier, sizeof(multiplier));
        for (unsigned int kernel = 0;
             kernel < RK3588_RKNN_RGB_CVT_KERNEL *
                      RK3588_RKNN_RGB_CVT_KERNEL; kernel++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_RGB_CVT_INPUT_CHANNELS; channel++) {
                size_t index = rk3588_rknn_rgb_cvt_weight_index(
                    output, kernel, channel);

                weights[index] = (output + kernel * 3 + channel * 5) % 7 == 0;
            }
        }
    }
}

static int8_t rk3588_rknn_rgb_cvt_expected(
    const uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS],
    const uint8_t input[RK3588_RKNN_RGB_CVT_INPUT_BYTES],
    const int8_t weights[RK3588_RKNN_RGB_CVT_WEIGHT_BYTES],
    const uint8_t bs[RK3588_RKNN_RGB_CVT_BS_BYTES],
    const uint16_t *bn,
    unsigned int output, unsigned int row, unsigned int column)
{
    int convolution = 0;
    int qd_sum = 0;
    uint8_t padding = rk3588_rknn_regcmd_value(
        commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,
        0x0201, 0x1184);

    for (unsigned int kernel_row = 0;
         kernel_row < RK3588_RKNN_RGB_CVT_KERNEL; kernel_row++) {
        unsigned int input_row = row * 2 + kernel_row;

        for (unsigned int kernel_column = 0;
             kernel_column < RK3588_RKNN_RGB_CVT_KERNEL; kernel_column++) {
            unsigned int input_column = column * 2 + kernel_column;
            unsigned int kernel =
                kernel_row * RK3588_RKNN_RGB_CVT_KERNEL + kernel_column;

            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_RGB_CVT_INPUT_CHANNELS; channel++) {
                int converted = (int8_t)padding;

                if (input_row < RK3588_RKNN_RGB_CVT_INPUT_HEIGHT &&
                    input_column < RK3588_RKNN_RGB_CVT_INPUT_WIDTH) {
                    size_t input_index =
                        (input_row * RK3588_RKNN_RGB_CVT_INPUT_WIDTH +
                         input_column) *
                        RK3588_RKNN_RGB_CVT_INPUT_CHANNELS + channel;

                    converted = rk3588_rknn_rgb_cvt_convert(
                        commands, channel, input[input_index]);
                }

                qd_sum += converted;
                convolution += converted * weights[
                    rk3588_rknn_rgb_cvt_weight_index(
                        output, kernel, channel)];
            }
        }
    }
    unsigned int group = output / 8;
    unsigned int lane = output % 8;
    uint32_t alu_le;
    uint16_t cpend_le;
    uint16_t multiplier_le;
    int64_t value;
    unsigned int shift;

    memcpy(&alu_le, bs + group * 0x40 + lane * 4, sizeof(alu_le));
    memcpy(&cpend_le, bs + group * 0x40 + 0x20 + lane * 2,
           sizeof(cpend_le));
    memcpy(&multiplier_le, bs + group * 0x40 + 0x30 + lane * 2,
           sizeof(multiplier_le));
    if (rk3588_rknn_regcmd_value(
            commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,
            0x1001, 0x4050) == 0x125) {
        value = convolution + (int16_t)le16_to_cpu(cpend_le) * qd_sum +
                (int32_t)le32_to_cpu(alu_le);
    } else {
        value = convolution + (int32_t)le32_to_cpu(alu_le);
    }
    value *= (int16_t)le16_to_cpu(multiplier_le);
    shift = value < 0 ?
        (rk3588_rknn_regcmd_value(
             commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,
             0x1001, 0x4010) >> 4) & 0x3f :
        (rk3588_rknn_regcmd_value(
             commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,
             0x1001, 0x4048) >> 8) & 0x3f;
    value = rk3588_rknn_rgb_cvt_round_shift(value, shift, false);
    if (bn && value < 0) {
        value *= (int16_t)le16_to_cpu(bn[output]);
    }
    return CLAMP(value, INT8_MIN, INT8_MAX);
}

static void rk3588_rknn_make_rgb_cvt_regcmd(uint64_t commands[])
{
    g_autofree void *original = NULL;
    gsize length;
    size_t index;

    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_REGCMD, &original, &length);
    g_assert_cmpuint(length, ==, RK3588_RKNN_SYNTH_CONV_COMMANDS *
                                  sizeof(*commands));
    memcpy(commands, original, length);

#define PATCH_RGB(_target, _reg, _value) do {                         \
    index = rk3588_rknn_find_regcmd(                                  \
        commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,               \
        (_target), (_reg));                                           \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    rk3588_rknn_patch_all_regcmd(
        commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,
        0x0201, 0x100c, 0x6000a000);
    PATCH_RGB(0x0201, 0x1010,
              RK3588_RKNN_RGB_CVT_INPUT_HEIGHT << 4);
    PATCH_RGB(0x0201, 0x1014, 0x12);
    PATCH_RGB(0x0201, 0x1020,
              (RK3588_RKNN_RGB_CVT_INPUT_WIDTH << 16) |
              RK3588_RKNN_RGB_CVT_INPUT_HEIGHT);
    PATCH_RGB(0x0201, 0x1024,
              ((RK3588_RKNN_RGB_CVT_INPUT_CHANNELS - 1) << 16) |
              RK3588_RKNN_RGB_CVT_STORAGE_CHANNELS);
    PATCH_RGB(0x0201, 0x1028, RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH);
    PATCH_RGB(0x0201, 0x102c,
              RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH *
              RK3588_RKNN_RGB_CVT_OUTPUT_HEIGHT);
    PATCH_RGB(0x0201, 0x1030,
              RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS *
              RK3588_RKNN_RGB_CVT_KERNEL *
              RK3588_RKNN_RGB_CVT_KERNEL *
              RK3588_RKNN_RGB_CVT_STORAGE_CHANNELS);
    PATCH_RGB(0x0201, 0x1034,
              RK3588_RKNN_RGB_CVT_KERNEL *
              RK3588_RKNN_RGB_CVT_KERNEL *
              RK3588_RKNN_RGB_CVT_STORAGE_CHANNELS);
    PATCH_RGB(0x0201, 0x1038,
              (RK3588_RKNN_RGB_CVT_KERNEL << 24) |
              (RK3588_RKNN_RGB_CVT_KERNEL << 16) |
              RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS);
    PATCH_RGB(0x0201, 0x104c, RK3588_RKNN_RGB_CVT_CON0);
    PATCH_RGB(0x0201, 0x1050, 0x0001ff80);
    PATCH_RGB(0x0201, 0x1054, 0x0003ff81);
    PATCH_RGB(0x0201, 0x1058, 0x0005ff7e);
    PATCH_RGB(0x0201, 0x105c, 0x10000);
    PATCH_RGB(0x0201, 0x1068, 0);
    PATCH_RGB(0x0201, 0x1070,
              RK3588_RKNN_SYNTH_CONV_INPUT_IOVA);
    PATCH_RGB(0x0201, 0x107c, RK3588_RKNN_RGB_CVT_INPUT_WIDTH);
    PATCH_RGB(0x0201, 0x1080,
              RK3588_RKNN_RGB_CVT_INPUT_WIDTH *
              (RK3588_RKNN_RGB_CVT_INPUT_HEIGHT - 1));
    PATCH_RGB(0x0201, 0x1084,
              (RK3588_RKNN_RGB_CVT_INPUT_WIDTH << 16) |
              RK3588_RKNN_RGB_CVT_INPUT_HEIGHT);
    PATCH_RGB(0x0201, 0x1088, RK3588_RKNN_RGB_CVT_STORAGE_CHANNELS);
    PATCH_RGB(0x0201, 0x1110,
              RK3588_RKNN_SYNTH_CONV_WEIGHT_IOVA);
    PATCH_RGB(0x0201, 0x1180, 0xfff);
    PATCH_RGB(0x0801, 0x3010, 1);
    PATCH_RGB(0x0801, 0x3014,
              ((RK3588_RKNN_RGB_CVT_OUTPUT_HEIGHT - 1) << 16) |
              (RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH - 1));
    PATCH_RGB(0x0801, 0x3018,
              RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS - 1);
    PATCH_RGB(0x0801, 0x301c, 0);
    PATCH_RGB(0x1001, 0x400c, 0x1e4);
    PATCH_RGB(0x1001, 0x4010, 0);
    PATCH_RGB(0x1001, 0x4020,
              RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA);
    PATCH_RGB(0x1001, 0x4024, RK3588_RKNN_RGB_CVT_SURFACE_STRIDE);
    PATCH_RGB(0x1001, 0x4030,
              RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH - 1);
    PATCH_RGB(0x1001, 0x4034,
              RK3588_RKNN_RGB_CVT_OUTPUT_HEIGHT - 1);
    PATCH_RGB(0x1001, 0x403c,
              ((RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS - 1) << 16) |
              (RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS - 1));
    PATCH_RGB(0x1001, 0x4040, 0x00020140);
    PATCH_RGB(0x1001, 0x4048, 1);
    PATCH_RGB(0x1001, 0x4050, 0x125);
    PATCH_RGB(0x1001, 0x4054, 0);
    PATCH_RGB(0x1001, 0x4058,
              RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS - 1);
    PATCH_RGB(0x1001, 0x405c,
              ((RK3588_RKNN_RGB_CVT_OUTPUT_HEIGHT - 1) << 16) |
              (RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH - 1));
    PATCH_RGB(0x1001, 0x4060, 0x53);
    PATCH_RGB(0x1001, 0x4070, 0x383);
    PATCH_RGB(0x1001, 0x4080, 0);
    PATCH_RGB(0x1001, 0x4084, 1);
    PATCH_RGB(0x1001, 0x4088, 0);
    PATCH_RGB(0x1001, 0x40c0,
              RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH *
              RK3588_RKNN_RGB_CVT_OUTPUT_HEIGHT * 32);
    PATCH_RGB(0x2001, 0x500c,
              RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH - 1);
    PATCH_RGB(0x2001, 0x5010,
              RK3588_RKNN_RGB_CVT_OUTPUT_HEIGHT - 1);
    PATCH_RGB(0x2001, 0x5014,
              RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS - 1);
    PATCH_RGB(0x2001, 0x501c, 0xe);
    PATCH_RGB(0x2001, 0x5020, RK3588_RKNN_RGB_CVT_BS_IOVA);
    PATCH_RGB(0x2001, 0x5034, 1);
    PATCH_RGB(0x2001, 0x5044, 0x7810);
    PATCH_RGB(0x2001, 0x5048, 0);
    PATCH_RGB(0x2001, 0x5064, 0);
    PATCH_RGB(0x2001, 0x5068, 0x01010101);
#undef PATCH_RGB
}

static void rk3588_rknn_prepare_rgb_cvt(
    QTestState *qts, const uint64_t commands[], const uint8_t input[],
    const int8_t weights[], const uint8_t bs[])
{
    qtest_writel(qts, RK3588_RKNN_MATMUL_DTE_ADDR + 64 * 4,
                 RK3588_RKNN_MATMUL_PTE_ADDR | RK_IOMMU_PTE_VALID);
    for (unsigned int page = 0;
         page < RK3588_RKNN_SYNTH_CONV_MAPPED_PAGES; page++) {
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + page * 4,
                     (RK3588_RKNN_SYNTH_CONV_ADDR + page * 0x1000) |
                     RK_IOMMU_PTE_RW);
    }
    qtest_memwrite(qts, RK3588_RKNN_SYNTH_CONV_ADDR, commands,
                   RK3588_RKNN_SYNTH_CONV_COMMANDS *
                   sizeof(*commands));
    qtest_memwrite(qts, rk3588_rknn_synth_conv_addr(
                   RK3588_RKNN_SYNTH_CONV_INPUT_IOVA),
                   input, RK3588_RKNN_RGB_CVT_INPUT_BYTES);
    qtest_memwrite(qts, rk3588_rknn_synth_conv_addr(
                   RK3588_RKNN_SYNTH_CONV_WEIGHT_IOVA),
                   weights, RK3588_RKNN_RGB_CVT_WEIGHT_BYTES);
    qtest_memwrite(qts, rk3588_rknn_synth_conv_addr(
                       RK3588_RKNN_RGB_CVT_BS_IOVA),
                   bs, RK3588_RKNN_RGB_CVT_BS_BYTES);
    qtest_memset(qts, rk3588_rknn_synth_conv_addr(
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA),
                 0xa5, RK3588_RKNN_RGB_CVT_SURFACE_STRIDE +
                       RK3588_RKNN_RGB_CVT_OUTPUT_SURFACE_BYTES + 1);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_DTE_ADDR,
                 RK3588_RKNN_MATMUL_DTE_ADDR);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_ENABLE_PAGING);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_BASE_ADDRESS,
                 RK3588_RKNN_SYNTH_CONV_REGCMD_IOVA);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(
                     RK3588_RKNN_SYNTH_CONV_COMMANDS));
}

static void rk3588_rknn_assert_rgb_cvt_result(
    QTestState *qts,
    const uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS],
    const uint8_t input[RK3588_RKNN_RGB_CVT_INPUT_BYTES],
    const int8_t weights[RK3588_RKNN_RGB_CVT_WEIGHT_BYTES],
    const uint8_t bs[RK3588_RKNN_RGB_CVT_BS_BYTES], const uint16_t *bn)
{
    uint8_t output[RK3588_RKNN_RGB_CVT_OUTPUT_BYTES];
    uint64_t output_addr = rk3588_rknn_synth_conv_addr(
        RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA);

    for (unsigned int surface = 0; surface < 2; surface++) {
        qtest_memread(qts,
                      output_addr +
                      surface * RK3588_RKNN_RGB_CVT_SURFACE_STRIDE,
                      output +
                      surface * RK3588_RKNN_RGB_CVT_OUTPUT_SURFACE_BYTES,
                      RK3588_RKNN_RGB_CVT_OUTPUT_SURFACE_BYTES);
        g_assert_cmphex(qtest_readb(
            qts, output_addr +
                 surface * RK3588_RKNN_RGB_CVT_SURFACE_STRIDE +
                 RK3588_RKNN_RGB_CVT_OUTPUT_SURFACE_BYTES), ==, 0xa5);
    }
    for (unsigned int row = 0;
         row < RK3588_RKNN_RGB_CVT_OUTPUT_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS; channel++) {
                size_t index = rk3588_rknn_rgb_cvt_output_index(
                    channel, row, column);
                int8_t expected = rk3588_rknn_rgb_cvt_expected(
                    commands, input, weights, bs, bn,
                    channel, row, column);

                if ((int8_t)output[index] != expected) {
                    g_error("RGB/CVT mismatch c=%u y=%u x=%u: %d != %d",
                            channel, row, column, (int8_t)output[index],
                            expected);
                }
            }
        }
    }
    rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_SUCCESS,
                             RKNN_PIPELINE_BANK1_INTERRUPT);
}

static void rk3588_rknn_assert_rgb_cvt_rejected(QTestState *qts)
{
    uint64_t output_addr = rk3588_rknn_synth_conv_addr(
        RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA);

    g_assert_cmphex(qtest_readb(qts, output_addr), ==, 0xa5);
    g_assert_cmphex(qtest_readb(
        qts, output_addr + RK3588_RKNN_RGB_CVT_SURFACE_STRIDE), ==, 0xa5);
    rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
}

static void test_rk3588_rknpu_rgb_cvt_convolution(void)
{
    uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
    uint8_t input[RK3588_RKNN_RGB_CVT_INPUT_BYTES];
    int8_t weights[RK3588_RKNN_RGB_CVT_WEIGHT_BYTES];
    uint8_t bs[RK3588_RKNN_RGB_CVT_BS_BYTES];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_rgb_cvt_regcmd(commands);
    rk3588_rknn_make_rgb_cvt_data(input, weights, bs);
    rk3588_rknn_prepare_rgb_cvt(qts, commands, input, weights, bs);
    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_assert_rgb_cvt_result(
        qts, commands, input, weights, bs, NULL);
    qtest_quit(qts);
}

static void rk3588_rknn_enable_rgb_cvt_nrdma(
    uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS],
    uint32_t nrdma_cfg, uint32_t bn_mul_cfg, uint32_t bn_iova)
{
    size_t index;

#define PATCH_NRDMA(_target, _reg, _value) do {                       \
    index = rk3588_rknn_find_regcmd(                                  \
        commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,              \
        (_target), (_reg));                                           \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    PATCH_NRDMA(0x1001, 0x4060, 0x62);
    PATCH_NRDMA(0x1001, 0x4068, bn_mul_cfg);
    PATCH_NRDMA(0x2001, RKNN_DPU_RDMA_NRDMA_CFG, nrdma_cfg);
    PATCH_NRDMA(0x2001, RKNN_DPU_RDMA_BN_BASE_ADDR, bn_iova);
#undef PATCH_NRDMA
}

static void test_rk3588_rknpu_int8_qd_nrdma(void)
{
    uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
    uint8_t input[RK3588_RKNN_RGB_CVT_INPUT_BYTES];
    int8_t weights[RK3588_RKNN_RGB_CVT_WEIGHT_BYTES];
    uint8_t bs[RK3588_RKNN_RGB_CVT_BS_BYTES];
    uint16_t bn[RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS];
    bool changed = false;
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_rgb_cvt_regcmd(commands);
    rk3588_rknn_enable_rgb_cvt_nrdma(
        commands, 8, 1, RK3588_RKNN_RGB_CVT_BN_IOVA);
    rk3588_rknn_make_rgb_cvt_data(input, weights, bs);
    for (unsigned int channel = 0; channel < ARRAY_SIZE(bn); channel++) {
        bn[channel] = cpu_to_le16(channel % 3 + 2);
    }
    for (unsigned int row = 0;
         row < RK3588_RKNN_RGB_CVT_OUTPUT_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH; column++) {
            for (unsigned int channel = 0; channel < ARRAY_SIZE(bn);
                 channel++) {
                changed |= rk3588_rknn_rgb_cvt_expected(
                    commands, input, weights, bs, NULL,
                    channel, row, column) !=
                    rk3588_rknn_rgb_cvt_expected(
                        commands, input, weights, bs, bn,
                        channel, row, column);
            }
        }
    }
    g_assert_true(changed);
    rk3588_rknn_prepare_rgb_cvt(qts, commands, input, weights, bs);
    qtest_memwrite(qts, rk3588_rknn_synth_conv_addr(
                       RK3588_RKNN_RGB_CVT_BN_IOVA), bn, sizeof(bn));
    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_assert_rgb_cvt_result(
        qts, commands, input, weights, bs, bn);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_int8_qd_nrdma_controls(void)
{
    static const struct {
        const char *name;
        uint32_t nrdma_cfg;
        uint32_t bn_mul_cfg;
    } cases[] = {
        { "unsupported-data-use", 6, 1 },
        { "reserved-control", 0x28, 1 },
        { "unused-nrdma", 8, 0 },
        { "missing-nrdma", 0, 1 },
    };

    for (unsigned int case_index = 0;
         case_index < ARRAY_SIZE(cases); case_index++) {
        uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
        uint8_t input[RK3588_RKNN_RGB_CVT_INPUT_BYTES];
        int8_t weights[RK3588_RKNN_RGB_CVT_WEIGHT_BYTES];
        uint8_t bs[RK3588_RKNN_RGB_CVT_BS_BYTES];
        QTestState *qts = rk3588_qtest_start_rknpu();

        g_test_message("NRDMA mutation: %s", cases[case_index].name);
        rk3588_rknn_make_rgb_cvt_regcmd(commands);
        rk3588_rknn_enable_rgb_cvt_nrdma(
            commands, cases[case_index].nrdma_cfg,
            cases[case_index].bn_mul_cfg, RK3588_RKNN_RGB_CVT_BN_IOVA);
        rk3588_rknn_make_rgb_cvt_data(input, weights, bs);
        rk3588_rknn_prepare_rgb_cvt(qts, commands, input, weights, bs);
        rk3588_rknn_run_matmul(qts);
        rk3588_rknn_assert_rgb_cvt_rejected(qts);
        qtest_quit(qts);
    }
}

static void test_rk3588_rknpu_int8_qd_brdma_no_cpend(void)
{
    uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
    uint64_t cpend_commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
    uint8_t input[RK3588_RKNN_RGB_CVT_INPUT_BYTES];
    int8_t weights[RK3588_RKNN_RGB_CVT_WEIGHT_BYTES];
    uint8_t bs[RK3588_RKNN_RGB_CVT_BS_BYTES];
    bool changed = false;
    size_t index;
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_rgb_cvt_regcmd(commands);
    memcpy(cpend_commands, commands, sizeof(commands));
    index = rk3588_rknn_find_regcmd(
        commands, ARRAY_SIZE(commands), 0x1001, 0x4050);
    commands[index] = rk3588_rknn_regcmd(0x1001, 0x4050, 0x124);
    rk3588_rknn_make_rgb_cvt_data(input, weights, bs);
    for (unsigned int row = 0;
         row < RK3588_RKNN_RGB_CVT_OUTPUT_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_RGB_CVT_OUTPUT_CHANNELS; channel++) {
                changed |= rk3588_rknn_rgb_cvt_expected(
                    commands, input, weights, bs, NULL,
                    channel, row, column) !=
                    rk3588_rknn_rgb_cvt_expected(
                        cpend_commands, input, weights, bs, NULL,
                        channel, row, column);
            }
        }
    }
    g_assert_true(changed);
    rk3588_rknn_prepare_rgb_cvt(qts, commands, input, weights, bs);
    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_assert_rgb_cvt_result(
        qts, commands, input, weights, bs, NULL);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_rgb_cvt_control_mutations(void)
{
    static const struct {
        const char *name;
        uint32_t reg;
        uint32_t value;
    } cases[] = {
        { "argb-in", 0x100c, 0x60009000 },
        { "nonalign-dma", 0x100c, 0x2000a000 },
        { "group-line-off", 0x100c, 0x4000a000 },
        { "deconv", 0x100c, 0x6001a000 },
        { "nn-mode", 0x1014, 0x10000012 },
        { "atrous-x", 0x1014, 0x00010012 },
        { "atrous-y", 0x1014, 0x00200012 },
        { "deconv-stride-x", 0x1014, 0x00000112 },
        { "deconv-stride-y", 0x1014, 0x00000812 },
        { "surface-mode", 0x102c,
          (1U << 22) | RK3588_RKNN_RGB_CVT_OUTPUT_WIDTH *
                       RK3588_RKNN_RGB_CVT_OUTPUT_HEIGHT },
        { "cvt-bypass", 0x104c, RK3588_RKNN_RGB_CVT_CON0 | (1U << 0) },
        { "cvt-type", 0x104c, RK3588_RKNN_RGB_CVT_CON0 | (1U << 1) },
        { "channel-0-cvt", 0x1180, 0xffe },
        { "channel-1-cvt", 0x1180, 0xffd },
        { "channel-2-cvt", 0x1180, 0xffb },
    };
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
        uint8_t input[RK3588_RKNN_RGB_CVT_INPUT_BYTES];
        int8_t weights[RK3588_RKNN_RGB_CVT_WEIGHT_BYTES];
        uint8_t bs[RK3588_RKNN_RGB_CVT_BS_BYTES];
        size_t index;

        if (i) {
            rk3588_rknpu_reset_fixture(qts);
        }
        g_test_message("RGB/CVT mutation: %s", cases[i].name);
        rk3588_rknn_make_rgb_cvt_regcmd(commands);
        if (cases[i].reg == 0x100c) {
            rk3588_rknn_patch_all_regcmd(
                commands, ARRAY_SIZE(commands), 0x0201,
                cases[i].reg, cases[i].value);
        } else {
            index = rk3588_rknn_find_regcmd(
                commands, ARRAY_SIZE(commands), 0x0201, cases[i].reg);
            commands[index] = rk3588_rknn_regcmd(
                0x0201, cases[i].reg, cases[i].value);
        }
        rk3588_rknn_make_rgb_cvt_data(input, weights, bs);
        rk3588_rknn_prepare_rgb_cvt(qts, commands, input, weights, bs);
        rk3588_rknn_run_matmul(qts);
        rk3588_rknn_assert_rgb_cvt_rejected(qts);
    }
    qtest_quit(qts);
}

static void rk3588_rknn_enable_int8_erdma(uint64_t commands[],
                                         bool consume_erdma,
                                         bool valid_notch)
{
    const uint32_t spatial = RK3588_RKNN_SYNTH_CONV_WIDTH *
                             RK3588_RKNN_SYNTH_CONV_HEIGHT;
    const uint32_t surface_stride = spatial * 2;
    const uint32_t surface_notch = surface_stride * 2 - spatial;
    size_t index;

#define PATCH_QD_ERDMA(_target, _reg, _value) do {                   \
    index = rk3588_rknn_find_regcmd(                                 \
        commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,              \
        (_target), (_reg));                                          \
    commands[index] = rk3588_rknn_regcmd(                            \
        (_target), (_reg), (_value));                                \
} while (0)
    PATCH_QD_ERDMA(0x1001, 0x4070,
                   consume_erdma ? 0x904202c0 : 1);
    PATCH_QD_ERDMA(0x1001, 0x4074, 0);
    PATCH_QD_ERDMA(0x1001, 0x4078, 1);
    PATCH_QD_ERDMA(
        0x2001, 0x5018,
        consume_erdma ? RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA :
                        0xfff00000);
    PATCH_QD_ERDMA(0x2001, 0x5034, 0x40000004);
    PATCH_QD_ERDMA(
        0x2001, 0x5038,
        consume_erdma ?
            RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA + spatial * 16 :
            0xfff00000);
    PATCH_QD_ERDMA(0x2001, 0x5040, surface_stride << 4);
    PATCH_QD_ERDMA(0x2001, 0x5044, 0x7d00);
    PATCH_QD_ERDMA(0x2001, 0x504c,
                   (surface_notch + !valid_notch) << 4);
    PATCH_QD_ERDMA(0x2001, 0x506c, surface_notch << 4);
#undef PATCH_QD_ERDMA
}

static void test_rk3588_rknpu_int8_qd_brdma_erdma(void)
{
    const size_t spatial =
        RK3588_RKNN_SYNTH_CONV_WIDTH *
        RK3588_RKNN_SYNTH_CONV_HEIGHT;
    const size_t operand_span = spatial * 128;
    const size_t stale_group_offset = spatial * 96;
    const size_t operand_group_bytes = spatial * 16;
    g_autofree uint64_t *commands = NULL;
    g_autofree void *input = NULL;
    g_autofree void *weights = NULL;
    g_autofree void *bs = NULL;
    g_autofree void *expected = NULL;
    g_autofree uint8_t *actual = NULL;
    gsize command_bytes;
    gsize input_length;
    gsize weights_length;
    gsize bs_length;
    gsize output_length;
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_REGCMD, (void **)&commands, &command_bytes);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_INPUT, &input, &input_length);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_WEIGHT, &weights, &weights_length);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_BS, &bs, &bs_length);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_OUTPUT, &expected, &output_length);
    rk3588_rknn_enable_int8_erdma(commands, true, true);
    rk3588_rknn_prepare_synth_conv(
        qts, commands, command_bytes / sizeof(*commands),
        input, input_length, weights, weights_length, bs, bs_length,
        RK3588_RKNN_SYNTH_CONV_HEIGHT, 0xe);
    qtest_memset(qts, rk3588_rknn_synth_conv_addr(
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA),
                 0, operand_span);
    qtest_memset(qts, rk3588_rknn_synth_conv_addr(
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA) +
                     stale_group_offset,
                 1, operand_group_bytes);
    qtest_memset(qts, rk3588_rknn_synth_conv_addr(
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA) +
                     stale_group_offset + spatial * 16,
                 1, operand_group_bytes);
    rk3588_rknn_run_matmul(qts);
    actual = g_malloc(output_length);
    for (unsigned int surface = 0;
         surface < RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS /
                   RK3588_RKNN_SYNTH_CONV_ATOM; surface++) {
        qtest_memread(
            qts,
            rk3588_rknn_synth_conv_addr(
                RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA) +
                surface * RK3588_RKNN_SYNTH_CONV_SURFACE_BYTES,
            actual + surface * output_length / 4, output_length / 4);
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    for (size_t offset = 0; offset < output_length; offset++) {
        if (actual[offset] != ((uint8_t *)expected)[offset]) {
            g_error("QD+ERDMA output mismatch at %zu: actual=%d "
                    "expected=%d", offset, (int8_t)actual[offset],
                    (int8_t)((uint8_t *)expected)[offset]);
        }
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_int8_qd_brdma_erdma_controls(void)
{
    g_autofree uint64_t *commands = NULL;
    g_autofree void *input = NULL;
    g_autofree void *weights = NULL;
    g_autofree void *bs = NULL;
    gsize command_bytes;
    gsize input_length;
    gsize weights_length;
    gsize bs_length;
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_REGCMD, (void **)&commands, &command_bytes);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_INPUT, &input, &input_length);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_WEIGHT, &weights, &weights_length);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_BS, &bs, &bs_length);
    rk3588_rknn_enable_int8_erdma(commands, true, false);
    rk3588_rknn_prepare_synth_conv(
        qts, commands, command_bytes / sizeof(*commands),
        input, input_length, weights, weights_length, bs, bs_length,
        RK3588_RKNN_SYNTH_CONV_HEIGHT, 0xe);
    rk3588_rknn_run_matmul(qts);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_FETCH_ERROR | 1);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_int8_qd_brdma_partial_channels(void)
{
    const unsigned int channels = 63;
    const size_t output_length =
        RK3588_RKNN_SYNTH_CONV_WIDTH *
        RK3588_RKNN_SYNTH_CONV_HEIGHT *
        RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS;
    g_autofree void *regcmd = NULL;
    g_autofree void *input = NULL;
    g_autofree void *weights = NULL;
    g_autofree void *bs = NULL;
    g_autofree uint8_t *expected = NULL;
    g_autofree uint8_t *actual = NULL;
    gsize regcmd_length;
    gsize input_length;
    gsize weights_length;
    gsize bs_length;
    gsize expected_length;
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t index;

    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_REGCMD, &regcmd, &regcmd_length);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_INPUT, &input, &input_length);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_WEIGHT, &weights, &weights_length);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_BS, &bs, &bs_length);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_OUTPUT, (void **)&expected, &expected_length);
#define PATCH_QD_CHANNELS(_target, _reg, _value) do {                 \
    index = rk3588_rknn_find_regcmd(                                  \
        regcmd, regcmd_length / sizeof(uint64_t), (_target), (_reg)); \
    ((uint64_t *)regcmd)[index] =                                     \
        rk3588_rknn_regcmd((_target), (_reg), (_value));              \
} while (0)
    PATCH_QD_CHANNELS(0x0201, 0x1030,
                      channels * RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS);
    PATCH_QD_CHANNELS(0x0201, 0x1038,
                      (1 << 24) | (1 << 16) | channels);
    PATCH_QD_CHANNELS(0x0801, 0x3018, channels - 1);
    PATCH_QD_CHANNELS(0x1001, 0x403c,
                      ((channels - 1) << 16) | (channels - 1));
    PATCH_QD_CHANNELS(0x1001, 0x4058, channels - 1);
    PATCH_QD_CHANNELS(0x2001, 0x5014, channels - 1);
#undef PATCH_QD_CHANNELS
    g_assert_cmpuint(expected_length, ==, output_length);
    rk3588_rknn_prepare_synth_conv(
        qts, regcmd, regcmd_length / sizeof(uint64_t),
        input, input_length, weights, weights_length, bs, bs_length,
        RK3588_RKNN_SYNTH_CONV_HEIGHT, 0xe);
    rk3588_rknn_run_matmul(qts);
    actual = g_malloc(output_length);
    rk3588_rknn_read_synth_conv_output(
        qts, actual, output_length, RK3588_RKNN_SYNTH_CONV_HEIGHT);

    for (unsigned int row = 0;
         row < RK3588_RKNN_SYNTH_CONV_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_SYNTH_CONV_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS;
                 channel++) {
                size_t output_index =
                    (channel / RK3588_RKNN_SYNTH_CONV_ATOM) *
                    RK3588_RKNN_SYNTH_CONV_HEIGHT *
                    RK3588_RKNN_SYNTH_CONV_WIDTH *
                    RK3588_RKNN_SYNTH_CONV_ATOM +
                    (row * RK3588_RKNN_SYNTH_CONV_WIDTH + column) *
                    RK3588_RKNN_SYNTH_CONV_ATOM +
                    channel % RK3588_RKNN_SYNTH_CONV_ATOM;

                g_assert_cmphex(actual[output_index], ==,
                                channel < channels ?
                                expected[output_index] : 0);
            }
        }
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_int8_qd_brdma_storage_channels(void)
{
    enum {
        STORAGE_CHANNELS = 48,
        TEST_HEIGHT = 1,
    };
    const size_t spatial = RK3588_RKNN_SYNTH_CONV_WIDTH * TEST_HEIGHT;
    g_autofree void *regcmd = NULL;
    g_autofree uint8_t *input = g_new0(uint8_t,
                                       spatial * STORAGE_CHANNELS);
    g_autofree uint8_t *weights = g_new0(
        uint8_t, RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS *
                 STORAGE_CHANNELS);
    g_autofree uint8_t *bs = g_new0(
        uint8_t, RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS * 8);
    g_autofree uint8_t *actual = g_malloc(
        spatial * RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS);
    gsize regcmd_length;
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t index;

    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_REGCMD, &regcmd, &regcmd_length);
#define PATCH_QD_STORAGE(_target, _reg, _value) do {                 \
    index = rk3588_rknn_find_regcmd(                                  \
        regcmd, regcmd_length / sizeof(uint64_t), (_target), (_reg)); \
    ((uint64_t *)regcmd)[index] =                                     \
        rk3588_rknn_regcmd((_target), (_reg), (_value));              \
} while (0)
    PATCH_QD_STORAGE(0x0201, 0x1024,
                     ((RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS - 1) <<
                      16) | STORAGE_CHANNELS);
    PATCH_QD_STORAGE(0x0201, 0x1030,
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS *
                     STORAGE_CHANNELS);
    PATCH_QD_STORAGE(0x0201, 0x1034, STORAGE_CHANNELS);
    PATCH_QD_STORAGE(0x0201, 0x1080,
                     (1U << 28) -
                     RK3588_RKNN_SYNTH_CONV_WIDTH * 3);
    PATCH_QD_STORAGE(0x0201, 0x1088, STORAGE_CHANNELS);
    PATCH_QD_STORAGE(0x1001, 0x4060, 1);
    PATCH_QD_STORAGE(0x1001, 0x4070, 1);
    PATCH_QD_STORAGE(0x1001, 0x4080, 0);
    PATCH_QD_STORAGE(0x1001, 0x4084, 1);
    PATCH_QD_STORAGE(0x1001, 0x4088, 0);
#undef PATCH_QD_STORAGE

    for (unsigned int column = 0;
         column < RK3588_RKNN_SYNTH_CONV_WIDTH; column++) {
        input[rk3588_rknn_spatial_feature_index(
            RK3588_RKNN_SYNTH_CONV_WIDTH, TEST_HEIGHT,
            RK3588_RKNN_SYNTH_CONV_ATOM,
            RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS,
            0, column)] = 1;
    }
    weights[32 * 32] = 1;
    for (unsigned int output = 0;
         output < RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS; output++) {
        stw_le_p(bs + (output / 8) * 0x40 + 0x30 +
                 (output % 8) * 2, 1 << 14);
    }

    rk3588_rknn_prepare_synth_conv(
        qts, regcmd, regcmd_length / sizeof(uint64_t),
        input, spatial * STORAGE_CHANNELS,
        weights, RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS *
                 STORAGE_CHANNELS,
        bs, RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS * 8,
        TEST_HEIGHT, 0xe);
    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_read_synth_conv_output(
        qts, actual,
        spatial * RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS,
        TEST_HEIGHT);
    for (unsigned int column = 0;
         column < RK3588_RKNN_SYNTH_CONV_WIDTH; column++) {
        for (unsigned int output = 0;
             output < RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS;
             output++) {
            size_t output_index = rk3588_rknn_spatial_feature_index(
                RK3588_RKNN_SYNTH_CONV_WIDTH, TEST_HEIGHT,
                RK3588_RKNN_SYNTH_CONV_ATOM,
                output, 0, column);

            g_assert_cmphex(actual[output_index], ==, output ? 0 : 1);
        }
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_int8_qd_brdma_layout_controls(void)
{
    static const struct {
        uint32_t target;
        uint32_t reg;
        uint32_t value;
    } cases[] = {
        { 0x2001, 0x501c, 0 },
        { 0x2001, 0x5034, 0 },
        { 0x2001, 0x5044, 0x7811 },
        { 0x2001, 0x5048, 1 },
        { 0x2001, 0x5064, 1 },
        { 0x2001, 0x5068, 0x01010100 },
        { 0x1001, 0x4050, 0x127 },
        { 0x1001, 0x4054, 1 },
        { 0x1001, 0x405c,
          (RK3588_RKNN_SYNTH_CONV_HEIGHT << 16) |
          (RK3588_RKNN_SYNTH_CONV_WIDTH - 1) },
        { 0x1001, 0x405c,
          ((RK3588_RKNN_SYNTH_CONV_HEIGHT - 1) << 16) |
          RK3588_RKNN_SYNTH_CONV_WIDTH },
        { 0x1001, 0x4034,
          (RK3588_RKNN_SYNTH_CONV_HEIGHT - 1) | (1U << 22) },
        { 0x1001, 0x4034,
          (RK3588_RKNN_SYNTH_CONV_HEIGHT - 1) | (1U << 23) },
        { 0x1001, 0x4034,
          (RK3588_RKNN_SYNTH_CONV_HEIGHT - 1) | (1U << 24) },
    };
    g_autofree void *original_regcmd = NULL;
    g_autofree void *input = NULL;
    g_autofree void *weights = NULL;
    g_autofree void *bs = NULL;
    gsize regcmd_length;
    gsize input_length;
    gsize weights_length;
    gsize bs_length;
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_REGCMD, &original_regcmd, &regcmd_length);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_INPUT, &input, &input_length);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_WEIGHT, &weights, &weights_length);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_BS, &bs, &bs_length);

    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree uint64_t *regcmd = g_memdup2(original_regcmd,
                                                regcmd_length);
        size_t index = rk3588_rknn_find_regcmd(
            regcmd, regcmd_length / sizeof(*regcmd), cases[i].target,
            cases[i].reg);

        if (i) {
            rk3588_rknpu_reset_fixture(qts);
        }
        rk3588_rknn_prepare_synth_conv(
            qts, regcmd, regcmd_length / sizeof(*regcmd),
            input, input_length, weights, weights_length, bs, bs_length,
            RK3588_RKNN_SYNTH_CONV_HEIGHT, 0xe);
        regcmd[index] = rk3588_rknn_regcmd(
            cases[i].target, cases[i].reg, cases[i].value);
        qtest_memwrite(qts, RK3588_RKNN_SYNTH_CONV_ADDR,
                       regcmd, regcmd_length);
        rk3588_rknn_run_matmul(qts);
        g_assert_cmphex(qtest_readb(
            qts, rk3588_rknn_synth_conv_addr(
                RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA)), ==, 0xa5);
        rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
    }
    qtest_quit(qts);
}

static void rk3588_rknn_make_int8_brdma_regcmd(uint64_t commands[],
                                               size_t command_count)
{
    const unsigned int input_valid_channels =
        RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS / 2;
    const unsigned int valid_channels =
        RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS - 1;
    size_t index;

#define PATCH_INT8_BRDMA(_target, _reg, _value) do {                 \
    index = rk3588_rknn_find_regcmd(                                 \
        commands, command_count, (_target), (_reg));                 \
    commands[index] = rk3588_rknn_regcmd(                            \
        (_target), (_reg), (_value));                                \
} while (0)
    PATCH_INT8_BRDMA(
        0x0201, 0x1024,
        ((input_valid_channels - 1) << 16) |
        RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS);
    PATCH_INT8_BRDMA(0x1001, 0x403c,
                     ((valid_channels - 1) << 16) |
                     (RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS - 1));
    PATCH_INT8_BRDMA(0x1001, 0x4040, 0x20150);
    PATCH_INT8_BRDMA(0x1001, 0x4048, 0);
    PATCH_INT8_BRDMA(0x1001, 0x4050, 0x124);
    PATCH_INT8_BRDMA(0x1001, 0x4054, 128);
    PATCH_INT8_BRDMA(0x1001, 0x4080, 0);
    PATCH_INT8_BRDMA(0x1001, 0x4084, 1);
    PATCH_INT8_BRDMA(0x1001, 0x4088, 0);
#undef PATCH_INT8_BRDMA
}

static void rk3588_rknn_make_int8_brdma_data(uint8_t input[],
                                              size_t input_length,
                                              uint8_t weights[],
                                              size_t weights_length,
                                              uint8_t bs[], size_t bs_length)
{
    memset(input, 1, input_length);
    memset(weights, 129, weights_length);
    memset(bs, 0, bs_length);
    for (unsigned int channel = 0;
         channel < RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS; channel++) {
        int32_t expected = (int32_t)channel + 10;

        stl_le_p(bs + channel * sizeof(uint32_t),
                 expected - RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS);
    }
}

static void test_rk3588_rknpu_int8_brdma(void)
{
    const size_t input_length =
        RK3588_RKNN_SYNTH_CONV_WIDTH *
        RK3588_RKNN_SYNTH_CONV_HEIGHT *
        RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS;
    const size_t weights_length =
        RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS *
        RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS;
    const size_t bs_length =
        RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS * 8;
    const size_t output_length =
        RK3588_RKNN_SYNTH_CONV_WIDTH *
        RK3588_RKNN_SYNTH_CONV_HEIGHT *
        RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS;
    g_autofree uint64_t *commands = NULL;
    g_autofree uint8_t *input = g_malloc(input_length);
    g_autofree uint8_t *weights = g_malloc(weights_length);
    g_autofree uint8_t *bs = g_malloc(bs_length);
    g_autofree uint8_t *output = g_malloc(output_length);
    gsize command_bytes;
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_REGCMD, (void **)&commands, &command_bytes);
    rk3588_rknn_make_int8_brdma_regcmd(
        commands, command_bytes / sizeof(*commands));
    rk3588_rknn_make_int8_brdma_data(
        input, input_length, weights, weights_length, bs, bs_length);
    rk3588_rknn_prepare_synth_conv(
        qts, commands, command_bytes / sizeof(*commands),
        input, input_length, weights, weights_length, bs, bs_length,
        RK3588_RKNN_SYNTH_CONV_HEIGHT, 2);
    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_read_synth_conv_output(
        qts, output, output_length, RK3588_RKNN_SYNTH_CONV_HEIGHT);

    for (unsigned int row = 0;
         row < RK3588_RKNN_SYNTH_CONV_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_SYNTH_CONV_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS;
                 channel++) {
                size_t output_index = rk3588_rknn_spatial_feature_index(
                    RK3588_RKNN_SYNTH_CONV_WIDTH,
                    RK3588_RKNN_SYNTH_CONV_HEIGHT,
                    RK3588_RKNN_SYNTH_CONV_ATOM,
                    channel, row, column);
                int8_t expected = channel + 1 ==
                    RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS ?
                    0 : (int32_t)channel + 10;

                if ((int8_t)output[output_index] != expected) {
                    g_error("INT8 BRDMA output mismatch at %u,%u,%u: "
                            "actual=%d expected=%d index=%zu",
                            row, column, channel,
                            (int8_t)output[output_index], expected,
                            output_index);
                }
            }
        }
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_int8_brdma_controls(void)
{
    static const struct {
        uint32_t target;
        uint32_t reg;
        uint32_t value;
    } cases[] = {
        { 0x1001, 0x4050, 0x125 },
        { 0x1001, 0x4050, 0x126 },
        { 0x2001, 0x501c, 0 },
        { 0x2001, 0x501c, 0xe },
        { 0x2001, 0x5034, 0 },
        { 0x2001, 0x5044, 0x7811 },
        { 0x2001, 0x5068, 0x01010100 },
    };
    const size_t input_length =
        RK3588_RKNN_SYNTH_CONV_WIDTH *
        RK3588_RKNN_SYNTH_CONV_HEIGHT *
        RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS;
    const size_t weights_length =
        RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS *
        RK3588_RKNN_SYNTH_CONV_INPUT_CHANNELS;
    const size_t bs_length =
        RK3588_RKNN_SYNTH_CONV_OUTPUT_CHANNELS * 8;
    g_autofree uint64_t *original_commands = NULL;
    g_autofree uint8_t *input = g_malloc(input_length);
    g_autofree uint8_t *weights = g_malloc(weights_length);
    g_autofree uint8_t *bs = g_malloc(bs_length);
    gsize command_bytes;
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_REGCMD, (void **)&original_commands,
        &command_bytes);
    rk3588_rknn_make_int8_brdma_data(
        input, input_length, weights, weights_length, bs, bs_length);
    for (unsigned int case_index = 0;
         case_index < ARRAY_SIZE(cases); case_index++) {
        g_autofree uint64_t *commands = g_memdup2(
            original_commands, command_bytes);
        size_t index;

        if (case_index) {
            rk3588_rknpu_reset_fixture(qts);
        }
        rk3588_rknn_make_int8_brdma_regcmd(
            commands, command_bytes / sizeof(*commands));
        index = rk3588_rknn_find_regcmd(
            commands, command_bytes / sizeof(*commands),
            cases[case_index].target, cases[case_index].reg);
        rk3588_rknn_prepare_synth_conv(
            qts, commands, command_bytes / sizeof(*commands),
            input, input_length, weights, weights_length, bs, bs_length,
            RK3588_RKNN_SYNTH_CONV_HEIGHT, 2);
        commands[index] = rk3588_rknn_regcmd(
            cases[case_index].target, cases[case_index].reg,
            cases[case_index].value);
        qtest_memwrite(qts, RK3588_RKNN_SYNTH_CONV_ADDR,
                       commands, command_bytes);
        rk3588_rknn_run_matmul(qts);
        g_assert_cmphex(qtest_readb(
            qts, rk3588_rknn_synth_conv_addr(
                RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA)), ==, 0xa5);
        g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                        RKNN_TASK_STATUS_FETCH_ERROR | 1);
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_int8_erdma_unused_controls(void)
{
    g_autofree uint64_t *original_commands = NULL;
    g_autofree void *input = NULL;
    g_autofree void *weights = NULL;
    g_autofree void *bs = NULL;
    gsize command_bytes;
    gsize input_length;
    gsize weights_length;
    gsize bs_length;

    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_REGCMD, (void **)&original_commands,
        &command_bytes);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_INPUT, &input, &input_length);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_WEIGHT, &weights, &weights_length);
    rk3588_rknn_make_synth_conv_buffer(
        RK3588_RKNN_SYNTH_CONV_BS, &bs, &bs_length);

    for (unsigned int qd = 0; qd < 2; qd++) {
        g_autofree uint64_t *commands = g_memdup2(
            original_commands, command_bytes);
        QTestState *qts = rk3588_qtest_start_rknpu();

        g_test_message("INT8 %s BRDMA with unused ERDMA",
                       qd ? "QD" : "ordinary");
        if (!qd) {
            rk3588_rknn_make_int8_brdma_regcmd(
                commands, command_bytes / sizeof(*commands));
        }
        rk3588_rknn_enable_int8_erdma(commands, false, true);
        rk3588_rknn_prepare_synth_conv(
            qts, commands, command_bytes / sizeof(*commands),
            input, input_length, weights, weights_length, bs, bs_length,
            RK3588_RKNN_SYNTH_CONV_HEIGHT, qd ? 0xe : 2);
        rk3588_rknn_run_matmul(qts);
        g_assert_cmphex(qtest_readb(
            qts, rk3588_rknn_synth_conv_addr(
                RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA)), ==, 0xa5);
        rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
        qtest_quit(qts);
    }
}

static void test_rk3588_rknpu_weight_size0_semantics(void)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t index;

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(commands, true);
    index = rk3588_rknn_find_regcmd(
        commands, ARRAY_SIZE(commands), 0x0201, 0x1030);
    commands[index] = rk3588_rknn_regcmd(
        0x0201, 0x1030,
        RK3588_RKNN_MATMUL_N * RK3588_RKNN_MATMUL_K - 1);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    rk3588_rknn_run_matmul(qts);
    g_assert_cmphex(qtest_readb(
        qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800), ==, 0xa5);
    rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_large_shape_safe_failure(void)
{
    enum {
        WIDTH = 2047,
        HEIGHT = 2047,
        INPUT_VALID = 16384,
        INPUT_STORAGE = 65535,
        OUTPUT_CHANNELS = 8192,
    };
    const uint32_t spatial = WIDTH * HEIGHT;
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t index;

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(commands, true);
#define PATCH_LARGE(_target, _reg, _value) do {                     \
    index = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),  \
                                    (_target), (_reg));              \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    PATCH_LARGE(0x0201, 0x1020, (WIDTH << 16) | HEIGHT);
    PATCH_LARGE(0x0201, 0x1024,
                ((INPUT_VALID - 1) << 16) | INPUT_STORAGE);
    PATCH_LARGE(0x0201, 0x1028, WIDTH);
    PATCH_LARGE(0x0201, 0x102c, spatial);
    PATCH_LARGE(0x0201, 0x1030,
                OUTPUT_CHANNELS * INPUT_STORAGE);
    PATCH_LARGE(0x0201, 0x1034, INPUT_STORAGE);
    PATCH_LARGE(0x0201, 0x1038,
                (1 << 24) | (1 << 16) | OUTPUT_CHANNELS);
    PATCH_LARGE(0x0201, 0x107c, WIDTH * 4);
    PATCH_LARGE(0x0201, 0x1080, 0);
    PATCH_LARGE(0x0201, 0x1084, (WIDTH << 16) | HEIGHT);
    PATCH_LARGE(0x0201, 0x1088, INPUT_STORAGE);
    PATCH_LARGE(0x0801, 0x3014,
                ((HEIGHT - 1) << 16) | (WIDTH - 1));
    PATCH_LARGE(0x0801, 0x3018, OUTPUT_CHANNELS - 1);
    PATCH_LARGE(0x1001, 0x4024, spatial << 4);
    PATCH_LARGE(0x1001, 0x4030, WIDTH - 1);
    PATCH_LARGE(0x1001, 0x4034, HEIGHT - 1);
    PATCH_LARGE(0x1001, 0x403c,
                ((OUTPUT_CHANNELS - 1) << 16) | (OUTPUT_CHANNELS - 1));
    PATCH_LARGE(0x1001, 0x4058, OUTPUT_CHANNELS - 1);
    PATCH_LARGE(0x1001, 0x405c,
                ((HEIGHT - 1) << 16) | (WIDTH - 1));
    PATCH_LARGE(0x1001, 0x40c0, (spatial * 8) << 4);
#undef PATCH_LARGE
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    rk3588_rknn_run_matmul(qts);
    g_assert_cmphex(qtest_readb(
        qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800), ==, 0xa5);
    rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_slave_decode_failure(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_BASE_ADDRESS, 1);
    qtest_writel(qts, RK3588_RKNN0_GLOBAL_BASE +
                 RKNN_GLOBAL_OPERATION_ENABLE, 0x0d);
    qtest_writel(qts, RK3588_RKNN0_GLOBAL_BASE +
                 RKNN_GLOBAL_OPERATION_ENABLE, 0);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_matmul_iommu_access(void)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    uint8_t input[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_K];
    uint8_t weights[RK3588_RKNN_MATMUL_N * RK3588_RKNN_MATMUL_K];
    uint32_t identity_output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N];
    uint8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_memread(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                  commands, sizeof(commands));
    qtest_memread(qts, RK3588_RKNN_MATMUL_INPUT_ADDR,
                  input, sizeof(input));
    qtest_memread(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR,
                  weights, sizeof(weights));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_IOVA,
                   commands, sizeof(commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_IOVA,
                   input, sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_IOVA,
                   weights, sizeof(weights));
    qtest_memset(qts, RK3588_RKNN_MATMUL_OUTPUT_IOVA, 0xa5,
                 sizeof(identity_output));
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_DISABLE_PAGING);
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_IOVA,
                  identity_output, sizeof(identity_output));
    rk3588_rknn_assert_matmul_output_data(identity_output);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS) &
                    (RKNN_DMA_READ_ERROR | RKNN_DMA_WRITE_ERROR), ==, 0);
    qtest_quit(qts);

    qts = rk3588_qtest_start_rknpu();
    rk3588_rknn_prepare_matmul(qts, true, 0x5a);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 3 * 4,
                 RK3588_RKNN_MATMUL_OUTPUT_ADDR0 |
                 (RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_READABLE));
    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0x5a);
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS) &
                    RKNN_DMA_WRITE_ERROR, ==, RKNN_DMA_WRITE_ERROR);
    qtest_quit(qts);

    qts = rk3588_qtest_start_rknpu();
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 3 * 4,
                 RK3588_RKNN_MATMUL_OUTPUT_ADDR0 |
                 (RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_WRITABLE));
    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_assert_matmul_output(qts);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS) &
                    (RKNN_DMA_READ_ERROR | RKNN_DMA_WRITE_ERROR), ==, 0);
    qtest_quit(qts);

    qts = rk3588_qtest_start_rknpu();
    rk3588_rknn_prepare_matmul(qts, true, 0x3c);
    rk3588_rknn_start_matmul(qts);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 1 * 4,
                 RK3588_RKNN_MATMUL_INPUT_ADDR |
                 (RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_WRITABLE));
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0x3c);
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS) &
                    RKNN_DMA_READ_ERROR, ==, RKNN_DMA_READ_ERROR);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_matmul_shape(void)
{
    enum {
        TEST_M = 3,
        OUTPUT_SURFACES = RK3588_RKNN_MATMUL_N / 4,
        OUTPUT_SURFACE_BYTES = TEST_M * 16,
        OUTPUT_STRIDE_BYTES = (TEST_M + 1) * 16,
    };
    const unsigned int m = TEST_M;
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    int8_t input[TEST_M * RK3588_RKNN_MATMUL_K];
    uint32_t output[TEST_M * RK3588_RKNN_MATMUL_N];
    uint8_t raw_output[OUTPUT_SURFACES * OUTPUT_STRIDE_BYTES];
    QTestState *qts = rk3588_qtest_start_rknpu();
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
    PATCH(0x1001, 0x4024, (m + 1) << 4);
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

    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800,
                  raw_output, sizeof(raw_output));
    for (unsigned int surface = 0; surface < OUTPUT_SURFACES; surface++) {
        memcpy((uint8_t *)output + surface * OUTPUT_SURFACE_BYTES,
               raw_output + surface * OUTPUT_STRIDE_BYTES,
               OUTPUT_SURFACE_BYTES);
        for (unsigned int byte = OUTPUT_SURFACE_BYTES;
             byte < OUTPUT_STRIDE_BYTES; byte++) {
            g_assert_cmphex(raw_output[surface * OUTPUT_STRIDE_BYTES + byte],
                            ==, 0xa5);
        }
    }
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

static size_t rk3588_rknn_spatial_feature_index(unsigned int width,
                                                unsigned int height,
                                                unsigned int atom,
                                                unsigned int channel,
                                                unsigned int row,
                                                unsigned int column)
{
    return (channel / atom) * height * width * atom +
           (row * width + column) * atom + channel % atom;
}

static size_t rk3588_rknn_spatial_weight_index(unsigned int channels,
                                               unsigned int kernel_area,
                                               unsigned int output,
                                               unsigned int kernel,
                                               unsigned int channel)
{
    unsigned int input_groups = channels / 32;

    return ((((output / 32) * input_groups + channel / 32) * kernel_area +
             kernel) * 32 + output % 32) * 32 + channel % 32;
}

static void rk3588_rknn_make_depthwise_regcmd(uint64_t commands[])
{
    size_t index;

    rk3588_rknn_make_matmul_regcmd(commands, true);
#define PATCH_DEPTHWISE(_target, _reg, _value) do {                  \
    index = rk3588_rknn_find_regcmd(commands,                        \
                                    RK3588_RKNN_MATMUL_COMMANDS,     \
                                    (_target), (_reg));              \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    PATCH_DEPTHWISE(0x0201, 0x100c, 3);
    PATCH_DEPTHWISE(0x0201, 0x1010,
                    (RK3588_RKNN_DEPTHWISE_HEIGHT + 1) << 4);
    PATCH_DEPTHWISE(0x0201, 0x1014, 0x09);
    PATCH_DEPTHWISE(0x0201, 0x1020,
                    (RK3588_RKNN_DEPTHWISE_WIDTH << 16) |
                    RK3588_RKNN_DEPTHWISE_HEIGHT);
    PATCH_DEPTHWISE(0x0201, 0x1024,
                    ((RK3588_RKNN_DEPTHWISE_CHANNELS - 1) << 16) |
                    RK3588_RKNN_DEPTHWISE_CHANNELS);
    PATCH_DEPTHWISE(0x0201, 0x1028, RK3588_RKNN_DEPTHWISE_WIDTH);
    PATCH_DEPTHWISE(0x0201, 0x102c,
                    RK3588_RKNN_DEPTHWISE_WIDTH *
                    RK3588_RKNN_DEPTHWISE_HEIGHT);
    PATCH_DEPTHWISE(0x0201, 0x1030,
                    RK3588_RKNN_DEPTHWISE_WEIGHT_BYTES);
    PATCH_DEPTHWISE(0x0201, 0x1034,
                    RK3588_RKNN_DEPTHWISE_WEIGHT_BYTES);
    PATCH_DEPTHWISE(0x0201, 0x1038,
                    (RK3588_RKNN_DEPTHWISE_KERNEL << 24) |
                    (RK3588_RKNN_DEPTHWISE_KERNEL << 16) | 1);
    PATCH_DEPTHWISE(0x0201, 0x1044, 4);
    PATCH_DEPTHWISE(0x0201, 0x1068, 0x11);
    PATCH_DEPTHWISE(0x0201, 0x107c, 0x20);
    PATCH_DEPTHWISE(0x0201, 0x1080, 0x20);
    PATCH_DEPTHWISE(0x0201, 0x1084,
                    (RK3588_RKNN_DEPTHWISE_WIDTH << 16) |
                    RK3588_RKNN_DEPTHWISE_HEIGHT);
    PATCH_DEPTHWISE(0x0201, 0x1088, RK3588_RKNN_DEPTHWISE_CHANNELS);
    PATCH_DEPTHWISE(0x0801, 0x3010, 2);
    PATCH_DEPTHWISE(0x0801, 0x3014,
                    ((RK3588_RKNN_DEPTHWISE_HEIGHT - 1) << 16) |
                    (RK3588_RKNN_DEPTHWISE_WIDTH - 1));
    PATCH_DEPTHWISE(0x0801, 0x3018,
                    RK3588_RKNN_DEPTHWISE_CUBE_CHANNELS - 1);
    PATCH_DEPTHWISE(0x1001, 0x400c, 0x1fc);
    PATCH_DEPTHWISE(0x1001, 0x4010, 4U << 29);
    PATCH_DEPTHWISE(0x1001, 0x4020,
                    RK3588_RKNN_DEPTHWISE_OUTPUT_IOVA);
    PATCH_DEPTHWISE(0x1001, 0x4024, 0x400);
    PATCH_DEPTHWISE(0x1001, 0x4030,
                    RK3588_RKNN_DEPTHWISE_WIDTH - 1);
    PATCH_DEPTHWISE(0x1001, 0x4034,
                    RK3588_RKNN_DEPTHWISE_HEIGHT - 1);
    PATCH_DEPTHWISE(0x1001, 0x403c,
                    ((RK3588_RKNN_DEPTHWISE_CHANNELS - 1) << 16) |
                    (RK3588_RKNN_DEPTHWISE_CUBE_CHANNELS - 1));
    PATCH_DEPTHWISE(0x1001, 0x4058,
                    RK3588_RKNN_DEPTHWISE_CUBE_CHANNELS - 1);
    PATCH_DEPTHWISE(0x1001, 0x405c,
                    ((RK3588_RKNN_DEPTHWISE_HEIGHT - 1) << 16) |
                    (RK3588_RKNN_DEPTHWISE_WIDTH - 1));
    PATCH_DEPTHWISE(0x1001, 0x40c0, 0x2000);
#undef PATCH_DEPTHWISE
}

static void rk3588_rknn_prepare_depthwise(QTestState *qts,
                                           const uint64_t commands[],
                                           uint8_t sentinel)
{
    int8_t input[RK3588_RKNN_DEPTHWISE_WIDTH *
                 RK3588_RKNN_DEPTHWISE_HEIGHT *
                 RK3588_RKNN_DEPTHWISE_CHANNELS] = { 0 };
    int8_t weights[RK3588_RKNN_DEPTHWISE_WEIGHT_BYTES];
    uint8_t output[RK3588_RKNN_DEPTHWISE_OUTPUT_WORDS * sizeof(uint32_t)];

    rk3588_rknn_prepare_matmul(qts, true, sentinel);
    for (unsigned int row = 0; row < RK3588_RKNN_DEPTHWISE_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_DEPTHWISE_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_DEPTHWISE_CHANNELS; channel++) {
                size_t input_index = rk3588_rknn_spatial_feature_index(
                    RK3588_RKNN_DEPTHWISE_WIDTH,
                    RK3588_RKNN_DEPTHWISE_HEIGHT, 16, channel, row, column);

                input[input_index] =
                    ((row * 13 + column * 7 + channel * 5 + 3) % 31) - 15;
            }
        }
    }
    for (unsigned int kernel_row = 0;
         kernel_row < RK3588_RKNN_DEPTHWISE_KERNEL; kernel_row++) {
        for (unsigned int kernel_column = 0;
             kernel_column < RK3588_RKNN_DEPTHWISE_KERNEL; kernel_column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_DEPTHWISE_CHANNELS; channel++) {
                size_t weight_index =
                    (kernel_row * RK3588_RKNN_DEPTHWISE_KERNEL +
                     kernel_column) * RK3588_RKNN_DEPTHWISE_CHANNELS +
                    channel;

                weights[weight_index] =
                    ((channel * 11 + kernel_row * 5 +
                      kernel_column * 3 + 1) % 19) - 9;
            }
        }
    }
    memset(output, sentinel, sizeof(output));

    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   RK3588_RKNN_MATMUL_COMMANDS * sizeof(*commands));
    for (unsigned int page = 0; page < 4; page++) {
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + (12 + page) * 4,
                     (RK3588_RKNN_DEPTHWISE_OUTPUT_ADDR + page * 0x1000) |
                     RK_IOMMU_PTE_RW);
    }
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR, input, sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR, weights,
                   sizeof(weights));
    qtest_memwrite(qts, RK3588_RKNN_DEPTHWISE_OUTPUT_ADDR, output,
                   sizeof(output));
}

static size_t rk3588_rknn_depthwise_output_index(unsigned int channel,
                                                  unsigned int row,
                                                  unsigned int column)
{
    const size_t surface_words = 2048;
    const size_t plane_words = 256;

    return (row / 4) * surface_words + (channel / 4) * plane_words +
           ((row % 4) * RK3588_RKNN_DEPTHWISE_WIDTH + column) * 8 +
           channel % 4;
}

static int32_t rk3588_rknn_depthwise_expected(unsigned int channel,
                                               unsigned int row,
                                               unsigned int column)
{
    int32_t expected = 0;

    for (unsigned int kernel_row = 0;
         kernel_row < RK3588_RKNN_DEPTHWISE_KERNEL; kernel_row++) {
        for (unsigned int kernel_column = 0;
             kernel_column < RK3588_RKNN_DEPTHWISE_KERNEL;
             kernel_column++) {
            int input_row = row - 1 + kernel_row;
            int input_column = column - 1 + kernel_column;

            if (input_row >= 0 &&
                input_row < RK3588_RKNN_DEPTHWISE_HEIGHT &&
                input_column >= 0 &&
                input_column < RK3588_RKNN_DEPTHWISE_WIDTH) {
                int8_t input =
                    ((input_row * 13 + input_column * 7 +
                      channel * 5 + 3) % 31) - 15;
                int8_t weight =
                    ((channel * 11 + kernel_row * 5 +
                      kernel_column * 3 + 1) % 19) - 9;

                expected += input * weight;
            }
        }
    }
    return expected;
}

static void test_rk3588_rknpu_depthwise_int32(void)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    uint32_t output[RK3588_RKNN_DEPTHWISE_OUTPUT_WORDS];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_depthwise_regcmd(commands);
    rk3588_rknn_prepare_depthwise(qts, commands, 0xa5);
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, RK3588_RKNN_DEPTHWISE_OUTPUT_ADDR, output,
                  sizeof(output));
    for (unsigned int row = 0; row < RK3588_RKNN_DEPTHWISE_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_DEPTHWISE_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_DEPTHWISE_CHANNELS; channel++) {
                size_t index = rk3588_rknn_depthwise_output_index(
                    channel, row, column);

                g_assert_cmpint((int32_t)le32_to_cpu(output[index]), ==,
                                rk3588_rknn_depthwise_expected(
                                    channel, row, column));
            }
            for (unsigned int plane = 0; plane < 8; plane++) {
                size_t base = rk3588_rknn_depthwise_output_index(
                    plane * 4, row, column);

                for (unsigned int lane = 4; lane < 8; lane++) {
                    g_assert_cmphex(le32_to_cpu(output[base + lane]), ==, 0);
                }
            }
        }
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void rk3588_rknn_make_depthwise_int8_regcmd(uint64_t commands[])
{
    const unsigned int width = RK3588_RKNN_DEPTHWISE_INT8_WIDTH;
    const unsigned int height = RK3588_RKNN_DEPTHWISE_INT8_HEIGHT;
    const unsigned int channels = RK3588_RKNN_DEPTHWISE_INT8_VALID_CHANNELS;
    const unsigned int storage =
        RK3588_RKNN_DEPTHWISE_INT8_STORAGE_CHANNELS;
    const unsigned int line_stride = width * 4;
    size_t index;

    rk3588_rknn_make_rgb_cvt_regcmd(commands);
#define PATCH_DEPTHWISE_INT8(_target, _reg, _value) do {             \
    index = rk3588_rknn_find_regcmd(                                 \
        commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,              \
        (_target), (_reg));                                          \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    rk3588_rknn_patch_all_regcmd(
        commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,
        0x0201, 0x100c, 3);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1010, (height + 1) << 4);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1014, 0x09);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1020, (width << 16) | height);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1024,
                         ((channels - 1) << 16) | channels);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1028, width);
    PATCH_DEPTHWISE_INT8(0x0201, 0x102c, width * height);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1030, channels * 9);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1034, channels * 9);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1038, 0x03030001);
    PATCH_DEPTHWISE_INT8(0x0201, 0x104c, 0x0b);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1068, 0x11);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1070,
                         RK3588_RKNN_SYNTH_CONV_INPUT_IOVA);
    PATCH_DEPTHWISE_INT8(0x0201, 0x107c, line_stride);
    PATCH_DEPTHWISE_INT8(
        0x0201, 0x1080,
        RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE / 16 - line_stride);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1084, (width << 16) | height);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1088, channels);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1110,
                         RK3588_RKNN_SYNTH_CONV_WEIGHT_IOVA);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1180, 0);
    PATCH_DEPTHWISE_INT8(0x0201, 0x1184, 0xffff8080);
    PATCH_DEPTHWISE_INT8(0x0801, 0x3010, 3);
    PATCH_DEPTHWISE_INT8(0x0801, 0x3014,
                         ((height - 1) << 16) | (width - 1));
    PATCH_DEPTHWISE_INT8(0x0801, 0x3018, storage - 1);
    PATCH_DEPTHWISE_INT8(0x1001, 0x400c, 0x1fc);
    PATCH_DEPTHWISE_INT8(0x1001, 0x4010, 5 << 4);
    PATCH_DEPTHWISE_INT8(0x1001, 0x4020,
                         RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA);
    PATCH_DEPTHWISE_INT8(0x1001, 0x4024,
                         RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE);
    PATCH_DEPTHWISE_INT8(0x1001, 0x4030, width - 1);
    PATCH_DEPTHWISE_INT8(0x1001, 0x4034, height - 1);
    PATCH_DEPTHWISE_INT8(0x1001, 0x403c,
                         ((channels - 1) << 16) | (storage - 1));
    PATCH_DEPTHWISE_INT8(0x1001, 0x4040, 0x00020140);
    PATCH_DEPTHWISE_INT8(0x1001, 0x4048, (5 << 8) | 1);
    PATCH_DEPTHWISE_INT8(0x1001, 0x4050, 0x36d);
    PATCH_DEPTHWISE_INT8(0x1001, 0x4058, storage - 1);
    PATCH_DEPTHWISE_INT8(0x1001, 0x405c,
                         ((height - 1) << 16) | (width - 1));
    PATCH_DEPTHWISE_INT8(0x1001, 0x4060, 0x53);
    PATCH_DEPTHWISE_INT8(0x1001, 0x4080, 0);
    PATCH_DEPTHWISE_INT8(0x1001, 0x4084, 1);
    PATCH_DEPTHWISE_INT8(0x1001, 0x4088, 0);
    PATCH_DEPTHWISE_INT8(
        0x1001, 0x40c0,
        4 * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE);
    PATCH_DEPTHWISE_INT8(0x2001, 0x500c, width - 1);
    PATCH_DEPTHWISE_INT8(0x2001, 0x5010, height - 1);
    PATCH_DEPTHWISE_INT8(0x2001, 0x5014, storage - 1);
    PATCH_DEPTHWISE_INT8(0x2001, 0x501c, 0xe);
    PATCH_DEPTHWISE_INT8(0x2001, 0x5020,
                         RK3588_RKNN_SYNTH_CONV_BS_IOVA);
    PATCH_DEPTHWISE_INT8(0x2001, 0x5044, 0x7816);
    PATCH_DEPTHWISE_INT8(0x2001, 0x5048, 0);
    PATCH_DEPTHWISE_INT8(0x2001, 0x5064, 0);
    PATCH_DEPTHWISE_INT8(0x2001, 0x5068, 0x01010101);
#undef PATCH_DEPTHWISE_INT8
}

static void rk3588_rknn_make_depthwise_int8_data(
    int8_t input[2 * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE],
    int8_t weights[RK3588_RKNN_DEPTHWISE_WEIGHT_BYTES],
    uint8_t bs[RK3588_RKNN_DEPTHWISE_INT8_BS_BYTES])
{
    memset(input, 0, 2 * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE);
    memset(bs, 0, RK3588_RKNN_DEPTHWISE_INT8_BS_BYTES);
    for (unsigned int row = 0;
         row < RK3588_RKNN_DEPTHWISE_INT8_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_DEPTHWISE_INT8_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_DEPTHWISE_INT8_VALID_CHANNELS;
                 channel++) {
                size_t index = channel / 16 *
                    RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE +
                    (row * RK3588_RKNN_DEPTHWISE_INT8_WIDTH + column) * 16 +
                    channel % 16;

                input[index] =
                    ((row * 7 + column * 5 + channel * 3) % 17) - 8;
            }
        }
    }
    for (unsigned int kernel = 0; kernel < 9; kernel++) {
        for (unsigned int channel = 0;
             channel < RK3588_RKNN_DEPTHWISE_INT8_VALID_CHANNELS;
             channel++) {
            weights[kernel * RK3588_RKNN_DEPTHWISE_INT8_VALID_CHANNELS +
                    channel] = ((kernel * 3 + channel * 5) % 5) - 2;
        }
    }
    for (unsigned int channel = 0;
         channel < RK3588_RKNN_DEPTHWISE_INT8_STORAGE_CHANNELS; channel++) {
        unsigned int group = channel / 8;
        unsigned int lane = channel % 8;
        uint32_t alu = cpu_to_le32((int32_t)channel - 16);
        uint16_t cpend = cpu_to_le16(channel % 3 + 1);
        uint16_t multiplier = cpu_to_le16(1);

        memcpy(bs + group * 0x40 + lane * 4, &alu, sizeof(alu));
        memcpy(bs + group * 0x40 + 0x20 + lane * 2,
               &cpend, sizeof(cpend));
        memcpy(bs + group * 0x40 + 0x30 + lane * 2,
               &multiplier, sizeof(multiplier));
    }
}

static void rk3588_rknn_make_depthwise_int8_deconv_regcmd(
    uint64_t commands[])
{
    enum {
        INPUT_WIDTH = 2,
        INPUT_HEIGHT = 2,
        OUTPUT_WIDTH = 4,
        OUTPUT_HEIGHT = 4,
        CHANNELS = 32,
        STORAGE_CHANNELS = 64,
        KERNEL = 4,
        WEIGHT_BYTES = KERNEL * KERNEL * CHANNELS,
    };
    size_t index;

    rk3588_rknn_make_depthwise_int8_regcmd(commands);
#define PATCH_DEPTHWISE_DECONV(_target, _reg, _value) do {           \
    index = rk3588_rknn_find_regcmd(                                 \
        commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,              \
        (_target), (_reg));                                          \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    rk3588_rknn_patch_all_regcmd(
        commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,
        0x0201, 0x100c, 0x00010003);
    PATCH_DEPTHWISE_DECONV(0x0201, 0x1014, 0x00000909);
    PATCH_DEPTHWISE_DECONV(0x0201, 0x1020,
                           (INPUT_WIDTH << 16) | INPUT_HEIGHT);
    PATCH_DEPTHWISE_DECONV(0x0201, 0x1028, OUTPUT_WIDTH);
    PATCH_DEPTHWISE_DECONV(0x0201, 0x102c,
                           OUTPUT_WIDTH * OUTPUT_HEIGHT);
    PATCH_DEPTHWISE_DECONV(0x0201, 0x1030, WEIGHT_BYTES);
    PATCH_DEPTHWISE_DECONV(0x0201, 0x1034, WEIGHT_BYTES);
    PATCH_DEPTHWISE_DECONV(0x0201, 0x1038,
                           (KERNEL << 24) | (KERNEL << 16) | 1);
    PATCH_DEPTHWISE_DECONV(0x0201, 0x1068, 0x22);
    PATCH_DEPTHWISE_DECONV(0x0201, 0x1184, 0);
    PATCH_DEPTHWISE_DECONV(0x0201, 0x107c, INPUT_WIDTH * 4);
    PATCH_DEPTHWISE_DECONV(
        0x0201, 0x1080,
        RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE / 16 -
        INPUT_WIDTH * 4);
    PATCH_DEPTHWISE_DECONV(0x0201, 0x1084,
                           (INPUT_WIDTH << 16) | INPUT_HEIGHT);
    PATCH_DEPTHWISE_DECONV(0x0801, 0x3014,
                           ((OUTPUT_HEIGHT - 1) << 16) |
                           (OUTPUT_WIDTH - 1));
    PATCH_DEPTHWISE_DECONV(0x0801, 0x3018, STORAGE_CHANNELS - 1);
    PATCH_DEPTHWISE_DECONV(0x1001, 0x4030, OUTPUT_WIDTH - 1);
    PATCH_DEPTHWISE_DECONV(0x1001, 0x4034, OUTPUT_HEIGHT - 1);
    PATCH_DEPTHWISE_DECONV(0x1001, 0x403c,
                           ((CHANNELS - 1) << 16) |
                           (STORAGE_CHANNELS - 1));
    PATCH_DEPTHWISE_DECONV(0x1001, 0x4058, STORAGE_CHANNELS - 1);
    PATCH_DEPTHWISE_DECONV(0x1001, 0x405c,
                           ((OUTPUT_HEIGHT - 1) << 16) |
                           (OUTPUT_WIDTH - 1));
    PATCH_DEPTHWISE_DECONV(0x2001, 0x500c, OUTPUT_WIDTH - 1);
    PATCH_DEPTHWISE_DECONV(0x2001, 0x5010, OUTPUT_HEIGHT - 1);
    PATCH_DEPTHWISE_DECONV(0x2001, 0x5014, STORAGE_CHANNELS - 1);
#undef PATCH_DEPTHWISE_DECONV
}

static int8_t rk3588_rknn_depthwise_int8_deconv_expected(
    const int8_t input[], const int8_t weights[], unsigned int channel,
    unsigned int row, unsigned int column)
{
    enum {
        INPUT_WIDTH = 2,
        INPUT_HEIGHT = 2,
        CHANNELS = 32,
        KERNEL = 4,
        STRIDE = 2,
        PAD = 2,
    };
    int accumulator = 0;

    for (unsigned int kernel_row = 0; kernel_row < KERNEL; kernel_row++) {
        int row_numerator = row + KERNEL - 1 - PAD - kernel_row;

        for (unsigned int kernel_column = 0;
             kernel_column < KERNEL; kernel_column++) {
            int column_numerator =
                column + KERNEL - 1 - PAD - kernel_column;
            bool valid = row_numerator >= 0 && column_numerator >= 0 &&
                row_numerator % STRIDE == 0 &&
                column_numerator % STRIDE == 0;
            int input_row = valid ? row_numerator / STRIDE : 0;
            int input_column = valid ? column_numerator / STRIDE : 0;
            int8_t input_value = 0;
            unsigned int kernel = kernel_row * KERNEL + kernel_column;

            if (valid && input_row < INPUT_HEIGHT &&
                input_column < INPUT_WIDTH) {
                size_t input_index = channel / 16 *
                    RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE +
                    (input_row * INPUT_WIDTH + input_column) * 16 +
                    channel % 16;

                input_value = input[input_index];
            }
            accumulator += input_value * weights[kernel * CHANNELS +
                                                 channel];
        }
    }
    return CLAMP(accumulator, INT8_MIN, INT8_MAX);
}

static void rk3588_rknn_prepare_depthwise_int8(
    QTestState *qts, const uint64_t commands[], const int8_t input[],
    const int8_t weights[], const uint8_t bs[])
{
    qtest_writel(qts, RK3588_RKNN_MATMUL_DTE_ADDR + 64 * 4,
                 RK3588_RKNN_MATMUL_PTE_ADDR | RK_IOMMU_PTE_VALID);
    for (unsigned int page = 0;
         page < RK3588_RKNN_SYNTH_CONV_MAPPED_PAGES; page++) {
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + page * 4,
                     (RK3588_RKNN_SYNTH_CONV_ADDR + page * 0x1000) |
                     RK_IOMMU_PTE_RW);
    }
    qtest_memwrite(qts, RK3588_RKNN_SYNTH_CONV_ADDR, commands,
                   RK3588_RKNN_SYNTH_CONV_COMMANDS *
                   sizeof(*commands));
    qtest_memwrite(qts, rk3588_rknn_synth_conv_addr(
                       RK3588_RKNN_SYNTH_CONV_INPUT_IOVA),
                   input, 2 * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE);
    qtest_memwrite(qts, rk3588_rknn_synth_conv_addr(
                       RK3588_RKNN_SYNTH_CONV_WEIGHT_IOVA),
                   weights, RK3588_RKNN_DEPTHWISE_WEIGHT_BYTES);
    qtest_memwrite(qts, rk3588_rknn_synth_conv_addr(
                       RK3588_RKNN_SYNTH_CONV_BS_IOVA),
                   bs, RK3588_RKNN_DEPTHWISE_INT8_BS_BYTES);
    qtest_memset(qts, rk3588_rknn_synth_conv_addr(
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA),
                 0xa5, RK3588_RKNN_DEPTHWISE_INT8_OUTPUT_BYTES + 1);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_DTE_ADDR,
                 RK3588_RKNN_MATMUL_DTE_ADDR);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_ENABLE_PAGING);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_BASE_ADDRESS,
                 RK3588_RKNN_SYNTH_CONV_REGCMD_IOVA);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(
                     RK3588_RKNN_SYNTH_CONV_COMMANDS));
}

static int8_t rk3588_rknn_depthwise_int8_expected(
    const int8_t input[], const int8_t weights[], const uint8_t bs[],
    unsigned int channel, unsigned int row, unsigned int column)
{
    int convolution = 0;
    int qd_sum = 0;
    unsigned int group = channel / 8;
    unsigned int lane = channel % 8;
    uint32_t alu_le;
    uint16_t cpend_le;
    uint16_t multiplier_le;

    for (unsigned int kernel_row = 0; kernel_row < 3; kernel_row++) {
        for (unsigned int kernel_column = 0; kernel_column < 3;
             kernel_column++) {
            int input_row = row - 1 + kernel_row;
            int input_column = column - 1 + kernel_column;
            int8_t value = -128;
            unsigned int kernel = kernel_row * 3 + kernel_column;

            if (input_row >= 0 &&
                input_row < RK3588_RKNN_DEPTHWISE_INT8_HEIGHT &&
                input_column >= 0 &&
                input_column < RK3588_RKNN_DEPTHWISE_INT8_WIDTH) {
                size_t index = channel / 16 *
                    RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE +
                    (input_row * RK3588_RKNN_DEPTHWISE_INT8_WIDTH +
                     input_column) * 16 + channel % 16;

                value = input[index];
            }
            qd_sum += value;
            convolution += value * weights[
                kernel * RK3588_RKNN_DEPTHWISE_INT8_VALID_CHANNELS +
                channel];
        }
    }
    memcpy(&alu_le, bs + group * 0x40 + lane * 4, sizeof(alu_le));
    memcpy(&cpend_le, bs + group * 0x40 + 0x20 + lane * 2,
           sizeof(cpend_le));
    memcpy(&multiplier_le, bs + group * 0x40 + 0x30 + lane * 2,
           sizeof(multiplier_le));
    int64_t value = convolution +
        (int16_t)le16_to_cpu(cpend_le) * qd_sum +
        (int32_t)le32_to_cpu(alu_le);

    value *= (int16_t)le16_to_cpu(multiplier_le);
    value = rk3588_rknn_rgb_cvt_round_shift(value, 5, false);
    return CLAMP(value, INT8_MIN, INT8_MAX);
}

static void rk3588_rknn_assert_depthwise_int8_result(
    QTestState *qts, const int8_t input[], const int8_t weights[],
    const uint8_t bs[])
{
    uint8_t output[RK3588_RKNN_DEPTHWISE_INT8_OUTPUT_BYTES + 1];
    uint64_t output_addr = rk3588_rknn_synth_conv_addr(
        RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA);
    const size_t active = RK3588_RKNN_DEPTHWISE_INT8_WIDTH *
                          RK3588_RKNN_DEPTHWISE_INT8_HEIGHT * 16;

    qtest_memread(qts, output_addr, output, sizeof(output));
    for (unsigned int surface = 0; surface < 4; surface++) {
        for (unsigned int row = 0;
             row < RK3588_RKNN_DEPTHWISE_INT8_HEIGHT; row++) {
            for (unsigned int column = 0;
                 column < RK3588_RKNN_DEPTHWISE_INT8_WIDTH; column++) {
                for (unsigned int lane = 0; lane < 16; lane++) {
                    unsigned int channel = surface * 16 + lane;
                    size_t index = surface *
                        RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE +
                        (row * RK3588_RKNN_DEPTHWISE_INT8_WIDTH + column) *
                        16 + lane;
                    int8_t expected;

                    if (channel < RK3588_RKNN_DEPTHWISE_INT8_VALID_CHANNELS) {
                        expected = rk3588_rknn_depthwise_int8_expected(
                            input, weights, bs, channel, row, column);
                    } else {
                        expected = (int8_t)0xa5;
                    }

                    g_assert_cmpint((int8_t)output[index], ==, expected);
                }
            }
        }
        for (size_t index = active;
             index < RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE; index++) {
            g_assert_cmphex(output[surface *
                                   RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE +
                                   index], ==, 0xa5);
        }
    }
    g_assert_cmphex(output[RK3588_RKNN_DEPTHWISE_INT8_OUTPUT_BYTES], ==,
                    0xa5);
    rk3588_rknn_assert_pc(
        qts, RKNN_TASK_STATUS_SUCCESS,
        RKNN_STAGE_RAW_STATUS_BITS | RKNN_PIPELINE_BANK1_INTERRUPT);
}

static void test_rk3588_rknpu_conv1x1_spatial_hardware_shape(void)
{
    enum {
        WIDTH = 4,
        HEIGHT = 3,
        CHANNELS = 128,
        OUTPUT_CHANNELS = 96,
        SPATIAL = WIDTH * HEIGHT,
    };
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    int8_t input[SPATIAL * CHANNELS];
    int8_t weights[OUTPUT_CHANNELS * CHANNELS];
    uint32_t output[SPATIAL * OUTPUT_CHANNELS];
    QTestState *qts = rk3588_qtest_start_rknpu();
    const uint32_t output_iova = 0x10005000;
    const uint64_t output_addr = RK3588_RAM_BASE + 0x30000;
    size_t index;

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(commands, true);

#define PATCH(_target, _reg, _value) do {                            \
    index = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),  \
                                    (_target), (_reg));              \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    PATCH(0x0201, 0x1010, (HEIGHT + 1) << 4);
    PATCH(0x0201, 0x1020, (WIDTH << 16) | HEIGHT);
    PATCH(0x0201, 0x1024, ((CHANNELS - 1) << 16) | CHANNELS);
    PATCH(0x0201, 0x1028, WIDTH);
    PATCH(0x0201, 0x102c, SPATIAL);
    PATCH(0x0201, 0x1030, OUTPUT_CHANNELS * CHANNELS);
    PATCH(0x0201, 0x1034, CHANNELS);
    PATCH(0x0201, 0x1038,
          (1 << 24) | (1 << 16) | OUTPUT_CHANNELS);
    PATCH(0x0201, 0x1044, DIV_ROUND_UP(WIDTH * CHANNELS, 64));
    PATCH(0x0201, 0x107c, WIDTH * 4);
    PATCH(0x0201, 0x1080, (uint32_t)-4 & 0x0fffffff);
    PATCH(0x0201, 0x1084, (WIDTH << 16) | HEIGHT);
    PATCH(0x0201, 0x1088, CHANNELS);
    PATCH(0x0801, 0x3014, ((HEIGHT - 1) << 16) | (WIDTH - 1));
    PATCH(0x0801, 0x3018, OUTPUT_CHANNELS - 1);
    PATCH(0x1001, 0x4024, SPATIAL << 4);
    PATCH(0x1001, 0x4020, output_iova);
    PATCH(0x1001, 0x4030, WIDTH - 1);
    PATCH(0x1001, 0x4034, HEIGHT - 1);
    PATCH(0x1001, 0x403c,
          ((OUTPUT_CHANNELS - 1) << 16) | (OUTPUT_CHANNELS - 1));
    PATCH(0x1001, 0x4058, OUTPUT_CHANNELS - 1);
    PATCH(0x1001, 0x405c, ((HEIGHT - 1) << 16) | (WIDTH - 1));
    PATCH(0x1001, 0x40c0, (SPATIAL * 8) << 4);
#undef PATCH

    memset(input, 0, sizeof(input));
    memset(weights, 0, sizeof(weights));
    memset(output, 0xa5, sizeof(output));
    for (unsigned int row = 0; row < HEIGHT; row++) {
        for (unsigned int column = 0; column < WIDTH; column++) {
            for (unsigned int channel = 0; channel < CHANNELS; channel++) {
                int8_t value = ((channel * 37 + 11) % 251) - 125 +
                               row * 2 + column;
                size_t input_index = rk3588_rknn_spatial_feature_index(
                    WIDTH, HEIGHT, 16, channel, row, column);

                input[input_index] = value;
            }
        }
    }
    for (unsigned int channel = 0; channel < OUTPUT_CHANNELS; channel++) {
        size_t weight_index = rk3588_rknn_spatial_weight_index(
            CHANNELS, 1, channel, 0, channel);

        g_assert_cmpuint(weight_index, <, ARRAY_SIZE(weights));
        weights[weight_index] = 1;
    }

    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 5 * 4,
                 output_addr | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 3 * 4,
                 (RK3588_RKNN_MATMUL_WEIGHT_ADDR + 0x1000) |
                 RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 4 * 4,
                 (RK3588_RKNN_MATMUL_WEIGHT_ADDR + 0x2000) |
                 RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 6 * 4,
                 (output_addr + 0x1000) | RK_IOMMU_PTE_RW);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR, input,
                   sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR, weights,
                   sizeof(weights));
    qtest_memwrite(qts, output_addr, output, sizeof(output));

    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, output_addr, output, sizeof(output));
    for (unsigned int row = 0; row < HEIGHT; row++) {
        for (unsigned int column = 0; column < WIDTH; column++) {
            for (unsigned int channel = 0; channel < OUTPUT_CHANNELS;
                 channel++) {
                int32_t expected = (int8_t)(((channel * 37 + 11) % 251) -
                                            125 + row * 2 + column);
                size_t output_index = rk3588_rknn_spatial_feature_index(
                    WIDTH, HEIGHT, 4, channel, row, column);

                g_assert_cmpint((int32_t)le32_to_cpu(output[output_index]),
                                ==, expected);
            }
        }
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_mc_surf_out_spatial(void)
{
    enum {
        WIDTH = 4,
        HEIGHT = 3,
        CHANNELS = 64,
        SPATIAL = WIDTH * HEIGHT,
        OUTPUT_WORDS = 2 * SPATIAL * CHANNELS,
    };
    const uint32_t sentinel = 0xa5a5a5a5;
    const size_t surface_words = SPATIAL * 8 * 4;
    const uint32_t output_iova = 0x10005000;
    const uint64_t output_addr = RK3588_RAM_BASE + 0x30000;
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    int8_t input[SPATIAL * CHANNELS] = { 0 };
    int8_t weights[CHANNELS * CHANNELS] = { 0 };
    uint32_t output[OUTPUT_WORDS];
    bool written[OUTPUT_WORDS] = { false };
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t index;

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(commands, true);

#define PATCH_MC_SPATIAL(_target, _reg, _value) do {                 \
    index = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),  \
                                    (_target), (_reg));              \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    PATCH_MC_SPATIAL(0x0201, 0x1010, (HEIGHT + 1) << 4);
    PATCH_MC_SPATIAL(0x0201, 0x1020, (WIDTH << 16) | HEIGHT);
    PATCH_MC_SPATIAL(0x0201, 0x1024,
                     ((CHANNELS - 1) << 16) | CHANNELS);
    PATCH_MC_SPATIAL(0x0201, 0x1028, WIDTH);
    PATCH_MC_SPATIAL(0x0201, 0x102c, SPATIAL);
    PATCH_MC_SPATIAL(0x0201, 0x1030, CHANNELS * CHANNELS);
    PATCH_MC_SPATIAL(0x0201, 0x1034, CHANNELS);
    PATCH_MC_SPATIAL(0x0201, 0x1038,
                     (1 << 24) | (1 << 16) | CHANNELS);
    PATCH_MC_SPATIAL(0x0201, 0x1044,
                     DIV_ROUND_UP(WIDTH * CHANNELS, 64));
    PATCH_MC_SPATIAL(0x0201, 0x107c, WIDTH * 4);
    PATCH_MC_SPATIAL(0x0201, 0x1080, (uint32_t)-4 & 0x0fffffff);
    PATCH_MC_SPATIAL(0x0201, 0x1084, (WIDTH << 16) | HEIGHT);
    PATCH_MC_SPATIAL(0x0201, 0x1088, CHANNELS);
    PATCH_MC_SPATIAL(0x0801, 0x3014,
                     ((HEIGHT - 1) << 16) | (WIDTH - 1));
    PATCH_MC_SPATIAL(0x0801, 0x3018, CHANNELS - 1);
    PATCH_MC_SPATIAL(0x1001, 0x4010, (4U << 29) | (1U << 3));
    PATCH_MC_SPATIAL(0x1001, 0x4020, output_iova);
    PATCH_MC_SPATIAL(0x1001, 0x4024, SPATIAL << 4);
    PATCH_MC_SPATIAL(0x1001, 0x4030, WIDTH - 1);
    PATCH_MC_SPATIAL(0x1001, 0x4034, HEIGHT - 1);
    PATCH_MC_SPATIAL(0x1001, 0x403c,
                     ((CHANNELS - 1) << 16) | (CHANNELS - 1));
    PATCH_MC_SPATIAL(0x1001, 0x4058, CHANNELS - 1);
    PATCH_MC_SPATIAL(0x1001, 0x405c,
                     ((HEIGHT - 1) << 16) | (WIDTH - 1));
    PATCH_MC_SPATIAL(0x1001, 0x40c0, (SPATIAL * 8) << 4);
#undef PATCH_MC_SPATIAL

    for (unsigned int position = 0; position < SPATIAL; position++) {
        input[rk3588_rknn_spatial_feature_index(
            WIDTH, HEIGHT, 16, 0, position / WIDTH,
            position % WIDTH)] = position + 1;
        input[rk3588_rknn_spatial_feature_index(
            WIDTH, HEIGHT, 16, 1, position / WIDTH,
            position % WIDTH)] = 1;
    }
    for (unsigned int channel = 0; channel < CHANNELS; channel++) {
        weights[rk3588_rknn_spatial_weight_index(
            CHANNELS, 1, channel, 0, 0)] = 64;
        weights[rk3588_rknn_spatial_weight_index(
            CHANNELS, 1, channel, 0, 1)] = channel;
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        output[i] = cpu_to_le32(sentinel);
    }

    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 5 * 4,
                 output_addr | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 6 * 4,
                 (output_addr + 0x1000) | RK_IOMMU_PTE_RW);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR, input,
                   sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR, weights,
                   sizeof(weights));
    qtest_memwrite(qts, output_addr, output, sizeof(output));

    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, output_addr, output, sizeof(output));

    for (unsigned int block = 0; block < 8; block++) {
        size_t block_offset = block / 2 * surface_words + block % 2 * 32;

        for (unsigned int channel = 0; channel < 32; channel++) {
            uint32_t expected = 64 * (block + 1) + channel;

            g_assert_cmphex(le32_to_cpu(output[block_offset + channel]),
                            ==, expected);
            written[block_offset + channel] = true;
        }
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        if (!written[i]) {
            g_assert_cmphex(le32_to_cpu(output[i]), ==, sentinel);
        }
    }
    qtest_quit(qts);
}

static void rk3588_test_rknpu_spatial_rdma_add(bool brdma,
                                               unsigned int channels,
                                               bool fail_second_group)
{
    enum {
        WIDTH = 4,
        HEIGHT = 3,
        MAX_CHANNELS = 64,
        SPATIAL = WIDTH * HEIGHT,
    };
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    int8_t input[SPATIAL * MAX_CHANNELS] = { 0 };
    int8_t weights[MAX_CHANNELS * MAX_CHANNELS] = { 0 };
    int8_t operand[2 * SPATIAL * MAX_CHANNELS] = { 0 };
    uint32_t output[SPATIAL * MAX_CHANNELS];
    QTestState *qts = rk3588_qtest_start_rknpu();
    uint32_t operand_iova = RK3588_RKNN_MATMUL_RDMA_IOVA;
    uint64_t operand_addr = RK3588_RKNN_MATMUL_RDMA_ADDR;
    size_t index;

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(commands, true);
    if (fail_second_group) {
        g_assert_false(brdma);
        g_assert_cmpuint(channels, ==, 64);
        operand_iova += 0xe80;
        operand_addr += 0xe80;
    }

#define PATCH(_target, _reg, _value) do {                            \
    index = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),  \
                                    (_target), (_reg));              \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    PATCH(0x0201, 0x1010, (HEIGHT + 1) << 4);
    PATCH(0x0201, 0x1020, (WIDTH << 16) | HEIGHT);
    g_assert_true(channels == 32 || channels == 64);
    PATCH(0x0201, 0x1024, ((channels - 1) << 16) | channels);
    PATCH(0x0201, 0x1028, WIDTH);
    PATCH(0x0201, 0x102c, SPATIAL);
    PATCH(0x0201, 0x1030, channels * channels);
    PATCH(0x0201, 0x1034, channels);
    PATCH(0x0201, 0x1038, (1 << 24) | (1 << 16) | channels);
    PATCH(0x0201, 0x1044, DIV_ROUND_UP(WIDTH * channels, 64));
    PATCH(0x0201, 0x107c, WIDTH * 4);
    PATCH(0x0201, 0x1080, (uint32_t)-4 & 0x0fffffff);
    PATCH(0x0201, 0x1084, (WIDTH << 16) | HEIGHT);
    PATCH(0x0201, 0x1088, channels);
    PATCH(0x0801, 0x3014, ((HEIGHT - 1) << 16) | (WIDTH - 1));
    PATCH(0x0801, 0x3018, channels - 1);
    PATCH(0x1001, 0x4024, SPATIAL << 4);
    PATCH(0x1001, 0x4030, WIDTH - 1);
    PATCH(0x1001, 0x4034, HEIGHT - 1);
    PATCH(0x1001, 0x403c,
          ((channels - 1) << 16) | (channels - 1));
    PATCH(0x1001, 0x4058, channels - 1);
    PATCH(0x1001, 0x405c, ((HEIGHT - 1) << 16) | (WIDTH - 1));
    PATCH(0x1001, 0x4040, brdma ? 0x20150 : 0x53);
    PATCH(0x1001, 0x4070, brdma ? 0x383 : 0x104203c0);
    PATCH(0x1001, 0x40c0, (SPATIAL * 8) << 4);
#undef PATCH

    commands[91] = rk3588_rknn_regcmd(0x2001, 0x500c, WIDTH - 1);
    commands[92] = rk3588_rknn_regcmd(0x2001, 0x5010, HEIGHT - 1);
    commands[93] = rk3588_rknn_regcmd(0x2001, 0x5014, channels - 1);
    commands[94] = rk3588_rknn_regcmd(
        0x2001, brdma ? 0x501c : 0x5018,
        brdma ? 2 : operand_iova);
    commands[95] = rk3588_rknn_regcmd(
        0x2001, brdma ? 0x5020 : 0x5034,
        brdma ? operand_iova : 0x40000004);
    commands[96] = rk3588_rknn_regcmd(
        0x2001, brdma ? 0x5034 : 0x5038,
        brdma ? 1 : (fail_second_group ? 0x10007000 :
                     operand_iova + SPATIAL * channels));
    commands[97] = rk3588_rknn_regcmd(
        0x2001, brdma ? 0x5044 : 0x5040,
        brdma ? 0x7810 : SPATIAL << 4);
    commands[98] = rk3588_rknn_regcmd(
        0x2001, brdma ? 0x5048 : 0x5044, brdma ? 0 : 0x7d00);
    commands[99] = rk3588_rknn_regcmd(0x2001, 0x5048, 0);
    commands[100] = rk3588_rknn_regcmd(
        0x2001, brdma ? 0x5068 : 0x504c,
        brdma ? 0x01010101 : SPATIAL << 4);
    commands[101] = rk3588_rknn_regcmd(0x2001, 0x5064, 0);
    commands[102] = rk3588_rknn_regcmd(
        0x2001, brdma ? 0x5038 : 0x5068,
        brdma ? 0 : 0x01010101);
    commands[103] = rk3588_rknn_regcmd(
        0x2001, 0x506c, brdma ? 0 : SPATIAL << 4);
    commands[107] = rk3588_rknn_regcmd(0x0081, 0x0008, 0x1d);

    memset(output, 0xa5, sizeof(output));
    for (unsigned int row = 0; row < HEIGHT; row++) {
        for (unsigned int column = 0; column < WIDTH; column++) {
            for (unsigned int channel = 0; channel < channels; channel++) {
                size_t input_index = rk3588_rknn_spatial_feature_index(
                    WIDTH, HEIGHT, 16, channel, row, column);

                input[input_index] =
                    ((channel * 37 + 11) % 251) - 125 + row * 2 + column;
            }
        }
    }
    for (unsigned int channel = 0; channel < channels; channel++) {
        size_t weight_index = rk3588_rknn_spatial_weight_index(
            channels, 1, channel, 0, channel);

        weights[weight_index] = 1;
    }
    if (brdma) {
        for (unsigned int channel = 0; channel < channels; channel++) {
            ((uint32_t *)operand)[channel] = cpu_to_le32(100 + channel);
        }
    } else {
        for (unsigned int group = 0; group < channels / 32; group++) {
            size_t surface_offset = group * SPATIAL * 32;

            for (unsigned int position = 0; position < SPATIAL; position++) {
                for (unsigned int lane = 0; lane < 16; lane++) {
                    size_t offset = surface_offset + position * 16 + lane;

                    operand[offset] = group * 32 + lane + 1;
                    operand[SPATIAL * channels + offset] =
                        group * 32 + lane + 17;
                }
            }
        }
    }

    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 5 * 4,
                 RK3588_RKNN_MATMUL_RDMA_ADDR | RK_IOMMU_PTE_RW);
    if (fail_second_group) {
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 6 * 4, 0);
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 7 * 4,
                     (RK3588_RKNN_MATMUL_RDMA_ADDR + 0x2000) |
                     RK_IOMMU_PTE_RW);
    }
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR, input,
                   sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR, weights,
                   sizeof(weights));
    if (fail_second_group) {
        qtest_memwrite(qts, operand_addr, operand, SPATIAL * channels);
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_RDMA_ADDR + 0x2000,
                       operand + SPATIAL * channels,
                       SPATIAL * channels);
    } else {
        qtest_memwrite(qts, operand_addr, operand, sizeof(operand));
    }
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800, output,
                   0x800);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR1,
                   (uint8_t *)output + 0x800, sizeof(output) - 0x800);

    rk3588_rknn_start_matmul(qts);
    qtest_writeq(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR +
                      91 * sizeof(uint64_t),
                 le64_to_cpu(rk3588_rknn_regcmd(0x2001, 0x500c, 0)));
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR0 + 0x800, output,
                  0x800);
    qtest_memread(qts, RK3588_RKNN_MATMUL_OUTPUT_ADDR1,
                  (uint8_t *)output + 0x800, sizeof(output) - 0x800);
    if (fail_second_group) {
        for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
            g_assert_cmphex(le32_to_cpu(output[i]), ==, 0xa5a5a5a5);
        }
        g_assert_cmphex(rk3588_rknn_read_pc(
                            qts, RKNN_PC_INTERRUPT_RAW_STATUS) &
                        RKNN_DMA_READ_ERROR, ==, RKNN_DMA_READ_ERROR);
        qtest_quit(qts);
        return;
    }
    for (unsigned int row = 0; row < HEIGHT; row++) {
        for (unsigned int column = 0; column < WIDTH; column++) {
            for (unsigned int channel = 0; channel < channels; channel++) {
                int32_t expected = (int8_t)(((channel * 37 + 11) % 251) -
                                            125 + row * 2 + column);
                size_t output_index = rk3588_rknn_spatial_feature_index(
                    WIDTH, HEIGHT, 4, channel, row, column);

                expected += brdma ? 100 + channel : channel + 1;
                g_assert_cmpint((int32_t)le32_to_cpu(output[output_index]),
                                ==, expected);
            }
        }
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_spatial_rdma_ew_add_grouped(void)
{
    rk3588_test_rknpu_spatial_rdma_add(false, 64, false);
}

static void test_rk3588_rknpu_spatial_rdma_grouped_iommu_failure(void)
{
    rk3588_test_rknpu_spatial_rdma_add(false, 64, true);
}

static void test_rk3588_rknpu_spatial_brdma_bs_add_grouped(void)
{
    rk3588_test_rknpu_spatial_rdma_add(true, 64, false);
}

static void rk3588_test_rknpu_conv3x3_padding_hardware_shape(
    unsigned int stride, unsigned int pad_top)
{
    enum {
        WIDTH = 5,
        HEIGHT = 4,
        CHANNELS = 64,
        OUTPUT_CHANNELS = 64,
        KERNEL = 3,
        SPATIAL = WIDTH * HEIGHT,
        WEIGHT_CHANNELS = KERNEL * KERNEL * CHANNELS,
    };
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    int8_t input[SPATIAL * CHANNELS];
    int8_t weights[OUTPUT_CHANNELS * WEIGHT_CHANNELS];
    uint32_t output[SPATIAL * OUTPUT_CHANNELS];
    QTestState *qts = rk3588_qtest_start_rknpu();
    const uint32_t output_iova = 0x1000c000;
    const uint64_t output_addr = RK3588_RAM_BASE + 0x30000;
    const unsigned int total_padding = pad_top ? 2 : 1;
    const unsigned int output_width =
        (WIDTH + total_padding - KERNEL) / stride + 1;
    const unsigned int output_height =
        (HEIGHT + total_padding - KERNEL) / stride + 1;
    const unsigned int output_spatial = output_width * output_height;
    size_t index;

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(commands, true);

#define PATCH(_target, _reg, _value) do {                            \
    index = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),  \
                                    (_target), (_reg));              \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    PATCH(0x0201, 0x1010, (HEIGHT + 1) << 4);
    PATCH(0x0201, 0x1014, (stride << 3) | stride);
    PATCH(0x0201, 0x1020, (WIDTH << 16) | HEIGHT);
    PATCH(0x0201, 0x1024, ((CHANNELS - 1) << 16) | CHANNELS);
    PATCH(0x0201, 0x1028, output_width);
    PATCH(0x0201, 0x102c, output_spatial);
    PATCH(0x0201, 0x1030, OUTPUT_CHANNELS * WEIGHT_CHANNELS);
    PATCH(0x0201, 0x1034, WEIGHT_CHANNELS);
    PATCH(0x0201, 0x1038,
          (KERNEL << 24) | (KERNEL << 16) | OUTPUT_CHANNELS);
    PATCH(0x0201, 0x1044, DIV_ROUND_UP(WIDTH * CHANNELS, 64));
    PATCH(0x0201, 0x1068, (1 << 4) | pad_top);
    PATCH(0x0201, 0x107c, WIDTH * 4);
    PATCH(0x0201, 0x1080, 0);
    PATCH(0x0201, 0x1084, (WIDTH << 16) | HEIGHT);
    PATCH(0x0201, 0x1088, CHANNELS);
    PATCH(0x0801, 0x3014,
          ((output_height - 1) << 16) | (output_width - 1));
    PATCH(0x0801, 0x3018, OUTPUT_CHANNELS - 1);
    PATCH(0x1001, 0x4024, output_spatial << 4);
    PATCH(0x1001, 0x4020, output_iova);
    PATCH(0x1001, 0x4030, output_width - 1);
    PATCH(0x1001, 0x4034, output_height - 1);
    PATCH(0x1001, 0x403c,
          ((OUTPUT_CHANNELS - 1) << 16) | (OUTPUT_CHANNELS - 1));
    PATCH(0x1001, 0x4058, OUTPUT_CHANNELS - 1);
    PATCH(0x1001, 0x405c,
          ((output_height - 1) << 16) | (output_width - 1));
    PATCH(0x1001, 0x40c0, (output_spatial * 8) << 4);
#undef PATCH

    memset(input, 0, sizeof(input));
    memset(weights, 0, sizeof(weights));
    memset(output, 0xa5, sizeof(output));
    for (unsigned int row = 0; row < HEIGHT; row++) {
        for (unsigned int column = 0; column < WIDTH; column++) {
            for (unsigned int channel = 0; channel < CHANNELS; channel++) {
                int8_t value = ((channel * 37 + 11) % 251) - 125 +
                               row * 2 + column;
                size_t input_index = rk3588_rknn_spatial_feature_index(
                    WIDTH, HEIGHT, 16, channel, row, column);

                input[input_index] = value;
            }
        }
    }
    for (unsigned int output_channel = 0;
         output_channel < OUTPUT_CHANNELS; output_channel++) {
        for (unsigned int kernel_row = 0; kernel_row < KERNEL; kernel_row++) {
            for (unsigned int kernel_column = 0;
                 kernel_column < KERNEL; kernel_column++) {
                unsigned int kernel =
                    kernel_row * KERNEL + kernel_column;
                size_t weight_index = rk3588_rknn_spatial_weight_index(
                    CHANNELS, KERNEL * KERNEL, output_channel, kernel,
                    output_channel);

                weights[weight_index] =
                    kernel_row * KERNEL + kernel_column + 1;
            }
        }
    }

    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    for (unsigned int page = 3; page <= 10; page++) {
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + page * 4,
                     (RK3588_RKNN_MATMUL_WEIGHT_ADDR +
                      (page - 2) * 0x1000) | RK_IOMMU_PTE_RW);
    }
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 12 * 4,
                 output_addr | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 13 * 4,
                 (output_addr + 0x1000) | RK_IOMMU_PTE_RW);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_INPUT_ADDR, input,
                   sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_WEIGHT_ADDR, weights,
                   sizeof(weights));
    qtest_memwrite(qts, output_addr, output,
                   output_spatial * OUTPUT_CHANNELS * sizeof(*output));

    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, output_addr, output,
                  output_spatial * OUTPUT_CHANNELS * sizeof(*output));
    for (int row = 0; row < output_height; row++) {
        for (int column = 0; column < output_width; column++) {
            for (unsigned int channel = 0; channel < OUTPUT_CHANNELS;
                 channel++) {
                int32_t expected = 0;
                size_t output_index = rk3588_rknn_spatial_feature_index(
                    output_width, output_height, 4, channel, row, column);

                for (int kernel_row = 0; kernel_row < KERNEL; kernel_row++) {
                    for (int kernel_column = 0; kernel_column < KERNEL;
                         kernel_column++) {
                        int input_row =
                            row * stride - pad_top + kernel_row;
                        int input_column =
                            column * stride - 1 + kernel_column;

                        if (input_row >= 0 && input_row < HEIGHT &&
                            input_column >= 0 && input_column < WIDTH) {
                            int8_t input_value =
                                ((channel * 37 + 11) % 251) - 125 +
                                input_row * 2 + input_column;

                            expected += input_value *
                                        (kernel_row * KERNEL +
                                         kernel_column + 1);
                        }
                    }
                }
                g_assert_cmpint((int32_t)le32_to_cpu(output[output_index]),
                                ==, expected);
            }
        }
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_conv3x3_padding_hardware_shape(void)
{
    rk3588_test_rknpu_conv3x3_padding_hardware_shape(1, 1);
}

static void test_rk3588_rknpu_conv3x3_stride2_hardware_shape(void)
{
    rk3588_test_rknpu_conv3x3_padding_hardware_shape(2, 1);
}

static void test_rk3588_rknpu_conv3x3_asymmetric_hardware_shape(void)
{
    rk3588_test_rknpu_conv3x3_padding_hardware_shape(1, 0);
}

static void test_rk3588_rknpu_depthwise_int8_qd(void)
{
    uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
    int8_t input[2 * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE];
    int8_t weights[RK3588_RKNN_DEPTHWISE_WEIGHT_BYTES];
    uint8_t bs[RK3588_RKNN_DEPTHWISE_INT8_BS_BYTES];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_depthwise_int8_regcmd(commands);
    rk3588_rknn_make_depthwise_int8_data(input, weights, bs);
    rk3588_rknn_prepare_depthwise_int8(
        qts, commands, input, weights, bs);
    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_assert_depthwise_int8_result(qts, input, weights, bs);
    qtest_quit(qts);
}

static void rk3588_rknn_run_depthwise_int8_qd_cpend_case(
    uint32_t bs_ow_cfg, bool nonzero_cpend, uint8_t output[])
{
    uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
    int8_t input[2 * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE];
    int8_t weights[RK3588_RKNN_DEPTHWISE_WEIGHT_BYTES];
    uint8_t bs[RK3588_RKNN_DEPTHWISE_INT8_BS_BYTES];
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t index;

    rk3588_rknn_make_depthwise_int8_regcmd(commands);
    index = rk3588_rknn_find_regcmd(
        commands, ARRAY_SIZE(commands), 0x1001, 0x4050);
    commands[index] = rk3588_rknn_regcmd(
        0x1001, 0x4050, bs_ow_cfg);
    rk3588_rknn_make_depthwise_int8_data(input, weights, bs);
    if (!nonzero_cpend) {
        for (unsigned int channel = 0;
             channel < RK3588_RKNN_DEPTHWISE_INT8_STORAGE_CHANNELS;
             channel++) {
            unsigned int group = channel / 8;
            unsigned int lane = channel % 8;

            stw_le_p(bs + group * 0x40 + 0x20 + lane * 2, 0);
        }
    }
    rk3588_rknn_prepare_depthwise_int8(
        qts, commands, input, weights, bs);
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, rk3588_rknn_synth_conv_addr(
                      RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA),
                  output, RK3588_RKNN_DEPTHWISE_INT8_OUTPUT_BYTES);
    rk3588_rknn_assert_pc(
        qts, RKNN_TASK_STATUS_SUCCESS,
        RKNN_STAGE_RAW_STATUS_BITS | RKNN_PIPELINE_BANK1_INTERRUPT);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_depthwise_int8_qd_no_cpend(void)
{
    enum {
        EXTERNAL_NONZERO,
        EXTERNAL_ZERO,
        BYPASS_NONZERO,
        BYPASS_ZERO,
        CASES,
    };
    uint8_t output[CASES][RK3588_RKNN_DEPTHWISE_INT8_OUTPUT_BYTES];
    bool external_changed = false;
    bool bypass_changed = false;
    bool mode_changed = false;

    rk3588_rknn_run_depthwise_int8_qd_cpend_case(
        0x36d, true, output[EXTERNAL_NONZERO]);
    rk3588_rknn_run_depthwise_int8_qd_cpend_case(
        0x36d, false, output[EXTERNAL_ZERO]);
    rk3588_rknn_run_depthwise_int8_qd_cpend_case(
        0x36c, true, output[BYPASS_NONZERO]);
    rk3588_rknn_run_depthwise_int8_qd_cpend_case(
        0x36c, false, output[BYPASS_ZERO]);

    for (unsigned int row = 0;
         row < RK3588_RKNN_DEPTHWISE_INT8_HEIGHT; row++) {
        for (unsigned int column = 0;
             column < RK3588_RKNN_DEPTHWISE_INT8_WIDTH; column++) {
            for (unsigned int channel = 0;
                 channel < RK3588_RKNN_DEPTHWISE_INT8_VALID_CHANNELS;
                 channel++) {
                size_t index = channel / 16 *
                    RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE +
                    (row * RK3588_RKNN_DEPTHWISE_INT8_WIDTH + column) * 16 +
                    channel % 16;

                external_changed |= output[EXTERNAL_NONZERO][index] !=
                                    output[EXTERNAL_ZERO][index];
                bypass_changed |= output[BYPASS_NONZERO][index] !=
                                  output[BYPASS_ZERO][index];
                mode_changed |= output[EXTERNAL_NONZERO][index] !=
                                output[BYPASS_NONZERO][index];
            }
        }
    }
    g_assert_true(external_changed);
    g_assert_false(bypass_changed);
    g_assert_true(mode_changed);
}

static void test_rk3588_rknpu_depthwise_int8_deconv(void)
{
    enum {
        INPUT_WIDTH = 2,
        INPUT_HEIGHT = 2,
        OUTPUT_WIDTH = 4,
        OUTPUT_HEIGHT = 4,
        CHANNELS = 32,
        STORAGE_CHANNELS = 64,
        KERNEL = 4,
        WEIGHT_BYTES = KERNEL * KERNEL * CHANNELS,
    };
    uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
    int8_t input[2 * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE] = { 0 };
    int8_t weights[WEIGHT_BYTES];
    uint8_t bs[RK3588_RKNN_DEPTHWISE_INT8_BS_BYTES] = { 0 };
    uint8_t output[4 * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE];
    QTestState *qts = rk3588_qtest_start_rknpu();
    uint64_t output_addr = rk3588_rknn_synth_conv_addr(
        RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA);

    rk3588_rknn_make_depthwise_int8_deconv_regcmd(commands);
    for (unsigned int row = 0; row < INPUT_HEIGHT; row++) {
        for (unsigned int column = 0; column < INPUT_WIDTH; column++) {
            for (unsigned int channel = 0; channel < CHANNELS; channel++) {
                size_t index = channel / 16 *
                    RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE +
                    (row * INPUT_WIDTH + column) * 16 + channel % 16;

                input[index] =
                    ((row * 7 + column * 5 + channel * 3) % 7) - 3;
            }
        }
    }
    for (unsigned int kernel = 0; kernel < KERNEL * KERNEL; kernel++) {
        for (unsigned int channel = 0; channel < CHANNELS; channel++) {
            weights[kernel * CHANNELS + channel] =
                ((kernel * 3 + channel * 5) % 3) - 1;
        }
    }
    for (unsigned int channel = 0; channel < STORAGE_CHANNELS; channel++) {
        unsigned int group = channel / 8;
        unsigned int lane = channel % 8;

        stw_le_p(bs + group * 0x40 + 0x30 + lane * 2, 32);
    }
    rk3588_rknn_prepare_depthwise_int8(
        qts, commands, input, weights, bs);
    qtest_memwrite(qts, rk3588_rknn_synth_conv_addr(
                       RK3588_RKNN_SYNTH_CONV_WEIGHT_IOVA),
                   weights, sizeof(weights));
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, output_addr, output, sizeof(output));

    for (unsigned int surface = 0; surface < 4; surface++) {
        for (unsigned int row = 0; row < OUTPUT_HEIGHT; row++) {
            for (unsigned int column = 0; column < OUTPUT_WIDTH; column++) {
                for (unsigned int lane = 0; lane < 16; lane++) {
                    unsigned int channel = surface * 16 + lane;
                    size_t index = surface *
                        RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE +
                        (row * OUTPUT_WIDTH + column) * 16 + lane;
                    int8_t expected = channel < CHANNELS ?
                        rk3588_rknn_depthwise_int8_deconv_expected(
                            input, weights, channel, row, column) :
                        (int8_t)0xa5;

                    g_assert_cmpint((int8_t)output[index], ==, expected);
                }
            }
        }
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_depthwise_int8_deconv_controls(void)
{
    uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
    int8_t input[2 * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE] = { 0 };
    int8_t weights[4 * 4 * 32] = { 0 };
    uint8_t bs[RK3588_RKNN_DEPTHWISE_INT8_BS_BYTES] = { 0 };
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t index;

    rk3588_rknn_make_depthwise_int8_deconv_regcmd(commands);
    index = rk3588_rknn_find_regcmd(
        commands, ARRAY_SIZE(commands), 0x0201, 0x1014);
    commands[index] = rk3588_rknn_regcmd(0x0201, 0x1014, 0x00000909);
    index = rk3588_rknn_find_regcmd(
        commands, ARRAY_SIZE(commands), 0x0801, 0x3014);
    commands[index] = rk3588_rknn_regcmd(0x0801, 0x3014, 0x00030004);
    rk3588_rknn_prepare_depthwise_int8(
        qts, commands, input, weights, bs);
    rk3588_rknn_run_matmul(qts);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_FETCH_ERROR | 1);
    g_assert_cmphex(qtest_readb(
                        qts, rk3588_rknn_synth_conv_addr(
                                 RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA)),
                    ==, 0xa5);
    qtest_quit(qts);
}

static void rk3588_rknn_make_depthwise_int8_brdma(
    uint64_t commands[], uint8_t bs[RK3588_RKNN_DEPTHWISE_INT8_BS_BYTES])
{
    size_t index;

#define PATCH_DEPTHWISE_BRDMA(_target, _reg, _value) do {             \
    index = rk3588_rknn_find_regcmd(                                  \
        commands, RK3588_RKNN_SYNTH_CONV_COMMANDS,               \
        (_target), (_reg));                                           \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    PATCH_DEPTHWISE_BRDMA(0x1001, 0x4048, 0x00010500);
    PATCH_DEPTHWISE_BRDMA(0x1001, 0x4050, 0x36c);
    PATCH_DEPTHWISE_BRDMA(0x1001, 0x4054, 18);
    PATCH_DEPTHWISE_BRDMA(0x1001, 0x40c0, 0);
    PATCH_DEPTHWISE_BRDMA(0x2001, 0x501c, 2);
#undef PATCH_DEPTHWISE_BRDMA

    memset(bs, 0, RK3588_RKNN_DEPTHWISE_INT8_BS_BYTES);
    for (unsigned int channel = 0;
         channel < RK3588_RKNN_DEPTHWISE_INT8_VALID_CHANNELS; channel++) {
        stl_le_p(bs + channel * sizeof(uint32_t), (int32_t)channel - 16);
    }
}

static int8_t rk3588_rknn_depthwise_int8_brdma_expected(
    const int8_t input[], const int8_t weights[],
    unsigned int channel, unsigned int row, unsigned int column)
{
    int value = (int32_t)channel - 16;

    for (unsigned int kernel_row = 0; kernel_row < 3; kernel_row++) {
        for (unsigned int kernel_column = 0; kernel_column < 3;
             kernel_column++) {
            int input_row = row - 1 + kernel_row;
            int input_column = column - 1 + kernel_column;
            int8_t input_value = -128;
            unsigned int kernel = kernel_row * 3 + kernel_column;

            if (input_row >= 0 &&
                input_row < RK3588_RKNN_DEPTHWISE_INT8_HEIGHT &&
                input_column >= 0 &&
                input_column < RK3588_RKNN_DEPTHWISE_INT8_WIDTH) {
                size_t input_index = channel / 16 *
                    RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE +
                    (input_row * RK3588_RKNN_DEPTHWISE_INT8_WIDTH +
                     input_column) * 16 + channel % 16;

                input_value = input[input_index];
            }
            value += input_value *
                (weights[kernel *
                         RK3588_RKNN_DEPTHWISE_INT8_VALID_CHANNELS +
                         channel] + 18);
        }
    }
    value = rk3588_rknn_rgb_cvt_round_shift(value, 5, false);
    return CLAMP(value, INT8_MIN, INT8_MAX);
}

static void test_rk3588_rknpu_depthwise_int8_brdma(void)
{
    uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
    int8_t input[2 * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE];
    int8_t weights[RK3588_RKNN_DEPTHWISE_WEIGHT_BYTES];
    uint8_t bs[RK3588_RKNN_DEPTHWISE_INT8_BS_BYTES];
    uint8_t output[RK3588_RKNN_DEPTHWISE_INT8_OUTPUT_BYTES + 1];
    uint64_t output_addr = rk3588_rknn_synth_conv_addr(
        RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA);
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_make_depthwise_int8_regcmd(commands);
    rk3588_rknn_make_depthwise_int8_data(input, weights, bs);
    rk3588_rknn_make_depthwise_int8_brdma(commands, bs);
    rk3588_rknn_prepare_depthwise_int8(qts, commands, input, weights, bs);
    rk3588_rknn_run_matmul(qts);
    qtest_memread(qts, output_addr, output, sizeof(output));

    for (unsigned int channel = 0;
         channel < RK3588_RKNN_DEPTHWISE_INT8_VALID_CHANNELS; channel++) {
        for (unsigned int row = 0;
             row < RK3588_RKNN_DEPTHWISE_INT8_HEIGHT; row++) {
            for (unsigned int column = 0;
                 column < RK3588_RKNN_DEPTHWISE_INT8_WIDTH; column++) {
                size_t output_index = channel / 16 *
                    RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE +
                    (row * RK3588_RKNN_DEPTHWISE_INT8_WIDTH + column) * 16 +
                    channel % 16;

                g_assert_cmpint((int8_t)output[output_index], ==,
                    rk3588_rknn_depthwise_int8_brdma_expected(
                        input, weights, channel, row, column));
            }
        }
    }
    g_assert_cmphex(output[RK3588_RKNN_DEPTHWISE_INT8_OUTPUT_BYTES], ==,
                    0xa5);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_depthwise_int8_brdma_controls(void)
{
    static const uint32_t unsupported[] = { 0x36d, 0x124 };

    for (unsigned int case_index = 0;
         case_index < ARRAY_SIZE(unsupported); case_index++) {
        uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
        int8_t input[2 * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE];
        int8_t weights[RK3588_RKNN_DEPTHWISE_WEIGHT_BYTES];
        uint8_t bs[RK3588_RKNN_DEPTHWISE_INT8_BS_BYTES];
        uint64_t output_addr = rk3588_rknn_synth_conv_addr(
            RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA);
        QTestState *qts = rk3588_qtest_start_rknpu();
        size_t index;

        rk3588_rknn_make_depthwise_int8_regcmd(commands);
        rk3588_rknn_make_depthwise_int8_data(input, weights, bs);
        rk3588_rknn_make_depthwise_int8_brdma(commands, bs);
        index = rk3588_rknn_find_regcmd(
            commands, ARRAY_SIZE(commands), 0x1001, 0x4050);
        commands[index] = rk3588_rknn_regcmd(
            0x1001, 0x4050, unsupported[case_index]);
        rk3588_rknn_prepare_depthwise_int8(
            qts, commands, input, weights, bs);
        rk3588_rknn_run_matmul(qts);

        g_assert_cmphex(qtest_readb(qts, output_addr), ==, 0xa5);
        rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
        qtest_quit(qts);
    }
}

static void test_rk3588_rknpu_depthwise_int8_grouped_weights(void)
{
    enum {
        CHANNELS = 128,
        SURFACES = CHANNELS / 16,
        WEIGHT_BYTES = CHANNELS * 9,
        BS_BYTES = CHANNELS / 8 * 0x40,
    };
    uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
    int8_t input[SURFACES * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE] = {};
    int8_t weights[WEIGHT_BYTES] = {};
    uint8_t bs[BS_BYTES] = {};
    uint16_t multiplier = cpu_to_le16(32);
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t index;

    rk3588_rknn_make_depthwise_int8_regcmd(commands);
#define PATCH_DEPTHWISE_GROUP(_target, _reg, _value) do {             \
    index = rk3588_rknn_find_regcmd(                                  \
        commands, ARRAY_SIZE(commands), (_target), (_reg));           \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    PATCH_DEPTHWISE_GROUP(0x0201, 0x1024,
                          ((CHANNELS - 1) << 16) | CHANNELS);
    PATCH_DEPTHWISE_GROUP(0x0201, 0x1030, WEIGHT_BYTES);
    PATCH_DEPTHWISE_GROUP(0x0201, 0x1034, WEIGHT_BYTES);
    PATCH_DEPTHWISE_GROUP(0x0201, 0x1088, CHANNELS);
    PATCH_DEPTHWISE_GROUP(0x0801, 0x3018, CHANNELS - 1);
    PATCH_DEPTHWISE_GROUP(0x1001, 0x403c,
                          ((CHANNELS - 1) << 16) | (CHANNELS - 1));
    PATCH_DEPTHWISE_GROUP(0x1001, 0x4058, CHANNELS - 1);
    PATCH_DEPTHWISE_GROUP(0x2001, 0x5014, CHANNELS - 1);
#undef PATCH_DEPTHWISE_GROUP

    input[4 * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE + 16] = 1;
    weights[64 * 9 + 64] = 7;
    memcpy(bs + 8 * 0x40 + 0x30, &multiplier, sizeof(multiplier));

    qtest_writel(qts, RK3588_RKNN_MATMUL_DTE_ADDR + 64 * 4,
                 RK3588_RKNN_MATMUL_PTE_ADDR | RK_IOMMU_PTE_VALID);
    for (unsigned int page = 0;
         page < RK3588_RKNN_SYNTH_CONV_MAPPED_PAGES; page++) {
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + page * 4,
                     (RK3588_RKNN_SYNTH_CONV_ADDR + page * 0x1000) |
                     RK_IOMMU_PTE_RW);
    }
    qtest_memwrite(qts, RK3588_RKNN_SYNTH_CONV_ADDR, commands,
                   sizeof(commands));
    qtest_memwrite(qts, rk3588_rknn_synth_conv_addr(
                       RK3588_RKNN_SYNTH_CONV_INPUT_IOVA),
                   input, sizeof(input));
    qtest_memwrite(qts, rk3588_rknn_synth_conv_addr(
                       RK3588_RKNN_SYNTH_CONV_WEIGHT_IOVA),
                   weights, sizeof(weights));
    qtest_memwrite(qts, rk3588_rknn_synth_conv_addr(
                       RK3588_RKNN_SYNTH_CONV_BS_IOVA),
                   bs, sizeof(bs));
    qtest_memset(qts, rk3588_rknn_synth_conv_addr(
                     RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA),
                 0xa5,
                 SURFACES * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_DTE_ADDR,
                 RK3588_RKNN_MATMUL_DTE_ADDR);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_ENABLE_PAGING);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_BASE_ADDRESS,
                 RK3588_RKNN_SYNTH_CONV_REGCMD_IOVA);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(ARRAY_SIZE(commands)));
    rk3588_rknn_run_matmul(qts);

    g_assert_cmpint((int8_t)qtest_readb(
                        qts, rk3588_rknn_synth_conv_addr(
                                 RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA) +
                                 4 *
                                 RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE +
                                 (RK3588_RKNN_DEPTHWISE_INT8_WIDTH + 1) * 16),
                    ==, 7);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_depthwise_int8_control_mutations(void)
{
    static const struct {
        const char *name;
        uint32_t target;
        uint32_t reg;
        uint32_t value;
    } cases[] = {
        { "bs-ow-convolution", 0x1001, 0x4050, 0x125 },
        { "brdma-convolution", 0x2001, 0x5044, 0x7810 },
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        uint64_t commands[RK3588_RKNN_SYNTH_CONV_COMMANDS];
        int8_t input[2 * RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE];
        int8_t weights[RK3588_RKNN_DEPTHWISE_WEIGHT_BYTES];
        uint8_t bs[RK3588_RKNN_DEPTHWISE_INT8_BS_BYTES];
        uint64_t output_addr = rk3588_rknn_synth_conv_addr(
            RK3588_RKNN_SYNTH_CONV_OUTPUT_IOVA);
        QTestState *qts = rk3588_qtest_start_rknpu();
        size_t index;

        g_test_message("depthwise INT8 mutation: %s", cases[i].name);
        rk3588_rknn_make_depthwise_int8_regcmd(commands);
        index = rk3588_rknn_find_regcmd(
            commands, ARRAY_SIZE(commands), cases[i].target, cases[i].reg);
        commands[index] = rk3588_rknn_regcmd(
            cases[i].target, cases[i].reg, cases[i].value);
        rk3588_rknn_make_depthwise_int8_data(input, weights, bs);
        rk3588_rknn_prepare_depthwise_int8(
            qts, commands, input, weights, bs);
        rk3588_rknn_run_matmul(qts);
        g_assert_cmphex(qtest_readb(qts, output_addr), ==, 0xa5);
        g_assert_cmphex(qtest_readb(
            qts, output_addr +
                 RK3588_RKNN_DEPTHWISE_INT8_SURFACE_STRIDE), ==, 0xa5);
        rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
        qtest_quit(qts);
    }
}

static void test_rk3588_rknpu_depthwise_control_mutations(void)
{
    static const struct {
        const char *name;
        uint32_t target;
        uint32_t reg;
        uint32_t value;
    } cases[] = {
        { "weight-bytes", 0x0201, 0x1030,
          RK3588_RKNN_DEPTHWISE_WEIGHT_BYTES - 1 },
        { "weight-bytes-per-kernel", 0x0201, 0x1034,
          RK3588_RKNN_DEPTHWISE_WEIGHT_BYTES - 1 },
        { "weight-kernels", 0x0201, 0x1038, 0x03030002 },
        { "cna-conv-mode", 0x0201, 0x100c, 0 },
        { "core-depthwise", 0x0801, 0x3010, 0 },
        { "dpu-conv-mode", 0x1001, 0x400c, 0x1e4 },
        { "valid-channel-planes", 0x1001, 0x403c, 0x001b003f },
        { "output-precision", 0x1001, 0x4010, 0 },
        { "surface-add", 0x1001, 0x40c0, 0x2010 },
    };
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
        uint8_t output[RK3588_RKNN_DEPTHWISE_OUTPUT_WORDS *
                       sizeof(uint32_t)];
        size_t index;

        if (i) {
            rk3588_rknpu_reset_fixture(qts);
        }
        g_test_message("depthwise mutation: %s", cases[i].name);
        rk3588_rknn_make_depthwise_regcmd(commands);
        index = rk3588_rknn_find_regcmd(
            commands, ARRAY_SIZE(commands), cases[i].target, cases[i].reg);
        commands[index] = rk3588_rknn_regcmd(
            cases[i].target, cases[i].reg, cases[i].value);
        rk3588_rknn_prepare_depthwise(qts, commands, 0xa5);
        rk3588_rknn_run_matmul(qts);
        qtest_memread(qts, RK3588_RKNN_DEPTHWISE_OUTPUT_ADDR, output,
                      sizeof(output));
        for (unsigned int byte = 0; byte < ARRAY_SIZE(output); byte++) {
            g_assert_cmphex(output[byte], ==, 0xa5);
        }
        rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_out_cvt(void)
{
    static const struct {
        int32_t offset;
        uint16_t scale;
        uint32_t shift;
    } cases[] = {
        {5, 1, 0},
        {-7, 2, 0},
        {0, 3, 1},
        {0, 3, (1U << 30) | 1},
        {INT32_MAX, 2, 0},
        {INT32_MIN, 2, 0},
        {5, 3, 1},
        {5, 3, (1U << 31) | 1},
        {5, 3, (1U << 12) | 1},
        {5, 3, (1U << 31) | (1U << 12) | 1},
    };
    static const int32_t hardware_scale3_shift1_first16[] = {
        304, -196, -306, -68, -308, 627, -222, -244,
        429, -159, -530, 448, 122, -554, 686, 98,
    };
    static const int32_t hardware_round_away_first16[] = {
        305, -197, -306, -68, -308, 627, -222, -245,
        429, -159, -530, 449, 122, -554, 686, 98,
    };
    static const int32_t hardware_type0_offset_first16[] = {
        310, -192, -301, -62, -302, 632, -217, -240,
        434, -154, -524, 454, 126, -548, 690, 102,
    };
    static const int32_t hardware_type1_offset_first16[] = {
        312, -189, -298, -60, -300, 634, -214, -237,
        436, -152, -522, 456, 129, -546, 693, 105,
    };
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    uint32_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N];
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int c = 0; c < ARRAY_SIZE(cases); c++) {
        size_t offset;
        size_t scale;
        size_t shift;

        if (c) {
            rk3588_rknpu_reset_fixture(qts);
        }
        rk3588_rknn_prepare_matmul(qts, true, 0xa5);
        rk3588_rknn_make_matmul_regcmd(commands, true);
        offset = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),
                                         0x1001, 0x4080);
        scale = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),
                                        0x1001, 0x4084);
        shift = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),
                                        0x1001, 0x4088);
        commands[offset] = rk3588_rknn_regcmd(0x1001, 0x4080,
                                               cases[c].offset);
        commands[scale] = rk3588_rknn_regcmd(0x1001, 0x4084,
                                              cases[c].scale);
        commands[shift] = rk3588_rknn_regcmd(0x1001, 0x4088,
                                              cases[c].shift);
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                       sizeof(commands));
        rk3588_rknn_run_matmul(qts);
        rk3588_rknn_read_matmul_output(qts, output, sizeof(output));

        for (unsigned int row = 0; row < RK3588_RKNN_MATMUL_M; row++) {
            for (unsigned int out = 0; out < RK3588_RKNN_MATMUL_N; out++) {
                int32_t accumulator = 0;
                size_t output_index = rk3588_rknn_feature_index(
                    RK3588_RKNN_MATMUL_N, RK3588_RKNN_MATMUL_M, 4,
                    out, row);
                unsigned int logical = row * RK3588_RKNN_MATMUL_N + out;
                int64_t converted;
                uint64_t magnitude;
                uint64_t quotient;
                uint64_t remainder;
                uint32_t shift_value = cases[c].shift & 0xfff;
                bool ties_away = cases[c].shift & (1U << 30);
                bool add_first = cases[c].shift & (1U << 31);

                for (unsigned int channel = 0;
                     channel < RK3588_RKNN_MATMUL_K; channel++) {
                    int8_t input =
                        ((row * 37 + channel * 11 + 3) % 31) - 15;
                    int8_t weight =
                        ((out * 19 + channel * 7 + 5) % 29) - 14;

                    accumulator += input * weight;
                }
                if (add_first) {
                    converted = ((int64_t)accumulator + cases[c].offset) *
                                cases[c].scale;
                } else {
                    converted = (int64_t)accumulator * cases[c].scale +
                                (int64_t)cases[c].offset *
                                (UINT64_C(1) << shift_value);
                }
                magnitude = converted < 0 ? -converted : converted;
                quotient = magnitude >> shift_value;
                remainder = 0;
                if (shift_value) {
                    remainder = magnitude &
                        ((UINT64_C(1) << shift_value) - 1);
                }
                if (shift_value &&
                    (remainder > (UINT64_C(1) << (shift_value - 1)) ||
                     (remainder == (UINT64_C(1) << (shift_value - 1)) &&
                      (ties_away || (quotient & 1))))) {
                    quotient++;
                }
                converted = converted < 0 ? -(int64_t)quotient : quotient;
                converted = CLAMP(converted, INT32_MIN, INT32_MAX);
                if (c == 2 &&
                    logical < ARRAY_SIZE(hardware_scale3_shift1_first16)) {
                    g_assert_cmpint(
                        (int32_t)le32_to_cpu(output[output_index]), ==,
                        hardware_scale3_shift1_first16[logical]);
                }
                if (c == 3 &&
                    logical < ARRAY_SIZE(hardware_round_away_first16)) {
                    g_assert_cmpint(
                        (int32_t)le32_to_cpu(output[output_index]), ==,
                        hardware_round_away_first16[logical]);
                }
                if ((c == 6 || c == 8) &&
                    logical < ARRAY_SIZE(hardware_type0_offset_first16)) {
                    g_assert_cmpint(
                        (int32_t)le32_to_cpu(output[output_index]), ==,
                        hardware_type0_offset_first16[logical]);
                }
                if ((c == 7 || c == 9) &&
                    logical < ARRAY_SIZE(hardware_type1_offset_first16)) {
                    g_assert_cmpint(
                        (int32_t)le32_to_cpu(output[output_index]), ==,
                        hardware_type1_offset_first16[logical]);
                }
                g_assert_cmpint(
                    (int32_t)le32_to_cpu(output[output_index]), ==,
                    converted);
            }
        }
    }
    qtest_quit(qts);
}

static int32_t rk3588_rknn_round_shift_s64(int64_t value,
                                           unsigned int shift)
{
    uint64_t magnitude = value < 0 ? -value : value;
    uint64_t quotient = magnitude >> shift;
    uint64_t remainder = magnitude & ((UINT64_C(1) << shift) - 1);
    uint64_t half = UINT64_C(1) << (shift - 1);

    if (remainder > half || (remainder == half && (quotient & 1))) {
        quotient++;
    }
    return value < 0 ? -(int64_t)quotient : quotient;
}

static void test_rk3588_rknpu_dpu_positive_controls(void)
{
    enum Control {
        CONTROL_CLIP,
        CONTROL_BS_PRELU,
        CONTROL_BN_PRELU,
        CONTROL_EW_PRELU,
    };
    static const struct {
        const char *name;
        enum Control control;
    } cases[] = {
        { "core-clip-truncate", CONTROL_CLIP },
        { "bs-prelu", CONTROL_BS_PRELU },
        { "bn-prelu", CONTROL_BN_PRELU },
        { "ew-prelu", CONTROL_EW_PRELU },
    };
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    uint32_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N];
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int c = 0; c < ARRAY_SIZE(cases); c++) {
        if (c) {
            rk3588_rknpu_reset_fixture(qts);
        }
        g_test_message("positive DPU control: %s", cases[c].name);
        rk3588_rknn_prepare_matmul(qts, true, 0xa5);
        rk3588_rknn_make_matmul_regcmd(commands, true);
        switch (cases[c].control) {
        case CONTROL_CLIP:
            rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                     RKNN_REGCMD_TARGET_CORE,
                                     RKNN_CORE_CLIP_TRUNCATE, 1);
            break;
        case CONTROL_BS_PRELU:
        case CONTROL_BN_PRELU: {
            uint32_t cfg = 0x4040;

            if (cases[c].control == CONTROL_BN_PRELU) {
                cfg = 0x4060;
            }

            rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                     RKNN_REGCMD_TARGET_DPU, cfg, 0x62);
            rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                     RKNN_REGCMD_TARGET_DPU, cfg + 8,
                                     2U << 16);
            break;
        }
        case CONTROL_EW_PRELU:
            rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                     RKNN_REGCMD_TARGET_DPU, 0x4070,
                                     0x3a4);
            for (uint32_t reg = 0x4090; reg <= 0x40ac; reg += 4) {
                rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                                         RKNN_REGCMD_TARGET_DPU, reg, 2);
            }
            break;
        }
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                       sizeof(commands));
        rk3588_rknn_run_matmul(qts);
        rk3588_rknn_read_matmul_output(qts, output, sizeof(output));

        for (unsigned int row = 0; row < RK3588_RKNN_MATMUL_M; row++) {
            for (unsigned int out = 0; out < RK3588_RKNN_MATMUL_N; out++) {
                size_t index = rk3588_rknn_feature_index(
                    RK3588_RKNN_MATMUL_N, RK3588_RKNN_MATMUL_M, 4,
                    out, row);
                int32_t expected = rk3588_rknn_matmul_expected(row, out);

                if (cases[c].control == CONTROL_CLIP) {
                    expected = rk3588_rknn_round_shift_s64(expected, 1);
                } else if (expected < 0) {
                    expected *= 2;
                }
                g_assert_cmpint((int32_t)le32_to_cpu(output[index]), ==,
                                expected);
            }
        }
        g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                        RKNN_TASK_STATUS_SUCCESS);
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_mul_shifts(void)
{
    static const struct {
        bool bn;
        uint8_t positive_shift;
        uint8_t negative_shift;
    } cases[] = {
        {false, 1, 0},
        {false, 2, 0},
        {false, 1, 1},
        {true, 1, 0},
        {true, 2, 0},
        {true, 1, 1},
    };
    static const int32_t hardware_first8[][8] = {
        {304, -393, -612, -135, -615, 627, -444, -489},
        {152, -393, -612, -135, -615, 314, -444, -489},
        {304, -196, -306, -68, -308, 627, -222, -244},
        {304, -393, -612, -135, -615, 627, -444, -489},
        {152, -393, -612, -135, -615, 314, -444, -489},
        {304, -196, -306, -68, -308, 627, -222, -244},
    };
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    uint32_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N];
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int c = 0; c < ARRAY_SIZE(cases); c++) {
        uint32_t mul_cfg = (3U << 16) | (cases[c].positive_shift << 8);
        uint32_t data_format = 4U << 29;
        uint32_t bs_cfg = 0x42;
        uint32_t bs_mul_cfg = mul_cfg;
        uint32_t bn_cfg = 0x53;
        uint32_t bn_mul_cfg = 0;

        if (c) {
            rk3588_rknpu_reset_fixture(qts);
        }
        if (cases[c].bn) {
            data_format |= cases[c].negative_shift << 10;
            bs_cfg = 0x53;
            bs_mul_cfg = 0;
            bn_cfg = 0x42;
            bn_mul_cfg = mul_cfg;
        } else {
            data_format |= cases[c].negative_shift << 4;
        }
        rk3588_rknn_prepare_matmul(qts, true, 0xa5);
        rk3588_rknn_make_matmul_regcmd(commands, true);
#define SET_SHIFT_DPU_REG(_reg, _value) do { \
        size_t index = rk3588_rknn_find_regcmd( \
            commands, ARRAY_SIZE(commands), 0x1001, (_reg)); \
        commands[index] = rk3588_rknn_regcmd(0x1001, (_reg), (_value)); \
    } while (0)
        SET_SHIFT_DPU_REG(0x4010, data_format);
        SET_SHIFT_DPU_REG(0x4040, bs_cfg);
        SET_SHIFT_DPU_REG(0x4048, bs_mul_cfg);
        SET_SHIFT_DPU_REG(0x4060, bn_cfg);
        SET_SHIFT_DPU_REG(0x4068, bn_mul_cfg);
#undef SET_SHIFT_DPU_REG
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                       sizeof(commands));
        rk3588_rknn_run_matmul(qts);
        rk3588_rknn_read_matmul_output(qts, output, sizeof(output));

        for (unsigned int row = 0; row < RK3588_RKNN_MATMUL_M; row++) {
            for (unsigned int out = 0; out < RK3588_RKNN_MATMUL_N; out++) {
                int32_t accumulator = 0;
                size_t output_index = rk3588_rknn_feature_index(
                    RK3588_RKNN_MATMUL_N, RK3588_RKNN_MATMUL_M, 4,
                    out, row);
                unsigned int logical = row * RK3588_RKNN_MATMUL_N + out;
                int64_t product;
                unsigned int shift;
                int32_t expected;

                for (unsigned int channel = 0;
                     channel < RK3588_RKNN_MATMUL_K; channel++) {
                    int8_t input =
                        ((row * 37 + channel * 11 + 3) % 31) - 15;
                    int8_t weight =
                        ((out * 19 + channel * 7 + 5) % 29) - 14;

                    accumulator += input * weight;
                }
                product = (int64_t)accumulator * 3;
                if (product < 0) {
                    shift = cases[c].negative_shift;
                } else {
                    shift = cases[c].positive_shift;
                }
                expected = shift ? rk3588_rknn_round_shift_s64(product,
                                                                shift) :
                                   product;
                if (logical < 8) {
                    g_assert_cmpint(
                        (int32_t)le32_to_cpu(output[output_index]), ==,
                        hardware_first8[c][logical]);
                }
                g_assert_cmpint(
                    (int32_t)le32_to_cpu(output[output_index]), ==, expected);
            }
        }
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_stage_wide_combination(void)
{
    static const struct {
        uint32_t ew_cfg;
        int32_t ew_operand;
        uint32_t shift;
    } cases[] = {
        {0x383, 0, 24},
        {0x383, 0, 32},
        {0x383, 0, 40},
        {0x384, INT32_MAX, 40},
    };
    static const int32_t hardware_shift24_first16[] = {
        128, -128, -128, -128, -128, 128, -128, -128,
        128, -128, -128, 128, 128, -128, 128, 128,
    };
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    uint32_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N];
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int c = 0; c < ARRAY_SIZE(cases); c++) {

        if (c) {
            rk3588_rknpu_reset_fixture(qts);
        }
        rk3588_rknn_prepare_matmul(qts, true, 0xa5);
        rk3588_rknn_make_matmul_regcmd(commands, true);
#define SET_WIDE_DPU_REG(_reg, _value) do { \
        size_t index = rk3588_rknn_find_regcmd( \
            commands, ARRAY_SIZE(commands), 0x1001, (_reg)); \
        commands[index] = rk3588_rknn_regcmd(0x1001, (_reg), (_value)); \
    } while (0)
        SET_WIDE_DPU_REG(0x4040, 0x42);
        SET_WIDE_DPU_REG(0x4048, 0x7fff0000);
        SET_WIDE_DPU_REG(0x4060, 0x42);
        SET_WIDE_DPU_REG(0x4068, 0x7fff0000);
        SET_WIDE_DPU_REG(0x4070, cases[c].ew_cfg);
        SET_WIDE_DPU_REG(0x4084, 1);
        SET_WIDE_DPU_REG(0x4088, cases[c].shift);
        for (uint32_t reg = 0x4090; reg <= 0x40ac; reg += 4) {
            SET_WIDE_DPU_REG(reg, cases[c].ew_operand);
        }
#undef SET_WIDE_DPU_REG
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                       sizeof(commands));
        rk3588_rknn_run_matmul(qts);
        rk3588_rknn_read_matmul_output(qts, output, sizeof(output));

        for (unsigned int row = 0; row < RK3588_RKNN_MATMUL_M; row++) {
            for (unsigned int out = 0; out < RK3588_RKNN_MATMUL_N; out++) {
                int32_t accumulator = 0;
                size_t output_index = rk3588_rknn_feature_index(
                    RK3588_RKNN_MATMUL_N, RK3588_RKNN_MATMUL_M, 4,
                    out, row);
                unsigned int logical = row * RK3588_RKNN_MATMUL_N + out;
                int32_t expected = 0;

                for (unsigned int channel = 0;
                     channel < RK3588_RKNN_MATMUL_K; channel++) {
                    int8_t input =
                        ((row * 37 + channel * 11 + 3) % 31) - 15;
                    int8_t weight =
                        ((out * 19 + channel * 7 + 5) % 29) - 14;

                    accumulator += input * weight;
                }
                if (c == 0) {
                    int64_t value = (int64_t)accumulator * 32767;
                    uint64_t magnitude;
                    uint64_t quotient;
                    uint64_t remainder;

                    value = CLAMP(value, INT32_MIN, INT32_MAX);
                    value *= 32767;
                    value = CLAMP(value, INT32_MIN, INT32_MAX);
                    magnitude = value < 0 ? -value : value;
                    quotient = magnitude >> 24;
                    remainder = magnitude & ((UINT64_C(1) << 24) - 1);
                    if (remainder > (UINT64_C(1) << 23) ||
                        (remainder == (UINT64_C(1) << 23) &&
                         (quotient & 1))) {
                        quotient++;
                    }
                    expected = value < 0 ? -(int64_t)quotient : quotient;
                    if (logical < ARRAY_SIZE(hardware_shift24_first16)) {
                        g_assert_cmpint(
                            (int32_t)le32_to_cpu(output[output_index]), ==,
                            hardware_shift24_first16[logical]);
                    }
                }
                g_assert_cmpint(
                    (int32_t)le32_to_cpu(output[output_index]), ==, expected);
            }
        }
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_semantic_regcmd(void)
{
    uint64_t original[RK3588_RKNN_MATMUL_COMMANDS];
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS + 1];
    QTestState *qts = rk3588_qtest_start_rknpu();
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
                 rk3588_rknn_register_amount(ARRAY_SIZE(commands)));

    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_assert_matmul_output(qts);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_reset_defaults(void)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t clip;

    rk3588_rknn_prepare_matmul(qts, true, 0x6b);
    rk3588_rknn_make_matmul_regcmd(commands, true);
    clip = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),
                                   0x0801, 0x301c);
    commands[clip] = rk3588_rknn_regcmd(0x0801, 0x3010, 0);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));

    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_assert_matmul_output(qts);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_pointer_completion(void)
{
    uint64_t original[RK3588_RKNN_MATMUL_COMMANDS];
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS + 1];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(original, true);
    commands[0] = rk3588_rknn_regcmd(0x0201, 0x1004, 0x0f);
    memcpy(commands + 1, original, sizeof(original));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(ARRAY_SIZE(commands)));

    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_assert_matmul_output(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0x0001000f);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_pointer_mode0(void)
{
    uint64_t original[RK3588_RKNN_MATMUL_COMMANDS];
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS + 1];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(original, true);
    commands[0] = rk3588_rknn_regcmd(0x0201, 0x1004, 0x07);
    memcpy(commands + 1, original, sizeof(original));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(ARRAY_SIZE(commands)));

    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_assert_matmul_output(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0x00010007);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_executor_disabled(void)
{
    uint64_t original[RK3588_RKNN_MATMUL_COMMANDS];
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS + 1];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(original, true);
    commands[0] = rk3588_rknn_regcmd(0x0201, 0x1004, 0x0f);
    memcpy(commands + 1, original, sizeof(original));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(ARRAY_SIZE(commands)));
    rk3588_rknn_run_matmul(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0x0001000f);

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(original, true);
    commands[0] = rk3588_rknn_regcmd(0x0201, 0x1004, 0x02);
    memcpy(commands + 1, original, sizeof(original));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(ARRAY_SIZE(commands)));
    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_assert_matmul_output(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0x00010002);
    qtest_quit(qts);
}

static void rk3588_rknn_prepare_multitask(QTestState *qts,
                                          uint64_t first[],
                                          uint64_t second[],
                                          uint64_t second_output_address)
{
    const uint32_t second_regcmd_offset = 0x5000;
    const uint32_t second_output_offset = 0x6800;
    const uint64_t second_regcmd_address = RK3588_RAM_BASE + 0x29000;
    size_t output;

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(first, true);
    rk3588_rknn_make_matmul_regcmd(second, true);
    first[RK3588_RKNN_MATMUL_COMMANDS - 4] = rk3588_rknn_regcmd(
        0x0101, RKNN_PC_BASE_ADDRESS,
        RK3588_RKNN_MATMUL_REGCMD_IOVA + second_regcmd_offset);
    first[RK3588_RKNN_MATMUL_COMMANDS - 3] = rk3588_rknn_regcmd(
        0x0101, RKNN_PC_REGISTER_AMOUNTS,
        rk3588_rknn_register_amount(RK3588_RKNN_MATMUL_COMMANDS));
    output = rk3588_rknn_find_regcmd(second, RK3588_RKNN_MATMUL_COMMANDS,
                                     0x1001, 0x4020);
    second[output] = rk3588_rknn_regcmd(
        0x1001, 0x4020,
        RK3588_RKNN_MATMUL_REGCMD_IOVA + second_output_offset);

    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, first,
                   RK3588_RKNN_MATMUL_COMMANDS * sizeof(*first));
    qtest_memwrite(qts, second_regcmd_address, second,
                   RK3588_RKNN_MATMUL_COMMANDS * sizeof(*second));
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 5 * 4,
                 second_regcmd_address | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 6 * 4,
                 second_output_address | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 7 * 4,
                 (second_output_address + 0x1000) | RK_IOMMU_PTE_RW);
}

static void rk3588_rknn_make_control_chain_link(uint64_t commands[],
                                                uint32_t next_iova,
                                                size_t next_count)
{
    commands[0] = rk3588_rknn_regcmd(0x0101, RKNN_PC_BASE_ADDRESS,
                                      next_iova);
    commands[1] = rk3588_rknn_regcmd(
        0x0101, RKNN_PC_REGISTER_AMOUNTS,
        rk3588_rknn_register_amount(next_count));
    commands[2] = rk3588_rknn_regcmd(0x0041, 0, 0);
    commands[3] = rk3588_rknn_regcmd(
        0x0081, RKNN_PC_OPERATION_ENABLE, 0);
}

static void rk3588_rknn_prepare_control_chain(QTestState *qts,
                                               uint8_t sentinel)
{
    uint64_t commands[RK3588_RKNN_CONTROL_CHAIN_COMMANDS];
    const size_t final = (RK3588_RKNN_CONTROL_CHAIN_TASKS - 1) *
                         RK3588_RKNN_CONTROL_CHAIN_LINK_COMMANDS;

    rk3588_rknn_prepare_matmul(qts, true, sentinel);
    for (unsigned int task = 0;
         task < RK3588_RKNN_CONTROL_CHAIN_TASKS - 1; task++) {
        size_t next_count =
            task + 2 == RK3588_RKNN_CONTROL_CHAIN_TASKS ?
            RK3588_RKNN_MATMUL_COMMANDS :
            RK3588_RKNN_CONTROL_CHAIN_LINK_COMMANDS;

        rk3588_rknn_make_control_chain_link(
            &commands[task * RK3588_RKNN_CONTROL_CHAIN_LINK_COMMANDS],
            RK3588_RKNN_MATMUL_RDMA_IOVA +
                (task + 1) * RK3588_RKNN_CONTROL_CHAIN_LINK_COMMANDS *
                sizeof(uint64_t),
            next_count);
    }
    rk3588_rknn_make_matmul_regcmd(&commands[final], true);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_RDMA_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 5 * 4,
                 RK3588_RKNN_MATMUL_RDMA_ADDR | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 6 * 4,
                 (RK3588_RKNN_MATMUL_RDMA_ADDR + 0x1000) |
                 RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_BASE_ADDRESS,
                 RK3588_RKNN_MATMUL_RDMA_IOVA);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(
                     RK3588_RKNN_CONTROL_CHAIN_LINK_COMMANDS));
}

static void rk3588_rknn_start_control_chain(QTestState *qts,
                                            uint32_t task_count)
{
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_TASK_CON,
                 0x00003000 | task_count);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_OPERATION_ENABLE,
                 RKNN_PC_OPERATION_ENABLE_OP_EN);
}

static void rk3588_rknn_step_tasks(QTestState *qts, unsigned int task_count)
{
    for (unsigned int task = 0; task < task_count; task++) {
        qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
        rk3588_rknn_wait_idle(qts);
    }
}

static void test_rk3588_rknpu_pipeline_long_task_chain(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_control_chain(qts, 0x4c);
    rk3588_rknn_start_control_chain(qts,
                                    RK3588_RKNN_CONTROL_CHAIN_TASKS);
    rk3588_rknn_step_tasks(qts, 1);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_OPERATION_ENABLE) &
                    RKNN_PC_OPERATION_ENABLE_OP_EN, ==,
                    RKNN_PC_OPERATION_ENABLE_OP_EN);
    rk3588_rknn_step_tasks(qts, RK3588_RKNN_CONTROL_CHAIN_TASKS - 1);
    rk3588_rknn_assert_matmul_output(qts);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void rk3588_rknn_start_sparse_pending_register_task(QTestState *qts)
{
    uint64_t commands[4] = {
        rk3588_rknn_regcmd(RKNN_REGCMD_TARGET_DPU,
                           RKNN_DPU_OFFSET_PEND, 0),
        rk3588_rknn_regcmd(0x0041, 0, 0),
        rk3588_rknn_regcmd(0x0081, RKNN_PC_OPERATION_ENABLE, 0x0d),
        0,
    };

    qtest_writel(qts, RK3588_RKNN0_CNA_BASE + RKNN_POINTER, 0);
    qtest_writel(qts, RK3588_RKNN0_CORE_BASE + RKNN_POINTER, 0);
    qtest_writel(qts, RK3588_RKNN0_DPU_BASE + RKNN_POINTER, 0);
    qtest_writel(qts, RK3588_RKNN0_DPU_BASE + 0x14, 1);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR,
                   commands, sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(ARRAY_SIZE(commands)));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_CLEAR,
                 UINT32_MAX);
    rk3588_rknn_start_matmul(qts);
    qtest_writel(qts, RK3588_RKNN0_DPU_BASE + 0x38, 0x1234);
    qtest_writel(qts, RK3588_RKNN0_CNA_BASE + RKNN_POINTER, 0x0f);
    qtest_writel(qts, RK3588_RKNN0_CNA_BASE + 0x20, 0x12345678);
}

static void test_rk3588_rknpu_pending_register_merge(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_start_sparse_pending_register_task(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);

    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_DPU_BASE + 0x14), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_DPU_BASE + 0x38), ==,
                    0x1234);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0x0000000f);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE + 0x20), ==,
                    0x12345678);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_multitask_broken_chain(void)
{
    uint8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_matmul(qts, true, 0x6d);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_TASK_CON, 0x00003002);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_OPERATION_ENABLE,
                 RKNN_PC_OPERATION_ENABLE_OP_EN);
    rk3588_rknn_step_tasks(qts, 2);
    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0x6d);
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_FETCH_ERROR);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_multitask_fetch_failure(void)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_matmul(qts, true, 0x2b);
    rk3588_rknn_make_matmul_regcmd(commands, true);
    commands[ARRAY_SIZE(commands) - 4] = rk3588_rknn_regcmd(
        0x0101, RKNN_PC_BASE_ADDRESS,
        RK3588_RKNN_MATMUL_REGCMD_IOVA + 0x5000);
    commands[ARRAY_SIZE(commands) - 3] = rk3588_rknn_regcmd(
        0x0101, RKNN_PC_REGISTER_AMOUNTS,
        rk3588_rknn_register_amount(ARRAY_SIZE(commands)));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_TASK_CON, 0x00003002);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_OPERATION_ENABLE,
                 RKNN_PC_OPERATION_ENABLE_OP_EN);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
    rk3588_rknn_assert_matmul_output(qts);
    rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_task_count_mask(void)
{
    uint64_t commands[6];
    uint8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_matmul(qts, true, 0x4c);
    commands[0] = rk3588_rknn_regcmd(
        0x0101, RKNN_PC_BASE_ADDRESS,
        RK3588_RKNN_MATMUL_RDMA_IOVA + 0x1000);
    commands[1] = rk3588_rknn_regcmd(
        0x0101, RKNN_PC_REGISTER_AMOUNTS,
        rk3588_rknn_register_amount(ARRAY_SIZE(commands)));
    commands[2] = rk3588_rknn_regcmd(0x0201, 0x1004, 0x0f);
    commands[3] = 0;
    commands[4] = rk3588_rknn_regcmd(0x0041, 0, 0);
    commands[5] = rk3588_rknn_regcmd(
        0x0081, RKNN_PC_OPERATION_ENABLE, 0);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_RDMA_ADDR, commands,
                   sizeof(commands));
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 5 * 4,
                 RK3588_RKNN_MATMUL_RDMA_ADDR | RK_IOMMU_PTE_RW);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_BASE_ADDRESS,
                 RK3588_RKNN_MATMUL_RDMA_IOVA);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(ARRAY_SIZE(commands)));
    qtest_irq_intercept_out_named(qts, RK3588_RKNN0_QOM, "sysbus-irq");
    rk3588_rknn_start_control_chain(qts, 1U << 12);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);

    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0x4c);
    }
    rk3588_rknn_assert_pc(qts, 0, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void test_rk3588_rknpu_pipeline_task_dma_base(void)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    const uint32_t task_base = 0x10000000;
    QTestState *qts = rk3588_qtest_start_rknpu();
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

    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_assert_matmul_output(qts);
    qtest_quit(qts);
}

#ifndef _WIN32
static void test_rk3588_rknpu_pipeline_migration_snapshot(void)
{
    uint64_t first[RK3588_RKNN_MATMUL_COMMANDS];
    uint64_t second[RK3588_RKNN_MATMUL_COMMANDS];
    uint8_t second_output[RK3588_RKNN_MATMUL_M *
                          RK3588_RKNN_MATMUL_N * 4];
    const uint64_t second_regcmd_address = RK3588_RAM_BASE + 0x29000;
    const uint64_t second_output_address = RK3588_RAM_BASE + 0x2a000;
    QTestState *source;
    QTestState *destination;
    size_t core_misc;

    source = rk3588_qtest_start_rknpu();
    memset(second_output, 0x5a, sizeof(second_output));
    rk3588_rknn_prepare_multitask(source, first, second,
                                  second_output_address);
    qtest_memwrite(source, second_output_address + 0x800, second_output,
                   sizeof(second_output));
    core_misc = rk3588_rknn_find_regcmd(second, ARRAY_SIZE(second),
                                        0x0801, 0x3010);
    qtest_writel(source, RK3588_RKNN0_PC_BASE + RKNN_PC_TASK_CON,
                 0x00003002);
    qtest_writel(source, RK3588_RKNN0_PC_BASE + RKNN_PC_OPERATION_ENABLE,
                 RKNN_PC_OPERATION_ENABLE_OP_EN);

    /* Later tasks are fetched from migrated RAM, not snapshotted at start. */
    qtest_writeq(source, second_regcmd_address +
                      core_misc * sizeof(uint64_t),
                 le64_to_cpu(rk3588_rknn_regcmd(0x0801, 0x3010, 2)));
    destination = rk3588_rknn_migrate(source, false);
    rk3588_rknn_step_tasks(destination, 2);
    qtest_memread(destination, second_output_address + 0x800, second_output,
                  sizeof(second_output));
    rk3588_rknn_assert_matmul_output(destination);
    for (unsigned int i = 0; i < ARRAY_SIZE(second_output); i++) {
        g_assert_cmphex(second_output[i], ==, 0x5a);
    }
    g_assert_cmphex(qtest_readl(destination, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_FETCH_ERROR | 2);
    g_assert_cmphex(qtest_readl(destination, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_INTERRUPT_RAW_STATUS), ==, 0);
    qtest_quit(source);
    qtest_quit(destination);
}

static void test_rk3588_rknpu_active_worker_migration(void)
{
    QTestState *source = rk3588_qtest_start_rknpu();
    QTestState *destination;

    rk3588_rknn_prepare_long_execution(source, 0xa5, 1);
    rk3588_rknn_start_matmul(source);
    qtest_clock_step(source, RKNN_COMPLETE_DELAY_NS);
    g_assert_true(qtest_qom_get_bool(source, RK3588_RKNN0_QOM,
                                     "x-execution-active"));

    destination = rk3588_rknn_migrate(source, false);
    g_assert_false(qtest_qom_get_bool(destination, RK3588_RKNN0_QOM,
                                      "x-execution-active"));
    rk3588_rknn_assert_long_execution_output(destination);
    rk3588_rknn_assert_pc(destination, RKNN_TASK_STATUS_SUCCESS,
                          RKNN_PIPELINE_BANK1_INTERRUPT);
    qtest_quit(source);
    qtest_quit(destination);
}

static void test_rk3588_rknpu_parallel_worker_migration(void)
{
    QTestState *source = rk3588_qtest_start_rknpu();
    QTestState *destination;

    rk3588_rknn_prepare_long_execution(source, 0xa5, 2);
    rk3588_rknn_prepare_peer_long_execution(source, 1);
    rk3588_rknn_start_matmul(source);
    rk3588_rknn_start_peer(source, 1);
    qtest_clock_step(source, RKNN_COMPLETE_DELAY_NS);
    g_assert_true(qtest_qom_get_bool(source, RK3588_RKNN0_QOM,
                                     "x-execution-active"));
    g_assert_true(qtest_qom_get_bool(source, RK3588_RKNN1_QOM,
                                     "x-execution-active"));
    destination = rk3588_rknn_migrate(source, false);
    g_assert_false(qtest_qom_get_bool(destination, RK3588_RKNN0_QOM,
                                      "x-execution-active"));
    g_assert_false(qtest_qom_get_bool(destination, RK3588_RKNN1_QOM,
                                      "x-execution-active"));
    rk3588_rknn_assert_long_execution_output(destination);
    rk3588_rknn_assert_peer_long_execution_output(destination, 1);
    g_assert_cmphex(qtest_readl(destination, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    g_assert_cmphex(qtest_readl(destination, RK3588_RKNN1_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(source);
    qtest_quit(destination);
}

static void test_rk3588_rknpu_rendezvous_wait_migration(void)
{
    QTestState *source = rk3588_qtest_start_rknpu();
    QTestState *destination;

    rk3588_rknn_prepare_long_execution(source, 0xa5, 2);
    rk3588_rknn_prepare_peer_long_execution(source, 1);
    rk3588_rknn_start_matmul(source);
    qtest_clock_step(source, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_long_execution_output(
        source, rk3588_rknn_long_output_addr);
    g_assert_true(qtest_qom_get_bool(source, RK3588_RKNN0_QOM,
                                     "x-execution-active"));
    g_assert_cmphex(qtest_readl(source, RK3588_RKNN0_PC_BASE +
                               RKNN_PC_TASK_STATUS), ==, 0);

    destination = rk3588_rknn_migrate(source, false);
    g_assert_true(qtest_qom_get_bool(destination, RK3588_RKNN0_QOM,
                                    "x-execution-active"));
    g_assert_false(qtest_qom_get_bool(destination, RK3588_RKNN1_QOM,
                                     "x-execution-active"));

    rk3588_rknn_start_peer(destination, 1);
    qtest_clock_step(destination, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_core_idle(destination, RK3588_RKNN0_QOM);
    rk3588_rknn_wait_core_idle(destination, RK3588_RKNN1_QOM);
    rk3588_rknn_assert_long_execution_output(destination);
    rk3588_rknn_assert_peer_long_execution_output(destination, 1);
    qtest_quit(source);
    qtest_quit(destination);
}

static void test_rk3588_rknpu_wide_state_migration(void)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    uint32_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N];
    QTestState *source = rk3588_qtest_start_rknpu();
    QTestState *destination;

    rk3588_rknn_prepare_matmul(source, true, 0xa5);
    rk3588_rknn_start_matmul(source);
    qtest_clock_step(source, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(source);
    rk3588_rknn_assert_matmul_output(source);

    qtest_writel(source, RK3588_RKNN0_DPU_BASE + 0x100, 0x00020000);
    for (unsigned int entry = 0; entry < RK3588_RKNN_DPU_LUT_ENTRIES;
         entry++) {
        qtest_writel(source, RK3588_RKNN0_DPU_BASE + 0x104, 0x1234);
    }
    qtest_writel(source, RK3588_RKNN0_DPU_BASE + 0x100, 0x00030000);
    for (unsigned int entry = 0; entry < RK3588_RKNN_DPU_LUT_ENTRIES;
         entry++) {
        qtest_writel(source, RK3588_RKNN0_DPU_BASE + 0x104, 0x2345);
    }

    rk3588_rknn_prepare_matmul(source, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(commands, true);
#define PATCH_MIGRATION_LUT(_reg, _value) \
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands), \
                             RKNN_REGCMD_TARGET_DPU, (_reg), (_value))
    PATCH_MIGRATION_LUT(0x4070, 0x302);
    PATCH_MIGRATION_LUT(0x4108, 0x68);
    PATCH_MIGRATION_LUT(0x410c, 0x00050500);
    PATCH_MIGRATION_LUT(0x4110, 0xffffc000);
    PATCH_MIGRATION_LUT(0x4114, 0);
    PATCH_MIGRATION_LUT(0x4118, 0);
    PATCH_MIGRATION_LUT(0x411c, 0x00004000);
    PATCH_MIGRATION_LUT(0x4120, 0);
    PATCH_MIGRATION_LUT(0x4124, 0);
    PATCH_MIGRATION_LUT(0x4128, 0);
    PATCH_MIGRATION_LUT(0x412c, 0);
#undef PATCH_MIGRATION_LUT
    qtest_writel(source, RK3588_RKNN0_DPU_BASE + 0x14, 1);
    qtest_memwrite(source, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    rk3588_rknn_start_matmul(source);

    /* Descriptor-written state merges; unrelated slave state survives. */
    qtest_writel(source, RK3588_RKNN0_DPU_BASE + 0xb8, 0x5678);

    destination = rk3588_rknn_migrate(source, false);
    g_assert_cmphex(qtest_readl(destination, RK3588_RKNN0_DPU_BASE +
                                0xb8), ==, 0x5678);
    qtest_clock_step_next(destination);
    rk3588_rknn_wait_idle(destination);
    rk3588_rknn_read_matmul_output(destination, output, sizeof(output));
    for (unsigned int row = 0; row < RK3588_RKNN_MATMUL_M; row++) {
        for (unsigned int out = 0; out < RK3588_RKNN_MATMUL_N; out++) {
            size_t index = rk3588_rknn_feature_index(
                RK3588_RKNN_MATMUL_N, RK3588_RKNN_MATMUL_M, 4, out, row);
            uint32_t expected = rk3588_rknn_matmul_expected(row, out) < 0 ?
                                0x1234 : 0x2345;

            g_assert_cmphex(le32_to_cpu(output[index]), ==, expected);
        }
    }
    g_assert_cmphex(qtest_readl(destination, RK3588_RKNN0_DPU_BASE +
                                0x14), ==, 0);
    g_assert_cmphex(qtest_readl(destination, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    qtest_quit(source);
    qtest_quit(destination);
}

static void test_rk3588_rknpu_slave_stage_migration(void)
{
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    uint32_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N];
    QTestState *source = rk3588_qtest_start_rknpu();
    QTestState *destination;

    rk3588_rknn_prepare_matmul(source, true, 0xa5);
    rk3588_rknn_make_matmul_regcmd(commands, true);
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                             RKNN_REGCMD_TARGET_DPU, 0x4040, 0x20050);
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands),
                             RKNN_REGCMD_TARGET_DPU, 0x4044, 5);
    rk3588_rknn_write_slave_commands(source, commands,
                                     ARRAY_SIZE(commands), 0x0e);

    /* Execution must use the submitted stage snapshot, not later MMIO. */
    qtest_writel(source, RK3588_RKNN0_DPU_BASE + 0x44, 100);
    destination = rk3588_rknn_migrate(source, false);
    qtest_clock_step_next(destination);
    rk3588_rknn_wait_idle(destination);
    rk3588_rknn_read_matmul_output(destination, output, sizeof(output));
    for (unsigned int row = 0; row < RK3588_RKNN_MATMUL_M; row++) {
        for (unsigned int out = 0; out < RK3588_RKNN_MATMUL_N; out++) {
            size_t index = rk3588_rknn_feature_index(
                RK3588_RKNN_MATMUL_N, RK3588_RKNN_MATMUL_M, 4, out, row);

            g_assert_cmpint((int32_t)le32_to_cpu(output[index]), ==,
                            rk3588_rknn_matmul_expected(row, out) + 5);
        }
    }
    g_assert_cmphex(qtest_readl(destination, RK3588_RKNN0_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==, 0x5000);
    qtest_quit(source);
    qtest_quit(destination);
}

#endif

static void test_rk3588_rknpu_dpu_output_controls(void)
{
    static const struct {
        const char *name;
        uint32_t reg;
        uint32_t value;
    } cases[] = {
        { "feature-mode", RKNN_DPU_FEATURE_MODE_CFG, 0x800001e4 },
        { "feature-flying-mode", RKNN_DPU_FEATURE_MODE_CFG, 0x1e5 },
        { "feature-burst-length", RKNN_DPU_FEATURE_MODE_CFG, 0x004 },
        { "offset-pend", RKNN_DPU_OFFSET_PEND, 1 },
        { "output-notch", RKNN_DPU_DATA_CUBE_NOTCH_ADDR, 1 },
    };
    QTestState *qts = rk3588_qtest_start_rknpu();

    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
        uint8_t output[RK3588_RKNN_MATMUL_M *
                       RK3588_RKNN_MATMUL_N * 4];
        size_t index;

        if (i) {
            rk3588_rknpu_reset_fixture(qts);
        }
        g_test_message("DPU output control: %s", cases[i].name);
        rk3588_rknn_prepare_matmul(qts, true, 0x5a);
        rk3588_rknn_make_matmul_regcmd(commands, true);
        index = rk3588_rknn_find_regcmd(
            commands, ARRAY_SIZE(commands), 0x1001, cases[i].reg);
        commands[index] = rk3588_rknn_regcmd(
            0x1001, cases[i].reg, cases[i].value);
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                       sizeof(commands));

        rk3588_rknn_run_matmul(qts);
        rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
        for (unsigned int byte = 0; byte < ARRAY_SIZE(output); byte++) {
            g_assert_cmphex(output[byte], ==, 0x5a);
        }
        rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
    }
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_resource_budget(void)
{
    enum {
        HEIGHT = 3,
        INPUT_CHANNELS = 16384,
        OUTPUT_CHANNELS = 8192,
    };
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    uint8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4];
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t index;

    rk3588_rknn_prepare_matmul(qts, true, 0x5a);
    rk3588_rknn_make_matmul_regcmd(commands, true);
#define PATCH_BUDGET(_target, _reg, _value) do {                    \
    index = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands), \
                                    (_target), (_reg));             \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg),        \
                                          (_value));                \
} while (0)
    PATCH_BUDGET(0x0201, 0x1020, (1 << 16) | 1);
    PATCH_BUDGET(0x0201, 0x1024,
                 ((INPUT_CHANNELS - 1) << 16) | INPUT_CHANNELS);
    PATCH_BUDGET(0x0201, 0x102c, HEIGHT);
    PATCH_BUDGET(0x0201, 0x1030,
                 OUTPUT_CHANNELS * INPUT_CHANNELS);
    PATCH_BUDGET(0x0201, 0x1034, INPUT_CHANNELS);
    PATCH_BUDGET(0x0201, 0x1038,
                 (1 << 24) | (1 << 16) | OUTPUT_CHANNELS);
    PATCH_BUDGET(0x0201, 0x1084, (1 << 16) | 1);
    PATCH_BUDGET(0x0201, 0x1088, INPUT_CHANNELS);
    PATCH_BUDGET(0x0801, 0x3014, (HEIGHT - 1) << 16);
    PATCH_BUDGET(0x0801, 0x3018, OUTPUT_CHANNELS - 1);
    PATCH_BUDGET(0x1001, 0x4034, HEIGHT - 1);
    PATCH_BUDGET(0x1001, 0x403c,
                 ((OUTPUT_CHANNELS - 1) << 16) |
                 (OUTPUT_CHANNELS - 1));
    PATCH_BUDGET(0x1001, 0x4058, OUTPUT_CHANNELS - 1);
#undef PATCH_BUDGET
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));

    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int byte = 0; byte < ARRAY_SIZE(output); byte++) {
        g_assert_cmphex(output[byte], ==, 0x5a);
    }
    rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_dpu_host_budget(void)
{
    enum {
        OUTPUT_WIDTH = 2047,
        OUTPUT_HEIGHT = 5,
        INPUT_CHANNELS = 1,
        OUTPUT_CHANNELS = 8192,
    };
    uint64_t commands[RK3588_RKNN_MATMUL_COMMANDS];
    uint8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4];
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t index;

    rk3588_rknn_prepare_matmul(qts, true, 0x5a);
    rk3588_rknn_make_matmul_regcmd(commands, true);
#define PATCH_HOST_BUDGET(_target, _reg, _value) do {               \
    index = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands), \
                                    (_target), (_reg));             \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg),        \
                                          (_value));                \
} while (0)
    PATCH_HOST_BUDGET(0x0201, 0x1020, (1 << 16) | 1);
    PATCH_HOST_BUDGET(0x0201, 0x1024, INPUT_CHANNELS);
    PATCH_HOST_BUDGET(0x0201, 0x1028, OUTPUT_WIDTH);
    PATCH_HOST_BUDGET(0x0201, 0x102c, OUTPUT_WIDTH * OUTPUT_HEIGHT);
    PATCH_HOST_BUDGET(0x0201, 0x1030,
                      OUTPUT_CHANNELS * INPUT_CHANNELS);
    PATCH_HOST_BUDGET(0x0201, 0x1034, INPUT_CHANNELS);
    PATCH_HOST_BUDGET(0x0201, 0x1038,
                      (1 << 24) | (1 << 16) | OUTPUT_CHANNELS);
    PATCH_HOST_BUDGET(0x0201, 0x1084, (1 << 16) | 1);
    PATCH_HOST_BUDGET(0x0201, 0x1088, INPUT_CHANNELS);
    PATCH_HOST_BUDGET(0x0801, 0x3014,
                      ((OUTPUT_HEIGHT - 1) << 16) |
                      (OUTPUT_WIDTH - 1));
    PATCH_HOST_BUDGET(0x0801, 0x3018, OUTPUT_CHANNELS - 1);
    PATCH_HOST_BUDGET(0x1001, 0x4030, OUTPUT_WIDTH - 1);
    PATCH_HOST_BUDGET(0x1001, 0x4034, OUTPUT_HEIGHT - 1);
    PATCH_HOST_BUDGET(0x1001, 0x403c,
                      ((OUTPUT_CHANNELS - 1) << 16) |
                      (OUTPUT_CHANNELS - 1));
    PATCH_HOST_BUDGET(0x1001, 0x4058, OUTPUT_CHANNELS - 1);
#undef PATCH_HOST_BUDGET
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));

    rk3588_rknn_run_matmul(qts);
    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int byte = 0; byte < ARRAY_SIZE(output); byte++) {
        g_assert_cmphex(output[byte], ==, 0x5a);
    }
    rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_matmul_reset_pending(void)
{
    uint8_t output[RK3588_RKNN_MATMUL_M * RK3588_RKNN_MATMUL_N * 4];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_matmul(qts, true, 0x3c);
    rk3588_rknn_start_matmul(qts);
    qtest_system_reset(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_read_matmul_output(qts, output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0x3c);
    }
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_TASK_STATUS), ==, 0);
    qtest_quit(qts);
}

static void rk3588_rknn_make_ppu_regcmd(
    uint64_t commands[RK3588_RKNN_PPU_COMMANDS], uint32_t task_base,
    uint32_t padding)
{
    unsigned int count = 0;

    g_assert_cmphex(task_base, <=, RK3588_RKNN_PPU_INPUT_IOVA);
#define PPU(_reg, _value) commands[count++] = \
    rk3588_rknn_regcmd(0x4001, (_reg), (_value))
#define PRDMA(_reg, _value) commands[count++] = \
    rk3588_rknn_regcmd(0x8001, (_reg), (_value))
    PPU(0x6004, 0x0e);
    PRDMA(0x7004, 0x0e);
    PPU(0x600c, 11);
    PPU(0x6010, 19);
    PPU(0x6014, 127);
    PPU(0x6018, 9);
    PPU(0x601c, 19);
    PPU(0x6020, 127);
    PPU(0x6024, 0x000a0011);
    PPU(0x6034, 0x404);
    PPU(0x6038, 0);
    PPU(0x603c, 0);
    PPU(0x6040, padding);
    PPU(0x6044, 0);
    PPU(0x6048, 0);
    PPU(0x6070, RK3588_RKNN_PPU_OUTPUT_IOVA - task_base);
    PPU(0x607c, RK3588_RKNN_PPU_SURF_STRIDE);
    PPU(0x6084, 0x10);
    PPU(0x60dc, 3);
    PRDMA(0x700c, 11);
    PRDMA(0x7010, 19);
    PRDMA(0x7014, 127);
    PRDMA(0x701c, RK3588_RKNN_PPU_INPUT_IOVA - task_base);
    PRDMA(0x7024, RK3588_RKNN_PPU_LINE_STRIDE);
    PRDMA(0x7028, RK3588_RKNN_PPU_SURF_STRIDE);
    PRDMA(0x7030, 1);
    commands[count++] = 0;
    commands[count++] = rk3588_rknn_regcmd(
        0x0101, RKNN_PC_REGISTER_AMOUNTS, 0x60000000);
    commands[count++] = rk3588_rknn_regcmd(0x0041, 0, 0);
    commands[count++] = rk3588_rknn_regcmd(
        0x0081, RKNN_PC_OPERATION_ENABLE, 0x60);
#undef PPU
#undef PRDMA
    g_assert_cmpuint(count, ==, RK3588_RKNN_PPU_COMMANDS);
}

static void rk3588_rknn_make_ppu_identity_regcmd(
    uint64_t commands[RK3588_RKNN_PPU_COMMANDS])
{
    size_t index;

    rk3588_rknn_make_ppu_regcmd(commands, 0, 0);
#define PATCH_PPU_IDENTITY(_target, _reg, _value) do {               \
    index = rk3588_rknn_find_regcmd(commands,                        \
                                    RK3588_RKNN_PPU_COMMANDS,         \
                                    (_target), (_reg));              \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg), (_value)); \
} while (0)
    PATCH_PPU_IDENTITY(0x4001, 0x600c, 0);
    PATCH_PPU_IDENTITY(0x4001, 0x6010, 0);
    PATCH_PPU_IDENTITY(0x4001, 0x6014, 31);
    PATCH_PPU_IDENTITY(0x4001, 0x6018, 0);
    PATCH_PPU_IDENTITY(0x4001, 0x601c, 0);
    PATCH_PPU_IDENTITY(0x4001, 0x6020, 31);
    PATCH_PPU_IDENTITY(0x4001, 0x6024, 0x11);
    PATCH_PPU_IDENTITY(0x4001, 0x6034, 0);
    PATCH_PPU_IDENTITY(0x4001, 0x607c, 0x10);
    PATCH_PPU_IDENTITY(0x8001, 0x700c, 0);
    PATCH_PPU_IDENTITY(0x8001, 0x7010, 0);
    PATCH_PPU_IDENTITY(0x8001, 0x7014, 31);
    PATCH_PPU_IDENTITY(0x8001, 0x7024, 0x10);
    PATCH_PPU_IDENTITY(0x8001, 0x7028, 0x10);
#undef PATCH_PPU_IDENTITY
}

static void rk3588_rknn_prepare_ppu_config(QTestState *qts,
                                           uint32_t task_base,
                                           uint32_t padding,
                                           bool board_pattern,
                                           bool intercept_irq)
{
    uint64_t commands[RK3588_RKNN_PPU_COMMANDS] = { 0 };
    uint8_t input[RK3588_RKNN_PPU_BUFFER_SIZE];
    uint8_t output[RK3588_RKNN_PPU_BUFFER_SIZE];

    rk3588_rknn_make_ppu_regcmd(commands, task_base, padding);
    if (board_pattern) {
        for (unsigned int i = 0; i < ARRAY_SIZE(input); i++) {
            unsigned int surface = i / RK3588_RKNN_PPU_SURF_STRIDE;

            input[i] = i * 37 + 11 + surface * 17;
        }
    } else {
        memset(input, 0x88, sizeof(input));
    }
    memset(output, 0xa5, sizeof(output));
    if (!board_pattern) {
        for (unsigned int surface = 0;
             surface < RK3588_RKNN_PPU_SURFACES; surface++) {
            input[surface * RK3588_RKNN_PPU_SURF_STRIDE] =
                surface << 4 | 0x8;
        }
        input[7 * RK3588_RKNN_PPU_SURF_STRIDE +
              5 * RK3588_RKNN_PPU_LINE_STRIDE +
              5 * RK3588_RKNN_PPU_BYTES_PER_PIXEL] = 0x87;
        input[1] = 0x81;
        input[RK3588_RKNN_PPU_BYTES_PER_PIXEL + 1] = 0x8f;
    }

    qtest_writel(qts, RK3588_RKNN_MATMUL_DTE_ADDR + 64 * 4,
                 RK3588_RKNN_MATMUL_PTE_ADDR | 1);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR,
                 RK3588_RKNN_MATMUL_REGCMD_ADDR | RK_IOMMU_PTE_RW);
    for (unsigned int page = 0;
         page < DIV_ROUND_UP(RK3588_RKNN_PPU_BUFFER_SIZE, 0x1000); page++) {
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + (1 + page) * 4,
                     (RK3588_RKNN_PPU_INPUT_ADDR + page * 0x1000) |
                     RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_READABLE);
        qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + (16 + page) * 4,
                     (RK3588_RKNN_PPU_OUTPUT_ADDR + page * 0x1000) |
                     RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_WRITABLE);
    }
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_memwrite(qts, RK3588_RKNN_PPU_INPUT_ADDR, input, sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_PPU_OUTPUT_ADDR, output, sizeof(output));
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_DTE_ADDR,
                 RK3588_RKNN_MATMUL_DTE_ADDR);
    qtest_writel(qts, RK3588_RKNN0_MMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_ENABLE_PAGING);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_BASE_ADDRESS,
                 RK3588_RKNN_MATMUL_REGCMD_IOVA);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_REGISTER_AMOUNTS,
                 rk3588_rknn_register_amount(RK3588_RKNN_PPU_COMMANDS));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_MASK,
                 RKNN_PPU_INTERRUPT_MASK);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_TASK_DMA_BASE_ADDR,
                 task_base);
    if (intercept_irq) {
        qtest_irq_intercept_in(qts, RK3588_GIC_QOM);
    }
}

static void rk3588_rknn_prepare_ppu(QTestState *qts, uint32_t task_base)
{
    rk3588_rknn_prepare_ppu_config(qts, task_base, 0x2022, false, true);
}

static void rk3588_rknn_start_ppu(QTestState *qts)
{
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_TASK_CON, 0x00003001);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_OPERATION_ENABLE,
                 RKNN_PC_OPERATION_ENABLE_OP_EN);
}

static void rk3588_rknn_assert_ppu_status(QTestState *qts)
{
    rk3588_rknn_assert_pc(
        qts, RKNN_PPU_TASK_STATUS,
        RKNN_STAGE_RAW_STATUS_BITS | RKNN_PPU_BANK0_INTERRUPT);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_STATUS), ==,
                    RKNN_PPU_BANK0_INTERRUPT);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_BASE), ==,
                    RKNN_PPU_STATUS_SUCCESS);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_RDMA_BASE), ==,
                    RKNN_PPU_STATUS_SUCCESS);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_BASE + RKNN_POINTER),
                    ==, 0x0001000e);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_RDMA_BASE +
                                RKNN_POINTER), ==, 0x0001000e);
    g_assert_true(qtest_get_irq(qts, RK3588_RKNN0_SPI));
}

static void rk3588_rknn_assert_ppu_result(QTestState *qts)
{
    uint8_t output[RK3588_RKNN_PPU_BUFFER_SIZE];

    qtest_memread(qts, RK3588_RKNN_PPU_OUTPUT_ADDR, output, sizeof(output));
    for (unsigned int surface = 0; surface < RK3588_RKNN_PPU_SURFACES;
         surface++) {
        size_t surface_offset = surface * RK3588_RKNN_PPU_SURF_STRIDE;

        for (unsigned int row = 0; row < 20; row++) {
            size_t row_offset = surface_offset +
                                row * RK3588_RKNN_PPU_LINE_STRIDE;

            for (unsigned int column = 0; column < 10; column++) {
                for (unsigned int byte = 0;
                     byte < RK3588_RKNN_PPU_BYTES_PER_PIXEL; byte++) {
                    uint8_t expected = 0x88;
                    size_t offset = row_offset +
                                    column *
                                    RK3588_RKNN_PPU_BYTES_PER_PIXEL + byte;

                    if (byte == 0 && row <= 2 && column <= 2) {
                        expected = surface << 4 | 0x8;
                    } else if (!surface && byte == 1 && row <= 2 &&
                               column <= 3) {
                        expected = 0x8f;
                    }
                    g_assert_cmphex(output[offset], ==, expected);
                }
            }
            for (unsigned int offset =
                     10 * RK3588_RKNN_PPU_BYTES_PER_PIXEL;
                 offset < RK3588_RKNN_PPU_LINE_STRIDE; offset++) {
                g_assert_cmphex(output[row_offset + offset], ==, 0xa5);
            }
        }
    }
    rk3588_rknn_assert_ppu_status(qts);
}

static uint8_t rk3588_rknn_ppu_board_input(size_t offset,
                                            unsigned int surface)
{
    return offset * 37 + 11 + surface * 17;
}

static uint64_t rk3588_rknn_ppu_hash(const uint8_t *data, size_t size)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void rk3588_rknn_assert_ppu_right_padding_result(QTestState *qts)
{
    uint8_t output[RK3588_RKNN_PPU_BUFFER_SIZE];

    qtest_memread(qts, RK3588_RKNN_PPU_OUTPUT_ADDR, output, sizeof(output));
    for (unsigned int surface = 0; surface < RK3588_RKNN_PPU_SURFACES;
         surface++) {
        size_t surface_offset = surface * RK3588_RKNN_PPU_SURF_STRIDE;

        for (unsigned int row = 0; row < 20; row++) {
            size_t row_offset = surface_offset +
                                row * RK3588_RKNN_PPU_LINE_STRIDE;

            for (unsigned int column = 0; column < 10; column++) {
                for (unsigned int byte = 0;
                     byte < RK3588_RKNN_PPU_BYTES_PER_PIXEL; byte++) {
                    int best = -8;
                    uint8_t expected = 0x88;

                    for (unsigned int kernel_y = 0; kernel_y < 5;
                         kernel_y++) {
                        int input_y = row - 2 + kernel_y;

                        if (input_y < 0 || input_y >= 20) {
                            continue;
                        }
                        for (unsigned int kernel_x = 0; kernel_x < 5;
                             kernel_x++) {
                            unsigned int input_x = column + kernel_x;
                            size_t input_offset;
                            uint8_t input;
                            int value;

                            if (input_x >= 12) {
                                continue;
                            }
                            input_offset = surface_offset +
                                input_y * RK3588_RKNN_PPU_LINE_STRIDE +
                                input_x * RK3588_RKNN_PPU_BYTES_PER_PIXEL +
                                byte;
                            input = rk3588_rknn_ppu_board_input(
                                input_offset, surface);
                            value = input >> 4;
                            if (value & 8) {
                                value -= 16;
                            }
                            if (value > best) {
                                best = value;
                                expected = input;
                            }
                        }
                    }
                    g_assert_cmphex(output[row_offset +
                                           column *
                                           RK3588_RKNN_PPU_BYTES_PER_PIXEL +
                                           byte], ==, expected);
                }
            }
            for (unsigned int offset =
                     10 * RK3588_RKNN_PPU_BYTES_PER_PIXEL;
                 offset < RK3588_RKNN_PPU_LINE_STRIDE; offset++) {
                g_assert_cmphex(output[row_offset + offset], ==, 0xa5);
            }
        }
    }
    g_assert_cmphex(rk3588_rknn_ppu_hash(output, sizeof(output)), ==,
                    UINT64_C(0x64475df9ae264e23));
    rk3588_rknn_assert_ppu_status(qts);
}

static void test_rk3588_rknpu_ppu_task_dma_base(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_ppu(qts, RK3588_RKNN_MATMUL_REGCMD_IOVA);
    rk3588_rknn_start_ppu(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
    rk3588_rknn_assert_ppu_result(qts);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_ppu_right_padding(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_ppu_config(qts, 0, 0x2220, true, true);
    rk3588_rknn_start_ppu(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
    rk3588_rknn_assert_ppu_right_padding_result(qts);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_ppu_identity(void)
{
    uint64_t commands[RK3588_RKNN_PPU_COMMANDS];
    uint8_t input[32];
    uint8_t output[64];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_ppu(qts, 0);
    rk3588_rknn_make_ppu_identity_regcmd(commands);
    for (unsigned int i = 0; i < ARRAY_SIZE(input); i++) {
        input[i] = i * 29 + 0x80;
    }
    memset(output, 0xa5, sizeof(output));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_memwrite(qts, RK3588_RKNN_PPU_INPUT_ADDR, input, sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_PPU_OUTPUT_ADDR, output, sizeof(output));
    rk3588_rknn_start_ppu(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
    qtest_memread(qts, RK3588_RKNN_PPU_OUTPUT_ADDR, output, sizeof(output));

    g_assert_cmpmem(output, sizeof(input), input, sizeof(input));
    for (unsigned int i = sizeof(input); i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0xa5);
    }
    rk3588_rknn_assert_ppu_status(qts);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_ppu_fp16_identity(void)
{
    enum {
        WIDTH = 2,
        HEIGHT = 2,
        CHANNELS = 24,
        LANES = 8,
        SURFACES = CHANNELS / LANES,
        LINE_STRIDE = WIDTH * 16,
        SURF_STRIDE = LINE_STRIDE * HEIGHT,
        BUFFER_SIZE = SURFACES * SURF_STRIDE,
    };
    uint64_t commands[RK3588_RKNN_PPU_COMMANDS];
    uint8_t input[BUFFER_SIZE];
    uint8_t output[BUFFER_SIZE];
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t index;

    rk3588_rknn_prepare_ppu(qts, 0);
    rk3588_rknn_make_ppu_identity_regcmd(commands);
#define PATCH_PPU_FP16_IDENTITY(_target, _reg, _value) do {          \
    index = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),  \
                                    (_target), (_reg));             \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg),         \
                                          (_value));                \
} while (0)
    PATCH_PPU_FP16_IDENTITY(0x4001, 0x600c, WIDTH - 1);
    PATCH_PPU_FP16_IDENTITY(0x4001, 0x6010, HEIGHT - 1);
    PATCH_PPU_FP16_IDENTITY(0x4001, 0x6014, CHANNELS - 1);
    PATCH_PPU_FP16_IDENTITY(0x4001, 0x6018, WIDTH - 1);
    PATCH_PPU_FP16_IDENTITY(0x4001, 0x601c, HEIGHT - 1);
    PATCH_PPU_FP16_IDENTITY(0x4001, 0x6020, CHANNELS - 1);
    PATCH_PPU_FP16_IDENTITY(0x4001, 0x607c, SURF_STRIDE);
    PATCH_PPU_FP16_IDENTITY(0x4001, 0x6084, 0x102);
    PATCH_PPU_FP16_IDENTITY(0x8001, 0x700c, WIDTH - 1);
    PATCH_PPU_FP16_IDENTITY(0x8001, 0x7010, HEIGHT - 1);
    PATCH_PPU_FP16_IDENTITY(0x8001, 0x7014, CHANNELS - 1);
    PATCH_PPU_FP16_IDENTITY(0x8001, 0x7024, LINE_STRIDE);
    PATCH_PPU_FP16_IDENTITY(0x8001, 0x7028, SURF_STRIDE);
    PATCH_PPU_FP16_IDENTITY(0x8001, 0x7030, 2);
#undef PATCH_PPU_FP16_IDENTITY

    for (unsigned int surface = 0; surface < SURFACES; surface++) {
        for (unsigned int row = 0; row < HEIGHT; row++) {
            for (unsigned int lane = 0; lane < WIDTH * LANES; lane++) {
                size_t offset = surface * SURF_STRIDE + row * LINE_STRIDE +
                                lane * sizeof(uint16_t);

                stw_le_p(input + offset,
                         0x3000 + surface * 0x100 + row * 0x20 + lane);
            }
        }
    }
    memset(output, 0xa5, sizeof(output));
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    qtest_memwrite(qts, RK3588_RKNN_PPU_INPUT_ADDR, input, sizeof(input));
    qtest_memwrite(qts, RK3588_RKNN_PPU_OUTPUT_ADDR, output, sizeof(output));
    rk3588_rknn_start_ppu(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
    qtest_memread(qts, RK3588_RKNN_PPU_OUTPUT_ADDR, output, sizeof(output));

    g_assert_cmpmem(output, sizeof(output), input, sizeof(input));
    rk3588_rknn_assert_ppu_status(qts);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_ppu_fp16_max_pool(void)
{
    static const uint16_t values[] = {
        0xc000, 0xbc00, 0x0000, 0x3c00,
        0x4000, 0x4200, 0x4400, 0x4500,
    };
    static const uint32_t data_formats[] = { 0x102, 0x42 };
    enum {
        IN_WIDTH = 4,
        IN_HEIGHT = 4,
        OUT_WIDTH = 2,
        OUT_HEIGHT = 2,
        CHANNELS = 24,
        LANES = 8,
        SURFACES = CHANNELS / LANES,
        INPUT_LINE_STRIDE = IN_WIDTH * 16,
        INPUT_SURF_STRIDE = INPUT_LINE_STRIDE * IN_HEIGHT,
        OUTPUT_LINE_STRIDE = OUT_WIDTH * 16,
        OUTPUT_SURF_STRIDE = OUTPUT_LINE_STRIDE * OUT_HEIGHT,
    };
    uint64_t commands[RK3588_RKNN_PPU_COMMANDS];
    uint8_t input[SURFACES * INPUT_SURF_STRIDE];
    uint8_t output[SURFACES * OUTPUT_SURF_STRIDE];
    QTestState *qts;

#define PATCH_PPU_FP16(_target, _reg, _value) \
    rk3588_rknn_patch_regcmd(commands, ARRAY_SIZE(commands), (_target), \
                             (_reg), (_value))
    for (unsigned int surface = 0; surface < SURFACES; surface++) {
        for (unsigned int row = 0; row < IN_HEIGHT; row++) {
            for (unsigned int column = 0; column < IN_WIDTH; column++) {
                for (unsigned int lane = 0; lane < LANES; lane++) {
                    unsigned int rank =
                        (surface * 3 + row * 5 + column * 7 + lane) %
                        ARRAY_SIZE(values);
                    size_t offset = surface * INPUT_SURF_STRIDE +
                        row * INPUT_LINE_STRIDE + column * 16 + lane * 2;

                    stw_le_p(input + offset, values[rank]);
                }
            }
        }
    }
    for (unsigned int format = 0; format < ARRAY_SIZE(data_formats);
         format++) {
        qts = rk3588_qtest_start_rknpu();
        rk3588_rknn_prepare_ppu(qts, 0);
        qtest_memread(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                      sizeof(commands));
        PATCH_PPU_FP16(0x4001, 0x600c, IN_WIDTH - 1);
        PATCH_PPU_FP16(0x4001, 0x6010, IN_HEIGHT - 1);
        PATCH_PPU_FP16(0x4001, 0x6014, CHANNELS - 1);
        PATCH_PPU_FP16(0x4001, 0x6018, OUT_WIDTH - 1);
        PATCH_PPU_FP16(0x4001, 0x601c, OUT_HEIGHT - 1);
        PATCH_PPU_FP16(0x4001, 0x6020, CHANNELS - 1);
        PATCH_PPU_FP16(0x4001, 0x6024, 0x11);
        PATCH_PPU_FP16(0x4001, 0x6034, 0x00110101);
        PATCH_PPU_FP16(0x4001, 0x6040, 0);
        PATCH_PPU_FP16(0x4001, 0x607c, OUTPUT_SURF_STRIDE);
        PATCH_PPU_FP16(0x4001, 0x6084, data_formats[format]);
        PATCH_PPU_FP16(0x8001, 0x700c, IN_WIDTH - 1);
        PATCH_PPU_FP16(0x8001, 0x7010, IN_HEIGHT - 1);
        PATCH_PPU_FP16(0x8001, 0x7014, CHANNELS - 1);
        PATCH_PPU_FP16(0x8001, 0x7024, INPUT_LINE_STRIDE);
        PATCH_PPU_FP16(0x8001, 0x7028, INPUT_SURF_STRIDE);
        PATCH_PPU_FP16(0x8001, 0x7030, 2);
        memset(output, 0xa5, sizeof(output));
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                       sizeof(commands));
        qtest_memwrite(qts, RK3588_RKNN_PPU_INPUT_ADDR, input,
                       sizeof(input));
        qtest_memwrite(qts, RK3588_RKNN_PPU_OUTPUT_ADDR, output,
                       sizeof(output));
        rk3588_rknn_start_ppu(qts);
        qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
        rk3588_rknn_wait_idle(qts);
        qtest_memread(qts, RK3588_RKNN_PPU_OUTPUT_ADDR, output,
                      sizeof(output));

        for (unsigned int surface = 0; surface < SURFACES; surface++) {
            for (unsigned int row = 0; row < OUT_HEIGHT; row++) {
                for (unsigned int column = 0; column < OUT_WIDTH; column++) {
                    for (unsigned int lane = 0; lane < LANES; lane++) {
                        unsigned int best = 0;

                        for (unsigned int kernel_row = 0; kernel_row < 2;
                             kernel_row++) {
                            for (unsigned int kernel_column = 0;
                                 kernel_column < 2; kernel_column++) {
                                unsigned int rank =
                                    (surface * 3 +
                                     (row * 2 + kernel_row) * 5 +
                                     (column * 2 + kernel_column) * 7 +
                                     lane) % ARRAY_SIZE(values);

                                best = MAX(best, rank);
                            }
                        }
                        size_t offset = surface * OUTPUT_SURF_STRIDE +
                            row * OUTPUT_LINE_STRIDE + column * 16 + lane * 2;

                        g_assert_cmphex(lduw_le_p(output + offset), ==,
                                        values[best]);
                    }
                }
            }
        }
        rk3588_rknn_assert_ppu_status(qts);
        qtest_quit(qts);
    }
#undef PATCH_PPU_FP16
}

static void rk3588_rknn_assert_ppu_model_error(QTestState *qts,
                                               uint32_t ppu_status);

static void test_rk3588_rknpu_ppu_resource_budget(void)
{
    uint64_t commands[RK3588_RKNN_PPU_COMMANDS];
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t index;

    rk3588_rknn_prepare_ppu(qts, 0);
    qtest_memread(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                  sizeof(commands));
#define PATCH_PPU_BUDGET(_target, _reg, _value) do {                \
    index = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands), \
                                    (_target), (_reg));             \
    commands[index] = rk3588_rknn_regcmd((_target), (_reg),        \
                                          (_value));                \
} while (0)
    PATCH_PPU_BUDGET(0x4001, 0x600c, 0x1fff);
    PATCH_PPU_BUDGET(0x4001, 0x6010, 0x1fff);
    PATCH_PPU_BUDGET(0x4001, 0x6014, 0x1fff);
    PATCH_PPU_BUDGET(0x4001, 0x6018, 0x1fff);
    PATCH_PPU_BUDGET(0x4001, 0x601c, 0x1fff);
    PATCH_PPU_BUDGET(0x4001, 0x6020, 0x1fff);
#undef PATCH_PPU_BUDGET
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));

    rk3588_rknn_start_ppu(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
    rk3588_rknn_assert_ppu_model_error(qts, RKNN_PPU_STATUS_FAULT);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_ppu_banks_w1c(void)
{
    uint64_t commands[RK3588_RKNN_PPU_COMMANDS];
    QTestState *qts = rk3588_qtest_start_rknpu();
    size_t ppu_pointer;
    size_t rdma_pointer;

    rk3588_rknn_prepare_ppu(qts, 0);
    rk3588_rknn_start_ppu(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
    rk3588_rknn_assert_ppu_status(qts);

    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_CLEAR,
                 RKNN_PPU_BANK1_INTERRUPT);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    RKNN_STAGE_RAW_STATUS_BITS |
                    RKNN_PPU_BANK0_INTERRUPT);
    g_assert_true(qtest_get_irq(qts, RK3588_RKNN0_SPI));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_CLEAR,
                 RKNN_PPU_BANK0_INTERRUPT);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    RKNN_STAGE_RAW_STATUS_BITS);
    g_assert_false(qtest_get_irq(qts, RK3588_RKNN0_SPI));

    qtest_memread(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                  sizeof(commands));
    ppu_pointer = rk3588_rknn_find_regcmd(
        commands, ARRAY_SIZE(commands), 0x4001, 0x6004);
    rdma_pointer = rk3588_rknn_find_regcmd(
        commands, ARRAY_SIZE(commands), 0x8001, 0x7004);
    commands[ppu_pointer] = rk3588_rknn_regcmd(0x4001, 0x6004, 0x0f);
    commands[rdma_pointer] = rk3588_rknn_regcmd(0x8001, 0x7004, 0x0f);
    qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                   sizeof(commands));
    rk3588_rknn_start_ppu(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);

    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    RKNN_STAGE_RAW_STATUS_BITS |
                    RKNN_PPU_BANK1_INTERRUPT);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_STATUS), ==,
                    RKNN_PPU_BANK1_INTERRUPT);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_BASE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_RDMA_BASE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_BASE + RKNN_POINTER),
                    ==, 0x0000000f);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_RDMA_BASE +
                                RKNN_POINTER), ==, 0x0000000f);
    g_assert_true(qtest_get_irq(qts, RK3588_RKNN0_SPI));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_CLEAR,
                 RKNN_PPU_BANK0_INTERRUPT);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    RKNN_STAGE_RAW_STATUS_BITS |
                    RKNN_PPU_BANK1_INTERRUPT);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_CLEAR,
                 RKNN_PPU_BANK1_INTERRUPT);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    RKNN_STAGE_RAW_STATUS_BITS);
    g_assert_false(qtest_get_irq(qts, RK3588_RKNN0_SPI));

    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_MASK,
                 UINT32_MAX);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_MASK), ==,
                    RKNN_PC_INTERRUPT_VALID_BITS);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_STATUS), ==,
                    0);
    g_assert_false(qtest_get_irq(qts, RK3588_RKNN0_SPI));
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_CLEAR,
                 UINT32_MAX);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    RKNN_STAGE_RAW_STATUS_BITS);
    rk3588_cru_rknpu_reset_pulse(
        qts, RK3588_CRU_SOFTRST_CON(30), BIT(6));
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    0);
    qtest_quit(qts);
}

static void rk3588_rknn_assert_ppu_no_dma(QTestState *qts,
                                          uint32_t ppu_status)
{
    uint8_t output[RK3588_RKNN_PPU_BUFFER_SIZE];

    qtest_memread(qts, RK3588_RKNN_PPU_OUTPUT_ADDR, output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0xa5);
    }
    rk3588_rknn_assert_pc(qts, RKNN_PPU_TASK_STATUS,
                             RKNN_STAGE_RAW_STATUS_BITS);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_BASE), ==,
                    ppu_status);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_RDMA_BASE), ==,
                    ppu_status);
    g_assert_false(qtest_get_irq(qts, RK3588_RKNN0_SPI));
}

static void rk3588_rknn_assert_ppu_model_error(QTestState *qts,
                                               uint32_t ppu_status)
{
    uint8_t output[RK3588_RKNN_PPU_BUFFER_SIZE];

    qtest_memread(qts, RK3588_RKNN_PPU_OUTPUT_ADDR, output, sizeof(output));
    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0xa5);
    }
    rk3588_rknn_assert_pc(qts, RKNN_TASK_STATUS_FETCH_ERROR | 1, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_BASE), ==, ppu_status);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_RDMA_BASE), ==,
                    ppu_status);
    g_assert_false(qtest_get_irq(qts, RK3588_RKNN0_SPI));
}

static void test_rk3588_rknpu_ppu_reset_pending(void)
{
    uint8_t output[RK3588_RKNN_PPU_BUFFER_SIZE];
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_ppu_config(qts, 0, 0x2022, false, false);
    qtest_irq_intercept_out_named(qts, RK3588_RKNN0_QOM, "sysbus-irq");
    rk3588_rknn_start_ppu(qts);
    qtest_system_reset(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    qtest_memread(qts, RK3588_RKNN_PPU_OUTPUT_ADDR, output, sizeof(output));

    for (unsigned int i = 0; i < ARRAY_SIZE(output); i++) {
        g_assert_cmphex(output[i], ==, 0xa5);
    }
    rk3588_rknn_assert_pc(qts, 0, 0);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_BASE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_BASE + RKNN_POINTER),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_RDMA_BASE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_RDMA_BASE +
                                RKNN_POINTER), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void test_rk3588_rknpu_ppu_source_read_fault(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_ppu(qts, 0);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 1 * 4,
                 RK3588_RKNN_PPU_INPUT_ADDR |
                 RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_WRITABLE);
    rk3588_rknn_start_ppu(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
    rk3588_rknn_assert_ppu_no_dma(qts, RKNN_PPU_STATUS_FAULT);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_ppu_destination_write_fault(void)
{
    QTestState *qts = rk3588_qtest_start_rknpu();

    rk3588_rknn_prepare_ppu(qts, 0);
    qtest_writel(qts, RK3588_RKNN_MATMUL_PTE_ADDR + 16 * 4,
                 RK3588_RKNN_PPU_OUTPUT_ADDR |
                 RK_IOMMU_PTE_VALID | RK_IOMMU_PTE_READABLE);
    rk3588_rknn_start_ppu(qts);
    qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(qts);
    rk3588_rknn_assert_ppu_no_dma(qts, RKNN_PPU_STATUS_FAULT);
    qtest_quit(qts);
}

static void test_rk3588_rknpu_ppu_field_semantics(void)
{
    static const struct {
        const char *name;
        uint32_t target;
        uint32_t reg;
        uint32_t value;
        bool executable;
    } cases[] = {
        { "rdma-width", 0x8001, 0x700c, 10, false },
        { "rdma-height", 0x8001, 0x7010, 18, false },
        { "rdma-channel", 0x8001, 0x7014, 126, false },
        { "reciprocal-width", 0x4001, 0x6038, 1, true },
        { "reciprocal-height", 0x4001, 0x603c, 1, true },
        { "padding-value-0", 0x4001, 0x6044, 1, false },
        { "padding-value-1", 0x4001, 0x6048, 1, false },
        { "index-output", 0x4001, 0x6024, 0x400a0011, false },
        { "inactive-index-add", 0x4001, 0x6084, 0x1900, true },
        { "process-precision", 0x4001, 0x6084, 0x11, false },
        { "dpu-fly-in", 0x4001, 0x6084, 0x18, false },
        { "misc-control", 0x4001, 0x60dc, 2, false },
    };
    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        uint64_t commands[RK3588_RKNN_PPU_COMMANDS];
        QTestState *qts = rk3588_qtest_start_rknpu();
        size_t index;

        g_test_message("PPU field semantic: %s", cases[i].name);
        rk3588_rknn_prepare_ppu(qts, 0);
        qtest_memread(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                      sizeof(commands));
        index = rk3588_rknn_find_regcmd(commands, ARRAY_SIZE(commands),
                                        cases[i].target, cases[i].reg);
        commands[index] = rk3588_rknn_regcmd(
            cases[i].target, cases[i].reg, cases[i].value);
        qtest_memwrite(qts, RK3588_RKNN_MATMUL_REGCMD_ADDR, commands,
                       sizeof(commands));
        rk3588_rknn_start_ppu(qts);
        qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
        rk3588_rknn_wait_idle(qts);
        if (cases[i].executable) {
            rk3588_rknn_assert_ppu_result(qts);
        } else {
            rk3588_rknn_assert_ppu_model_error(
                qts, RKNN_PPU_STATUS_FAULT);
        }
        qtest_quit(qts);
    }
}

#ifndef _WIN32
static void test_rk3588_rknpu_ppu_migration_snapshot(void)
{
    QTestState *source;
    QTestState *destination;

    source = rk3588_qtest_start_rknpu();
    rk3588_rknn_prepare_ppu_config(source, 0, 0x2220, true, true);
    rk3588_rknn_start_ppu(source);
    destination = rk3588_rknn_migrate(source, true);
    qtest_clock_step(destination, RKNN_COMPLETE_DELAY_NS);
    rk3588_rknn_wait_idle(destination);
    rk3588_rknn_assert_ppu_right_padding_result(destination);
    qtest_quit(source);
    qtest_quit(destination);
}

#endif

static void test_rk3588_rknpu_reset_state(void)
{
    static const struct {
        uint64_t cna_base;
        uint32_t reset_offset;
        uint32_t reset_bits[2];
    } reset_cases[] = {
        { RK3588_RKNN0_CNA_BASE, RK3588_CRU_SOFTRST_CON(30),
          { BIT(6), BIT(8) } },
        { RK3588_RKNN1_CNA_BASE, RK3588_CRU_SOFTRST_CON(27),
          { BIT(0), BIT(2) } },
        { RK3588_RKNN2_CNA_BASE, RK3588_CRU_SOFTRST_CON(28),
          { BIT(0), BIT(2) } },
    };
    QTestState *qts;

    qts = rk3588_qtest_start_rknpu();
    qtest_irq_intercept_in(qts, RK3588_GIC_QOM);
    rk3588_rknn_prepare_matmul(qts, true, 0xa5);
    qtest_writel(qts, RK3588_RKNN0_PC_BASE + RKNN_PC_INTERRUPT_MASK, 0);
    rk3588_rknn_run_matmul(qts);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==,
                    RKNN_PIPELINE_BANK1_INTERRUPT);

    qtest_writel(qts, RK3588_RKNN0_CNA_BASE + RKNN_POINTER, 0xffffffff);
    qtest_writel(qts, RK3588_RKNN0_CORE_BASE + RKNN_POINTER, 0xffffffff);
    qtest_writel(qts, RK3588_RKNN0_PPU_BASE + RKNN_POINTER, 0xffffffff);
    qtest_writel(qts, RK3588_RKNN0_PPU_RDMA_BASE + RKNN_POINTER,
                 0xffffffff);
    qtest_writel(qts, RK3588_RKNN0_PPU_BASE, 0xffffffff);
    qtest_writel(qts, RK3588_RKNN0_PPU_RDMA_BASE, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_BASE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_RDMA_BASE), ==, 0);
    rk3588_rknn_start_matmul(qts);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_OPERATION_ENABLE) &
                    RKNN_PC_OPERATION_ENABLE_OP_EN, ==,
                    RKNN_PC_OPERATION_ENABLE_OP_EN);
    rk3588_cru_rknpu_reset_pulse(qts, RK3588_CRU_SOFTRST_CON(30), BIT(6));
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_VERSION), ==,
                    RKNN_PC_VERSION_VALUE);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_VERSION_NUM), ==,
                    RKNN_PC_VERSION_NUM_VALUE);
    g_assert_cmphex(rk3588_rknn_read_pc(qts, RKNN_PC_INTERRUPT_MASK), ==,
                    RKNN_PC_INTERRUPT_VALID_BITS);
    g_assert_cmphex(rk3588_rknn_read_pc(
                        qts, RKNN_PC_INTERRUPT_RAW_STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CNA_BASE +
                                RKNN_POINTER), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_CORE_BASE +
                                RKNN_POINTER), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_BASE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_BASE + RKNN_POINTER),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_RDMA_BASE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_RKNN0_PPU_RDMA_BASE +
                                RKNN_POINTER), ==, 0);
    g_assert_false(qtest_get_irq(qts, RK3588_RKNN0_SPI));

    for (unsigned int core = 0; core < ARRAY_SIZE(reset_cases); core++) {
        unsigned int neighbor = (core + 1) % ARRAY_SIZE(reset_cases);

        for (unsigned int signal = 0;
             signal < ARRAY_SIZE(reset_cases[core].reset_bits); signal++) {
            uint32_t bit = reset_cases[core].reset_bits[signal];
            uint32_t neighbor_value;

            qtest_writel(qts, reset_cases[core].cna_base + RKNN_POINTER,
                         0x100 + core * 2 + signal);
            qtest_writel(qts, reset_cases[neighbor].cna_base + RKNN_POINTER,
                         0xfeed0000 + core * 2 + signal);
            neighbor_value = qtest_readl(
                qts, reset_cases[neighbor].cna_base + RKNN_POINTER);
            rk3588_cru_rknpu_reset_pulse(
                qts, reset_cases[core].reset_offset, bit);
            g_assert_cmphex(qtest_readl(qts, reset_cases[core].cna_base +
                                        RKNN_POINTER), ==, core << 28);
            g_assert_cmphex(qtest_readl(qts, reset_cases[neighbor].cna_base +
                                        RKNN_POINTER), ==,
                            neighbor_value);
            qtest_writel(qts, reset_cases[neighbor].cna_base + RKNN_POINTER,
                         0);
        }
    }

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3588_EVB_MACHINE)) {
        g_test_skip(RK3588_EVB_MACHINE " machine not available");
        return 0;
    }
    if (!qtest_has_device("rockchip.rk3588-rknn-core")) {
        g_test_skip("rockchip.rk3588-rknn-core device not available");
        return 0;
    }

    qtest_add_func("/rk3588/rknpu-disabled-by-default",
                   test_rk3588_rknpu_disabled_by_default);
    qtest_add_func("/rk3588s-roc-pc/rknpu-fdt",
                   test_rk3588s_roc_pc_rknpu_fdt);
    qtest_add_func("/rk3588/rknpu-fdt", test_rk3588_evb_rknpu_fdt);
    qtest_add_func("/rock-5b-plus/rknpu-fdt",
                   test_rock_5b_plus_rknpu_fdt);
    qtest_add_func("/rk3588/rknpu-version-and-cores",
                   test_rk3588_rknpu_version_and_cores);
    qtest_add_func("/rk3588/rknpu-ppu-windows-all-cores",
                   test_rk3588_rknpu_ppu_windows_all_cores);
    qtest_add_func("/rk3588/rknpu-iommu-mmio",
                   test_rk3588_rknpu_iommu_mmio);
    qtest_add_func("/rk3588/rknpu-iommu-v2-high-address",
                   test_rk3588_rknpu_iommu_v2_high_address);
    qtest_add_func("/rk3588/rknpu-iommu-fault-irq",
                   test_rk3588_rknpu_iommu_fault_irq);
    qtest_add_func("/rk3588/rknpu-iommu-permission-fault-bank",
                   test_rk3588_rknpu_iommu_permission_fault_bank);
    qtest_add_func("/rk3588/rknpu-start-fetch-error",
                   test_rk3588_rknpu_start_fetch_error);
    qtest_add_func("/rk3588/rknpu-matmul",
                   test_rk3588_rknpu_matmul);
    qtest_add_func("/rk3588/rknpu-execution-responsive",
                   test_rk3588_rknpu_execution_responsive);
    qtest_add_func("/rk3588/rknpu-multicore-delayed-start-parallel",
                   test_rk3588_rknpu_multicore_delayed_start_parallel);
    qtest_add_func("/rk3588/rknpu-two-core-rendezvous",
                   test_rk3588_rknpu_two_core_rendezvous);
    qtest_add_func("/rk3588/rknpu-three-core-rendezvous",
                   test_rk3588_rknpu_three_core_rendezvous);
    qtest_add_func("/rk3588/rknpu-matmul-int8-quantized-planar",
                   test_rk3588_rknpu_matmul_int8_quantized_planar);
    qtest_add_func("/rk3588/rknpu-matmul-int8-out-cvt-qd-disabled",
                   test_rk3588_rknpu_matmul_int8_out_cvt_qd_disabled);
    qtest_add_func("/rk3588/rknpu-matmul-int8-out-cvt-qd-enabled",
                   test_rk3588_rknpu_matmul_int8_out_cvt_qd_enabled);
    qtest_add_func("/rk3588/rknpu-matmul-fp16",
                   test_rk3588_rknpu_matmul_fp16);
    qtest_add_func("/rk3588/rknpu-matmul-fp16-weight-groups",
                   test_rk3588_rknpu_matmul_fp16_weight_groups);
    qtest_add_func("/rk3588/rknpu-matmul-fp16-control-mutations",
                   test_rk3588_rknpu_matmul_fp16_control_mutations);
    qtest_add_func("/rk3588/rknpu-fp16-compact-large-broadcast",
                   test_rk3588_rknpu_fp16_compact_large_broadcast);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-fp16-ew-data-mode",
                   test_rk3588_rknpu_dpu_rdma_fp16_ew_data_mode);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-fp16-ew-controls",
                   test_rk3588_rknpu_dpu_rdma_fp16_ew_controls);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-fp16-divide",
                   test_rk3588_rknpu_dpu_rdma_fp16_divide);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-raw16-compact-u8",
                   test_rk3588_rknpu_dpu_rdma_raw16_compact_u8);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-raw16-reshape",
                   test_rk3588_rknpu_dpu_rdma_raw16_reshape);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-int8-to-fp16",
                   test_rk3588_rknpu_dpu_rdma_int8_to_fp16);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-int8-unary",
                   test_rk3588_rknpu_dpu_rdma_int8_unary);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-int8-spatial-reshape",
                   test_rk3588_rknpu_dpu_rdma_int8_spatial_reshape);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-int8-binary",
                   test_rk3588_rknpu_dpu_rdma_int8_binary);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-int8-pipeline-controls",
                   test_rk3588_rknpu_dpu_rdma_int8_pipeline_controls);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-int16-unpool-odd-height",
                   test_rk3588_rknpu_dpu_rdma_int16_unpool_odd_height);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-int16-unpool-controls",
                   test_rk3588_rknpu_dpu_rdma_int16_unpool_controls);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-fp16-bs",
                   test_rk3588_rknpu_dpu_rdma_fp16_bs);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-fp16-lut",
                   test_rk3588_rknpu_dpu_rdma_fp16_lut);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-fp16-lut-controls",
                   test_rk3588_rknpu_dpu_rdma_fp16_lut_controls);
    qtest_add_func("/rk3588/rknpu-dpu-rdma-int8-to-fp16-iommu",
                   test_rk3588_rknpu_dpu_rdma_int8_to_fp16_iommu);
    qtest_add_func("/rk3588/rknpu-matmul-slave",
                   test_rk3588_rknpu_matmul_slave);
    qtest_add_func("/rk3588/rknpu-int8-qd-brdma-erdma",
                   test_rk3588_rknpu_int8_qd_brdma_erdma);
    qtest_add_func("/rk3588/rknpu-int8-qd-brdma-erdma-controls",
                   test_rk3588_rknpu_int8_qd_brdma_erdma_controls);
    qtest_add_func("/rk3588/rknpu-rgb-cvt-convolution",
                   test_rk3588_rknpu_rgb_cvt_convolution);
    qtest_add_func("/rk3588/rknpu-int8-qd-nrdma",
                   test_rk3588_rknpu_int8_qd_nrdma);
    qtest_add_func("/rk3588/rknpu-int8-qd-nrdma-controls",
                   test_rk3588_rknpu_int8_qd_nrdma_controls);
    qtest_add_func("/rk3588/rknpu-int8-qd-brdma-no-cpend",
                   test_rk3588_rknpu_int8_qd_brdma_no_cpend);
    qtest_add_func("/rk3588/rknpu-rgb-cvt-control-mutations",
                   test_rk3588_rknpu_rgb_cvt_control_mutations);
    qtest_add_func("/rk3588/rknpu-int8-qd-brdma-partial-channels",
                   test_rk3588_rknpu_int8_qd_brdma_partial_channels);
    qtest_add_func("/rk3588/rknpu-int8-qd-brdma-storage-channels",
                   test_rk3588_rknpu_int8_qd_brdma_storage_channels);
    qtest_add_func("/rk3588/rknpu-int8-qd-brdma-layout-controls",
                   test_rk3588_rknpu_int8_qd_brdma_layout_controls);
    qtest_add_func("/rk3588/rknpu-int8-brdma",
                   test_rk3588_rknpu_int8_brdma);
    qtest_add_func("/rk3588/rknpu-int8-brdma-controls",
                   test_rk3588_rknpu_int8_brdma_controls);
    qtest_add_func("/rk3588/rknpu-int8-erdma-unused-controls",
                   test_rk3588_rknpu_int8_erdma_unused_controls);
    qtest_add_func("/rk3588/rknpu-depthwise-int8-brdma",
                   test_rk3588_rknpu_depthwise_int8_brdma);
    qtest_add_func("/rk3588/rknpu-depthwise-int8-brdma-controls",
                   test_rk3588_rknpu_depthwise_int8_brdma_controls);
    qtest_add_func("/rk3588/rknpu-weight-size0-semantics",
                   test_rk3588_rknpu_weight_size0_semantics);
    qtest_add_func("/rk3588/rknpu-large-shape-safe-failure",
                   test_rk3588_rknpu_large_shape_safe_failure);
    qtest_add_func("/rk3588/rknpu-slave-decode-failure",
                   test_rk3588_rknpu_slave_decode_failure);
    qtest_add_func("/rk3588/rknpu-matmul-iommu-access",
                   test_rk3588_rknpu_matmul_iommu_access);
    qtest_add_func("/rk3588/rknpu-matmul-shape",
                   test_rk3588_rknpu_matmul_shape);
    qtest_add_func("/rk3588/rknpu-conv1x1-spatial-hardware-shape",
                   test_rk3588_rknpu_conv1x1_spatial_hardware_shape);
    qtest_add_func("/rk3588/rknpu-spatial-rdma-ew-add-grouped",
                   test_rk3588_rknpu_spatial_rdma_ew_add_grouped);
    qtest_add_func("/rk3588/rknpu-spatial-rdma-grouped-iommu-failure",
                   test_rk3588_rknpu_spatial_rdma_grouped_iommu_failure);
    qtest_add_func("/rk3588/rknpu-spatial-brdma-bs-add-grouped",
                   test_rk3588_rknpu_spatial_brdma_bs_add_grouped);
    qtest_add_func("/rk3588/rknpu-conv3x3-padding-hardware-shape",
                   test_rk3588_rknpu_conv3x3_padding_hardware_shape);
    qtest_add_func("/rk3588/rknpu-conv3x3-stride2-hardware-shape",
                   test_rk3588_rknpu_conv3x3_stride2_hardware_shape);
    qtest_add_func("/rk3588/rknpu-conv3x3-asymmetric-hardware-shape",
                   test_rk3588_rknpu_conv3x3_asymmetric_hardware_shape);
    qtest_add_func("/rk3588/rknpu-depthwise-int32",
                   test_rk3588_rknpu_depthwise_int32);
    qtest_add_func("/rk3588/rknpu-depthwise-int8-qd",
                   test_rk3588_rknpu_depthwise_int8_qd);
    qtest_add_func("/rk3588/rknpu-depthwise-int8-qd-no-cpend",
                   test_rk3588_rknpu_depthwise_int8_qd_no_cpend);
    qtest_add_func("/rk3588/rknpu-depthwise-int8-deconv",
                   test_rk3588_rknpu_depthwise_int8_deconv);
    qtest_add_func("/rk3588/rknpu-depthwise-int8-deconv-controls",
                   test_rk3588_rknpu_depthwise_int8_deconv_controls);
    qtest_add_func("/rk3588/rknpu-depthwise-int8-grouped-weights",
                   test_rk3588_rknpu_depthwise_int8_grouped_weights);
    qtest_add_func("/rk3588/rknpu-depthwise-int8-control-mutations",
                   test_rk3588_rknpu_depthwise_int8_control_mutations);
    qtest_add_func("/rk3588/rknpu-depthwise-control-mutations",
                   test_rk3588_rknpu_depthwise_control_mutations);
    qtest_add_func("/rk3588/rknpu-dpu-out-cvt",
                   test_rk3588_rknpu_dpu_out_cvt);
    qtest_add_func("/rk3588/rknpu-dpu-positive-controls",
                   test_rk3588_rknpu_dpu_positive_controls);
    qtest_add_func("/rk3588/rknpu-dpu-mul-shifts",
                   test_rk3588_rknpu_dpu_mul_shifts);
    qtest_add_func("/rk3588/rknpu-dpu-mc-surf-out-spatial",
                   test_rk3588_rknpu_dpu_mc_surf_out_spatial);
    qtest_add_func("/rk3588/rknpu-dpu-stage-wide-combination",
                   test_rk3588_rknpu_dpu_stage_wide_combination);
    qtest_add_func("/rk3588/rknpu-dpu-output-controls",
                   test_rk3588_rknpu_dpu_output_controls);
    qtest_add_func("/rk3588/rknpu-dpu-resource-budget",
                   test_rk3588_rknpu_dpu_resource_budget);
    qtest_add_func("/rk3588/rknpu-dpu-host-budget",
                   test_rk3588_rknpu_dpu_host_budget);
    qtest_add_func("/rk3588/rknpu-pipeline-semantic-regcmd",
                   test_rk3588_rknpu_pipeline_semantic_regcmd);
    qtest_add_func("/rk3588/rknpu-pipeline-reset-defaults",
                   test_rk3588_rknpu_pipeline_reset_defaults);
    qtest_add_func("/rk3588/rknpu-pipeline-pointer-completion",
                   test_rk3588_rknpu_pipeline_pointer_completion);
    qtest_add_func("/rk3588/rknpu-pipeline-pointer-mode0",
                   test_rk3588_rknpu_pipeline_pointer_mode0);
    qtest_add_func("/rk3588/rknpu-pipeline-executor-disabled",
                   test_rk3588_rknpu_pipeline_executor_disabled);
    qtest_add_func("/rk3588/rknpu-pending-register-merge",
                   test_rk3588_rknpu_pending_register_merge);
    qtest_add_func("/rk3588/rknpu-pipeline-three-task-chain",
                   test_rk3588_rknpu_pipeline_three_task_chain);
    qtest_add_func("/rk3588/rknpu-pipeline-long-task-chain",
                   test_rk3588_rknpu_pipeline_long_task_chain);
    qtest_add_func("/rk3588/rknpu-pipeline-multitask-broken-chain",
                   test_rk3588_rknpu_pipeline_multitask_broken_chain);
    qtest_add_func("/rk3588/rknpu-pipeline-multitask-fetch-failure",
                   test_rk3588_rknpu_pipeline_multitask_fetch_failure);
    qtest_add_func("/rk3588/rknpu-pipeline-task-count-mask",
                   test_rk3588_rknpu_pipeline_task_count_mask);
    qtest_add_func("/rk3588/rknpu-pipeline-task-dma-base",
                   test_rk3588_rknpu_pipeline_task_dma_base);
#ifndef _WIN32
    qtest_add_func("/rk3588/rknpu-pipeline-migration-snapshot",
                   test_rk3588_rknpu_pipeline_migration_snapshot);
    qtest_add_func("/rk3588/rknpu-active-worker-migration",
                   test_rk3588_rknpu_active_worker_migration);
    qtest_add_func("/rk3588/rknpu-parallel-worker-migration",
                   test_rk3588_rknpu_parallel_worker_migration);
    qtest_add_func("/rk3588/rknpu-rendezvous-wait-migration",
                   test_rk3588_rknpu_rendezvous_wait_migration);
    qtest_add_func("/rk3588/rknpu-wide-state-migration",
                   test_rk3588_rknpu_wide_state_migration);
    qtest_add_func("/rk3588/rknpu-slave-stage-migration",
                   test_rk3588_rknpu_slave_stage_migration);
    qtest_add_func("/rk3588/rknpu-ppu-migration-snapshot",
                   test_rk3588_rknpu_ppu_migration_snapshot);
#endif
    qtest_add_func("/rk3588/rknpu-matmul-reset-pending",
                   test_rk3588_rknpu_matmul_reset_pending);
    qtest_add_func("/rk3588/rknpu-ppu-task-dma-base",
                   test_rk3588_rknpu_ppu_task_dma_base);
    qtest_add_func("/rk3588/rknpu-ppu-right-padding",
                   test_rk3588_rknpu_ppu_right_padding);
    qtest_add_func("/rk3588/rknpu-ppu-identity",
                   test_rk3588_rknpu_ppu_identity);
    qtest_add_func("/rk3588/rknpu-ppu-fp16-identity",
                   test_rk3588_rknpu_ppu_fp16_identity);
    qtest_add_func("/rk3588/rknpu-ppu-fp16-max-pool",
                   test_rk3588_rknpu_ppu_fp16_max_pool);
    qtest_add_func("/rk3588/rknpu-ppu-resource-budget",
                   test_rk3588_rknpu_ppu_resource_budget);
    qtest_add_func("/rk3588/rknpu-ppu-banks-w1c",
                   test_rk3588_rknpu_ppu_banks_w1c);
    qtest_add_func("/rk3588/rknpu-ppu-reset-pending",
                   test_rk3588_rknpu_ppu_reset_pending);
    qtest_add_func("/rk3588/rknpu-ppu-field-semantics",
                   test_rk3588_rknpu_ppu_field_semantics);
    qtest_add_func("/rk3588/rknpu-ppu-source-read-fault",
                   test_rk3588_rknpu_ppu_source_read_fault);
    qtest_add_func("/rk3588/rknpu-ppu-destination-write-fault",
                   test_rk3588_rknpu_ppu_destination_write_fault);
    qtest_add_func("/rk3588/rknpu-reset-state",
                   test_rk3588_rknpu_reset_state);

    return g_test_run();
}
