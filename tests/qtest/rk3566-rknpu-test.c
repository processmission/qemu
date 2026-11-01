/*
 * QTest for the Rockchip RK3566 RKNPU core
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qemu/bitops.h"
#include "exec/hwaddr.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "rk3566-test-common.h"

#define RK3566_RKNN_PC_BASE      0xfde40000ULL
#define RK3566_RKNN_CNA_BASE     0xfde41000ULL
#define RK3566_RKNN_CORE_BASE    0xfde43000ULL
#define RK3566_RKNN_DPU_BASE     0xfde44000ULL
#define RK3566_RKNN_PPU_BASE     0xfde46000ULL
#define RK3566_RKNN_PPU_RDMA_BASE 0xfde47000ULL
#define RK3566_RKNN_GLOBAL_BASE  0xfde4f000ULL
#define RK3566_RKNN_AMOUNT_BASE  0xfde48000ULL
#define RK3566_RKNN_IOMMU_BASE   0xfde4b000ULL
#define RK3566_CRU_BASE          0xfdd20000ULL
#define RK3566_CRU_SOFTRST_CON2  (0x400 + 4 * 2)
#define RK3566_RKNN_SPI          151
#define RK3566_GIC_QOM           "/machine/soc/gic"
#define RK3566_RKNN_QOM          "/machine/soc/npu"

#define RKNN_PC_VERSION           0x0000
#define RKNN_PC_VERSION_NUM       0x0004
#define RKNN_PC_OPERATION_ENABLE  0x0008
#define RKNN_PC_BASE_ADDRESS      0x0010
#define RKNN_PC_REGISTER_AMOUNTS  0x0014
#define RKNN_PC_INTERRUPT_MASK    0x0020
#define RKNN_PC_INTERRUPT_CLEAR   0x0024
#define RKNN_PC_INTERRUPT_STATUS  0x0028
#define RKNN_PC_INTERRUPT_RAW     0x002c
#define RKNN_PC_TASK_CON          0x0030
#define RKNN_PC_TASK_DMA_BASE     0x0034
#define RKNN_PC_TASK_STATUS       0x003c
#define RKNN_GLOBAL_OPERATION_ENABLE 0x0008
#define RKNN_POINTER              0x0004
#define RKNN_OP_EN                0x1
#define RKNN_PC_VERSION_VALUE     0x46495245
#define RKNN_PC_INTERRUPT_VALID   0x0001ffff
#define RKNN_DPU_INTERRUPT_BITS   0x00000300
#define RKNN_TASK_STATUS_SUCCESS  0x0000f000
#define RKNN_TASK_STATUS_FETCH_ERROR 0x0000a000
#define RKNN_COMPLETE_DELAY_NS    (100 * 1000)

#define RKNN_AMOUNT_CLR_ALL       0x8010
#define RKNN_AMOUNT_DT_WR         0x8034
#define RKNN_AMOUNT_DT_RD         0x8038
#define RKNN_AMOUNT_WT_RD         0x803c

#define RKNN_RAM_BASE             0x00200000ULL
#define RKNN_DTE_ADDR             (RKNN_RAM_BASE + 0x10000)
#define RKNN_PTE_ADDR             (RKNN_RAM_BASE + 0x00000)
#define RKNN_CONTROL_REGCMD_ADDR  (RKNN_RAM_BASE + 0x20000)
#define RKNN_REGCMD_ADDR          (RKNN_RAM_BASE + 0x30000)
#define RKNN_INPUT_ADDR           (RKNN_RAM_BASE + 0x40000)
#define RKNN_WEIGHT_ADDR          (RKNN_RAM_BASE + 0x50000)
#define RKNN_OUTPUT0_ADDR         (RKNN_RAM_BASE + 0x60000)
#define RKNN_OUTPUT1_ADDR         (RKNN_RAM_BASE + 0x62000)
#define RKNN_CONTROL_REGCMD_IOVA  0x10000000U
#define RKNN_REGCMD_IOVA          0x10000000U
#define RKNN_INPUT_IOVA           0x10001000U
#define RKNN_WEIGHT_IOVA          0x10002000U
#define RKNN_OUTPUT_IOVA          0x10003800U

#define RK_IOMMU_DTE_ADDR         0x00
#define RK_IOMMU_COMMAND          0x08
#define RK_IOMMU_CMD_ENABLE_PAGING 0
#define RK_IOMMU_PTE_RW           0x7

#define RKNN_MATMUL_M             32
#define RKNN_MATMUL_K             64
#define RKNN_MATMUL_N             32
#define RKNN_MATMUL_COMMANDS      108

#define RKNN_REGCMD(target, value, reg) \
    (cpu_to_le64(((uint64_t)(target) << 48) | \
                 ((uint64_t)(uint32_t)(value) << 16) | (reg)))

static void rk3566_rknn_program_iommu(QTestState *qts, hwaddr dte,
                                      hwaddr pte,
                                      const hwaddr *pages, size_t count)
{
    g_assert_cmpuint(count, <=, 64);

    qtest_writel(qts, dte + 64 * 4, pte | 1);
    for (size_t i = 0; i < count; i++) {
        qtest_writel(qts, pte + i * 4, pages[i] | RK_IOMMU_PTE_RW);
    }
    qtest_writel(qts, RK3566_RKNN_IOMMU_BASE + RK_IOMMU_DTE_ADDR, dte);
    qtest_writel(qts, RK3566_RKNN_IOMMU_BASE + RK_IOMMU_COMMAND,
                 RK_IOMMU_CMD_ENABLE_PAGING);
}

static void rk3566_rknn_start_pc_task(QTestState *qts, uint32_t iova,
                                      uint32_t amount, uint32_t task_count)
{
    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_BASE_ADDRESS, iova);
    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_REGISTER_AMOUNTS, amount);
    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_TASK_CON, task_count);
    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_TASK_DMA_BASE, 0);
    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_INTERRUPT_CLEAR,
                 UINT32_MAX);
    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_OPERATION_ENABLE,
                 RKNN_OP_EN);
}

static void rk3566_rknn_wait_idle(QTestState *qts)
{
    for (unsigned int i = 0; i < 20; i++) {
        if (!(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                          RKNN_PC_OPERATION_ENABLE) & RKNN_OP_EN)) {
            return;
        }
        qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
    }
    g_assert_not_reached();
}

static void rk3566_rknn_prepare_control(QTestState *qts, bool irq_enabled)
{
    uint64_t commands[2] = {
        RKNN_REGCMD(0x0041, 0, 0),
        RKNN_REGCMD(0x0081, 0, RKNN_PC_OPERATION_ENABLE),
    };
    const hwaddr pages[] = { RKNN_CONTROL_REGCMD_ADDR };

    rk3566_rknn_program_iommu(qts, RKNN_DTE_ADDR, RKNN_PTE_ADDR, pages,
                              ARRAY_SIZE(pages));
    qtest_memwrite(qts, RKNN_CONTROL_REGCMD_ADDR, commands, sizeof(commands));
    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_INTERRUPT_MASK,
                 irq_enabled ? RKNN_DPU_INTERRUPT_BITS : 0);
    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_INTERRUPT_CLEAR,
                 UINT32_MAX);
}

static void test_rk3566_rknpu_version(void)
{
    QTestState *qts = rk3566_qtest_start();
    g_autofree char *npu_type =
        rk3566_qom_child_object_type(qts, "/machine/soc", "npu");
    g_autofree char *peer1 =
        rk3566_qom_child_object_type(qts, "/machine/soc", "rknn1");
    g_autofree char *peer2 =
        rk3566_qom_child_object_type(qts, "/machine/soc", "rknn2");

    g_assert_cmpstr(npu_type, ==, "rk3566-rknpu");
    g_assert_null(peer1);
    g_assert_null(peer2);
    g_assert_true(rk3566_qom_object_exists(qts, RK3566_RKNN_QOM));

    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE + RKNN_PC_VERSION),
                    ==, RKNN_PC_VERSION_VALUE);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_VERSION_NUM), ==, 0);

    qtest_quit(qts);
}

static void test_rk3566_rknpu_pointer_tags(void)
{
    QTestState *qts = rk3566_qtest_start();

    qtest_writel(qts, RK3566_RKNN_CNA_BASE + RKNN_POINTER, 0x0001003f);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_CNA_BASE +
                                RKNN_POINTER), ==, 0xf);
    qtest_writel(qts, RK3566_RKNN_CORE_BASE + RKNN_POINTER, 0x00010030);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_CORE_BASE +
                                RKNN_POINTER), ==, 0);
    qtest_writel(qts, RK3566_RKNN_CNA_BASE + RKNN_POINTER, 0xfffeffc0);
    qtest_writel(qts, RK3566_RKNN_CORE_BASE + RKNN_POINTER, 0xfffeffc0);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_CNA_BASE +
                                RKNN_POINTER), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_CORE_BASE +
                                RKNN_POINTER), ==, 0);

    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PPU_BASE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PPU_RDMA_BASE), ==, 0);
    qtest_writel(qts, RK3566_RKNN_PPU_BASE + RKNN_POINTER, UINT32_MAX);
    qtest_writel(qts, RK3566_RKNN_PPU_RDMA_BASE + RKNN_POINTER, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PPU_BASE +
                                RKNN_POINTER), ==, 0x3000000f);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PPU_RDMA_BASE +
                                RKNN_POINTER), ==, 0x3000000f);

    qtest_quit(qts);
}

static void test_rk3566_rknpu_control_complete(void)
{
    QTestState *qts = rk3566_qtest_start();

    qtest_irq_intercept_in(qts, RK3566_GIC_QOM);
    g_assert_false(qtest_get_irq(qts, RK3566_RKNN_SPI));

    rk3566_rknn_prepare_control(qts, true);
    rk3566_rknn_start_pc_task(qts, RKNN_CONTROL_REGCMD_IOVA, 1, 1);
    rk3566_rknn_wait_idle(qts);

    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_SUCCESS);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_INTERRUPT_RAW), ==,
                    RKNN_DPU_INTERRUPT_BITS);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_INTERRUPT_STATUS), ==,
                    RKNN_DPU_INTERRUPT_BITS);
    g_assert_true(qtest_get_irq(qts, RK3566_RKNN_SPI));

    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_INTERRUPT_CLEAR,
                 UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_INTERRUPT_RAW), ==, 0);
    g_assert_false(qtest_get_irq(qts, RK3566_RKNN_SPI));

    qtest_quit(qts);
}

static void test_rk3566_rknpu_irq_mask(void)
{
    QTestState *qts = rk3566_qtest_start();

    qtest_irq_intercept_in(qts, RK3566_GIC_QOM);
    rk3566_rknn_prepare_control(qts, false);
    rk3566_rknn_start_pc_task(qts, RKNN_CONTROL_REGCMD_IOVA, 1, 1);
    rk3566_rknn_wait_idle(qts);

    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_INTERRUPT_RAW), ==,
                    RKNN_DPU_INTERRUPT_BITS);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_INTERRUPT_STATUS), ==, 0);
    g_assert_false(qtest_get_irq(qts, RK3566_RKNN_SPI));

    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_INTERRUPT_MASK,
                 RKNN_DPU_INTERRUPT_BITS);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_INTERRUPT_STATUS), ==,
                    RKNN_DPU_INTERRUPT_BITS);
    g_assert_true(qtest_get_irq(qts, RK3566_RKNN_SPI));

    qtest_quit(qts);
}

static void test_rk3566_rknpu_fetch_error(void)
{
    QTestState *qts = rk3566_qtest_start();

    qtest_irq_intercept_in(qts, RK3566_GIC_QOM);
    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_INTERRUPT_MASK,
                 RKNN_DPU_INTERRUPT_BITS);
    rk3566_rknn_start_pc_task(qts, RKNN_CONTROL_REGCMD_IOVA, 1, 1);
    rk3566_rknn_wait_idle(qts);

    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_TASK_STATUS), ==,
                    RKNN_TASK_STATUS_FETCH_ERROR);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_INTERRUPT_RAW), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_INTERRUPT_STATUS), ==, 0);
    g_assert_false(qtest_get_irq(qts, RK3566_RKNN_SPI));

    qtest_quit(qts);
}

static void test_rk3566_rknpu_dma_32bit(void)
{
    QTestState *qts = rk3566_qtest_start();
    /*
     * A page table entry pointing above the 32-bit DMA limit must be
     * rejected instead of touching host memory.
     */
    const hwaddr pages[] = { 0x100002000ULL };

    rk3566_rknn_program_iommu(qts, RKNN_DTE_ADDR, RKNN_PTE_ADDR, pages,
                              ARRAY_SIZE(pages));
    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_INTERRUPT_MASK,
                 RKNN_DPU_INTERRUPT_BITS);
    rk3566_rknn_start_pc_task(qts, RKNN_CONTROL_REGCMD_IOVA, 1, 1);
    rk3566_rknn_wait_idle(qts);

    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_TASK_STATUS) & 0xf000, ==,
                    RKNN_TASK_STATUS_FETCH_ERROR);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_OPERATION_ENABLE) & RKNN_OP_EN, ==, 0);

    qtest_quit(qts);
}

static void test_rk3566_rknpu_amount_window(void)
{
    QTestState *qts = rk3566_qtest_start();

    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_AMOUNT_BASE +
                                (RKNN_AMOUNT_DT_WR - 0x8000)), ==, 0);
    qtest_writel(qts, RK3566_RKNN_AMOUNT_BASE + (RKNN_AMOUNT_DT_WR - 0x8000),
                 0x11223344);
    qtest_writel(qts, RK3566_RKNN_AMOUNT_BASE + (RKNN_AMOUNT_DT_RD - 0x8000),
                 0x55667788);
    qtest_writel(qts, RK3566_RKNN_AMOUNT_BASE + (RKNN_AMOUNT_WT_RD - 0x8000),
                 0x00000042);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_AMOUNT_BASE +
                                (RKNN_AMOUNT_DT_WR - 0x8000)), ==,
                    0x11223344);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_AMOUNT_BASE +
                                (RKNN_AMOUNT_DT_RD - 0x8000)), ==,
                    0x55667788);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_AMOUNT_BASE +
                                (RKNN_AMOUNT_WT_RD - 0x8000)), ==, 0x42);

    /* The vendor driver clears with 0x80000101 followed by 0x00000101. */
    qtest_writel(qts, RK3566_RKNN_AMOUNT_BASE + (RKNN_AMOUNT_CLR_ALL - 0x8000),
                 0x80000101);
    qtest_writel(qts, RK3566_RKNN_AMOUNT_BASE + (RKNN_AMOUNT_CLR_ALL - 0x8000),
                 0x00000101);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_AMOUNT_BASE +
                                (RKNN_AMOUNT_DT_WR - 0x8000)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_AMOUNT_BASE +
                                (RKNN_AMOUNT_DT_RD - 0x8000)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_AMOUNT_BASE +
                                (RKNN_AMOUNT_WT_RD - 0x8000)), ==, 0);

    /* A write without the bit 31 strobe leaves the counters alone. */
    qtest_writel(qts, RK3566_RKNN_AMOUNT_BASE + (RKNN_AMOUNT_DT_WR - 0x8000),
                 0x55);
    qtest_writel(qts, RK3566_RKNN_AMOUNT_BASE + (RKNN_AMOUNT_CLR_ALL - 0x8000),
                 0x00000101);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_AMOUNT_BASE +
                                (RKNN_AMOUNT_DT_WR - 0x8000)), ==, 0x55);

    qtest_quit(qts);
}

static void test_rk3566_rknpu_reset(void)
{
    QTestState *qts = rk3566_qtest_start();
    const uint32_t assert_mask = BIT(11) | BIT(12);

    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_BASE_ADDRESS, 0x12340);
    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_REGISTER_AMOUNTS, 0x55);
    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_INTERRUPT_MASK, 0);
    qtest_writel(qts, RK3566_RKNN_AMOUNT_BASE + (RKNN_AMOUNT_DT_RD - 0x8000),
                 0x1234);

    /*
     * SRST_A_NPU/SRST_H_NPU live in SOFTRST_CON2 bits 11/12 and use the
     * Rockchip high-word write mask.
     */
    qtest_writel(qts, RK3566_CRU_BASE + RK3566_CRU_SOFTRST_CON2,
                 (assert_mask << 16) | assert_mask);
    qtest_writel(qts, RK3566_CRU_BASE + RK3566_CRU_SOFTRST_CON2,
                 assert_mask << 16);

    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_BASE_ADDRESS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_REGISTER_AMOUNTS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_INTERRUPT_MASK), ==,
                    RKNN_PC_INTERRUPT_VALID);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE + RKNN_PC_VERSION),
                    ==, RKNN_PC_VERSION_VALUE);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_AMOUNT_BASE +
                                (RKNN_AMOUNT_DT_RD - 0x8000)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_OPERATION_ENABLE) & RKNN_OP_EN, ==, 0);

    qtest_quit(qts);
}

static size_t rk3566_rknn_register_amount(size_t command_count)
{
    g_assert_cmpuint(command_count, >, 0);
    return command_count - 1;
}

static void rk3566_rknn_make_matmul_regcmd(uint64_t commands[],
                                           uint32_t co_work_mode)
{
    const uint32_t cna = 0x0201;
    const uint32_t core = 0x0801;
    const uint32_t dpu = 0x1001;
    unsigned int count = 0;

#define EMIT(_target, _reg, _value) \
    commands[count++] = RKNN_REGCMD(_target, _value, _reg)
    EMIT(dpu, 0x4004, 0x0e);
    EMIT(cna, 0x100c, 0);
    EMIT(cna, 0x1010, ((RKNN_MATMUL_M + 1) << 4) | (co_work_mode << 28));
    EMIT(cna, 0x1014, 0x09);
    EMIT(cna, 0x1020, (1 << 16) | RKNN_MATMUL_M);
    EMIT(cna, 0x1024, ((RKNN_MATMUL_K - 1) << 16) | RKNN_MATMUL_K);
    EMIT(cna, 0x1028, 1);
    EMIT(cna, 0x102c, RKNN_MATMUL_M);
    EMIT(cna, 0x1030, RKNN_MATMUL_N * RKNN_MATMUL_K);
    EMIT(cna, 0x1034, RKNN_MATMUL_K);
    EMIT(cna, 0x1038, (1 << 24) | (1 << 16) | RKNN_MATMUL_N);
    EMIT(cna, 0x1040, (11 << 4) | 1);
    EMIT(cna, 0x1044, 1);
    EMIT(cna, 0x104c, 0x0b);
    for (uint32_t reg = 0x1050; reg <= 0x105c; reg += 4) {
        EMIT(cna, reg, 1 << 16);
    }
    EMIT(cna, 0x1060, 0);
    EMIT(cna, 0x1064, 0);
    EMIT(cna, 0x1068, 0);
    EMIT(cna, 0x1070, RKNN_INPUT_IOVA);
    EMIT(cna, 0x1074, 0);
    EMIT(cna, 0x1078, 0x000f000f);
    EMIT(cna, 0x107c, 4);
    EMIT(cna, 0x1080, 28);
    EMIT(cna, 0x1084, (1 << 16) | RKNN_MATMUL_M);
    EMIT(cna, 0x1088, RKNN_MATMUL_K);
    EMIT(cna, 0x1100, 0);
    EMIT(cna, 0x1104, 0);
    EMIT(cna, 0x1110, RKNN_WEIGHT_IOVA);
    for (uint32_t reg = 0x1140; reg < 0x1180; reg += 4) {
        EMIT(cna, reg, 0);
    }
    EMIT(cna, 0x1180, 0);
    EMIT(cna, 0x1184, 0);

    EMIT(core, 0x3010, 0);
    EMIT(core, 0x3014, (RKNN_MATMUL_M - 1) << 16);
    EMIT(core, 0x3018, RKNN_MATMUL_N - 1);
    EMIT(core, 0x301c, 0);
    EMIT(core, 0x3030, 0);

    EMIT(dpu, 0x400c, 0x1e4);
    EMIT(dpu, 0x4010, 4 << 29);
    EMIT(dpu, 0x4014, 0);
    EMIT(dpu, 0x4020, RKNN_OUTPUT_IOVA);
    EMIT(dpu, 0x4024, RKNN_MATMUL_M << 4);
    EMIT(dpu, 0x4030, 0);
    EMIT(dpu, 0x4034, RKNN_MATMUL_M - 1);
    EMIT(dpu, 0x4038, 0);
    EMIT(dpu, 0x403c, ((RKNN_MATMUL_N - 1) << 16) |
         (RKNN_MATMUL_N - 1));
    EMIT(dpu, 0x4040, 0x53);
    EMIT(dpu, 0x4044, 0);
    EMIT(dpu, 0x4048, 0);
    EMIT(dpu, 0x404c, 0);
    EMIT(dpu, 0x4050, 0x7fe);
    EMIT(dpu, 0x4054, 0);
    EMIT(dpu, 0x4058, RKNN_MATMUL_N - 1);
    EMIT(dpu, 0x405c, (RKNN_MATMUL_M - 1) << 16);
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
    EMIT(dpu, 0x40c0, (RKNN_MATMUL_M * 8) << 4);
    EMIT(dpu, 0x40c4, 0);
    for (uint32_t reg = 0x4100; reg < 0x4130; reg += 4) {
        EMIT(dpu, reg, 0);
    }

    commands[count++] = 0;
    EMIT(0x0101, 0x0014, 0);
    EMIT(0x0041, 0, 0);
    EMIT(0x0081, 0x0008, 0x0d);
#undef EMIT
    g_assert_cmpuint(count, ==, RKNN_MATMUL_COMMANDS);
}

static void test_rk3566_rknpu_single_core_cowork(void)
{
    QTestState *qts = rk3566_qtest_start();
    uint64_t commands[RKNN_MATMUL_COMMANDS];
    const hwaddr pages[] = {
        RKNN_REGCMD_ADDR, RKNN_INPUT_ADDR, RKNN_WEIGHT_ADDR,
        RKNN_OUTPUT0_ADDR, RKNN_OUTPUT1_ADDR,
    };
    uint32_t status = 0;

    /*
     * Ask for a three-core co-work mode: a single-core RK3566 must not
     * wait for peers that do not exist.
     */
    rk3566_rknn_make_matmul_regcmd(commands, 2);
    rk3566_rknn_program_iommu(qts, RKNN_DTE_ADDR, RKNN_PTE_ADDR, pages,
                              ARRAY_SIZE(pages));
    qtest_memwrite(qts, RKNN_REGCMD_ADDR, commands, sizeof(commands));
    qtest_writel(qts, RK3566_RKNN_PC_BASE + RKNN_PC_INTERRUPT_MASK,
                 RKNN_DPU_INTERRUPT_BITS);
    rk3566_rknn_start_pc_task(qts, RKNN_REGCMD_IOVA,
                              rk3566_rknn_register_amount(
                                  ARRAY_SIZE(commands)), 1);

    for (unsigned int i = 0; i < 30 && !status; i++) {
        qtest_clock_step(qts, RKNN_COMPLETE_DELAY_NS);
        status = qtest_readl(qts, RK3566_RKNN_PC_BASE +
                             RKNN_PC_TASK_STATUS);
    }

    /* Any completion code proves the task did not wait for a peer. */
    g_assert_cmphex(status & 0xf000, !=, 0);
    g_assert_cmphex(qtest_readl(qts, RK3566_RKNN_PC_BASE +
                                RKNN_PC_OPERATION_ENABLE) & RKNN_OP_EN, ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566/rknpu/version", test_rk3566_rknpu_version);
    qtest_add_func("/rk3566/rknpu/pointer-tags",
                   test_rk3566_rknpu_pointer_tags);
    qtest_add_func("/rk3566/rknpu/control-complete",
                   test_rk3566_rknpu_control_complete);
    qtest_add_func("/rk3566/rknpu/irq-mask", test_rk3566_rknpu_irq_mask);
    qtest_add_func("/rk3566/rknpu/fetch-error",
                   test_rk3566_rknpu_fetch_error);
    qtest_add_func("/rk3566/rknpu/dma-32bit", test_rk3566_rknpu_dma_32bit);
    qtest_add_func("/rk3566/rknpu/amount-window",
                   test_rk3566_rknpu_amount_window);
    qtest_add_func("/rk3566/rknpu/reset", test_rk3566_rknpu_reset);
    qtest_add_func("/rk3566/rknpu/single-core-cowork",
                   test_rk3566_rknpu_single_core_cowork);

    return g_test_run();
}
