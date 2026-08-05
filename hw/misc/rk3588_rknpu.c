/*
 * Rockchip RK3588 RKNPU core
 *
 * The functional backend decodes RK3588 register command streams and executes
 * the hardware modes modeled below. Board captures are regression evidence;
 * they do not determine which tasks the device accepts.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "block/thread-pool.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "hw/misc/rk3588_rknpu.h"
#include "hw/misc/rockchip_iommu.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/int128.h"
#include "qemu/aio-wait.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "fpu/softfloat.h"
#include "system/dma.h"
#include "system/runstate.h"
#include "trace/control.h"
#include "trace.h"

REG32(PC_VERSION, 0x0000)
REG32(PC_VERSION_NUM, 0x0004)
REG32(PC_OPERATION_ENABLE, 0x0008)
    FIELD(PC_OPERATION_ENABLE, OP_EN, 0, 1)
REG32(PC_BASE_ADDRESS, 0x0010)
REG32(PC_REGISTER_AMOUNTS, 0x0014)
REG32(PC_INTERRUPT_MASK, 0x0020)
REG32(PC_INTERRUPT_CLEAR, 0x0024)
REG32(PC_INTERRUPT_STATUS, 0x0028)
REG32(PC_INTERRUPT_RAW_STATUS, 0x002c)
REG32(PC_TASK_CON, 0x0030)
REG32(PC_TASK_DMA_BASE_ADDR, 0x0034)
REG32(PC_TASK_STATUS, 0x003c)

REG32(GLOBAL_OPERATION_ENABLE, 0x0008)

REG32(CNA_S_POINTER, 0x0004)
REG32(CORE_S_POINTER, 0x0004)
REG32(PPU_S_STATUS, 0x0000)
REG32(PPU_S_POINTER, 0x0004)
REG32(PPU_RDMA_S_STATUS, 0x0000)
REG32(PPU_RDMA_S_POINTER, 0x0004)

#define ROCKCHIP_RKNN_PC_VERSION 0x46495245
#define ROCKCHIP_RKNN_PC_VERSION_NUM 0x00000000
#define ROCKCHIP_RKNN_DPU_INTERRUPT_BITS 0x00000300
#define ROCKCHIP_RKNN_PIPELINE_BANK0_INTERRUPT 0x000002aa
#define ROCKCHIP_RKNN_PIPELINE_BANK1_INTERRUPT 0x00000155
#define ROCKCHIP_RKNN_INTERRUPT_VALID_BITS 0x0001ffff
#define ROCKCHIP_RKNN_INTERRUPT_RESERVED_BITS 0xfffe0000
/*
 * Orange Pi 5 Plus PPU and depthwise captures show RAW_STATUS[31:30] set
 * after the production 0x1ffff interrupt acknowledgement.  Preserve this
 * board-observed sticky stage status, but exclude it from masked interrupt
 * status and treat its reserved CLEAR fields as reset-only.
 */
#define ROCKCHIP_RKNN_STAGE_RAW_STATUS_BITS 0xc0000000
#define ROCKCHIP_RKNN_PPU_BANK0_INTERRUPT BIT(10)
#define ROCKCHIP_RKNN_PPU_BANK1_INTERRUPT BIT(11)
#define ROCKCHIP_RKNN_PPU_STATUS_SUCCESS 0x0000000c
#define ROCKCHIP_RKNN_PPU_STATUS_FAULT 0x00000005
#define ROCKCHIP_RKNN_DMA_READ_ERROR 0x00001000
#define ROCKCHIP_RKNN_DMA_WRITE_ERROR 0x00002000
#define ROCKCHIP_RKNN_TASK_STATUS_SUCCESS 0x0000f000
#define ROCKCHIP_RKNN_PPU_TASK_STATUS 0x0000f000
#define ROCKCHIP_RKNN_TASK_STATUS_FETCH_ERROR 0x0000a000
#define ROCKCHIP_RKNN_COMPLETE_DELAY_NS (100 * 1000)
#define ROCKCHIP_RKNN_REGCMD_SUMMARY_BYTES 16
#define ROCKCHIP_RKNN_REGCMD_SAMPLE_COMMANDS_MAX 256
#define ROCKCHIP_RKNN_REGCMD_SAMPLE_BYTES_MAX \
    (ROCKCHIP_RKNN_REGCMD_SAMPLE_COMMANDS_MAX * sizeof(uint64_t))
#define ROCKCHIP_RKNN_DMA_CHUNK_SIZE (64 * KiB)
#define ROCKCHIP_RKNN_PC_BASE_ADDRESS_MASK 0xfffffff0U
#define ROCKCHIP_RKNN_PC_SLAVE_MODE BIT(0)
#define ROCKCHIP_RKNN_PC_REGISTER_AMOUNTS_MASK 0x0000ffffU
#define ROCKCHIP_RKNN_REGCMD_TARGET_PC 0x0101
#define ROCKCHIP_RKNN_REGCMD_TARGET_CNA 0x0201
#define ROCKCHIP_RKNN_REGCMD_TARGET_CORE 0x0801
#define ROCKCHIP_RKNN_REGCMD_TARGET_DPU 0x1001
#define ROCKCHIP_RKNN_REGCMD_TARGET_DPU_RDMA 0x2001
#define ROCKCHIP_RKNN_REGCMD_TARGET_PPU 0x4001
#define ROCKCHIP_RKNN_REGCMD_TARGET_PPU_RDMA 0x8001
#define ROCKCHIP_RKNN_REGCMD_CNA_BASE 0x1000
#define ROCKCHIP_RKNN_REGCMD_CORE_BASE 0x3000
#define ROCKCHIP_RKNN_REGCMD_DPU_BASE 0x4000
#define ROCKCHIP_RKNN_REGCMD_DPU_RDMA_BASE 0x5000
#define ROCKCHIP_RKNN_REGCMD_PPU_BASE 0x6000
#define ROCKCHIP_RKNN_REGCMD_PPU_RDMA_BASE 0x7000
#define ROCKCHIP_RKNN_CNA_DATA_SIZE0 0x020
#define ROCKCHIP_RKNN_CNA_DATA_SIZE1 0x024
#define ROCKCHIP_RKNN_CNA_FEATURE_DATA_ADDR 0x070
#define ROCKCHIP_RKNN_CNA_DMA_CON1 0x07c
#define ROCKCHIP_RKNN_CNA_DMA_CON2 0x080
#define ROCKCHIP_RKNN_CNA_FC_DATA_SIZE0 0x084
#define ROCKCHIP_RKNN_CNA_FC_DATA_SIZE1 0x088
#define ROCKCHIP_RKNN_CNA_DCOMP_ADDR0 0x110
#define ROCKCHIP_RKNN_CORE_MISC_CFG 0x010
#define ROCKCHIP_RKNN_CORE_DATAOUT_SIZE_0 0x014
#define ROCKCHIP_RKNN_CORE_DATAOUT_SIZE_1 0x018
#define ROCKCHIP_RKNN_CORE_CLIP_TRUNCATE 0x01c
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_CFG 0x00c
#define ROCKCHIP_RKNN_DPU_DATA_FORMAT 0x010
#define ROCKCHIP_RKNN_DPU_OFFSET_PEND 0x014
#define ROCKCHIP_RKNN_DPU_DST_BASE_ADDR 0x020
#define ROCKCHIP_RKNN_DPU_DST_SURF_STRIDE 0x024
#define ROCKCHIP_RKNN_DPU_DATA_CUBE_WIDTH 0x030
#define ROCKCHIP_RKNN_DPU_DATA_CUBE_HEIGHT 0x034
#define ROCKCHIP_RKNN_DPU_DATA_CUBE_NOTCH_ADDR 0x038
#define ROCKCHIP_RKNN_DPU_DATA_CUBE_CHANNEL 0x03c
#define ROCKCHIP_RKNN_DPU_BS_OW_CFG 0x050
#define ROCKCHIP_RKNN_DPU_BS_OW_OP 0x054
#define ROCKCHIP_RKNN_DPU_WDMA_SIZE_0 0x058
#define ROCKCHIP_RKNN_DPU_WDMA_SIZE_1 0x05c
#define ROCKCHIP_RKNN_DPU_BS_OW_CFG_CONV 0x125
#define ROCKCHIP_RKNN_DPU_BS_OW_CFG_CONV_NO_CPEND 0x124
#define ROCKCHIP_RKNN_DPU_BS_OW_CFG_DEPTHWISE 0x36d
#define ROCKCHIP_RKNN_DPU_BS_OW_CFG_DEPTHWISE_NO_CPEND 0x36c
#define ROCKCHIP_RKNN_DPU_BS_OW_CFG_DEPTHWISE_FP16_GROUP 0x36e
#define ROCKCHIP_RKNN_DPU_BS_OW_CFG_RDMA 0x126
#define ROCKCHIP_RKNN_DPU_BS_OW_OP_SUPPORTED 0
#define ROCKCHIP_RKNN_DPU_RDMA_DATA_CUBE_WIDTH 0x00c
#define ROCKCHIP_RKNN_DPU_RDMA_DATA_CUBE_HEIGHT 0x010
#define ROCKCHIP_RKNN_DPU_RDMA_DATA_CUBE_CHANNEL 0x014
#define ROCKCHIP_RKNN_DPU_RDMA_SRC_BASE_ADDR 0x018
#define ROCKCHIP_RKNN_DPU_RDMA_BRDMA_CFG 0x01c
#define ROCKCHIP_RKNN_DPU_RDMA_BS_BASE_ADDR 0x020
#define ROCKCHIP_RKNN_DPU_RDMA_NRDMA_CFG 0x028
#define ROCKCHIP_RKNN_DPU_RDMA_BN_BASE_ADDR 0x02c
#define ROCKCHIP_RKNN_DPU_RDMA_ERDMA_CFG 0x034
#define ROCKCHIP_RKNN_DPU_RDMA_EW_BASE_ADDR 0x038
#define ROCKCHIP_RKNN_DPU_RDMA_EW_SURF_STRIDE 0x040
#define ROCKCHIP_RKNN_DPU_RDMA_FEATURE_MODE_CFG 0x044
#define ROCKCHIP_RKNN_DPU_RDMA_SRC_DMA_CFG 0x048
#define ROCKCHIP_RKNN_DPU_RDMA_SURF_NOTCH 0x04c
#define ROCKCHIP_RKNN_DPU_RDMA_PAD_CFG 0x064
#define ROCKCHIP_RKNN_DPU_RDMA_WEIGHT 0x068
#define ROCKCHIP_RKNN_DPU_RDMA_EW_SURF_NOTCH 0x06c
#define ROCKCHIP_RKNN_PPU_IN_WIDTH 0x00c
#define ROCKCHIP_RKNN_PPU_IN_HEIGHT 0x010
#define ROCKCHIP_RKNN_PPU_IN_CHANNEL 0x014
#define ROCKCHIP_RKNN_PPU_OUT_WIDTH 0x018
#define ROCKCHIP_RKNN_PPU_OUT_HEIGHT 0x01c
#define ROCKCHIP_RKNN_PPU_OUT_CHANNEL 0x020
#define ROCKCHIP_RKNN_PPU_MODE 0x024
#define ROCKCHIP_RKNN_PPU_KERNEL 0x034
#define ROCKCHIP_RKNN_PPU_RECIPROCAL_WIDTH 0x038
#define ROCKCHIP_RKNN_PPU_RECIPROCAL_HEIGHT 0x03c
#define ROCKCHIP_RKNN_PPU_PADDING 0x040
#define ROCKCHIP_RKNN_PPU_PADDING_VALUE_0 0x044
#define ROCKCHIP_RKNN_PPU_PADDING_VALUE_1 0x048
#define ROCKCHIP_RKNN_PPU_DST_BASE 0x070
#define ROCKCHIP_RKNN_PPU_DST_STRIDE 0x07c
#define ROCKCHIP_RKNN_PPU_DATA_FORMAT 0x084
#define ROCKCHIP_RKNN_PPU_MISC_CTRL 0x0dc
#define ROCKCHIP_RKNN_PPU_RDMA_IN_WIDTH 0x00c
#define ROCKCHIP_RKNN_PPU_RDMA_IN_HEIGHT 0x010
#define ROCKCHIP_RKNN_PPU_RDMA_IN_CHANNEL 0x014
#define ROCKCHIP_RKNN_PPU_RDMA_SRC_BASE 0x01c
#define ROCKCHIP_RKNN_PPU_RDMA_LINE_STRIDE 0x024
#define ROCKCHIP_RKNN_PPU_RDMA_SURF_STRIDE 0x028
#define ROCKCHIP_RKNN_PPU_RDMA_DATA_FORMAT 0x030
#define ROCKCHIP_RKNN_PPU_POOLING_METHOD_SHIFT 0
#define ROCKCHIP_RKNN_PPU_POOLING_METHOD_LENGTH 2
#define ROCKCHIP_RKNN_PPU_FLYING_MODE BIT(4)
#define ROCKCHIP_RKNN_PPU_INDEX_ENABLE BIT(30)
#define ROCKCHIP_RKNN_PPU_KERNEL_WIDTH_SHIFT 0
#define ROCKCHIP_RKNN_PPU_KERNEL_HEIGHT_SHIFT 8
#define ROCKCHIP_RKNN_PPU_KERNEL_STRIDE_WIDTH_SHIFT 16
#define ROCKCHIP_RKNN_PPU_KERNEL_STRIDE_HEIGHT_SHIFT 20
#define ROCKCHIP_RKNN_PPU_KERNEL_FIELD_LENGTH 4
#define ROCKCHIP_RKNN_TASK_NUMBER_MASK 0x00000fffU
#define ROCKCHIP_RKNN_REGISTER_WORDS (ROCKCHIP_RKNN_WINDOW_SIZE / 4)
#define ROCKCHIP_RKNN_PRESENT_WORDS \
    DIV_ROUND_UP(ROCKCHIP_RKNN_REGISTER_WORDS, 32)
#define ROCKCHIP_RKNN_REGCMD_PRE_ENABLE 0x0041
#define ROCKCHIP_RKNN_REGCMD_BLOCK_ENABLE 0x0081
#define ROCKCHIP_RKNN_BLOCK_CNA BIT(0)
#define ROCKCHIP_RKNN_BLOCK_CORE BIT(2)
#define ROCKCHIP_RKNN_BLOCK_DPU BIT(3)
#define ROCKCHIP_RKNN_BLOCK_DPU_RDMA BIT(4)
#define ROCKCHIP_RKNN_BLOCK_PPU BIT(5)
#define ROCKCHIP_RKNN_BLOCK_PPU_RDMA BIT(6)
#define ROCKCHIP_RKNN_CNA_CONV_CON1 0x00c
#define ROCKCHIP_RKNN_CNA_CONV_CON2 0x010
#define ROCKCHIP_RKNN_CNA_CONV_CON3 0x014
#define ROCKCHIP_RKNN_CNA_DATA_SIZE2 0x028
#define ROCKCHIP_RKNN_CNA_DATA_SIZE3 0x02c
#define ROCKCHIP_RKNN_CNA_WEIGHT_SIZE0 0x030
#define ROCKCHIP_RKNN_CNA_WEIGHT_SIZE1 0x034
#define ROCKCHIP_RKNN_CNA_WEIGHT_SIZE2 0x038
#define ROCKCHIP_RKNN_CNA_CVT_CON0 0x04c
#define ROCKCHIP_RKNN_CNA_CVT_CON1 0x050
#define ROCKCHIP_RKNN_CNA_CVT_CON5 0x180
#define ROCKCHIP_RKNN_CNA_PAD_CON0 0x068
#define ROCKCHIP_RKNN_CNA_PAD_CON1 0x184
#define ROCKCHIP_RKNN_CNA_CVT_BYPASS BIT(0)
#define ROCKCHIP_RKNN_CNA_CVT_TYPE BIT(1)
#define ROCKCHIP_RKNN_CNA_CVT_ROUND_TYPE BIT(2)
#define ROCKCHIP_RKNN_CNA_CVT_DATA_SIGN BIT(3)
#define ROCKCHIP_RKNN_CNA_RGB888_FORMAT 10
#define ROCKCHIP_RKNN_DPU_BS_CFG 0x040
#define ROCKCHIP_RKNN_DPU_BS_ALU_CFG 0x044
#define ROCKCHIP_RKNN_DPU_BS_MUL_CFG 0x048
#define ROCKCHIP_RKNN_DPU_BS_RELUX_CMP_VALUE 0x04c
#define ROCKCHIP_RKNN_DPU_DST_DMA_CFG 0x050
#define ROCKCHIP_RKNN_DPU_BN_CFG 0x060
#define ROCKCHIP_RKNN_DPU_BN_ALU_CFG 0x064
#define ROCKCHIP_RKNN_DPU_BN_MUL_CFG 0x068
#define ROCKCHIP_RKNN_DPU_BN_RELUX_CMP_VALUE 0x06c
#define ROCKCHIP_RKNN_DPU_EW_CFG 0x070
#define ROCKCHIP_RKNN_DPU_EW_CVT_OFFSET_VALUE 0x074
#define ROCKCHIP_RKNN_DPU_EW_CVT_SCALE_VALUE 0x078
#define ROCKCHIP_RKNN_DPU_EW_RELUX_CMP_VALUE 0x07c
#define ROCKCHIP_RKNN_DPU_OUT_CVT_OFFSET 0x080
#define ROCKCHIP_RKNN_DPU_OUT_CVT_SCALE 0x084
#define ROCKCHIP_RKNN_DPU_OUT_CVT_SHIFT 0x088
#define ROCKCHIP_RKNN_DPU_EW_OP_VALUE_0 0x090
#define ROCKCHIP_RKNN_DPU_SURFACE_ADD 0x0c0
#define ROCKCHIP_RKNN_DPU_LUT_ACCESS_CFG 0x100
#define ROCKCHIP_RKNN_DPU_LUT_ACCESS_DATA 0x104
#define ROCKCHIP_RKNN_DPU_LUT_CFG 0x108
#define ROCKCHIP_RKNN_DPU_LUT_INFO 0x10c
#define ROCKCHIP_RKNN_DPU_LUT_LE_START 0x110
#define ROCKCHIP_RKNN_DPU_LUT_LE_END 0x114
#define ROCKCHIP_RKNN_DPU_LUT_LO_START 0x118
#define ROCKCHIP_RKNN_DPU_LUT_LO_END 0x11c
#define ROCKCHIP_RKNN_DPU_LUT_LE_SLOPE_SCALE 0x120
#define ROCKCHIP_RKNN_DPU_LUT_LE_SLOPE_SHIFT 0x124
#define ROCKCHIP_RKNN_DPU_LUT_LO_SLOPE_SCALE 0x128
#define ROCKCHIP_RKNN_DPU_LUT_LO_SLOPE_SHIFT 0x12c
#define ROCKCHIP_RKNN_LUT_ENTRIES 513
#define ROCKCHIP_RKNN_LUT_VENDOR_LO_OFLOW_SCALE 0x40320000
#define ROCKCHIP_RKNN_LUT_VENDOR_LO_OFLOW_SHIFT 0x000001a0
#define ROCKCHIP_RKNN_DPU_STAGE_BYPASS BIT(0)
#define ROCKCHIP_RKNN_DPU_STAGE_ALU_BYPASS BIT(1)
#define ROCKCHIP_RKNN_DPU_STAGE_MUL_BYPASS BIT(4)
#define ROCKCHIP_RKNN_DPU_STAGE_MUL_PRELU BIT(5)
#define ROCKCHIP_RKNN_DPU_STAGE_RELU_BYPASS BIT(6)
#define ROCKCHIP_RKNN_DPU_STAGE_RELUX_ENABLE BIT(7)
#define ROCKCHIP_RKNN_DPU_STAGE_ALU_SOURCE BIT(8)
#define ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_SHIFT 16
#define ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_LENGTH 4
#define ROCKCHIP_RKNN_DPU_MUL_SOURCE BIT(0)
#define ROCKCHIP_RKNN_DPU_MUL_TRUNCATE_SOURCE BIT(1)
#define ROCKCHIP_RKNN_DPU_MUL_SHIFT_SHIFT 8
#define ROCKCHIP_RKNN_DPU_MUL_SHIFT_LENGTH 6
#define ROCKCHIP_RKNN_DPU_MUL_OPERAND_SHIFT 16
#define ROCKCHIP_RKNN_DPU_EW_BYPASS BIT(0)
#define ROCKCHIP_RKNN_DPU_EW_OP_BYPASS BIT(1)
#define ROCKCHIP_RKNN_DPU_EW_OP_TYPE BIT(2)
#define ROCKCHIP_RKNN_DPU_EW_MUL_PRELU BIT(5)
#define ROCKCHIP_RKNN_DPU_EW_OP_SOURCE BIT(6)
#define ROCKCHIP_RKNN_DPU_EW_LUT_BYPASS BIT(7)
#define ROCKCHIP_RKNN_DPU_EW_OP_CVT_BYPASS BIT(8)
#define ROCKCHIP_RKNN_DPU_EW_RELU_BYPASS BIT(9)
#define ROCKCHIP_RKNN_DPU_EW_RELUX_ENABLE BIT(10)
#define ROCKCHIP_RKNN_DPU_EW_ALU_ALGO_SHIFT 16
#define ROCKCHIP_RKNN_DPU_EW_ALU_ALGO_LENGTH 4
#define ROCKCHIP_RKNN_DPU_EW_BINARY_ENABLE BIT(20)
#define ROCKCHIP_RKNN_DPU_EW_EQUAL_ENABLE BIT(21)
#define ROCKCHIP_RKNN_DPU_EW_DATA_SIZE_SHIFT 22
#define ROCKCHIP_RKNN_DPU_EW_DATA_SIZE_LENGTH 2
#define ROCKCHIP_RKNN_DPU_EW_DATA_MODE_SHIFT 28
#define ROCKCHIP_RKNN_DPU_EW_DATA_MODE_LENGTH 2
#define ROCKCHIP_RKNN_DPU_EW_CVT_ROUND BIT(30)
#define ROCKCHIP_RKNN_DPU_EW_CVT_TYPE BIT(31)
#define ROCKCHIP_RKNN_DPU_BRDMA_DATA_USE_SHIFT 1
#define ROCKCHIP_RKNN_DPU_BRDMA_DATA_USE_LENGTH 4
#define ROCKCHIP_RKNN_DPU_ERDMA_DATA_SIZE_SHIFT 2
#define ROCKCHIP_RKNN_DPU_ERDMA_DATA_SIZE_LENGTH 2
#define ROCKCHIP_RKNN_DPU_ERDMA_DATA_MODE_SHIFT 30
#define ROCKCHIP_RKNN_DPU_ERDMA_DATA_MODE_LENGTH 2
#define ROCKCHIP_RKNN_DPU_ERDMA_DISABLE BIT(0)
#define ROCKCHIP_RKNN_DPU_RDMA_FEATURE_FLYING_MODE BIT(0)
#define ROCKCHIP_RKNN_DPU_RDMA_FEATURE_CONV_MODE_SHIFT 1
#define ROCKCHIP_RKNN_DPU_RDMA_FEATURE_CONV_MODE_LENGTH 2
#define ROCKCHIP_RKNN_DPU_RDMA_FEATURE_FP16_TO_FP32 BIT(3)
#define ROCKCHIP_RKNN_DPU_RDMA_FEATURE_MAIN_DISABLE BIT(4)
#define ROCKCHIP_RKNN_DPU_RDMA_FEATURE_PROCESS_PRECISION_SHIFT 5
#define ROCKCHIP_RKNN_DPU_RDMA_FEATURE_PROCESS_PRECISION_LENGTH 3
#define ROCKCHIP_RKNN_DPU_RDMA_FEATURE_COMBINATION_SHIFT 8
#define ROCKCHIP_RKNN_DPU_RDMA_FEATURE_COMBINATION_LENGTH 3
#define ROCKCHIP_RKNN_DPU_RDMA_FEATURE_BURST_LENGTH_SHIFT 11
#define ROCKCHIP_RKNN_DPU_RDMA_FEATURE_BURST_LENGTH_LENGTH 4
#define ROCKCHIP_RKNN_DPU_RDMA_FEATURE_INPUT_PRECISION_SHIFT 15
#define ROCKCHIP_RKNN_DPU_RDMA_FEATURE_INPUT_PRECISION_LENGTH 3
#define ROCKCHIP_RKNN_DPU_RDMA_UNPOOLING_CFG 0x00001249
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_OUTPUT_MODE_SHIFT 1
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_OUTPUT_MODE_LENGTH 2
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_FLYING_MODE BIT(0)
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_BURST_LEN_SHIFT 5
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_BURST_LEN_LENGTH 4
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_CONV_MODE_SHIFT 3
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_CONV_MODE_LENGTH 2
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_NONALIGN BIT(25)
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_TP_EN BIT(30)
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_COMB_USE BIT(31)
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_RGP_TYPE_SHIFT 26
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_RGP_TYPE_LENGTH 4
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_SHIFT 9
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_LENGTH 16
#define ROCKCHIP_RKNN_DPU_FEATURE_MODE_SUPPORTED_OUTPUT_MODE 2
#define ROCKCHIP_RKNN_DEFAULT_MAX_HOST_BYTES (UINT64_C(256) * 1024 * 1024)
#define ROCKCHIP_RKNN_DEFAULT_MAX_MAC_OPERATIONS (UINT64_C(1) << 28)
#define ROCKCHIP_RKNN_DEFAULT_MAX_PPU_WORK_ITEMS (UINT64_C(1) << 24)
#define ROCKCHIP_RKNN_POINTER_BANK BIT(0)
#define ROCKCHIP_RKNN_POINTER_PP_EN BIT(1)
#define ROCKCHIP_RKNN_EXECUTOR_PP_EN BIT(2)
#define ROCKCHIP_RKNN_POINTER_PP_MODE BIT(3)
#define ROCKCHIP_RKNN_POINTER_PP_CLEAR BIT(4)
#define ROCKCHIP_RKNN_EXECUTOR_PP_CLEAR BIT(5)
#define ROCKCHIP_RKNN_EXECUTOR_BANK BIT(16)
#define ROCKCHIP_RKNN_CORE_TAG_MASK (BIT(28) | BIT(29))
#define ROCKCHIP_RKNN_CORE_TAG_SHIFT 28
#define ROCKCHIP_RKNN_CO_WORK_64X32 1
#define ROCKCHIP_RKNN_CO_WORK_96X32 2
#define ROCKCHIP_RKNN_CO_WORK_32X64 4
#define ROCKCHIP_RKNN_CO_WORK_32X96 5
#define ROCKCHIP_RKNN_POINTER_WRITABLE_MASK \
    (ROCKCHIP_RKNN_POINTER_BANK | ROCKCHIP_RKNN_POINTER_PP_EN | \
     ROCKCHIP_RKNN_EXECUTOR_PP_EN | ROCKCHIP_RKNN_POINTER_PP_MODE | \
     ROCKCHIP_RKNN_CORE_TAG_MASK)

typedef struct RockchipRKNNRegcmdStats {
    uint32_t pc;
    uint32_t cna;
    uint32_t core;
    uint32_t dpu;
    uint32_t dpu_rdma;
    uint32_t ppu;
    uint32_t ppu_rdma;
    uint32_t raw;
    uint32_t unknown;
} RockchipRKNNRegcmdStats;

typedef enum RockchipRKNNRegcmdDomain {
    ROCKCHIP_RKNN_DOMAIN_PC,
    ROCKCHIP_RKNN_DOMAIN_CNA,
    ROCKCHIP_RKNN_DOMAIN_CORE,
    ROCKCHIP_RKNN_DOMAIN_DPU,
    ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
    ROCKCHIP_RKNN_DOMAIN_PPU,
    ROCKCHIP_RKNN_DOMAIN_PPU_RDMA,
    ROCKCHIP_RKNN_DOMAIN_COUNT,
} RockchipRKNNRegcmdDomain;

static void rockchip_rknn_commit_domain_runtime(
    RockchipRKNNDomainRuntimeState *state);

typedef struct RockchipRKNNTensorView {
    uint32_t iova;
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t line_stride;
    uint32_t surface_stride;
    uint8_t precision;
    uint8_t atom;
} RockchipRKNNTensorView;

typedef struct RockchipRKNNDepthwiseOutputLayout {
    size_t planes;
    size_t plane_atom;
    uint64_t surface_words;
    uint64_t plane_words;
    uint64_t rows_per_surface;
} RockchipRKNNDepthwiseOutputLayout;

typedef struct RockchipRKNNCNAConfig {
    RockchipRKNNTensorView input;
    uint32_t weight_iova;
    uint32_t weight_bytes;
    uint32_t weight_bytes_per_kernel;
    uint32_t output_atomics;
    uint32_t cvt_con0;
    uint32_t cvt_channel[4];
    uint32_t per_channel_cvt;
    uint32_t pad_value;
    uint32_t fc_data_size0;
    uint32_t fc_data_size1;
    uint16_t input_channels_valid;
    uint16_t output_width;
    uint16_t weight_kernels;
    uint8_t kernel_groups;
    uint16_t feature_grains;
    uint8_t nn_mode;
    uint8_t atrous_x_dilation;
    uint8_t atrous_y_dilation;
    uint8_t deconv_stride_x;
    uint8_t deconv_stride_y;
    uint8_t surface_mode;
    uint8_t kernel_width;
    uint8_t kernel_height;
    uint8_t conv_mode;
    uint8_t co_work_mode;
    uint8_t argb_in;
    uint8_t input_precision;
    uint8_t process_precision;
    uint8_t stride_x;
    uint8_t stride_y;
    uint8_t pad_left;
    uint8_t pad_top;
    bool nonalign_dma;
    bool group_line_off;
    bool deconv;
    bool csc_weight_output_disable;
    bool csc_data_output_disable;
    bool cmd_fifo_soft_reset;
} RockchipRKNNCNAConfig;

typedef struct RockchipRKNNCoreConfig {
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint8_t process_precision;
    uint8_t clip_truncate;
    bool depthwise;
    bool quantify;
} RockchipRKNNCoreConfig;

typedef struct RockchipRKNNDPUConfig {
    RockchipRKNNTensorView output;
    uint32_t feature_mode;
    uint32_t data_format;
    uint16_t offset_pend;
    uint16_t output_notch_0;
    uint16_t output_notch_1;
    uint8_t minmax_ctl;
    uint32_t bs_cfg;
    uint32_t bs_ow_cfg;
    uint16_t bs_ow_op;
    uint32_t bs_relux_cmp;
    uint32_t dst_dma_cfg;
    uint32_t bn_cfg;
    uint32_t bn_relux_cmp;
    uint32_t ew_cfg;
    uint32_t ew_relux_cmp;
    int32_t out_cvt_offset;
    uint16_t out_cvt_scale;
    uint16_t out_cvt_shift;
    uint8_t out_cvt_minus_exp;
    bool out_cvt_type;
    bool out_fp32_to_fp16;
    uint32_t surface_add;
    uint16_t output_channels_valid;
    uint16_t wdma_channels;
    uint16_t wdma_width;
    uint16_t wdma_height;
    uint16_t wdma_size_c;
    bool wdma_tp_precision;
    uint8_t input_precision;
    uint8_t process_precision;
    uint8_t output_precision;
    uint32_t lut_cfg;
    uint32_t lut_info;
    uint32_t lut_le_start, lut_le_end;
    uint32_t lut_lo_start, lut_lo_end;
    uint32_t lut_le_slope_scale, lut_le_slope_shift;
    uint32_t lut_lo_slope_scale, lut_lo_slope_shift;
} RockchipRKNNDPUConfig;

typedef struct RockchipRKNNDpuRdmaConfig {
    uint32_t src_iova;
    uint32_t bs_iova;
    uint32_t bn_iova;
    uint32_t ew_iova;
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t brdma_cfg;
    uint32_t nrdma_cfg;
    uint32_t erdma_cfg;
    uint32_t ew_surface_stride;
    uint32_t feature_mode;
    uint32_t src_dma_cfg;
    uint32_t surface_notch;
    uint32_t pad_cfg;
    uint32_t weight;
    uint32_t ew_surface_notch;
} RockchipRKNNDpuRdmaConfig;

typedef struct RockchipRKNNPPUConfig {
    uint32_t src_iova;
    uint32_t dst_iova;
    uint32_t in_width, in_height, in_channels;
    uint32_t rdma_in_width, rdma_in_height, rdma_in_channels;
    uint32_t out_width, out_height, out_channels;
    uint32_t mode, kernel, reciprocal_width, reciprocal_height;
    uint32_t padding, padding_value_0, padding_value_1;
    uint32_t dst_stride, line_stride, surf_stride;
    uint32_t data_format, rdma_data_format, misc_ctrl;
} RockchipRKNNPPUConfig;

struct RockchipRKNNPipelineTask {
    RockchipRKNNCNAConfig cna;
    RockchipRKNNCoreConfig core;
    RockchipRKNNDPUConfig dpu;
    RockchipRKNNDpuRdmaConfig dpu_rdma;
    RockchipRKNNPPUConfig ppu;
    uint32_t task_dma_base;
    uint32_t enabled_blocks;
};

static uint32_t rockchip_rknn_regcmd_shadow_read(const uint32_t shadow[],
                                                 uint32_t rel);

typedef enum RockchipRKNNDMAResult {
    ROCKCHIP_RKNN_DMA_OK,
    ROCKCHIP_RKNN_DMA_IOMMU_FAULT,
    ROCKCHIP_RKNN_DMA_BUS_ERROR,
} RockchipRKNNDMAResult;

typedef enum RockchipRKNNExecutionMode {
    ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED,
    ROCKCHIP_RKNN_EXECUTION_DPU_INT32,
    ROCKCHIP_RKNN_EXECUTION_DPU_INT8,
    ROCKCHIP_RKNN_EXECUTION_DPU_INT8_BRDMA,
    ROCKCHIP_RKNN_EXECUTION_DPU_INT8_QD_BRDMA,
    ROCKCHIP_RKNN_EXECUTION_DPU_FP16,
    ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_INT8_PIPELINE,
    ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_INT16_UNPOOL,
    ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_INT8_TO_FP16,
    ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_FP16_TO_INT8_COMBINE,
    ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_FP16_PIPELINE,
    ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_FP16_MINMAX,
    ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_FP16_LUT,
    ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_RAW16_TENSOR,
    ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_RAW16_COMPACT_U8,
    ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_RAW16_TAIL,
    ROCKCHIP_RKNN_EXECUTION_PPU_BYPASS,
    ROCKCHIP_RKNN_EXECUTION_PPU_INT8_MAX_POOL,
    ROCKCHIP_RKNN_EXECUTION_PPU_FP16_MAX_POOL,
} RockchipRKNNExecutionMode;

typedef enum RockchipRKNNExecutionResult {
    ROCKCHIP_RKNN_EXECUTION_OK,
    ROCKCHIP_RKNN_EXECUTION_IOMMU_FAULT,
    ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT,
    ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT,
    ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR,
} RockchipRKNNExecutionResult;

static bool rockchip_rknn_iommu_range_mapped(RockchipRKNNCoreState *s,
                                             uint32_t iova, size_t length,
                                             bool write)
{
    IOMMUMemoryRegion *iommu = memory_region_get_iommu(s->dma_mr);
    IOMMUMemoryRegionClass *imrc;
    IOMMUAccessFlags access = write ? IOMMU_WO : IOMMU_RO;
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    int iommu_idx;

    if (!iommu) {
        return false;
    }

    imrc = memory_region_get_iommu_class_nocheck(iommu);
    iommu_idx = memory_region_iommu_attrs_to_index(iommu, attrs);
    while (length) {
        IOMMUTLBEntry entry;
        hwaddr page_offset;
        size_t chunk;
        BQL_LOCK_GUARD();

        entry = imrc->translate(iommu, iova, access, iommu_idx);
        if (!entry.target_as || (entry.perm & access) != access) {
            return false;
        }

        page_offset = iova & entry.addr_mask;
        chunk = entry.addr_mask == HWADDR_MAX ? length :
                MIN(length, (size_t)(entry.addr_mask - page_offset + 1));
        if (chunk < length && iova > UINT32_MAX - chunk) {
            return false;
        }
        iova += chunk;
        length -= chunk;
    }

    return true;
}

static RockchipRKNNDMAResult rockchip_rknn_iommu_dma_result(
    RockchipRKNNCoreState *s, uint32_t iova, void *buffer, size_t length,
    bool write)
{
    size_t offset = 0;

    while (offset < length) {
        size_t chunk = MIN(length - offset,
                           (size_t)ROCKCHIP_RKNN_DMA_CHUNK_SIZE);
        MemTxResult result;
        BQL_LOCK_GUARD();

        if (!rockchip_rknn_iommu_range_mapped(s, iova + offset, chunk,
                                               write)) {
            return ROCKCHIP_RKNN_DMA_IOMMU_FAULT;
        }
        if (write) {
            result = dma_memory_write(s->dma_as, iova + offset,
                                      (uint8_t *)buffer + offset, chunk,
                                      MEMTXATTRS_UNSPECIFIED);
        } else {
            result = dma_memory_read(s->dma_as, iova + offset,
                                     (uint8_t *)buffer + offset, chunk,
                                     MEMTXATTRS_UNSPECIFIED);
        }
        if (result != MEMTX_OK) {
            return ROCKCHIP_RKNN_DMA_BUS_ERROR;
        }
        offset += chunk;
    }

    return ROCKCHIP_RKNN_DMA_OK;
}

static bool rockchip_rknn_iommu_dma(RockchipRKNNCoreState *s, uint32_t iova,
                                    void *buffer, size_t length, bool write)
{
    return rockchip_rknn_iommu_dma_result(s, iova, buffer, length, write) ==
           ROCKCHIP_RKNN_DMA_OK;
}

static bool rockchip_rknn_domain_from_target(uint32_t target,
                                             RockchipRKNNRegcmdDomain *domain,
                                             uint32_t *base)
{
    switch (target) {
    case ROCKCHIP_RKNN_REGCMD_TARGET_PC:
        *domain = ROCKCHIP_RKNN_DOMAIN_PC;
        *base = 0;
        return true;
    case ROCKCHIP_RKNN_REGCMD_TARGET_CNA:
        *domain = ROCKCHIP_RKNN_DOMAIN_CNA;
        *base = ROCKCHIP_RKNN_REGCMD_CNA_BASE;
        return true;
    case ROCKCHIP_RKNN_REGCMD_TARGET_CORE:
        *domain = ROCKCHIP_RKNN_DOMAIN_CORE;
        *base = ROCKCHIP_RKNN_REGCMD_CORE_BASE;
        return true;
    case ROCKCHIP_RKNN_REGCMD_TARGET_DPU:
        *domain = ROCKCHIP_RKNN_DOMAIN_DPU;
        *base = ROCKCHIP_RKNN_REGCMD_DPU_BASE;
        return true;
    case ROCKCHIP_RKNN_REGCMD_TARGET_DPU_RDMA:
        *domain = ROCKCHIP_RKNN_DOMAIN_DPU_RDMA;
        *base = ROCKCHIP_RKNN_REGCMD_DPU_RDMA_BASE;
        return true;
    case ROCKCHIP_RKNN_REGCMD_TARGET_PPU:
        *domain = ROCKCHIP_RKNN_DOMAIN_PPU;
        *base = ROCKCHIP_RKNN_REGCMD_PPU_BASE;
        return true;
    case ROCKCHIP_RKNN_REGCMD_TARGET_PPU_RDMA:
        *domain = ROCKCHIP_RKNN_DOMAIN_PPU_RDMA;
        *base = ROCKCHIP_RKNN_REGCMD_PPU_RDMA_BASE;
        return true;
    default:
        return false;
    }
}

static void rockchip_rknn_set_pointer_state(
    RockchipRKNNDomainRuntimeState *runtime, uint32_t value,
    bool fixed_core_identity)
{
    uint32_t writable = ROCKCHIP_RKNN_POINTER_WRITABLE_MASK;

    if (fixed_core_identity) {
        writable &= ~ROCKCHIP_RKNN_CORE_TAG_MASK;
    }
    runtime->pointer_value = value & writable;
    runtime->pointer_bank = extract32(value, 0, 1);
    runtime->pointer_pingpong = extract32(value, 1, 1);
    runtime->executor_pingpong = extract32(value, 2, 1);
    runtime->pingpong_mode = extract32(value, 3, 1);

}

static unsigned int rockchip_rknn_domain_write_bank(
    const RockchipRKNNDomainRuntimeState *runtime)
{
    return runtime->pointer_bank ^
           (runtime->pointer_pingpong && runtime->pingpong_mode ?
            runtime->executor_bank : 0);
}

static bool rockchip_rknn_register_write(
    RockchipRKNNRegisterFile *file, uint32_t target, uint32_t reg,
    uint32_t value,
    uint32_t writes[ROCKCHIP_RKNN_PENDING_WRITE_R_MAX])
{
    RockchipRKNNRegcmdDomain domain;
    RockchipRKNNDomainState *state;
    RockchipRKNNRegisterBank *bank;
    uint32_t base;
    uint32_t rel;
    uint32_t index;
    unsigned int bank_index;

    if (!rockchip_rknn_domain_from_target(target, &domain, &base) ||
        reg < base || reg >= base + ROCKCHIP_RKNN_WINDOW_SIZE) {
        return false;
    }

    rel = reg - base;
    if (rel & 3) {
        return false;
    }
    index = rel / sizeof(uint32_t);
    state = &file->domain[domain];
    if (domain != ROCKCHIP_RKNN_DOMAIN_PC && rel == 0x004) {
        RockchipRKNNDomainRuntimeState *runtime = &file->runtime[domain];

        rockchip_rknn_set_pointer_state(
            runtime, value,
            domain == ROCKCHIP_RKNN_DOMAIN_CNA ||
            domain == ROCKCHIP_RKNN_DOMAIN_CORE);
        state->write_bank = rockchip_rknn_domain_write_bank(runtime);
        value = runtime->pointer_value;
    }
    bank_index = domain == ROCKCHIP_RKNN_DOMAIN_PC ? 0 : state->write_bank;
    bank = &state->bank[bank_index];
    bank->regs[index] = value;
    bank->present[index / 32] |= BIT(index % 32);
    if (writes) {
        writes[((domain * 2 + bank_index) *
                ROCKCHIP_RKNN_PRESENT_R_MAX) + index / 32] |=
            BIT(index % 32);
    }
    return true;
}

static void rockchip_rknn_register_file_init(
    RockchipRKNNRegisterFile *file,
    const RockchipRKNNRegisterFile *persistent)
{
    memcpy(file->domain, persistent->domain, sizeof(file->domain));
    for (unsigned int i = 0; i < ROCKCHIP_RKNN_DOMAIN_COUNT; i++) {
        file->runtime[i] = persistent->runtime[i];
        file->domain[i].write_bank = i == ROCKCHIP_RKNN_DOMAIN_PC ? 0 :
            rockchip_rknn_domain_write_bank(&file->runtime[i]);
    }
}

static void rockchip_rknn_lut_write(RockchipRKNNCoreState *s,
                                    uint32_t rel, uint32_t value)
{
    switch (rel) {
    case ROCKCHIP_RKNN_DPU_LUT_ACCESS_CFG:
        s->lut_access_cfg = value;
        break;
    case ROCKCHIP_RKNN_DPU_LUT_ACCESS_DATA: {
        unsigned int table = extract32(s->lut_access_cfg, 16, 1);
        unsigned int addr = extract32(s->lut_access_cfg, 0, 10);
        if (extract32(s->lut_access_cfg, 17, 1) && table < 2 &&
            addr < ROCKCHIP_RKNN_LUT_ENTRIES) {
            s->lut[table][addr] = value & 0xffff;
            s->lut_access_cfg = deposit32(s->lut_access_cfg, 0, 10,
                                           addr + 1);
        }
        break;
    }
    case ROCKCHIP_RKNN_DPU_LUT_CFG:
        s->lut_cfg = value;
        break;
    case ROCKCHIP_RKNN_DPU_LUT_INFO:
        s->lut_info = value;
        break;
    case ROCKCHIP_RKNN_DPU_LUT_LE_START:
        s->lut_le_start = value;
        break;
    case ROCKCHIP_RKNN_DPU_LUT_LE_END:
        s->lut_le_end = value;
        break;
    case ROCKCHIP_RKNN_DPU_LUT_LO_START:
        s->lut_lo_start = value;
        break;
    case ROCKCHIP_RKNN_DPU_LUT_LO_END:
        s->lut_lo_end = value;
        break;
    case ROCKCHIP_RKNN_DPU_LUT_LE_SLOPE_SCALE:
        s->lut_le_slope_scale = value; break;
    case ROCKCHIP_RKNN_DPU_LUT_LE_SLOPE_SHIFT:
        s->lut_le_slope_shift = value; break;
    case ROCKCHIP_RKNN_DPU_LUT_LO_SLOPE_SCALE:
        s->lut_lo_slope_scale = value; break;
    case ROCKCHIP_RKNN_DPU_LUT_LO_SLOPE_SHIFT:
        s->lut_lo_slope_shift = value; break;
    default:
        break;
    }
}

static bool rockchip_rknn_fetch_register_file(RockchipRKNNCoreState *s,
                                              RockchipRKNNRegisterFile *file,
                                              uint32_t iova,
                                              uint32_t command_count)
{
    g_autofree uint64_t *commands = NULL;

    if (!s->dma_mr) {
        return false;
    }
    commands = g_new(uint64_t, command_count);
    if (!rockchip_rknn_iommu_dma(s, iova, commands,
                                 command_count * sizeof(*commands), false)) {
        return false;
    }

    for (uint32_t i = 0; i < command_count; i++) {
        uint64_t command = le64_to_cpu(commands[i]);
        uint32_t reg = command & 0xffff;
        uint32_t value = (command >> 16) & 0xffffffff;
        uint32_t target = command >> 48;

        if (!command) {
            continue;
        }
        if (file->block_enable) {
            return false;
        }
        if (target == ROCKCHIP_RKNN_REGCMD_PRE_ENABLE) {
            file->pre_enable = true;
            continue;
        }
        if (target == ROCKCHIP_RKNN_REGCMD_BLOCK_ENABLE) {
            if (!file->pre_enable || reg != A_PC_OPERATION_ENABLE) {
                return false;
            }
            file->enabled_blocks = value & 0x7f;
            file->block_enable = true;
            continue;
        }
        if (!rockchip_rknn_register_write(
                file, target, reg, value, s->pending_register_writes)) {
            return false;
        }
        if (target == ROCKCHIP_RKNN_REGCMD_TARGET_DPU &&
            reg >= ROCKCHIP_RKNN_REGCMD_DPU_BASE) {
            rockchip_rknn_lut_write(s,
                reg - ROCKCHIP_RKNN_REGCMD_DPU_BASE, value);
        }
    }

    return file->block_enable;
}

static const RockchipRKNNRegisterBank *
rockchip_rknn_register_bank(const RockchipRKNNRegisterFile *file,
                            RockchipRKNNRegcmdDomain domain)
{
    const RockchipRKNNDomainState *state = &file->domain[domain];

    return &state->bank[domain == ROCKCHIP_RKNN_DOMAIN_PC ? 0 :
                        state->write_bank];
}

static bool rockchip_rknn_register_read_present(
    const RockchipRKNNRegisterFile *file, RockchipRKNNRegcmdDomain domain,
    uint32_t rel, uint32_t *value)
{
    const RockchipRKNNRegisterBank *bank =
        rockchip_rknn_register_bank(file, domain);
    uint32_t index = rel / sizeof(uint32_t);

    if (rel >= ROCKCHIP_RKNN_WINDOW_SIZE ||
        !(bank->present[index / 32] & BIT(index % 32))) {
        return false;
    }
    *value = bank->regs[index];
    return true;
}

static bool rockchip_rknn_register_read(const RockchipRKNNRegisterFile *file,
                                        RockchipRKNNRegcmdDomain domain,
                                        uint32_t rel, uint32_t *value)
{
    if (!rockchip_rknn_register_read_present(file, domain, rel, value)) {
        *value = 0;
    }
    return true;
}

static bool rockchip_rknn_add_task_base(uint32_t base, uint32_t offset,
                                        uint32_t *iova)
{
    if (base > UINT32_MAX - offset) {
        return false;
    }
    *iova = base + offset;
    return true;
}

static bool rockchip_rknn_decode_ppu(RockchipRKNNPipelineTask *task,
                                     RockchipRKNNRegisterFile *file)
{
    RockchipRKNNPPUConfig *ppu = &task->ppu;
    uint32_t in_w, in_h, in_c, out_w, out_h, out_c;
    uint32_t rdma_in_w, rdma_in_h, rdma_in_c;

    if (file->enabled_blocks != (ROCKCHIP_RKNN_BLOCK_PPU |
                                 ROCKCHIP_RKNN_BLOCK_PPU_RDMA) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU,
                                     ROCKCHIP_RKNN_PPU_IN_WIDTH, &in_w) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU,
                                     ROCKCHIP_RKNN_PPU_IN_HEIGHT, &in_h) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU,
                                     ROCKCHIP_RKNN_PPU_IN_CHANNEL, &in_c) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU,
                                     ROCKCHIP_RKNN_PPU_OUT_WIDTH, &out_w) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU,
                                     ROCKCHIP_RKNN_PPU_OUT_HEIGHT, &out_h) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU,
                                     ROCKCHIP_RKNN_PPU_OUT_CHANNEL, &out_c) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU,
                                     ROCKCHIP_RKNN_PPU_MODE, &ppu->mode) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU,
                                     ROCKCHIP_RKNN_PPU_KERNEL, &ppu->kernel) ||
        !rockchip_rknn_register_read(
            file, ROCKCHIP_RKNN_DOMAIN_PPU,
            ROCKCHIP_RKNN_PPU_RECIPROCAL_WIDTH,
            &ppu->reciprocal_width) ||
        !rockchip_rknn_register_read(
            file, ROCKCHIP_RKNN_DOMAIN_PPU,
            ROCKCHIP_RKNN_PPU_RECIPROCAL_HEIGHT,
            &ppu->reciprocal_height) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU,
                                     ROCKCHIP_RKNN_PPU_PADDING,
                                     &ppu->padding) ||
        !rockchip_rknn_register_read(
            file, ROCKCHIP_RKNN_DOMAIN_PPU,
            ROCKCHIP_RKNN_PPU_PADDING_VALUE_0,
            &ppu->padding_value_0) ||
        !rockchip_rknn_register_read(
            file, ROCKCHIP_RKNN_DOMAIN_PPU,
            ROCKCHIP_RKNN_PPU_PADDING_VALUE_1,
            &ppu->padding_value_1) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU,
                                     ROCKCHIP_RKNN_PPU_DST_BASE,
                                     &ppu->dst_iova) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU,
                                     ROCKCHIP_RKNN_PPU_DST_STRIDE,
                                     &ppu->dst_stride) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU,
                                     ROCKCHIP_RKNN_PPU_DATA_FORMAT,
                                     &ppu->data_format) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU,
                                     ROCKCHIP_RKNN_PPU_MISC_CTRL,
                                     &ppu->misc_ctrl) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU_RDMA,
                                     ROCKCHIP_RKNN_PPU_RDMA_IN_WIDTH,
                                     &rdma_in_w) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU_RDMA,
                                     ROCKCHIP_RKNN_PPU_RDMA_IN_HEIGHT,
                                     &rdma_in_h) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU_RDMA,
                                     ROCKCHIP_RKNN_PPU_RDMA_IN_CHANNEL,
                                     &rdma_in_c) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU_RDMA,
                                     ROCKCHIP_RKNN_PPU_RDMA_SRC_BASE,
                                     &ppu->src_iova) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU_RDMA,
                                     ROCKCHIP_RKNN_PPU_RDMA_LINE_STRIDE,
                                     &ppu->line_stride) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU_RDMA,
                                     ROCKCHIP_RKNN_PPU_RDMA_SURF_STRIDE,
                                     &ppu->surf_stride) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PPU_RDMA,
                                     ROCKCHIP_RKNN_PPU_RDMA_DATA_FORMAT,
                                     &ppu->rdma_data_format)) {
        return false;
    }
    ppu->in_width = extract32(in_w, 0, 13) + 1;
    ppu->in_height = extract32(in_h, 0, 13) + 1;
    ppu->in_channels = extract32(in_c, 0, 13) + 1;
    ppu->rdma_in_width = extract32(rdma_in_w, 0, 13) + 1;
    ppu->rdma_in_height = extract32(rdma_in_h, 0, 13) + 1;
    ppu->rdma_in_channels = extract32(rdma_in_c, 0, 13) + 1;
    ppu->out_width = extract32(out_w, 0, 13) + 1;
    ppu->out_height = extract32(out_h, 0, 13) + 1;
    ppu->out_channels = extract32(out_c, 0, 13) + 1;
    ppu->dst_iova &= ~0xfU;
    ppu->dst_stride &= ~0xfU;
    ppu->line_stride &= ~0xfU;
    ppu->surf_stride &= ~0xfU;
    task->enabled_blocks = file->enabled_blocks;
    return rockchip_rknn_add_task_base(task->task_dma_base, ppu->src_iova,
                                       &ppu->src_iova) &&
           rockchip_rknn_add_task_base(task->task_dma_base, ppu->dst_iova,
                                       &ppu->dst_iova);
}

static bool rockchip_rknn_decode_pipeline(RockchipRKNNCoreState *s,
                                          RockchipRKNNPipelineTask *task,
                                          RockchipRKNNDPUStageSnapshot *stage,
                                          RockchipRKNNRegisterFile *file)
{
    uint32_t cna_conv1, cna_conv2, cna_conv3, data0, data1, data2, data3;
    uint32_t weight0, weight1, weight2, cna_cvt, cna_cvt_channel[4];
    uint32_t cna_per_channel_cvt, cna_pad, cna_pad_value;
    uint32_t cna_dma1, cna_dma2;
    uint32_t cna_fc0, cna_fc1, core_misc, core_size0, core_size1, core_clip;
    uint32_t dpu_feature, dpu_format, dpu_offset_pend, dpu_stride;
    uint32_t dpu_width, dpu_height, dpu_notch, dpu_channel, dpu_bs;
    uint32_t dpu_bs_relux_cmp, dpu_dma, dpu_wdma0;
    uint32_t dpu_bs_ow_cfg, dpu_bs_ow_op;
    uint32_t dpu_bn, dpu_bn_relux_cmp, dpu_ew, dpu_ew_relux_cmp;
    uint32_t dpu_ew_cvt_offset, dpu_ew_cvt_scale;
    uint32_t dpu_bs_alu, dpu_bs_mul, dpu_bn_alu, dpu_bn_mul;
    uint32_t dpu_cvt_offset, dpu_cvt_scale, dpu_cvt_shift, dpu_surface_add;
    uint32_t dpu_wdma1;
    uint32_t lut_cfg = 0, lut_info = 0, lut_le_start = 0, lut_le_end = 0;
    uint32_t lut_lo_start = 0, lut_lo_end = 0, lut_le_slope_scale = 0;
    uint32_t lut_le_slope_shift = 0, lut_lo_slope_scale = 0;
    uint32_t lut_lo_slope_shift = 0;
    uint32_t rdma_width = 0, rdma_height = 0, rdma_channels = 0;
    uint32_t rdma_src = 0, rdma_brdma = 0, rdma_bs = 0, rdma_nrdma = 0;
    uint32_t rdma_bn = 0, rdma_erdma = 0;
    uint32_t rdma_ew = 0, rdma_ew_stride = 0;
    uint32_t rdma_feature = 0, rdma_src_dma = 0, rdma_notch = 0;
    uint32_t rdma_pad = 0, rdma_weight = 0, rdma_ew_notch = 0;
    uint32_t input_offset, weight_offset, output_offset;

    task->task_dma_base = s->pc_regs[R_PC_TASK_DMA_BASE_ADDR] &
                          ROCKCHIP_RKNN_PC_BASE_ADDRESS_MASK;
    if (!file->enabled_blocks) {
        return false;
    }
    if (file->enabled_blocks == (ROCKCHIP_RKNN_BLOCK_PPU |
                                 ROCKCHIP_RKNN_BLOCK_PPU_RDMA)) {
        return rockchip_rknn_decode_ppu(task, file);
    }

    if (!rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_CONV_CON1,
                                     &cna_conv1) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_CONV_CON2,
                                     &cna_conv2) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_CONV_CON3,
                                     &cna_conv3) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_DATA_SIZE0, &data0) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_DATA_SIZE1, &data1) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_DATA_SIZE2, &data2) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_DATA_SIZE3, &data3) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_WEIGHT_SIZE0,
                                     &weight0) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_WEIGHT_SIZE1,
                                     &weight1) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_WEIGHT_SIZE2,
                                     &weight2) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_CVT_CON0, &cna_cvt) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_CVT_CON1,
                                     &cna_cvt_channel[0]) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_CVT_CON1 + 4,
                                     &cna_cvt_channel[1]) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_CVT_CON1 + 8,
                                     &cna_cvt_channel[2]) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_CVT_CON1 + 12,
                                     &cna_cvt_channel[3]) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_CVT_CON5,
                                     &cna_per_channel_cvt) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_PAD_CON0, &cna_pad) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_PAD_CON1,
                                     &cna_pad_value) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_DMA_CON1,
                                     &cna_dma1) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_DMA_CON2,
                                     &cna_dma2) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_FC_DATA_SIZE0,
                                     &cna_fc0) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_FC_DATA_SIZE1,
                                     &cna_fc1) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_FEATURE_DATA_ADDR,
                                     &input_offset) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CNA,
                                     ROCKCHIP_RKNN_CNA_DCOMP_ADDR0,
                                     &weight_offset) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CORE,
                                     ROCKCHIP_RKNN_CORE_MISC_CFG,
                                     &core_misc) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CORE,
                                     ROCKCHIP_RKNN_CORE_DATAOUT_SIZE_0,
                                     &core_size0) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CORE,
                                     ROCKCHIP_RKNN_CORE_DATAOUT_SIZE_1,
                                     &core_size1) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_CORE,
                                     ROCKCHIP_RKNN_CORE_CLIP_TRUNCATE,
                                     &core_clip) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_FEATURE_MODE_CFG,
                                     &dpu_feature) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_DATA_FORMAT,
                                     &dpu_format) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_OFFSET_PEND,
                                     &dpu_offset_pend) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_DST_BASE_ADDR,
                                     &output_offset) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_DST_SURF_STRIDE,
                                     &dpu_stride) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_DATA_CUBE_WIDTH,
                                     &dpu_width) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_DATA_CUBE_HEIGHT,
                                     &dpu_height) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_DATA_CUBE_NOTCH_ADDR,
                                     &dpu_notch) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_DATA_CUBE_CHANNEL,
                                     &dpu_channel) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BS_CFG, &dpu_bs) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BS_OW_CFG,
                                     &dpu_bs_ow_cfg) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BS_OW_OP,
                                     &dpu_bs_ow_op) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BS_ALU_CFG,
                                     &dpu_bs_alu) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BS_MUL_CFG,
                                     &dpu_bs_mul) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BS_RELUX_CMP_VALUE,
                                     &dpu_bs_relux_cmp) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_DST_DMA_CFG,
                                     &dpu_dma) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_WDMA_SIZE_0,
                                     &dpu_wdma0) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_WDMA_SIZE_1,
                                     &dpu_wdma1) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BN_CFG, &dpu_bn) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BN_ALU_CFG,
                                     &dpu_bn_alu) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BN_MUL_CFG,
                                     &dpu_bn_mul) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BN_RELUX_CMP_VALUE,
                                     &dpu_bn_relux_cmp) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_EW_CFG, &dpu_ew) ||
        !rockchip_rknn_register_read(
            file, ROCKCHIP_RKNN_DOMAIN_DPU,
            ROCKCHIP_RKNN_DPU_EW_CVT_OFFSET_VALUE, &dpu_ew_cvt_offset) ||
        !rockchip_rknn_register_read(
            file, ROCKCHIP_RKNN_DOMAIN_DPU,
            ROCKCHIP_RKNN_DPU_EW_CVT_SCALE_VALUE, &dpu_ew_cvt_scale) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_EW_RELUX_CMP_VALUE,
                                     &dpu_ew_relux_cmp) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_OUT_CVT_OFFSET,
                                     &dpu_cvt_offset) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_OUT_CVT_SCALE,
                                     &dpu_cvt_scale) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_OUT_CVT_SHIFT,
                                     &dpu_cvt_shift) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_SURFACE_ADD,
                                     &dpu_surface_add)) {
        return false;
    }

    if (!(dpu_ew & ROCKCHIP_RKNN_DPU_EW_BYPASS) &&
        !(dpu_ew & ROCKCHIP_RKNN_DPU_EW_LUT_BYPASS) &&
        (!rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_LUT_CFG, &lut_cfg) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_LUT_INFO, &lut_info) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_LUT_LE_START,
                                     &lut_le_start) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_LUT_LE_END,
                                     &lut_le_end) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_LUT_LO_START,
                                     &lut_lo_start) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_LUT_LO_END,
                                     &lut_lo_end) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_LUT_LE_SLOPE_SCALE,
                                     &lut_le_slope_scale) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_LUT_LE_SLOPE_SHIFT,
                                     &lut_le_slope_shift) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_LUT_LO_SLOPE_SCALE,
                                     &lut_lo_slope_scale) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_LUT_LO_SLOPE_SHIFT,
                                     &lut_lo_slope_shift))) {
        return false;
    }

    if (file->enabled_blocks & ROCKCHIP_RKNN_BLOCK_DPU_RDMA) {
        if (!rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_DATA_CUBE_WIDTH, &rdma_width) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_DATA_CUBE_HEIGHT, &rdma_height) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_DATA_CUBE_CHANNEL, &rdma_channels) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_ERDMA_CFG, &rdma_erdma) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_FEATURE_MODE_CFG, &rdma_feature) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_SRC_DMA_CFG, &rdma_src_dma) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_PAD_CFG, &rdma_pad) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_WEIGHT, &rdma_weight) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_BRDMA_CFG, &rdma_brdma) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_BS_BASE_ADDR, &rdma_bs)) {
            return false;
        }
        if (!rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_NRDMA_CFG, &rdma_nrdma) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_BN_BASE_ADDR, &rdma_bn)) {
            return false;
        }
        if (!(rdma_feature &
              ROCKCHIP_RKNN_DPU_RDMA_FEATURE_MAIN_DISABLE) &&
            (!rockchip_rknn_register_read(
                 file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                 ROCKCHIP_RKNN_DPU_RDMA_SRC_BASE_ADDR, &rdma_src) ||
             !rockchip_rknn_register_read(
                 file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                 ROCKCHIP_RKNN_DPU_RDMA_SURF_NOTCH, &rdma_notch))) {
            return false;
        }
        if (!(rdma_erdma & ROCKCHIP_RKNN_DPU_ERDMA_DISABLE) &&
            (!rockchip_rknn_register_read(
                       file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                       ROCKCHIP_RKNN_DPU_RDMA_EW_BASE_ADDR, &rdma_ew) ||
             !rockchip_rknn_register_read(
                       file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                       ROCKCHIP_RKNN_DPU_RDMA_EW_SURF_STRIDE,
                       &rdma_ew_stride) ||
             !rockchip_rknn_register_read(
                       file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                       ROCKCHIP_RKNN_DPU_RDMA_EW_SURF_NOTCH,
                       &rdma_ew_notch))) {
            return false;
        }
    }

    task->enabled_blocks = file->enabled_blocks;
    task->cna.conv_mode = extract32(cna_conv1, 0, 4);
    task->cna.co_work_mode = extract32(cna_conv2, 28, 3);
    task->cna.argb_in = extract32(cna_conv1, 12, 4);
    task->cna.nonalign_dma = extract32(cna_conv1, 30, 1);
    task->cna.group_line_off = extract32(cna_conv1, 29, 1);
    task->cna.deconv = extract32(cna_conv1, 16, 1);
    task->cna.input_precision = extract32(cna_conv1, 4, 3);
    task->cna.process_precision = extract32(cna_conv1, 7, 3);
    task->cna.stride_x = extract32(cna_conv3, 0, 3);
    task->cna.stride_y = extract32(cna_conv3, 3, 3);
    task->cna.deconv_stride_x = extract32(cna_conv3, 8, 3);
    task->cna.deconv_stride_y = extract32(cna_conv3, 11, 3);
    task->cna.atrous_x_dilation = extract32(cna_conv3, 16, 5);
    task->cna.atrous_y_dilation = extract32(cna_conv3, 21, 5);
    task->cna.nn_mode = extract32(cna_conv3, 28, 3);
    task->cna.pad_left = extract32(cna_pad, 4, 4);
    task->cna.pad_top = extract32(cna_pad, 0, 4);
    task->cna.input.width = extract32(data0, 16, 11);
    task->cna.input.height = extract32(data0, 0, 11);
    task->cna.input.channels = extract32(data1, 0, 16);
    task->cna.input_channels_valid = extract32(data1, 16, 14) + 1;
    task->cna.output_width = extract32(data2, 0, 11);
    task->cna.output_atomics = extract32(data3, 0, 22);
    task->cna.surface_mode = extract32(data3, 22, 2);
    task->cna.input.precision = task->cna.input_precision;
    task->cna.input.atom = task->cna.input_precision == 2 ? 8 : 16;
    task->cna.kernel_width = extract32(weight2, 24, 5);
    task->cna.kernel_height = extract32(weight2, 16, 5);
    task->cna.weight_kernels = extract32(weight2, 0, 14);
    task->cna.kernel_groups = extract32(cna_conv2, 16, 8) + 1;
    task->cna.feature_grains = extract32(cna_conv2, 4, 10);
    task->cna.csc_weight_output_disable = extract32(cna_conv2, 2, 1);
    task->cna.csc_data_output_disable = extract32(cna_conv2, 1, 1);
    task->cna.cmd_fifo_soft_reset = extract32(cna_conv2, 0, 1);
    task->cna.weight_bytes = weight0;
    task->cna.weight_bytes_per_kernel = extract32(weight1, 0, 19);
    task->cna.cvt_con0 = cna_cvt;
    memcpy(task->cna.cvt_channel, cna_cvt_channel,
           sizeof(task->cna.cvt_channel));
    task->cna.per_channel_cvt = cna_per_channel_cvt;
    task->cna.pad_value = cna_pad_value;
    task->cna.input.line_stride = extract32(cna_dma1, 0, 28);
    task->cna.input.surface_stride = extract32(cna_dma2, 0, 28);
    task->cna.fc_data_size0 = cna_fc0;
    task->cna.fc_data_size1 = cna_fc1;
    task->core.process_precision = extract32(core_misc, 8, 3);
    task->core.depthwise = extract32(core_misc, 1, 1);
    task->core.quantify = extract32(core_misc, 0, 1);
    task->core.clip_truncate = extract32(core_clip, 0, 5);
    task->core.width = extract32(core_size0, 0, 16) + 1;
    task->core.height = extract32(core_size0, 16, 16) + 1;
    task->core.channels = extract32(core_size1, 0, 16) + 1;
    task->dpu.data_format = dpu_format;
    task->dpu.feature_mode = dpu_feature;
    task->dpu.offset_pend = extract32(dpu_offset_pend, 0, 16);
    task->dpu.output_notch_0 = extract32(dpu_notch, 0, 13);
    task->dpu.output_notch_1 = extract32(dpu_notch, 16, 13);
    task->dpu.output_precision = extract32(dpu_format, 29, 3);
    task->dpu.input_precision = extract32(dpu_format, 26, 3);
    task->dpu.process_precision = extract32(dpu_format, 0, 3);
    task->dpu.output.width = extract32(dpu_width, 0, 13) + 1;
    task->dpu.output.height = extract32(dpu_height, 0, 13) + 1;
    task->dpu.minmax_ctl = extract32(dpu_height, 22, 3);
    task->dpu.output.channels = extract32(dpu_channel, 0, 13) + 1;
    task->dpu.output_channels_valid = extract32(dpu_channel, 16, 13) + 1;
    task->dpu.output.precision = task->dpu.output_precision;
    task->dpu.output.atom = task->dpu.output_precision == 0 ? 16 :
                           (task->dpu.output_precision == 1 ||
                            task->dpu.output_precision == 2) ? 8 : 4;
    task->dpu.output.surface_stride = extract32(dpu_stride, 4, 28);
    task->dpu.bs_cfg = dpu_bs;
    task->dpu.bs_ow_cfg = dpu_bs_ow_cfg;
    task->dpu.bs_ow_op = extract32(dpu_bs_ow_op, 0, 16);
    task->dpu.bs_relux_cmp = dpu_bs_relux_cmp;
    task->dpu.dst_dma_cfg = dpu_dma;
    task->dpu.wdma_channels = extract32(dpu_wdma0, 0, 13) + 1;
    task->dpu.wdma_size_c = extract32(dpu_wdma0, 16, 11);
    task->dpu.wdma_tp_precision = extract32(dpu_wdma0, 27, 1);
    task->dpu.wdma_width = extract32(dpu_wdma1, 0, 13) + 1;
    task->dpu.wdma_height = extract32(dpu_wdma1, 16, 13) + 1;
    task->dpu.bn_cfg = dpu_bn;
    task->dpu.bn_relux_cmp = dpu_bn_relux_cmp;
    task->dpu.ew_cfg = dpu_ew;
    task->dpu.ew_relux_cmp = dpu_ew_relux_cmp;
    task->dpu.out_cvt_offset = dpu_cvt_offset;
    task->dpu.out_cvt_scale = extract32(dpu_cvt_scale, 0, 16);
    task->dpu.out_fp32_to_fp16 = extract32(dpu_cvt_scale, 16, 1);
    task->dpu.out_cvt_shift = extract32(dpu_cvt_shift, 0, 12);
    task->dpu.out_cvt_minus_exp = extract32(dpu_cvt_shift, 12, 8);
    task->dpu.out_cvt_type = extract32(dpu_cvt_shift, 31, 1);
    task->dpu.surface_add = dpu_surface_add;
    task->dpu.lut_cfg = lut_cfg;
    task->dpu.lut_info = lut_info;
    task->dpu.lut_le_start = lut_le_start;
    task->dpu.lut_le_end = lut_le_end;
    task->dpu.lut_lo_start = lut_lo_start;
    task->dpu.lut_lo_end = lut_lo_end;
    task->dpu.lut_le_slope_scale = lut_le_slope_scale;
    task->dpu.lut_le_slope_shift = lut_le_slope_shift;
    task->dpu.lut_lo_slope_scale = lut_lo_slope_scale;
    task->dpu.lut_lo_slope_shift = lut_lo_slope_shift;
    task->dpu_rdma.width = extract32(rdma_width, 0, 13) + 1;
    task->dpu_rdma.height = extract32(rdma_height, 0, 13) + 1;
    task->dpu_rdma.channels = extract32(rdma_channels, 0, 13) + 1;
    task->dpu_rdma.brdma_cfg = rdma_brdma;
    task->dpu_rdma.nrdma_cfg = rdma_nrdma;
    task->dpu_rdma.erdma_cfg = rdma_erdma;
    task->dpu_rdma.ew_surface_stride = extract32(rdma_ew_stride, 4, 28);
    task->dpu_rdma.feature_mode = rdma_feature;
    task->dpu_rdma.src_dma_cfg = rdma_src_dma;
    task->dpu_rdma.surface_notch = extract32(rdma_notch, 4, 28);
    task->dpu_rdma.pad_cfg = rdma_pad;
    task->dpu_rdma.weight = rdma_weight;
    task->dpu_rdma.ew_surface_notch = extract32(rdma_ew_notch, 4, 28);
    stage->bs_alu_operand = dpu_bs_alu;
    stage->bs_mul_cfg = dpu_bs_mul;
    stage->bn_alu_operand = dpu_bn_alu;
    stage->bn_mul_cfg = dpu_bn_mul;
    stage->ew_cvt_offset = dpu_ew_cvt_offset;
    stage->ew_cvt_scale = dpu_ew_cvt_scale;
    stage->out_cvt_round = extract32(dpu_cvt_shift, 30, 1);
    for (unsigned int i = 0; i < ARRAY_SIZE(stage->ew_operand); i++) {
        uint32_t operand;

        if (!rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU,
                ROCKCHIP_RKNN_DPU_EW_OP_VALUE_0 + i * sizeof(uint32_t),
                &operand)) {
            return false;
        }
        stage->ew_operand[i] = operand;
    }

    if (dpu_cvt_scale & ~(BIT(16) | 0xffffU) ||
        dpu_cvt_shift & ~(BIT(31) | BIT(30) | 0xfffffU)) {
        return false;
    }

    return rockchip_rknn_add_task_base(task->task_dma_base, input_offset,
                                       &task->cna.input.iova) &&
           rockchip_rknn_add_task_base(task->task_dma_base, weight_offset,
                                       &task->cna.weight_iova) &&
           rockchip_rknn_add_task_base(task->task_dma_base, output_offset,
                                       &task->dpu.output.iova) &&
           (!(file->enabled_blocks & ROCKCHIP_RKNN_BLOCK_DPU_RDMA) ||
            (rockchip_rknn_add_task_base(task->task_dma_base, rdma_src,
                                         &task->dpu_rdma.src_iova) &&
             rockchip_rknn_add_task_base(task->task_dma_base, rdma_bs,
                                         &task->dpu_rdma.bs_iova) &&
             rockchip_rknn_add_task_base(task->task_dma_base, rdma_bn,
                                         &task->dpu_rdma.bn_iova) &&
             rockchip_rknn_add_task_base(task->task_dma_base, rdma_ew,
                                         &task->dpu_rdma.ew_iova)));
}

static uint32_t rockchip_rknn_cna_execution_channels(
    const RockchipRKNNCNAConfig *cna);

static bool rockchip_rknn_pipeline_shape_is_well_formed(
    const RockchipRKNNPipelineTask *task)
{
    uint32_t m = task->cna.input.height;
    uint32_t k_valid = task->cna.input_channels_valid;
    uint32_t k_storage = task->cna.input.channels;
    uint32_t n_weight = task->cna.weight_kernels;
    uint32_t n_valid = task->dpu.output_channels_valid;
    uint32_t n_cube = task->dpu.output.channels;

    if (!m || !k_storage || !k_valid || k_valid > k_storage ||
        !n_valid || n_valid > n_cube) {
        return false;
    }
    if (task->core.depthwise) {
        return n_weight == 1 &&
               n_valid <= rockchip_rknn_cna_execution_channels(&task->cna);
    }
    return n_valid <= n_weight && n_weight <= n_cube;
}

static bool rockchip_rknn_dpu_stage_uses_alu_source(uint32_t cfg)
{
    return !(cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) &&
           !(cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_BYPASS) &&
           (cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_SOURCE);
}

static bool rockchip_rknn_dpu_stage_uses_mul_source(uint32_t cfg,
                                                     uint32_t mul_cfg)
{
    return !(cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) &&
           !(cfg & ROCKCHIP_RKNN_DPU_STAGE_MUL_BYPASS) &&
           (mul_cfg & ROCKCHIP_RKNN_DPU_MUL_SOURCE);
}

static bool rockchip_rknn_dpu_ew_uses_rdma(uint32_t cfg)
{
    return !(cfg & ROCKCHIP_RKNN_DPU_EW_BYPASS) &&
           !(cfg & ROCKCHIP_RKNN_DPU_EW_OP_BYPASS) &&
           (cfg & ROCKCHIP_RKNN_DPU_EW_OP_SOURCE);
}

static bool rockchip_rknn_dpu_stage_is_supported(uint32_t cfg,
                                                  uint32_t mul_cfg,
                                                  bool rdma_alu,
                                                  bool rdma_mul)
{
    unsigned int algorithm;

    if (cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) {
        return true;
    }
    if (!(cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_BYPASS)) {
        algorithm = extract32(cfg,
                              ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_SHIFT,
                              ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_LENGTH);
        if ((algorithm != 2 && algorithm != 4) ||
            ((cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_SOURCE) && !rdma_alu)) {
            return false;
        }
    }
    if (!(cfg & ROCKCHIP_RKNN_DPU_STAGE_MUL_BYPASS)) {
        if (mul_cfg & ~(0xffff0000U | 0x3f00U |
                        ROCKCHIP_RKNN_DPU_MUL_SOURCE |
                        ROCKCHIP_RKNN_DPU_MUL_TRUNCATE_SOURCE)) {
            return false;
        }
        if ((mul_cfg & ROCKCHIP_RKNN_DPU_MUL_SOURCE) && !rdma_mul) {
            return false;
        }
        if (mul_cfg & ROCKCHIP_RKNN_DPU_MUL_TRUNCATE_SOURCE) {
            return false;
        }
    }
    return true;
}

static bool rockchip_rknn_dpu_ew_is_supported(uint32_t cfg,
                                               bool rdma_operand)
{
    unsigned int algorithm;
    unsigned int data_mode;
    unsigned int data_size;

    if (cfg & ROCKCHIP_RKNN_DPU_EW_BYPASS) {
        return true;
    }
    if (cfg & (ROCKCHIP_RKNN_DPU_EW_BINARY_ENABLE |
               ROCKCHIP_RKNN_DPU_EW_EQUAL_ENABLE)) {
        return false;
    }
    if (cfg & ROCKCHIP_RKNN_DPU_EW_OP_BYPASS) {
        return true;
    }
    algorithm = extract32(cfg, ROCKCHIP_RKNN_DPU_EW_ALU_ALGO_SHIFT,
                          ROCKCHIP_RKNN_DPU_EW_ALU_ALGO_LENGTH);
    if (cfg & ROCKCHIP_RKNN_DPU_EW_OP_TYPE) {
        if (algorithm != 0) {
            return false;
        }
    } else if (algorithm != 2) {
        return false;
    }
    if ((cfg & ROCKCHIP_RKNN_DPU_EW_MUL_PRELU) &&
        !(cfg & ROCKCHIP_RKNN_DPU_EW_OP_TYPE)) {
        return false;
    }
    if ((cfg & ROCKCHIP_RKNN_DPU_EW_OP_SOURCE) && !rdma_operand) {
        return false;
    }
    data_mode = extract32(cfg, ROCKCHIP_RKNN_DPU_EW_DATA_MODE_SHIFT,
                          ROCKCHIP_RKNN_DPU_EW_DATA_MODE_LENGTH);
    data_size = extract32(cfg, ROCKCHIP_RKNN_DPU_EW_DATA_SIZE_SHIFT,
                          ROCKCHIP_RKNN_DPU_EW_DATA_SIZE_LENGTH);
    if (cfg & ROCKCHIP_RKNN_DPU_EW_OP_SOURCE) {
        return data_mode == 1 && data_size == 1;
    }
    return data_mode == 0 && data_size == 0;
}

static bool rockchip_rknn_dpu_qd_bs_ow_is_supported(
    const RockchipRKNNPipelineTask *task)
{
    uint32_t cpend_cfg = task->core.depthwise ?
        ROCKCHIP_RKNN_DPU_BS_OW_CFG_DEPTHWISE :
        ROCKCHIP_RKNN_DPU_BS_OW_CFG_CONV;
    uint32_t no_cpend_cfg = task->core.depthwise ?
        ROCKCHIP_RKNN_DPU_BS_OW_CFG_DEPTHWISE_NO_CPEND :
        ROCKCHIP_RKNN_DPU_BS_OW_CFG_CONV_NO_CPEND;

    return (task->dpu.bs_ow_cfg == cpend_cfg ||
            task->dpu.bs_ow_cfg == no_cpend_cfg) &&
           task->dpu.bs_ow_op == ROCKCHIP_RKNN_DPU_BS_OW_OP_SUPPORTED;
}

static bool rockchip_rknn_dpu_qd_uses_cpend(
    const RockchipRKNNPipelineTask *task)
{
    uint32_t cpend_cfg = task->core.depthwise ?
        ROCKCHIP_RKNN_DPU_BS_OW_CFG_DEPTHWISE :
        ROCKCHIP_RKNN_DPU_BS_OW_CFG_CONV;

    return task->dpu.bs_ow_cfg == cpend_cfg;
}

static bool rockchip_rknn_brdma_layout_is_supported(
    const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    uint32_t expected_feature_mode = task->core.depthwise ? 0x7816 : 0x7810;

    return rdma->feature_mode == expected_feature_mode &&
           !rdma->src_dma_cfg &&
           !rdma->surface_notch && !rdma->pad_cfg &&
           rdma->weight == 0x01010101 && !rdma->ew_surface_stride &&
           !rdma->ew_surface_notch;
}

static bool rockchip_rknn_erdma_layout_is_supported(
    const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    uint64_t spatial = (uint64_t)task->dpu.output.width *
                       task->dpu.output.height;
    uint64_t surface_notch = (uint64_t)rdma->ew_surface_stride * 2 -
                             spatial;

    return rdma->feature_mode == 0x7d00 && !rdma->src_dma_cfg &&
           rdma->ew_surface_stride >= spatial &&
           rdma->surface_notch == surface_notch && !rdma->pad_cfg &&
           rdma->weight == 0x01010101 &&
           rdma->ew_surface_notch == surface_notch;
}

static bool rockchip_rknn_dpu_feature_mode_is_supported(uint32_t feature_mode,
                                                         uint8_t conv_mode,
                                                         bool flying_mode)
{
    return !!(feature_mode & ROCKCHIP_RKNN_DPU_FEATURE_MODE_FLYING_MODE) ==
               flying_mode &&
           !(feature_mode & (ROCKCHIP_RKNN_DPU_FEATURE_MODE_COMB_USE |
                             ROCKCHIP_RKNN_DPU_FEATURE_MODE_TP_EN |
                             ROCKCHIP_RKNN_DPU_FEATURE_MODE_NONALIGN)) &&
           extract32(feature_mode,
                     ROCKCHIP_RKNN_DPU_FEATURE_MODE_BURST_LEN_SHIFT,
                     ROCKCHIP_RKNN_DPU_FEATURE_MODE_BURST_LEN_LENGTH) == 0xf &&
           !extract32(feature_mode,
                      ROCKCHIP_RKNN_DPU_FEATURE_MODE_RGP_TYPE_SHIFT,
                      ROCKCHIP_RKNN_DPU_FEATURE_MODE_RGP_TYPE_LENGTH) &&
           !extract32(feature_mode,
                      ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_SHIFT,
                      ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_LENGTH) &&
           extract32(feature_mode,
                     ROCKCHIP_RKNN_DPU_FEATURE_MODE_CONV_MODE_SHIFT,
                     ROCKCHIP_RKNN_DPU_FEATURE_MODE_CONV_MODE_LENGTH) ==
               conv_mode &&
           extract32(feature_mode,
                     ROCKCHIP_RKNN_DPU_FEATURE_MODE_OUTPUT_MODE_SHIFT,
                     ROCKCHIP_RKNN_DPU_FEATURE_MODE_OUTPUT_MODE_LENGTH) ==
               ROCKCHIP_RKNN_DPU_FEATURE_MODE_SUPPORTED_OUTPUT_MODE;
}

static bool rockchip_rknn_dpu_rdma_main_int8_is_supported(
    uint32_t feature_mode)
{
    return !(feature_mode & ~MAKE_64BIT_MASK(0, 18)) &&
           (feature_mode & ROCKCHIP_RKNN_DPU_RDMA_FEATURE_FLYING_MODE) &&
           !(feature_mode & (ROCKCHIP_RKNN_DPU_RDMA_FEATURE_FP16_TO_FP32 |
                             ROCKCHIP_RKNN_DPU_RDMA_FEATURE_MAIN_DISABLE)) &&
           !extract32(
               feature_mode,
               ROCKCHIP_RKNN_DPU_RDMA_FEATURE_CONV_MODE_SHIFT,
               ROCKCHIP_RKNN_DPU_RDMA_FEATURE_CONV_MODE_LENGTH) &&
           !extract32(
               feature_mode,
               ROCKCHIP_RKNN_DPU_RDMA_FEATURE_PROCESS_PRECISION_SHIFT,
               ROCKCHIP_RKNN_DPU_RDMA_FEATURE_PROCESS_PRECISION_LENGTH) &&
           !extract32(
               feature_mode,
               ROCKCHIP_RKNN_DPU_RDMA_FEATURE_COMBINATION_SHIFT,
               ROCKCHIP_RKNN_DPU_RDMA_FEATURE_COMBINATION_LENGTH) &&
           extract32(
               feature_mode,
               ROCKCHIP_RKNN_DPU_RDMA_FEATURE_BURST_LENGTH_SHIFT,
               ROCKCHIP_RKNN_DPU_RDMA_FEATURE_BURST_LENGTH_LENGTH) == 0xf &&
           !extract32(
               feature_mode,
               ROCKCHIP_RKNN_DPU_RDMA_FEATURE_INPUT_PRECISION_SHIFT,
               ROCKCHIP_RKNN_DPU_RDMA_FEATURE_INPUT_PRECISION_LENGTH);
}

static bool rockchip_rknn_cna_fp16_compact(const RockchipRKNNCNAConfig *cna)
{
    return cna->input_precision == 2 && cna->process_precision == 2 &&
           cna->nonalign_dma && cna->group_line_off && cna->argb_in == 8;
}

static bool rockchip_rknn_cna_fp16_interleaved(
    const RockchipRKNNCNAConfig *cna)
{
    return cna->input_precision == 2 && cna->process_precision == 2 &&
           cna->nonalign_dma && cna->group_line_off &&
           cna->argb_in == ROCKCHIP_RKNN_CNA_RGB888_FORMAT &&
           cna->input_channels_valid &&
           cna->input_channels_valid <= cna->input.channels;
}

static uint32_t rockchip_rknn_cna_logical_width(
    const RockchipRKNNCNAConfig *cna)
{
    return extract32(cna->fc_data_size0, 16, 14);
}

static uint32_t rockchip_rknn_cna_logical_height(
    const RockchipRKNNCNAConfig *cna)
{
    return extract32(cna->fc_data_size0, 0, 11);
}

static bool rockchip_rknn_cna_storage_layout_is_supported(
    const RockchipRKNNCNAConfig *cna)
{
    uint32_t width = rockchip_rknn_cna_logical_width(cna);
    uint32_t height = rockchip_rknn_cna_logical_height(cna);

    if (extract32(cna->fc_data_size1, 0, 16) != cna->input.channels) {
        return false;
    }
    if (rockchip_rknn_cna_fp16_compact(cna) ||
        rockchip_rknn_cna_fp16_interleaved(cna)) {
        return width && height && width <= cna->input.width &&
               height <= cna->input.height;
    }
    return width == cna->input.width && height == cna->input.height;
}

static bool rockchip_rknn_deconv_shape_is_supported(
    const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNCNAConfig *cna = &task->cna;
    uint32_t stride_x = cna->deconv_stride_x + 1;
    uint32_t stride_y = cna->deconv_stride_y + 1;
    int64_t pad_x;
    int64_t pad_y;
    int64_t output_width;
    int64_t output_height;

    if (!cna->deconv || cna->kernel_width <= cna->pad_left ||
        cna->kernel_height <= cna->pad_top ||
        stride_x <= 1 || stride_y <= 1) {
        return false;
    }
    pad_x = cna->kernel_width - 1 - cna->pad_left;
    pad_y = cna->kernel_height - 1 - cna->pad_top;
    output_width = ((int64_t)rockchip_rknn_cna_logical_width(cna) - 1) *
                   stride_x + cna->kernel_width - 2 * pad_x;
    output_height = ((int64_t)rockchip_rknn_cna_logical_height(cna) - 1) *
                    stride_y + cna->kernel_height - 2 * pad_y;
    return output_width == task->core.width &&
           output_height == task->core.height;
}

static bool rockchip_rknn_fp16_deconv_is_supported(
    const RockchipRKNNPipelineTask *task)
{
    if (!task->cna.deconv) {
        return !task->cna.deconv_stride_x &&
               !task->cna.deconv_stride_y;
    }
    return !task->core.depthwise &&
           rockchip_rknn_deconv_shape_is_supported(task);
}

static bool rockchip_rknn_cna_interleaved_input(
    const RockchipRKNNCNAConfig *cna)
{
    return !rockchip_rknn_cna_fp16_compact(cna) &&
           (cna->nonalign_dma || cna->argb_in);
}

static uint32_t rockchip_rknn_cna_execution_channels(
    const RockchipRKNNCNAConfig *cna)
{
    return rockchip_rknn_cna_interleaved_input(cna) ?
        cna->input_channels_valid : cna->input.channels;
}

static bool rockchip_rknn_cna_input_mode_is_supported(
    const RockchipRKNNCNAConfig *cna)
{
    if (rockchip_rknn_cna_fp16_interleaved(cna)) {
        return cna->cvt_con0 & ROCKCHIP_RKNN_CNA_CVT_BYPASS;
    }
    if (!rockchip_rknn_cna_interleaved_input(cna)) {
        if (cna->cvt_con0 & ROCKCHIP_RKNN_CNA_CVT_BYPASS) {
            return (cna->cvt_con0 & ROCKCHIP_RKNN_CNA_CVT_DATA_SIGN) &&
                   !cna->per_channel_cvt;
        }
        return cna->input_precision == 0 &&
               !(cna->cvt_con0 & 0xf0000000U) &&
               cna->per_channel_cvt ==
                   MAKE_64BIT_MASK(0, cna->input.atom);
    }

    return cna->nonalign_dma && cna->group_line_off &&
           (cna->argb_in == 0 || cna->argb_in == 8 ||
            cna->argb_in == ROCKCHIP_RKNN_CNA_RGB888_FORMAT) &&
           cna->input_channels_valid && cna->input_channels_valid <= 4 &&
           !(cna->cvt_con0 & (ROCKCHIP_RKNN_CNA_CVT_BYPASS |
                              ROCKCHIP_RKNN_CNA_CVT_TYPE)) &&
           !(cna->cvt_con0 & 0xf0000000U) &&
           (cna->per_channel_cvt &
            ((1U << cna->input_channels_valid) - 1)) ==
               ((1U << cna->input_channels_valid) - 1);
}

static bool rockchip_rknn_depthwise_int32_layout(
    const RockchipRKNNPipelineTask *task,
    RockchipRKNNDepthwiseOutputLayout *layout);

static bool rockchip_rknn_depthwise_int32_notched_output_is_supported(
    const RockchipRKNNPipelineTask *task);

static bool rockchip_rknn_depthwise_int32_flat_wdma_is_supported(
    const RockchipRKNNPipelineTask *task);

static bool rockchip_rknn_strided_output_layout_valid(
    const RockchipRKNNTensorView *view, size_t element_bytes);

static bool rockchip_rknn_dpu_rdma_tensor_layout_is_supported(
    const RockchipRKNNPipelineTask *task, size_t element_bytes)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;

    return dpu->output.width && dpu->output.height &&
           dpu->output_channels_valid &&
           dpu->output_channels_valid <= dpu->output.channels &&
           dpu->wdma_channels == dpu->output.channels &&
           dpu->wdma_width == dpu->output.width &&
           dpu->wdma_height == dpu->output.height &&
           rdma->width == dpu->output.width &&
           rdma->height == dpu->output.height &&
           rdma->channels == dpu->output.channels &&
           rockchip_rknn_strided_output_layout_valid(
               &dpu->output, element_bytes);
}

static bool rockchip_rknn_fp16_compact_output_layout_is_supported(
    const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    uint64_t output_bytes;

    return dpu->output_precision == 2 && dpu->output.width &&
           dpu->output.height && dpu->output_channels_valid &&
           dpu->output_channels_valid <= dpu->output.channels &&
           dpu->wdma_width == dpu->output.width &&
           dpu->wdma_height == dpu->output.height &&
           dpu->wdma_channels >= dpu->output.channels &&
           task->core.channels == dpu->wdma_channels &&
           !(dpu->wdma_channels % dpu->output.atom) &&
           (output_bytes = (uint64_t)dpu->output.width *
                           dpu->output.height * dpu->wdma_channels) &&
           output_bytes <= UINT32_MAX / sizeof(uint16_t) &&
           dpu->surface_add == output_bytes * sizeof(uint16_t);
}

static bool rockchip_rknn_fp16_group_line_output_is_supported(
    const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    uint64_t wdma_channels;
    uint64_t surface_stride;
    unsigned int surfaces;

    if (!task->core.depthwise || !task->cna.group_line_off ||
        dpu->output_precision != 2 || !dpu->output.width ||
        !dpu->output.height || !dpu->output.channels ||
        dpu->output.channels % dpu->output.atom ||
        dpu->output_notch_0 != 1 || dpu->output_notch_1 != 1 ||
        dpu->wdma_width != 1 ||
        dpu->wdma_height != dpu->output.height || dpu->wdma_size_c ||
        dpu->wdma_tp_precision ||
        dpu->dst_dma_cfg !=
            ROCKCHIP_RKNN_DPU_BS_OW_CFG_DEPTHWISE_FP16_GROUP ||
        dpu->bs_ow_cfg !=
            ROCKCHIP_RKNN_DPU_BS_OW_CFG_DEPTHWISE_FP16_GROUP ||
        dpu->bs_ow_op) {
        return false;
    }
    surfaces = dpu->output.channels / dpu->output.atom;
    return !__builtin_mul_overflow((uint64_t)dpu->output.width,
                                   dpu->output.channels, &wdma_channels) &&
           wdma_channels == dpu->wdma_channels &&
           !__builtin_mul_overflow((uint64_t)dpu->output.width,
                                   dpu->output.height, &surface_stride) &&
           !__builtin_mul_overflow(surface_stride, surfaces,
                                   &surface_stride) &&
           surface_stride == dpu->output.surface_stride &&
           wdma_channels <= UINT32_MAX / sizeof(uint16_t) &&
           dpu->surface_add == wdma_channels * sizeof(uint16_t) &&
           rockchip_rknn_strided_output_layout_valid(
               &dpu->output, sizeof(uint16_t));
}

static bool rockchip_rknn_dpu_rdma_int8_pipeline_layout_is_supported(
    const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;

    return dpu->output.width && dpu->output.height &&
           dpu->output_channels_valid &&
           dpu->output_channels_valid <= dpu->output.channels &&
           dpu->wdma_channels == dpu->output.channels &&
           rdma->width && rdma->height &&
           rdma->width <= dpu->output.width &&
           rdma->height <= dpu->output.height &&
           !(dpu->output.width % rdma->width) &&
           !(dpu->output.height % rdma->height) &&
           ((dpu->wdma_width == dpu->output.width &&
             dpu->wdma_height == dpu->output.height) ||
            (dpu->wdma_width == rdma->width &&
             dpu->wdma_height == rdma->height)) &&
           rdma->channels == dpu->output.channels &&
           rockchip_rknn_strided_output_layout_valid(&dpu->output, 1);
}

static bool rockchip_rknn_dpu_rdma_int8_reshape_is_supported(
    const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    uint32_t surfaces = DIV_ROUND_UP(dpu->output.channels, 16);
    uint32_t source_line_notch = extract32(rdma->src_dma_cfg, 19, 13);
    int64_t source_surface_atoms =
        (int64_t)rdma->width * rdma->height +
        (int64_t)source_line_notch * (rdma->height - 1) +
        sextract32(rdma->surface_notch, 0, 28);
    uint64_t input_spatial = (uint64_t)rdma->width * rdma->height;
    uint64_t output_spatial = (uint64_t)dpu->output.width *
                              dpu->output.height;

    return dpu->output.width && dpu->output.height &&
           dpu->output.atom == 16 && dpu->output_channels_valid &&
           dpu->output_channels_valid <= dpu->output.channels &&
           rdma->width && rdma->height &&
           input_spatial == output_spatial &&
           rdma->channels == dpu->output.channels &&
           dpu->wdma_channels == dpu->output.channels &&
           dpu->wdma_width == dpu->output.width &&
           dpu->wdma_height == dpu->output.height &&
           dpu->wdma_size_c + 1 == surfaces &&
           !dpu->wdma_tp_precision && dpu->output.surface_stride == 1 &&
           dpu->output_notch_0 ==
               (uint64_t)dpu->output.width * (surfaces - 1) &&
           dpu->output_notch_1 == dpu->output_notch_0 &&
           dpu->surface_add == (uint64_t)dpu->output.width * 16 &&
           source_line_notch + rdma->width ==
               (uint64_t)rdma->width * surfaces &&
           source_surface_atoms == rdma->width;
}

static bool rockchip_rknn_dpu_rdma_int8_src_dma_is_supported(
    const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;

    return !(rdma->src_dma_cfg & MAKE_64BIT_MASK(0, 19));
}

static bool rockchip_rknn_dpu_rdma_fp16_layout_is_supported(
    const RockchipRKNNDpuRdmaConfig *rdma)
{
    return !rdma->brdma_cfg &&
           !(rdma->src_dma_cfg & MAKE_64BIT_MASK(0, 19)) &&
           rdma->weight == 0x01010101;
}

static bool rockchip_rknn_dpu_rdma_fp16_padding_is_supported(
    const RockchipRKNNDpuRdmaConfig *rdma)
{
    const uint32_t supported = MAKE_64BIT_MASK(0, 3) |
                               MAKE_64BIT_MASK(4, 3) |
                               MAKE_64BIT_MASK(16, 16);
    unsigned int pad_left = extract32(rdma->pad_cfg, 0, 3);
    unsigned int pad_top = extract32(rdma->pad_cfg, 4, 3);

    return !(rdma->pad_cfg & ~supported) &&
           pad_left < rdma->width && pad_top < rdma->height;
}

static bool rockchip_rknn_dpu_surface_add_matches_output(
    const RockchipRKNNDPUConfig *dpu)
{
    return dpu->output.surface_stride <= UINT32_MAX >> 4 &&
           dpu->surface_add == dpu->output.surface_stride << 4;
}

static bool rockchip_rknn_dpu_rdma_int8_to_fp16_is_supported(
    const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    uint64_t surface_add = (uint64_t)dpu->output.width *
                           dpu->output.height * sizeof(uint16_t);

    return rockchip_rknn_dpu_rdma_tensor_layout_is_supported(
               task, sizeof(uint16_t)) &&
           rockchip_rknn_dpu_feature_mode_is_supported(
               dpu->feature_mode, 0, true) &&
           dpu->data_format == (2U << 29) &&
           !dpu->offset_pend && !dpu->minmax_ctl &&
           (dpu->bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) &&
           dpu->bs_ow_cfg == ROCKCHIP_RKNN_DPU_BS_OW_CFG_RDMA &&
           dpu->bs_ow_op == ROCKCHIP_RKNN_DPU_BS_OW_OP_SUPPORTED &&
           (dpu->bn_cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) &&
           (dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_BYPASS) &&
           !dpu->out_cvt_offset && dpu->out_cvt_scale == 1 &&
           !dpu->out_cvt_shift && !dpu->out_cvt_type &&
           dpu->out_fp32_to_fp16 &&
           surface_add <= UINT32_MAX >> 4 &&
           dpu->surface_add == surface_add << 4 &&
           !rdma->brdma_cfg && !rdma->src_dma_cfg &&
           rdma->weight == 0x01010101 &&
           !rdma->pad_cfg &&
           rdma->erdma_cfg == ROCKCHIP_RKNN_DPU_ERDMA_DISABLE &&
           rockchip_rknn_dpu_rdma_main_int8_is_supported(
               rdma->feature_mode);
}

static bool rockchip_rknn_dpu_rdma_int8_pipeline_is_supported(
    const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    uint64_t surface_add = (uint64_t)dpu->output.width *
                           dpu->output.height;
    const uint32_t data_format_controls =
        MAKE_64BIT_MASK(4, 6) | MAKE_64BIT_MASK(16, 10);
    bool ew_rdma = rockchip_rknn_dpu_ew_uses_rdma(dpu->ew_cfg);
    bool reshape = rockchip_rknn_dpu_rdma_int8_reshape_is_supported(task);
    bool layout =
        reshape ||
        rockchip_rknn_dpu_rdma_int8_pipeline_layout_is_supported(task);
    bool dpu_feature = rockchip_rknn_dpu_feature_mode_is_supported(
                           dpu->feature_mode, 0, true) &&
                       !(dpu->data_format & ~data_format_controls) &&
                       !dpu->minmax_ctl &&
                       dpu->dst_dma_cfg == 2;
    bool dpu_stages = rockchip_rknn_dpu_stage_is_supported(
            dpu->bs_cfg, stage->bs_mul_cfg, false, false) &&
        dpu->bs_ow_cfg == 2 && !dpu->bs_ow_op &&
        rockchip_rknn_dpu_stage_is_supported(
            dpu->bn_cfg, stage->bn_mul_cfg, false, false) &&
        rockchip_rknn_dpu_ew_is_supported(dpu->ew_cfg, ew_rdma);
    bool dpu_output = !dpu->out_cvt_minus_exp &&
        !dpu->out_fp32_to_fp16 &&
        (reshape || rockchip_rknn_dpu_surface_add_matches_output(dpu));
    bool dpu_controls = dpu_feature && dpu_stages && dpu_output;
    bool rdma_controls = !rdma->brdma_cfg &&
        rockchip_rknn_dpu_rdma_int8_src_dma_is_supported(task) &&
        rdma->weight == 0x01010101 && !rdma->pad_cfg &&
        rdma->erdma_cfg == (ew_rdma ? 0x40000004 :
                            ROCKCHIP_RKNN_DPU_ERDMA_DISABLE) &&
        rockchip_rknn_dpu_rdma_main_int8_is_supported(rdma->feature_mode);
    uint64_t ew_surface_notch =
        (uint64_t)rdma->ew_surface_stride - surface_add;
    bool ew_layout = !ew_rdma ||
        (rdma->ew_surface_stride >= surface_add &&
         ew_surface_notch <= UINT32_MAX &&
         rdma->ew_surface_notch == ew_surface_notch);

    trace_rockchip_rknn_pipeline_rdma_int8_gate(
        layout, dpu_feature, dpu_stages, dpu_output, rdma_controls,
        ew_layout, ew_rdma);
    return layout && dpu_controls && rdma_controls && ew_layout;
}

static bool rockchip_rknn_dpu_rdma_int16_unpool_is_supported(
    const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    uint64_t spatial = (uint64_t)dpu->output.width * dpu->output.height;

    return rdma->width <= UINT32_MAX / 2 &&
           rdma->channels <= UINT32_MAX / 2 &&
           dpu->output.width == rdma->width * 2 &&
           dpu->output.height == rdma->height &&
           dpu->output.channels == rdma->channels * 2 &&
           dpu->output_channels_valid == dpu->output.channels &&
           dpu->output.atom == 16 && dpu->output.surface_stride ==
               dpu->output.width &&
           dpu->output_notch_0 == dpu->output.width &&
           dpu->output_notch_1 == dpu->output.width &&
           dpu->wdma_channels == dpu->output.channels &&
           dpu->wdma_width == dpu->output.width &&
           dpu->wdma_height == dpu->output.height &&
           rockchip_rknn_dpu_feature_mode_is_supported(
               dpu->feature_mode, 0, true) &&
           dpu->data_format == (1U << 26) && !dpu->offset_pend &&
           !dpu->minmax_ctl && dpu->dst_dma_cfg == 0x126 &&
           (dpu->bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) &&
           dpu->bs_ow_cfg == ROCKCHIP_RKNN_DPU_BS_OW_CFG_RDMA &&
           !dpu->bs_ow_op &&
           (dpu->bn_cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) &&
           (dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_BYPASS) &&
           !dpu->out_cvt_offset && dpu->out_cvt_scale == 1 &&
           !dpu->out_cvt_shift && !dpu->out_cvt_minus_exp &&
           !dpu->out_cvt_type && !dpu->out_fp32_to_fp16 &&
           !stage->out_cvt_round && spatial <= UINT32_MAX / 32 &&
           dpu->surface_add == spatial * 32 && !rdma->brdma_cfg &&
           rdma->erdma_cfg == ROCKCHIP_RKNN_DPU_ERDMA_DISABLE &&
           rdma->feature_mode == 0xf801 &&
           rdma->src_dma_cfg == ROCKCHIP_RKNN_DPU_RDMA_UNPOOLING_CFG &&
           !rdma->surface_notch && !rdma->pad_cfg &&
           rdma->weight == 0x01010101 && !rdma->ew_surface_stride &&
           !rdma->ew_surface_notch;
}

static bool rockchip_rknn_dpu_rdma_fp16_lut_is_supported(
    const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;

    return rockchip_rknn_dpu_rdma_tensor_layout_is_supported(
               task, sizeof(uint16_t)) &&
           rockchip_rknn_dpu_feature_mode_is_supported(
               dpu->feature_mode, 0, true) &&
           dpu->data_format == 0x48000002 &&
           !dpu->offset_pend && !dpu->minmax_ctl &&
           dpu->dst_dma_cfg == 2 &&
           (dpu->bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) &&
           dpu->bs_ow_cfg == 2 && !dpu->bs_ow_op &&
           dpu->bn_cfg == 0x00020040 &&
           !(stage->bn_mul_cfg & 0xffffU) &&
           dpu->ew_cfg == 0x302 &&
           !stage->ew_cvt_offset && stage->ew_cvt_scale == 1 &&
           dpu->out_cvt_offset == 1 && dpu->out_cvt_scale == 1 &&
           !dpu->out_cvt_shift && !stage->out_cvt_round &&
           dpu->out_cvt_minus_exp == 15 &&
           !dpu->out_cvt_type && dpu->out_fp32_to_fp16 &&
           dpu->lut_cfg == 0x68 && dpu->lut_info == 0x00050500 &&
           dpu->lut_le_start == 0xffffc000 && !dpu->lut_le_end &&
           !dpu->lut_lo_start && dpu->lut_lo_end == 0x00004000 &&
           !dpu->lut_le_slope_scale && !dpu->lut_le_slope_shift &&
           !dpu->lut_lo_slope_scale && !dpu->lut_lo_slope_shift &&
           rockchip_rknn_dpu_surface_add_matches_output(dpu) &&
           rockchip_rknn_dpu_rdma_fp16_layout_is_supported(rdma) &&
           rockchip_rknn_dpu_rdma_fp16_padding_is_supported(rdma) &&
           rdma->erdma_cfg == ROCKCHIP_RKNN_DPU_ERDMA_DISABLE &&
           rdma->feature_mode == 0x17849;
}

typedef enum RockchipRKNNDpuRdmaFP16EWMode {
    ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_INVALID,
    ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_BYPASS,
    ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_ADD,
    ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_SUBTRACT,
    ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_DIVIDE,
} RockchipRKNNDpuRdmaFP16EWMode;

static bool rockchip_rknn_dpu_fp16_bs_is_supported(
    const RockchipRKNNDPUConfig *dpu,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const uint32_t supported = ROCKCHIP_RKNN_DPU_STAGE_BYPASS |
        ROCKCHIP_RKNN_DPU_STAGE_ALU_BYPASS |
        ROCKCHIP_RKNN_DPU_STAGE_MUL_BYPASS |
        ROCKCHIP_RKNN_DPU_STAGE_MUL_PRELU |
        ROCKCHIP_RKNN_DPU_STAGE_RELU_BYPASS |
        ROCKCHIP_RKNN_DPU_STAGE_RELUX_ENABLE |
        ROCKCHIP_RKNN_DPU_STAGE_ALU_SOURCE |
        MAKE_64BIT_MASK(ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_SHIFT,
                        ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_LENGTH);
    bool bs_bypass = dpu->bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS;
    bool alu_bypass = dpu->bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_BYPASS;
    bool mul_bypass = dpu->bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_MUL_BYPASS;

    return bs_bypass ||
           (!(dpu->bs_cfg & ~supported) &&
            !(dpu->bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_SOURCE) &&
            (!(dpu->bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_RELUX_ENABLE) ||
             !(dpu->bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_RELU_BYPASS)) &&
            (alu_bypass ||
             (extract32(dpu->bs_cfg,
                        ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_SHIFT,
                        ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_LENGTH) == 2 ||
              extract32(dpu->bs_cfg,
                        ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_SHIFT,
                        ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_LENGTH) == 4)) &&
            (mul_bypass || !(stage->bs_mul_cfg & 0xffffU)));
}

static bool rockchip_rknn_dpu_fp16_stage_is_supported(
    uint32_t cfg, uint32_t mul_cfg, bool rdma_alu)
{
    const uint32_t supported = ROCKCHIP_RKNN_DPU_STAGE_BYPASS |
        ROCKCHIP_RKNN_DPU_STAGE_ALU_BYPASS |
        ROCKCHIP_RKNN_DPU_STAGE_MUL_BYPASS |
        ROCKCHIP_RKNN_DPU_STAGE_MUL_PRELU |
        ROCKCHIP_RKNN_DPU_STAGE_RELU_BYPASS |
        ROCKCHIP_RKNN_DPU_STAGE_RELUX_ENABLE |
        ROCKCHIP_RKNN_DPU_STAGE_ALU_SOURCE |
        MAKE_64BIT_MASK(ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_SHIFT,
                        ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_LENGTH);
    unsigned int algorithm;

    if (cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) {
        return true;
    }
    if (cfg & ~supported) {
        return false;
    }
    if (!(cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_BYPASS)) {
        algorithm = extract32(cfg,
                              ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_SHIFT,
                              ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_LENGTH);
        if ((algorithm != 2 && algorithm != 4) ||
            ((cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_SOURCE) && !rdma_alu)) {
            return false;
        }
    }
    return (cfg & ROCKCHIP_RKNN_DPU_STAGE_MUL_BYPASS) ||
           !(mul_cfg & 0xffffU);
}

static bool rockchip_rknn_dpu_fp16_ew_add_is_supported(
    const RockchipRKNNDPUConfig *dpu,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const uint32_t supported = ROCKCHIP_RKNN_DPU_EW_OP_SOURCE |
        ROCKCHIP_RKNN_DPU_EW_LUT_BYPASS |
        ROCKCHIP_RKNN_DPU_EW_RELU_BYPASS |
        MAKE_64BIT_MASK(ROCKCHIP_RKNN_DPU_EW_ALU_ALGO_SHIFT,
                        ROCKCHIP_RKNN_DPU_EW_ALU_ALGO_LENGTH) |
        MAKE_64BIT_MASK(ROCKCHIP_RKNN_DPU_EW_DATA_SIZE_SHIFT,
                        ROCKCHIP_RKNN_DPU_EW_DATA_SIZE_LENGTH) |
        MAKE_64BIT_MASK(ROCKCHIP_RKNN_DPU_EW_DATA_MODE_SHIFT,
                        ROCKCHIP_RKNN_DPU_EW_DATA_MODE_LENGTH);

    return !(dpu->ew_cfg & ~supported) &&
           (dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_OP_SOURCE) &&
           (dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_LUT_BYPASS) &&
           extract32(dpu->ew_cfg,
                     ROCKCHIP_RKNN_DPU_EW_ALU_ALGO_SHIFT,
                     ROCKCHIP_RKNN_DPU_EW_ALU_ALGO_LENGTH) == 2 &&
           extract32(dpu->ew_cfg,
                     ROCKCHIP_RKNN_DPU_EW_DATA_SIZE_SHIFT,
                     ROCKCHIP_RKNN_DPU_EW_DATA_SIZE_LENGTH) == 2 &&
           extract32(dpu->ew_cfg,
                     ROCKCHIP_RKNN_DPU_EW_DATA_MODE_SHIFT,
                     ROCKCHIP_RKNN_DPU_EW_DATA_MODE_LENGTH) == 1 &&
           !stage->ew_cvt_offset && stage->ew_cvt_scale == 1;
}

static bool rockchip_rknn_dpu_fp16_lut_is_supported(
    const RockchipRKNNDPUConfig *dpu,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    return dpu->output_precision == 2 && dpu->ew_cfg == 0x302 &&
           !stage->ew_cvt_offset && stage->ew_cvt_scale == 1 &&
           dpu->out_cvt_offset == 1 && dpu->out_cvt_scale == 1 &&
           !dpu->out_cvt_shift && dpu->out_cvt_minus_exp == 15 &&
           !dpu->out_cvt_type &&
           dpu->out_fp32_to_fp16 && !stage->out_cvt_round &&
           dpu->lut_cfg == 0x68 && dpu->lut_info == 0x00050500 &&
           dpu->lut_le_start == 0xffffc000 && !dpu->lut_le_end &&
           !dpu->lut_lo_start && dpu->lut_lo_end == 0x00004000 &&
           !dpu->lut_le_slope_scale && !dpu->lut_le_slope_shift &&
           !dpu->lut_lo_slope_scale && !dpu->lut_lo_slope_shift;
}

static bool rockchip_rknn_fp16_brdma_layout_is_supported(
    const RockchipRKNNPipelineTask *task, bool ew_rdma,
    const char **reason)
{
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    uint32_t feature = rdma->feature_mode;
    uint64_t spatial = (uint64_t)task->core.width * task->core.height;
    uint64_t expected_notch = (uint64_t)rdma->ew_surface_stride * 2;

    if (ew_rdma && expected_notch < spatial) {
        *reason = "fp16-brdma-ew-stride";
        return false;
    }
    expected_notch = ew_rdma ? expected_notch - spatial : 0;
    if (extract32(rdma->brdma_cfg,
                  ROCKCHIP_RKNN_DPU_BRDMA_DATA_USE_SHIFT,
                  ROCKCHIP_RKNN_DPU_BRDMA_DATA_USE_LENGTH) != 1 ||
        (rdma->brdma_cfg & ~MAKE_64BIT_MASK(1, 4)) ||
        rdma->erdma_cfg != (ew_rdma ? 0x40000008 :
                            ROCKCHIP_RKNN_DPU_ERDMA_DISABLE)) {
        *reason = "fp16-brdma-control";
        return false;
    }
    if (rdma->width != task->core.width ||
        rdma->height != task->core.height ||
        rdma->channels != task->core.channels) {
        *reason = "fp16-brdma-shape";
        return false;
    }
    if ((feature & ~MAKE_64BIT_MASK(0, 18)) ||
        (feature & ROCKCHIP_RKNN_DPU_RDMA_FEATURE_FLYING_MODE) ||
        !!(feature & ROCKCHIP_RKNN_DPU_RDMA_FEATURE_MAIN_DISABLE) !=
            !ew_rdma ||
        (feature & ROCKCHIP_RKNN_DPU_RDMA_FEATURE_FP16_TO_FP32) ||
        extract32(feature,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_CONV_MODE_SHIFT,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_CONV_MODE_LENGTH) !=
            (task->core.depthwise ? 3 : 0) ||
        extract32(feature,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_PROCESS_PRECISION_SHIFT,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_PROCESS_PRECISION_LENGTH) !=
            2 ||
        extract32(feature,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_COMBINATION_SHIFT,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_COMBINATION_LENGTH) !=
            (ew_rdma ? 5 : 0) ||
        extract32(feature,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_BURST_LENGTH_SHIFT,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_BURST_LENGTH_LENGTH) !=
            0xf ||
        extract32(feature,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_INPUT_PRECISION_SHIFT,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_INPUT_PRECISION_LENGTH) !=
            2) {
        *reason = "fp16-brdma-feature";
        return false;
    }
    if (rdma->src_dma_cfg || rdma->pad_cfg ||
        rdma->weight != 0x01010101 ||
        (ew_rdma ?
         (!rdma->ew_surface_stride ||
          rdma->surface_notch != expected_notch ||
          rdma->ew_surface_notch != expected_notch) :
         (rdma->surface_notch || rdma->ew_surface_stride ||
          rdma->ew_surface_notch))) {
        *reason = "fp16-brdma-dma-layout";
        return false;
    }
    return true;
}

static RockchipRKNNDpuRdmaFP16EWMode rockchip_rknn_dpu_fp16_ew_mode(
    const RockchipRKNNDPUConfig *dpu,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const uint32_t supported = ROCKCHIP_RKNN_DPU_EW_OP_SOURCE |
        ROCKCHIP_RKNN_DPU_EW_LUT_BYPASS |
        ROCKCHIP_RKNN_DPU_EW_OP_CVT_BYPASS |
        ROCKCHIP_RKNN_DPU_EW_RELU_BYPASS |
        MAKE_64BIT_MASK(ROCKCHIP_RKNN_DPU_EW_ALU_ALGO_SHIFT,
                        ROCKCHIP_RKNN_DPU_EW_ALU_ALGO_LENGTH) |
        MAKE_64BIT_MASK(ROCKCHIP_RKNN_DPU_EW_DATA_SIZE_SHIFT,
                        ROCKCHIP_RKNN_DPU_EW_DATA_SIZE_LENGTH) |
        MAKE_64BIT_MASK(ROCKCHIP_RKNN_DPU_EW_DATA_MODE_SHIFT,
                        ROCKCHIP_RKNN_DPU_EW_DATA_MODE_LENGTH);
    unsigned int algorithm;

    if (dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_BYPASS) {
        return ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_BYPASS;
    }
    if (rockchip_rknn_dpu_fp16_ew_add_is_supported(dpu, stage)) {
        return ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_ADD;
    }
    if ((dpu->ew_cfg & ~supported) ||
        !(dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_OP_SOURCE) ||
        !(dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_LUT_BYPASS) ||
        !(dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_RELU_BYPASS) ||
        extract32(dpu->ew_cfg, ROCKCHIP_RKNN_DPU_EW_DATA_SIZE_SHIFT,
                  ROCKCHIP_RKNN_DPU_EW_DATA_SIZE_LENGTH) != 2) {
        return ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_INVALID;
    }
    algorithm = extract32(dpu->ew_cfg,
                          ROCKCHIP_RKNN_DPU_EW_ALU_ALGO_SHIFT,
                          ROCKCHIP_RKNN_DPU_EW_ALU_ALGO_LENGTH);
    if (algorithm == 4 &&
        !(dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_OP_CVT_BYPASS) &&
        !stage->ew_cvt_offset && stage->ew_cvt_scale == 1 &&
        extract32(dpu->ew_cfg, ROCKCHIP_RKNN_DPU_EW_DATA_MODE_SHIFT,
                  ROCKCHIP_RKNN_DPU_EW_DATA_MODE_LENGTH) <= 1) {
        return ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_SUBTRACT;
    }
    if (algorithm == 3 &&
        (dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_OP_CVT_BYPASS) &&
        extract32(dpu->ew_cfg, ROCKCHIP_RKNN_DPU_EW_DATA_MODE_SHIFT,
                  ROCKCHIP_RKNN_DPU_EW_DATA_MODE_LENGTH) <= 1) {
        return ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_DIVIDE;
    }
    return ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_INVALID;
}

static bool rockchip_rknn_dpu_rdma_fp16_pipeline_is_supported(
    const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    uint64_t spatial = (uint64_t)dpu->output.width * dpu->output.height;
    RockchipRKNNDpuRdmaFP16EWMode mode =
        rockchip_rknn_dpu_fp16_ew_mode(dpu, stage);
    bool ew_enabled = mode != ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_BYPASS;
    bool ew_dual_port = extract32(
        dpu->ew_cfg, ROCKCHIP_RKNN_DPU_EW_DATA_MODE_SHIFT,
        ROCKCHIP_RKNN_DPU_EW_DATA_MODE_LENGTH) == 1;
    bool fp16_to_fp32 = rdma->feature_mode &
                        ROCKCHIP_RKNN_DPU_RDMA_FEATURE_FP16_TO_FP32;
    bool layout = dpu->output.width && dpu->output.height &&
        dpu->output_channels_valid &&
        dpu->output_channels_valid <= dpu->output.channels &&
        dpu->wdma_channels == dpu->output.channels &&
        dpu->wdma_width == rdma->width &&
        dpu->wdma_height == rdma->height &&
        (rdma->width == dpu->output.width || rdma->width == 1) &&
        (rdma->height == dpu->output.height || rdma->height == 1) &&
        rdma->channels == dpu->output.channels &&
        rockchip_rknn_strided_output_layout_valid(
            &dpu->output, sizeof(uint16_t));
    bool dpu_feature = rockchip_rknn_dpu_feature_mode_is_supported(
            dpu->feature_mode, 0, true) &&
        dpu->data_format == 0x48000002 && !dpu->minmax_ctl &&
        dpu->dst_dma_cfg == 2;
    bool dpu_stages = rockchip_rknn_dpu_fp16_bs_is_supported(dpu, stage) &&
        dpu->bs_ow_cfg == 2 && !dpu->bs_ow_op &&
        (dpu->bn_cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS);
    bool dpu_output = !dpu->out_cvt_offset && dpu->out_cvt_scale == 1 &&
        !dpu->out_cvt_shift && !dpu->out_cvt_minus_exp &&
        !dpu->out_cvt_type &&
        dpu->out_fp32_to_fp16 == !!fp16_to_fp32 &&
        !stage->out_cvt_round && !dpu->lut_cfg && !dpu->lut_info &&
        !dpu->lut_le_start && !dpu->lut_le_end && !dpu->lut_lo_start &&
        !dpu->lut_lo_end && !dpu->lut_le_slope_scale &&
        !dpu->lut_le_slope_shift && !dpu->lut_lo_slope_scale &&
        !dpu->lut_lo_slope_shift &&
        rockchip_rknn_dpu_surface_add_matches_output(dpu);
    bool rdma_controls =
        rockchip_rknn_dpu_rdma_fp16_layout_is_supported(rdma) &&
        rockchip_rknn_dpu_rdma_fp16_padding_is_supported(rdma) &&
        rdma->erdma_cfg == (ew_dual_port ? 0x40000008 : ew_enabled ? 8 :
                            ROCKCHIP_RKNN_DPU_ERDMA_DISABLE);
    bool rdma_feature =
        !(rdma->feature_mode & ~MAKE_64BIT_MASK(0, 18)) &&
        (rdma->feature_mode & ROCKCHIP_RKNN_DPU_RDMA_FEATURE_FLYING_MODE) &&
        !(rdma->feature_mode & ROCKCHIP_RKNN_DPU_RDMA_FEATURE_MAIN_DISABLE) &&
        !extract32(rdma->feature_mode,
                   ROCKCHIP_RKNN_DPU_RDMA_FEATURE_CONV_MODE_SHIFT,
                   ROCKCHIP_RKNN_DPU_RDMA_FEATURE_CONV_MODE_LENGTH) &&
        extract32(rdma->feature_mode,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_PROCESS_PRECISION_SHIFT,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_PROCESS_PRECISION_LENGTH) ==
            2 &&
        !extract32(rdma->feature_mode,
                   ROCKCHIP_RKNN_DPU_RDMA_FEATURE_COMBINATION_SHIFT,
                   ROCKCHIP_RKNN_DPU_RDMA_FEATURE_COMBINATION_LENGTH) &&
        extract32(rdma->feature_mode,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_BURST_LENGTH_SHIFT,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_BURST_LENGTH_LENGTH) ==
            0xf &&
        extract32(rdma->feature_mode,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_INPUT_PRECISION_SHIFT,
                  ROCKCHIP_RKNN_DPU_RDMA_FEATURE_INPUT_PRECISION_LENGTH) ==
            2 &&
        fp16_to_fp32 ==
            (mode != ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_DIVIDE);
    uint64_t ew_surface_notch =
        (uint64_t)rdma->ew_surface_stride - spatial;
    bool ew_layout = !ew_enabled ||
        (rdma->ew_surface_stride >= spatial &&
         ew_surface_notch <= UINT32_MAX &&
         rdma->ew_surface_notch == ew_surface_notch &&
         (ew_dual_port || !ew_surface_notch));

    trace_rockchip_rknn_pipeline_rdma_fp16_gate(
        mode != ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_INVALID, layout,
        dpu_feature, dpu_stages, dpu_output, rdma_controls, rdma_feature,
        ew_layout);
    return mode != ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_INVALID && layout &&
           dpu_feature && dpu_stages && dpu_output && rdma_controls &&
           rdma_feature && ew_layout;
}

static bool rockchip_rknn_dpu_rdma_fp16_to_int8_combine_is_supported(
    const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    uint32_t rdma_feature = rdma->feature_mode;
    uint64_t spatial = (uint64_t)dpu->output.width * dpu->output.height;

    return spatial && spatial <= UINT32_MAX / 2 &&
           !(dpu->output.channels % 16) &&
           dpu->output_channels_valid &&
           dpu->output_channels_valid <= dpu->output.channels &&
           dpu->wdma_channels == dpu->output.channels &&
           dpu->wdma_width == dpu->output.width &&
           dpu->wdma_height == dpu->output.height &&
           rdma->width == dpu->output.width &&
           rdma->height == dpu->output.height &&
           rdma->channels == dpu->output.channels &&
           rockchip_rknn_strided_output_layout_valid(&dpu->output, 1) &&
           (dpu->feature_mode & ROCKCHIP_RKNN_DPU_FEATURE_MODE_COMB_USE) &&
           rockchip_rknn_dpu_feature_mode_is_supported(
               dpu->feature_mode & ~ROCKCHIP_RKNN_DPU_FEATURE_MODE_COMB_USE,
               0, true) &&
           dpu->data_format == 0x08000002 && !dpu->offset_pend &&
           !dpu->minmax_ctl && dpu->dst_dma_cfg == 2 &&
           rockchip_rknn_dpu_fp16_bs_is_supported(dpu, stage) &&
           dpu->bs_ow_cfg == 2 && !dpu->bs_ow_op &&
           rockchip_rknn_dpu_fp16_stage_is_supported(
               dpu->bn_cfg, stage->bn_mul_cfg, false) &&
           (dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_BYPASS) &&
           !dpu->out_cvt_offset && dpu->out_cvt_scale == 1 &&
           !dpu->out_cvt_shift && !dpu->out_cvt_minus_exp &&
           !dpu->out_cvt_type && dpu->out_fp32_to_fp16 &&
           !stage->out_cvt_round && !dpu->lut_cfg && !dpu->lut_info &&
           !dpu->lut_le_start && !dpu->lut_le_end &&
           !dpu->lut_lo_start && !dpu->lut_lo_end &&
           !dpu->lut_le_slope_scale && !dpu->lut_le_slope_shift &&
           !dpu->lut_lo_slope_scale && !dpu->lut_lo_slope_shift &&
           rockchip_rknn_dpu_surface_add_matches_output(dpu) &&
           !rdma->brdma_cfg && !rdma->nrdma_cfg &&
           rdma->erdma_cfg == 0x40000008 && !rdma->src_dma_cfg &&
           !rdma->pad_cfg && rdma->weight == 0x01010101 &&
           rdma->surface_notch == spatial &&
           rdma->ew_surface_stride == spatial * 2 &&
           rdma->ew_surface_notch == spatial &&
           !(rdma_feature & ~MAKE_64BIT_MASK(0, 18)) &&
           (rdma_feature & ROCKCHIP_RKNN_DPU_RDMA_FEATURE_FLYING_MODE) &&
           (rdma_feature & ROCKCHIP_RKNN_DPU_RDMA_FEATURE_FP16_TO_FP32) &&
           !(rdma_feature & ROCKCHIP_RKNN_DPU_RDMA_FEATURE_MAIN_DISABLE) &&
           !extract32(rdma_feature,
                      ROCKCHIP_RKNN_DPU_RDMA_FEATURE_CONV_MODE_SHIFT,
                      ROCKCHIP_RKNN_DPU_RDMA_FEATURE_CONV_MODE_LENGTH) &&
           extract32(rdma_feature,
                     ROCKCHIP_RKNN_DPU_RDMA_FEATURE_PROCESS_PRECISION_SHIFT,
                     ROCKCHIP_RKNN_DPU_RDMA_FEATURE_PROCESS_PRECISION_LENGTH) ==
               2 &&
           extract32(rdma_feature,
                     ROCKCHIP_RKNN_DPU_RDMA_FEATURE_COMBINATION_SHIFT,
                     ROCKCHIP_RKNN_DPU_RDMA_FEATURE_COMBINATION_LENGTH) == 3 &&
           extract32(rdma_feature,
                     ROCKCHIP_RKNN_DPU_RDMA_FEATURE_BURST_LENGTH_SHIFT,
                     ROCKCHIP_RKNN_DPU_RDMA_FEATURE_BURST_LENGTH_LENGTH) ==
               0xf &&
           extract32(rdma_feature,
                     ROCKCHIP_RKNN_DPU_RDMA_FEATURE_INPUT_PRECISION_SHIFT,
                     ROCKCHIP_RKNN_DPU_RDMA_FEATURE_INPUT_PRECISION_LENGTH) ==
               2;
}

static bool rockchip_rknn_dpu_rdma_fp16_minmax_is_supported(
    const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    uint32_t surface_length = extract32(
        dpu->feature_mode, ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_SHIFT,
        ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_LENGTH);
    uint32_t expected_surface_length = DIV_ROUND_UP(dpu->output.width, 8);
    uint32_t feature_controls = ROCKCHIP_RKNN_DPU_FEATURE_MODE_FLYING_MODE |
        ROCKCHIP_RKNN_DPU_FEATURE_MODE_NONALIGN |
        MAKE_64BIT_MASK(ROCKCHIP_RKNN_DPU_FEATURE_MODE_OUTPUT_MODE_SHIFT,
                        ROCKCHIP_RKNN_DPU_FEATURE_MODE_OUTPUT_MODE_LENGTH) |
        MAKE_64BIT_MASK(ROCKCHIP_RKNN_DPU_FEATURE_MODE_CONV_MODE_SHIFT,
                        ROCKCHIP_RKNN_DPU_FEATURE_MODE_CONV_MODE_LENGTH) |
        MAKE_64BIT_MASK(ROCKCHIP_RKNN_DPU_FEATURE_MODE_BURST_LEN_SHIFT,
                        ROCKCHIP_RKNN_DPU_FEATURE_MODE_BURST_LEN_LENGTH) |
        MAKE_64BIT_MASK(ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_SHIFT,
                        ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_LENGTH);

    return !(dpu->feature_mode & ~feature_controls) &&
           (dpu->feature_mode &
            ROCKCHIP_RKNN_DPU_FEATURE_MODE_FLYING_MODE) &&
           (dpu->feature_mode & ROCKCHIP_RKNN_DPU_FEATURE_MODE_NONALIGN) &&
           extract32(dpu->feature_mode,
                     ROCKCHIP_RKNN_DPU_FEATURE_MODE_OUTPUT_MODE_SHIFT,
                     ROCKCHIP_RKNN_DPU_FEATURE_MODE_OUTPUT_MODE_LENGTH) ==
               ROCKCHIP_RKNN_DPU_FEATURE_MODE_SUPPORTED_OUTPUT_MODE &&
           !extract32(dpu->feature_mode,
                      ROCKCHIP_RKNN_DPU_FEATURE_MODE_CONV_MODE_SHIFT,
                      ROCKCHIP_RKNN_DPU_FEATURE_MODE_CONV_MODE_LENGTH) &&
           extract32(dpu->feature_mode,
                     ROCKCHIP_RKNN_DPU_FEATURE_MODE_BURST_LEN_SHIFT,
                     ROCKCHIP_RKNN_DPU_FEATURE_MODE_BURST_LEN_LENGTH) ==
               0xf &&
           surface_length == MAX(1U, expected_surface_length) &&
           dpu->data_format == 0x48000002 && dpu->output.width &&
           dpu->output.height &&
           dpu->output.channels && dpu->output_channels_valid &&
           dpu->output_channels_valid <= dpu->output.channels &&
           !dpu->output.surface_stride &&
           dpu->offset_pend && !(dpu->minmax_ctl & ~7U) &&
           (dpu->minmax_ctl & BIT(0)) && dpu->dst_dma_cfg == 2 &&
           !dpu->output_notch_0 && !dpu->output_notch_1 &&
           dpu->wdma_channels == dpu->output.channels &&
           dpu->wdma_width == dpu->output.width &&
           dpu->wdma_height == dpu->output.height &&
           (dpu->bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) &&
           dpu->bs_ow_cfg == 2 &&
           !dpu->bs_ow_op &&
           (dpu->bn_cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) &&
           (dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_BYPASS) &&
           !dpu->out_cvt_offset &&
           dpu->out_cvt_scale == 1 && !dpu->out_cvt_shift &&
           !dpu->out_cvt_minus_exp &&
           stage->out_cvt_round && !dpu->out_cvt_type &&
           !dpu->out_fp32_to_fp16 &&
           !dpu->lut_cfg && !dpu->lut_info && !dpu->lut_le_start &&
           !dpu->lut_le_end && !dpu->lut_lo_start && !dpu->lut_lo_end &&
           !dpu->lut_le_slope_scale && !dpu->lut_le_slope_shift &&
           !dpu->lut_lo_slope_scale && !dpu->lut_lo_slope_shift &&
           !dpu->surface_add && rdma->width == dpu->output.width &&
           rdma->height == dpu->output.height &&
           rdma->channels == dpu->output.channels && !rdma->brdma_cfg &&
           !rdma->nrdma_cfg &&
           rdma->erdma_cfg == (ROCKCHIP_RKNN_DPU_ERDMA_DISABLE | BIT(1)) &&
           rdma->feature_mode == 0x17841 && !rdma->src_dma_cfg &&
           !rdma->pad_cfg && rdma->weight == 0x01010101 &&
           !rdma->ew_surface_stride && !rdma->ew_surface_notch;
}

static bool rockchip_rknn_dpu_rdma_raw16_tensor_is_supported(
    const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    uint64_t spatial = (uint64_t)dpu->output.width * dpu->output.height;
    uint32_t line_notch = extract32(rdma->src_dma_cfg, 19, 13);
    int64_t signed_surface_notch = sextract32(rdma->surface_notch, 0, 28);
    int64_t surface_atoms = spatial +
        (uint64_t)line_notch * (rdma->height - 1) +
        signed_surface_notch;
    uint64_t output_atoms = spatial +
        (uint64_t)dpu->output_notch_0 * (dpu->output.height - 1);
    const uint32_t rdma_feature_controls = MAKE_64BIT_MASK(0, 18);
    const uint32_t surface_length_mask = MAKE_64BIT_MASK(
        ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_SHIFT,
        ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_LENGTH);
    uint32_t surface_length = extract32(
        dpu->feature_mode, ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_SHIFT,
        ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_LENGTH);
    uint32_t surfaces = DIV_ROUND_UP(dpu->output.channels,
                                     dpu->output.atom);
    bool transpose = dpu->feature_mode &
                     ROCKCHIP_RKNN_DPU_FEATURE_MODE_TP_EN;
    bool reshape = !transpose && surface_length;
    bool compact_reorder = transpose || reshape;
    bool reshape_collapsed_source =
        line_notch + rdma->width >= surfaces &&
        surface_atoms == rdma->width;
    bool reshape_compact_source = !line_notch && !signed_surface_notch &&
                                  surface_atoms == spatial;
    bool reshape_channel_major =
        dpu->wdma_tp_precision &&
        dpu->output.surface_stride == dpu->wdma_width &&
        (dpu->wdma_width == 1 ?
         (reshape_collapsed_source ?
          dpu->output_notch_0 + dpu->output.width ==
              (uint64_t)(rdma->width + line_notch) * dpu->output.atom :
          dpu->output_notch_0 == dpu->output.channels - 1) :
         !dpu->output_notch_0) &&
        dpu->surface_add ==
            (uint64_t)dpu->wdma_width * dpu->output.atom * 16;
    bool reshape_wdma_tiled =
        !dpu->wdma_tp_precision &&
        dpu->wdma_width <= dpu->output.atom &&
        dpu->output.surface_stride == dpu->wdma_width &&
        dpu->output.width + dpu->output_notch_0 == dpu->output.atom &&
        dpu->surface_add ==
            (uint64_t)dpu->wdma_width * dpu->wdma_height *
            dpu->output.atom * 16;
    bool reshape_spatial_major =
        !dpu->wdma_tp_precision &&
        dpu->output.surface_stride == dpu->wdma_size_c + 1 &&
        dpu->output_notch_0 + dpu->wdma_width ==
            (uint64_t)(dpu->wdma_size_c + 1) * dpu->output.atom &&
        dpu->surface_add == (uint64_t)dpu->wdma_width * 16;
    uint64_t reshape_wdma_elements =
        (uint64_t)dpu->wdma_width * dpu->wdma_height *
        (dpu->wdma_size_c + 1) * dpu->output.atom;

    return rockchip_rknn_dpu_feature_mode_is_supported(
               dpu->feature_mode &
                   ~(ROCKCHIP_RKNN_DPU_FEATURE_MODE_TP_EN |
                     surface_length_mask), 0, true) &&
           dpu->data_format == 0x24000001 &&
           dpu->output.width == rdma->width &&
           dpu->output.height == rdma->height &&
           dpu->output.channels == rdma->channels &&
           dpu->output.atom == 8 && dpu->output_channels_valid &&
           dpu->output_channels_valid <= dpu->output.channels &&
           !dpu->minmax_ctl &&
           dpu->output_notch_0 == dpu->output_notch_1 &&
           dpu->dst_dma_cfg == (transpose ? 0x7fe :
                                reshape ? 0x080007fe : 2) &&
           dpu->wdma_channels == (compact_reorder ? dpu->output.channels :
               ROUND_UP(dpu->output.channels, dpu->output.atom)) &&
           (!reshape || reshape_wdma_elements == surface_length) &&
           (transpose ?
            (dpu->wdma_width == dpu->output.height &&
             dpu->wdma_height == surfaces) :
            (reshape ||
             (dpu->wdma_width == dpu->output.width &&
              dpu->wdma_height == dpu->output.height))) &&
           (reshape || dpu->wdma_size_c ==
               (!compact_reorder &&
                dpu->output.channels < dpu->output.atom ?
                    dpu->output.channels - 1 : 0)) &&
           dpu->wdma_tp_precision ==
               (transpose || (reshape && reshape_channel_major)) &&
           rockchip_rknn_dpu_stage_is_supported(
               dpu->bs_cfg, stage->bs_mul_cfg, false, false) &&
           dpu->bs_ow_cfg == (transpose ? 0x7fe :
                              reshape ? 0x080007fe : 2) &&
           !dpu->bs_ow_op &&
           rockchip_rknn_dpu_stage_is_supported(
               dpu->bn_cfg, stage->bn_mul_cfg, false, false) &&
           (dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_BYPASS) &&
           !dpu->out_cvt_offset && dpu->out_cvt_scale == 1 &&
           !dpu->out_cvt_shift && !dpu->out_cvt_minus_exp &&
           !dpu->out_cvt_type && !dpu->out_fp32_to_fp16 &&
           !stage->out_cvt_round && !dpu->lut_cfg && !dpu->lut_info &&
           !dpu->lut_le_start && !dpu->lut_le_end &&
           !dpu->lut_lo_start && !dpu->lut_lo_end &&
           !dpu->lut_le_slope_scale && !dpu->lut_le_slope_shift &&
           !dpu->lut_lo_slope_scale && !dpu->lut_lo_slope_shift &&
           (compact_reorder ?
            ((transpose ?
              (dpu->output_notch_0 ==
                   dpu->output.height * (dpu->output.width - 1) &&
               dpu->output.surface_stride == dpu->output.height &&
               dpu->surface_add == spatial * 16) :
              (reshape_channel_major || reshape_wdma_tiled ||
               reshape_spatial_major)) &&
             (transpose ?
              (line_notch + rdma->width == rdma->channels &&
               surface_atoms == rdma->width) :
              ((reshape_channel_major || reshape_wdma_tiled ||
                reshape_spatial_major) &&
               (reshape_collapsed_source || reshape_compact_source))) &&
             surface_length == (reshape ? spatial * surfaces : 0) &&
             (!reshape ||
              (surface_length >= dpu->output.channels &&
               !(surface_length % dpu->output.channels)))) :
            (rockchip_rknn_dpu_surface_add_matches_output(dpu) &&
             surface_atoms == dpu->output.surface_stride &&
             output_atoms <= dpu->output.surface_stride)) &&
           !rdma->brdma_cfg &&
           rdma->erdma_cfg == ROCKCHIP_RKNN_DPU_ERDMA_DISABLE &&
           !(rdma->feature_mode & ~rdma_feature_controls) &&
           (rdma->feature_mode &
            ROCKCHIP_RKNN_DPU_RDMA_FEATURE_FLYING_MODE) &&
           !(rdma->feature_mode &
             ROCKCHIP_RKNN_DPU_RDMA_FEATURE_MAIN_DISABLE) &&
           !extract32(rdma->feature_mode,
                      ROCKCHIP_RKNN_DPU_RDMA_FEATURE_CONV_MODE_SHIFT,
                      ROCKCHIP_RKNN_DPU_RDMA_FEATURE_CONV_MODE_LENGTH) &&
           extract32(rdma->feature_mode,
                     ROCKCHIP_RKNN_DPU_RDMA_FEATURE_PROCESS_PRECISION_SHIFT,
                     ROCKCHIP_RKNN_DPU_RDMA_FEATURE_PROCESS_PRECISION_LENGTH) ==
               1 &&
           !extract32(rdma->feature_mode,
                      ROCKCHIP_RKNN_DPU_RDMA_FEATURE_COMBINATION_SHIFT,
                      ROCKCHIP_RKNN_DPU_RDMA_FEATURE_COMBINATION_LENGTH) &&
           extract32(rdma->feature_mode,
                     ROCKCHIP_RKNN_DPU_RDMA_FEATURE_BURST_LENGTH_SHIFT,
                     ROCKCHIP_RKNN_DPU_RDMA_FEATURE_BURST_LENGTH_LENGTH) ==
               0xf &&
           extract32(rdma->feature_mode,
                     ROCKCHIP_RKNN_DPU_RDMA_FEATURE_INPUT_PRECISION_SHIFT,
                     ROCKCHIP_RKNN_DPU_RDMA_FEATURE_INPUT_PRECISION_LENGTH) ==
               1 &&
           !(rdma->src_dma_cfg & MAKE_64BIT_MASK(0, 19)) &&
           !rdma->pad_cfg &&
           rdma->weight == 0x01010101 && !rdma->ew_surface_stride &&
           !rdma->ew_surface_notch;
}

static bool rockchip_rknn_dpu_rdma_raw16_tail_is_supported(
    const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    uint64_t spatial = (uint64_t)dpu->output.width * dpu->output.height;
    uint64_t plane_stride = MAX(1, DIV_ROUND_UP(spatial, 8));
    uint64_t surfaces = DIV_ROUND_UP(dpu->output.channels, 8);
    const uint32_t surface_length_mask = MAKE_64BIT_MASK(
        ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_SHIFT,
        ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_LENGTH);
    uint32_t surface_length = extract32(
        dpu->feature_mode, ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_SHIFT,
        ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_LENGTH);
    uint64_t wdma_elements = (uint64_t)dpu->wdma_width *
                             dpu->wdma_height *
                             (dpu->wdma_size_c + 1) * dpu->output.atom;
    bool vector_layout = dpu->output.width == 1 &&
                         dpu->output.height == 1 &&
                         dpu->surface_add == 16;

    return (dpu->feature_mode & ~surface_length_mask) == 0x1e5 &&
           surface_length == (vector_layout ? 0 : spatial * surfaces) &&
           wdma_elements == (vector_layout ? dpu->output.atom :
                                             surface_length) &&
           dpu->data_format == 0x24000001 && dpu->output.width &&
           dpu->output.height && dpu->output.channels >= 8 &&
           dpu->output_channels_valid &&
           dpu->output_channels_valid <= dpu->output.channels &&
           (spatial != 1 || dpu->output_channels_valid == 1) &&
           dpu->output.surface_stride == plane_stride && !dpu->minmax_ctl &&
           !dpu->output_notch_0 && !dpu->output_notch_1 &&
           dpu->dst_dma_cfg == (vector_layout ? 2 : 0x080007fe) &&
           dpu->wdma_channels == (vector_layout ? 8 : dpu->output.channels) &&
           dpu->wdma_width == (vector_layout ? 1 : dpu->output.height) &&
           dpu->wdma_height == (vector_layout ? 1 :
                                DIV_ROUND_UP(dpu->output.width, 8)) &&
           dpu->wdma_size_c == (vector_layout ? 0 : surfaces - 1) &&
           dpu->wdma_tp_precision == !vector_layout &&
           (dpu->bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) &&
           dpu->bs_ow_cfg == (vector_layout ? 2 : 0x080007fe) &&
           !dpu->bs_ow_op &&
           (dpu->bn_cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) &&
           (dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_BYPASS) &&
           !dpu->out_cvt_offset &&
           dpu->out_cvt_scale == 1 && !dpu->out_cvt_shift &&
           !dpu->out_cvt_minus_exp && !dpu->out_cvt_type &&
           !dpu->out_fp32_to_fp16 && !stage->out_cvt_round &&
           (vector_layout ||
            dpu->surface_add == plane_stride * 16 * dpu->output.atom) &&
           rdma->width == (vector_layout ? 1 : dpu->output.width) &&
           rdma->height == (vector_layout ? 1 : dpu->output.height) &&
           rdma->channels == (vector_layout ? 8 : dpu->output.channels) &&
           !rdma->brdma_cfg &&
           rdma->erdma_cfg == 1 &&
           rdma->feature_mode == 0xf821 && !rdma->src_dma_cfg &&
           !rdma->surface_notch && !rdma->pad_cfg &&
           rdma->weight == 0x01010101;
}

static bool rockchip_rknn_dpu_rdma_raw16_compact_u8_is_supported(
    const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const RockchipRKNNDPUConfig *dpu = &task->dpu;
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    uint64_t spatial = (uint64_t)dpu->output.width * dpu->output.height;
    uint64_t surfaces = DIV_ROUND_UP(dpu->output.channels, 8);
    const uint32_t layout_mask =
        ROCKCHIP_RKNN_DPU_FEATURE_MODE_NONALIGN |
        MAKE_64BIT_MASK(ROCKCHIP_RKNN_DPU_FEATURE_MODE_RGP_TYPE_SHIFT,
                        ROCKCHIP_RKNN_DPU_FEATURE_MODE_RGP_TYPE_LENGTH) |
        MAKE_64BIT_MASK(ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_SHIFT,
                        ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_LENGTH);
    uint32_t surface_length = extract32(
        dpu->feature_mode, ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_SHIFT,
        ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_LENGTH);

    return (dpu->feature_mode & ~layout_mask) == 0x1e5 &&
           (dpu->feature_mode &
            ROCKCHIP_RKNN_DPU_FEATURE_MODE_NONALIGN) &&
           extract32(dpu->feature_mode,
                     ROCKCHIP_RKNN_DPU_FEATURE_MODE_RGP_TYPE_SHIFT,
                     ROCKCHIP_RKNN_DPU_FEATURE_MODE_RGP_TYPE_LENGTH) == 2 &&
           surface_length == DIV_ROUND_UP(spatial * surfaces, 16) &&
           dpu->data_format == 0x24000001 && dpu->output.width &&
           dpu->output.height && dpu->output.channels &&
           !(dpu->output.channels % 8) && dpu->output_channels_valid &&
           dpu->output_channels_valid <= dpu->output.channels &&
           !dpu->output.surface_stride && !dpu->minmax_ctl &&
           !dpu->output_notch_0 && !dpu->output_notch_1 &&
           dpu->dst_dma_cfg == 2 &&
           dpu->wdma_channels == dpu->output.channels &&
           dpu->wdma_width == dpu->output.width &&
           dpu->wdma_height == dpu->output.height &&
           !dpu->wdma_size_c && !dpu->wdma_tp_precision &&
           rockchip_rknn_dpu_stage_is_supported(
               dpu->bs_cfg, stage->bs_mul_cfg, false, false) &&
           dpu->bs_ow_cfg == 2 && !dpu->bs_ow_op &&
           rockchip_rknn_dpu_stage_is_supported(
               dpu->bn_cfg, stage->bn_mul_cfg, false, false) &&
           (dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_BYPASS) &&
           !dpu->out_cvt_offset && dpu->out_cvt_scale == 1 &&
           !dpu->out_cvt_shift && !dpu->out_cvt_minus_exp &&
           !dpu->out_cvt_type && !dpu->out_fp32_to_fp16 &&
           !stage->out_cvt_round && !dpu->lut_cfg && !dpu->lut_info &&
           !dpu->lut_le_start && !dpu->lut_le_end &&
           !dpu->lut_lo_start && !dpu->lut_lo_end &&
           !dpu->lut_le_slope_scale && !dpu->lut_le_slope_shift &&
           !dpu->lut_lo_slope_scale && !dpu->lut_lo_slope_shift &&
           !dpu->surface_add && rdma->width == dpu->output.width &&
           rdma->height == dpu->output.height &&
           rdma->channels == dpu->output.channels && !rdma->brdma_cfg &&
           rdma->erdma_cfg == ROCKCHIP_RKNN_DPU_ERDMA_DISABLE &&
           rdma->feature_mode == 0xf821 && !rdma->src_dma_cfg &&
           !rdma->surface_notch && !rdma->pad_cfg &&
           rdma->weight == 0x01010101 && !rdma->ew_surface_stride &&
           !rdma->ew_surface_notch;
}

static unsigned int rockchip_rknn_co_work_core_count(uint8_t mode)
{
    switch (mode) {
    case 0:
        return 1;
    case ROCKCHIP_RKNN_CO_WORK_64X32:
    case ROCKCHIP_RKNN_CO_WORK_32X64:
        return 2;
    case ROCKCHIP_RKNN_CO_WORK_96X32:
    case ROCKCHIP_RKNN_CO_WORK_32X96:
        return 3;
    default:
        return 0;
    }
}

static RockchipRKNNExecutionMode rockchip_rknn_execution_mode(
    const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage, const char **reason)
{
    const uint32_t dpu_blocks = ROCKCHIP_RKNN_BLOCK_CNA |
                                ROCKCHIP_RKNN_BLOCK_CORE |
                                ROCKCHIP_RKNN_BLOCK_DPU;
    const RockchipRKNNPPUConfig *ppu = &task->ppu;
    bool dpu_rdma_enabled;
    bool erdma_disabled;
    bool bs_rdma_available;
    bool bn_rdma_available;
    bool ew_rdma_available;
    bool int8_depthwise_deconv;
    unsigned int brdma_data_use;
    unsigned int nrdma_data_use;
    unsigned int kernel_width;
    unsigned int kernel_height;
    unsigned int stride_width;
    unsigned int stride_height;
    bool fp16;

    *reason = "unsupported-control";
    if ((task->enabled_blocks & ROCKCHIP_RKNN_BLOCK_CNA) &&
        !rockchip_rknn_co_work_core_count(task->cna.co_work_mode)) {
        *reason = "cna-co-work-mode";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (task->enabled_blocks == (ROCKCHIP_RKNN_BLOCK_PPU |
                                 ROCKCHIP_RKNN_BLOCK_PPU_RDMA)) {
        fp16 = extract32(ppu->data_format, 0, 3) == 2 &&
               !(ppu->data_format & BIT(3)) &&
               ppu->rdma_data_format == 2;
        if ((!fp16 && (extract32(ppu->data_format, 0, 3) != 0 ||
                       (ppu->data_format & BIT(3)) ||
                       ppu->rdma_data_format != 1)) ||
            ppu->misc_ctrl != 3) {
            *reason = "ppu-format";
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
        if (ppu->rdma_in_width != ppu->in_width ||
            ppu->rdma_in_height != ppu->in_height ||
            ppu->rdma_in_channels != ppu->in_channels ||
            ppu->out_channels != ppu->in_channels) {
            *reason = "ppu-layout";
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
        if (ppu->padding_value_0 || ppu->padding_value_1) {
            *reason = "ppu-padding-value";
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
        if (!(ppu->mode & ROCKCHIP_RKNN_PPU_FLYING_MODE) ||
            (ppu->mode & ROCKCHIP_RKNN_PPU_INDEX_ENABLE) ||
            extract32(ppu->mode,
                      ROCKCHIP_RKNN_PPU_POOLING_METHOD_SHIFT,
                      ROCKCHIP_RKNN_PPU_POOLING_METHOD_LENGTH) != 1) {
            *reason = "ppu-mode";
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
        kernel_width = extract32(
            ppu->kernel, ROCKCHIP_RKNN_PPU_KERNEL_WIDTH_SHIFT,
            ROCKCHIP_RKNN_PPU_KERNEL_FIELD_LENGTH) + 1;
        kernel_height = extract32(
            ppu->kernel, ROCKCHIP_RKNN_PPU_KERNEL_HEIGHT_SHIFT,
            ROCKCHIP_RKNN_PPU_KERNEL_FIELD_LENGTH) + 1;
        stride_width = extract32(
            ppu->kernel, ROCKCHIP_RKNN_PPU_KERNEL_STRIDE_WIDTH_SHIFT,
            ROCKCHIP_RKNN_PPU_KERNEL_FIELD_LENGTH) + 1;
        stride_height = extract32(
            ppu->kernel, ROCKCHIP_RKNN_PPU_KERNEL_STRIDE_HEIGHT_SHIFT,
            ROCKCHIP_RKNN_PPU_KERNEL_FIELD_LENGTH) + 1;
        if (kernel_width == 1 && kernel_height == 1 &&
            stride_width == 1 && stride_height == 1 && !ppu->padding &&
            ppu->out_width == ppu->in_width &&
            ppu->out_height == ppu->in_height) {
            *reason = "ppu-bypass";
            return ROCKCHIP_RKNN_EXECUTION_PPU_BYPASS;
        }
        *reason = fp16 ? "ppu-fp16-max-pool" : "ppu-int8-max-pool";
        return fp16 ? ROCKCHIP_RKNN_EXECUTION_PPU_FP16_MAX_POOL :
                      ROCKCHIP_RKNN_EXECUTION_PPU_INT8_MAX_POOL;
    }
    if (task->enabled_blocks == (ROCKCHIP_RKNN_BLOCK_DPU |
                                 ROCKCHIP_RKNN_BLOCK_DPU_RDMA)) {
        if (rockchip_rknn_dpu_rdma_fp16_to_int8_combine_is_supported(
                task, stage)) {
            *reason = "dpu-rdma-fp16-int8-combine";
            return ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_FP16_TO_INT8_COMBINE;
        }
        if (rockchip_rknn_dpu_rdma_int8_pipeline_is_supported(task,
                                                               stage)) {
            *reason = "dpu-rdma-int8-pipeline";
            return ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_INT8_PIPELINE;
        }
        if (rockchip_rknn_dpu_rdma_int16_unpool_is_supported(task, stage)) {
            *reason = "dpu-rdma-int16-unpool";
            return ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_INT16_UNPOOL;
        }
        if (rockchip_rknn_dpu_rdma_int8_to_fp16_is_supported(task)) {
            *reason = "dpu-rdma-int8-fp16";
            return ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_INT8_TO_FP16;
        }
        if (rockchip_rknn_dpu_rdma_fp16_pipeline_is_supported(task,
                                                               stage)) {
            *reason = "dpu-rdma-fp16-pipeline";
            return ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_FP16_PIPELINE;
        }
        if (rockchip_rknn_dpu_rdma_fp16_minmax_is_supported(task, stage)) {
            *reason = "dpu-rdma-fp16-minmax";
            return ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_FP16_MINMAX;
        }
        if (rockchip_rknn_dpu_rdma_raw16_tensor_is_supported(task, stage)) {
            *reason = "dpu-rdma-raw16-tensor";
            return ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_RAW16_TENSOR;
        }
        if (rockchip_rknn_dpu_rdma_raw16_compact_u8_is_supported(
                task, stage)) {
            *reason = "dpu-rdma-raw16-compact-u8";
            return ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_RAW16_COMPACT_U8;
        }
        if (rockchip_rknn_dpu_rdma_raw16_tail_is_supported(task, stage)) {
            *reason = "dpu-rdma-raw16-tail";
            return ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_RAW16_TAIL;
        }
        if (rockchip_rknn_dpu_rdma_fp16_lut_is_supported(task, stage)) {
            *reason = "dpu-rdma-fp16-lut";
            return ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_FP16_LUT;
        }
        *reason = "dpu-rdma-control";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (task->enabled_blocks != dpu_blocks &&
        task->enabled_blocks !=
            (dpu_blocks | ROCKCHIP_RKNN_BLOCK_DPU_RDMA)) {
        *reason = "block-combination";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (!rockchip_rknn_dpu_feature_mode_is_supported(
            task->dpu.feature_mode, task->core.depthwise ? 3 : 0, false)) {
        *reason = "dpu-feature-mode";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if ((task->dpu.offset_pend && task->dpu.output_precision != 2) ||
        ((task->dpu.output_notch_0 || task->dpu.output_notch_1) &&
         !rockchip_rknn_depthwise_int32_notched_output_is_supported(task) &&
         !rockchip_rknn_fp16_group_line_output_is_supported(task))) {
        *reason = "dpu-output-notch";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (task->dpu.minmax_ctl) {
        *reason = "dpu-minmax";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (!rockchip_rknn_pipeline_shape_is_well_formed(task)) {
        *reason = "dpu-shape";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (!task->cna.input.width || !task->cna.input.height ||
        !task->cna.kernel_width || !task->cna.kernel_height ||
        !task->cna.stride_x || !task->cna.stride_y) {
        *reason = "cna-shape";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (task->cna.output_width != task->core.width ||
        task->cna.output_atomics !=
            (uint64_t)task->core.width * task->core.height) {
        *reason = "cna-output-layout";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (!rockchip_rknn_cna_storage_layout_is_supported(&task->cna)) {
        *reason = "cna-storage-layout";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (task->core.width != task->dpu.output.width ||
        task->core.height != task->dpu.output.height ||
        (task->core.channels != task->dpu.output.channels &&
         !rockchip_rknn_fp16_compact_output_layout_is_supported(task))) {
        *reason = "dpu-core-layout";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if ((task->dpu.wdma_channels != task->dpu.output.channels ||
         task->dpu.wdma_width != task->dpu.output.width ||
         task->dpu.wdma_height != task->dpu.output.height) &&
        !rockchip_rknn_depthwise_int32_flat_wdma_is_supported(task) &&
        !rockchip_rknn_fp16_compact_output_layout_is_supported(task) &&
        !rockchip_rknn_fp16_group_line_output_is_supported(task)) {
        *reason = "dpu-wdma-layout";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    fp16 = task->cna.input_precision == 2 &&
           task->cna.process_precision == 2 &&
           task->core.process_precision == 2 &&
           task->dpu.input_precision == 2 &&
           task->dpu.process_precision == 2 &&
           (task->dpu.output_precision == 2 ||
            task->dpu.output_precision == 5);
    if (!fp16 && (task->cna.input_precision ||
                  task->cna.process_precision ||
                  task->core.process_precision ||
                  task->dpu.input_precision ||
                  task->dpu.process_precision)) {
        *reason = "precision-or-conv-mode";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (fp16) {
        bool compact_weights = !task->core.quantify;
        bool compact_output = compact_weights &&
                              !task->dpu.output.surface_stride;
        bool fp16_brdma = task->enabled_blocks ==
            (dpu_blocks | ROCKCHIP_RKNN_BLOCK_DPU_RDMA);
        bool fp16_ew_rdma = fp16_brdma &&
                            rockchip_rknn_dpu_ew_uses_rdma(
                                task->dpu.ew_cfg);
        bool fp16_lut = !(task->dpu.ew_cfg &
                          ROCKCHIP_RKNN_DPU_EW_BYPASS) &&
                        !(task->dpu.ew_cfg &
                          ROCKCHIP_RKNN_DPU_EW_LUT_BYPASS);

        if ((task->enabled_blocks != dpu_blocks && !fp16_brdma) ||
            task->dpu.data_format & BIT(3) ||
            task->cna.kernel_groups != 1 ||
            (task->core.depthwise ? task->cna.conv_mode != 3 :
                                    task->cna.conv_mode) ||
            (task->core.depthwise && !compact_weights) ||
            task->cna.nn_mode ||
            task->cna.atrous_x_dilation || task->cna.atrous_y_dilation ||
            !rockchip_rknn_fp16_deconv_is_supported(task) ||
            task->cna.surface_mode ||
            task->cna.csc_weight_output_disable ||
            task->cna.csc_data_output_disable ||
            task->cna.cmd_fifo_soft_reset ||
            !rockchip_rknn_cna_input_mode_is_supported(&task->cna) ||
            (rockchip_rknn_cna_interleaved_input(&task->cna) &&
             !rockchip_rknn_cna_fp16_interleaved(&task->cna)) ||
            task->dpu.out_cvt_shift || task->dpu.out_cvt_type ||
            stage->out_cvt_round ||
            (task->dpu.offset_pend && task->dpu.output_precision != 2) ||
            task->dpu.out_fp32_to_fp16 != (task->dpu.output_precision == 2) ||
            task->cna.pad_value) {
            *reason = task->enabled_blocks != dpu_blocks && !fp16_brdma ?
                "fp16-blocks" :
                task->dpu.data_format & BIT(3) ? "fp16-format" :
                (task->core.depthwise && !compact_weights) ?
                "fp16-depthwise-weights" :
                !rockchip_rknn_fp16_deconv_is_supported(task) ?
                "fp16-deconv" :
                task->cna.kernel_groups != 1 ? "fp16-groups" :
                !rockchip_rknn_cna_input_mode_is_supported(&task->cna) ?
                "fp16-input-mode" :
                (rockchip_rknn_cna_interleaved_input(&task->cna) &&
                 !rockchip_rknn_cna_fp16_interleaved(&task->cna)) ?
                "fp16-interleave" : "fp16-control";
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
        if ((!compact_output &&
             !rockchip_rknn_strided_output_layout_valid(
                 &task->dpu.output, task->dpu.output_precision == 2 ?
                 sizeof(uint16_t) : sizeof(uint32_t))) ||
            (compact_output &&
             (task->dpu.output_precision != 2 ||
              ((task->core.channels != task->dpu.output.channels ||
                task->dpu.wdma_channels != task->dpu.output.channels) &&
               !rockchip_rknn_fp16_compact_output_layout_is_supported(
                   task))))) {
            *reason = "fp16-output-layout";
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
        if (fp16_brdma &&
            !rockchip_rknn_fp16_brdma_layout_is_supported(
                task, fp16_ew_rdma, reason)) {
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
        if (fp16_brdma &&
            !rockchip_rknn_dpu_stage_uses_alu_source(task->dpu.bs_cfg)) {
            *reason = "fp16-brdma-unused";
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
        if (!rockchip_rknn_dpu_fp16_stage_is_supported(
                task->dpu.bs_cfg, stage->bs_mul_cfg, fp16_brdma) ||
            !rockchip_rknn_dpu_fp16_stage_is_supported(
                task->dpu.bn_cfg, stage->bn_mul_cfg, false) ||
            (fp16_lut &&
             !rockchip_rknn_dpu_fp16_lut_is_supported(
                 &task->dpu, stage)) ||
            (!fp16_lut &&
             !(task->dpu.ew_cfg & ROCKCHIP_RKNN_DPU_EW_BYPASS) &&
             !rockchip_rknn_dpu_fp16_ew_add_is_supported(
                 &task->dpu, stage))) {
            *reason = "fp16-stage-mode";
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
        *reason = "dpu-fp16";
        return ROCKCHIP_RKNN_EXECUTION_DPU_FP16;
    }
    if ((task->core.depthwise && task->cna.conv_mode != 3) ||
        (!task->core.depthwise && task->cna.conv_mode) ||
        task->cna.kernel_groups != 1) {
        *reason = "depthwise-or-grouped";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    int8_depthwise_deconv = task->core.depthwise &&
        rockchip_rknn_deconv_shape_is_supported(task);
    if ((task->cna.deconv && !int8_depthwise_deconv) ||
        (!task->cna.deconv && (task->cna.deconv_stride_x ||
                               task->cna.deconv_stride_y)) ||
        task->cna.nn_mode ||
        task->cna.atrous_x_dilation || task->cna.atrous_y_dilation ||
        task->cna.surface_mode) {
        *reason = "cna-convolution-control";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (task->core.depthwise && task->dpu.output_precision != 0 &&
        !rockchip_rknn_depthwise_int32_layout(task, NULL)) {
        *reason = "depthwise-output-layout";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (task->cna.csc_weight_output_disable ||
        task->cna.csc_data_output_disable ||
        task->cna.cmd_fifo_soft_reset ||
        !rockchip_rknn_cna_input_mode_is_supported(&task->cna)) {
        *reason = task->cna.csc_weight_output_disable ?
            "cna-weight-output" : task->cna.csc_data_output_disable ?
            "cna-data-output" : task->cna.cmd_fifo_soft_reset ?
            "cna-fifo-reset" : "cna-input-mode";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    dpu_rdma_enabled = task->enabled_blocks & ROCKCHIP_RKNN_BLOCK_DPU_RDMA;
    erdma_disabled = task->dpu_rdma.erdma_cfg &
                     ROCKCHIP_RKNN_DPU_ERDMA_DISABLE;
    if (dpu_rdma_enabled &&
        (extract32(task->dpu_rdma.erdma_cfg,
                  ROCKCHIP_RKNN_DPU_ERDMA_DATA_SIZE_SHIFT,
                  ROCKCHIP_RKNN_DPU_ERDMA_DATA_SIZE_LENGTH) !=
            (erdma_disabled ? 0 : 1) ||
         extract32(task->dpu_rdma.erdma_cfg,
                  ROCKCHIP_RKNN_DPU_ERDMA_DATA_MODE_SHIFT,
                  ROCKCHIP_RKNN_DPU_ERDMA_DATA_MODE_LENGTH) !=
            (erdma_disabled ? 0 : 1))) {
        *reason = "dpu-erdma-mode";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    brdma_data_use = extract32(
        task->dpu_rdma.brdma_cfg,
        ROCKCHIP_RKNN_DPU_BRDMA_DATA_USE_SHIFT,
        ROCKCHIP_RKNN_DPU_BRDMA_DATA_USE_LENGTH);
    nrdma_data_use = extract32(
        task->dpu_rdma.nrdma_cfg,
        ROCKCHIP_RKNN_DPU_BRDMA_DATA_USE_SHIFT,
        ROCKCHIP_RKNN_DPU_BRDMA_DATA_USE_LENGTH);
    bs_rdma_available = dpu_rdma_enabled && brdma_data_use == 1;
    bn_rdma_available = dpu_rdma_enabled && nrdma_data_use == 4;
    ew_rdma_available = dpu_rdma_enabled && !erdma_disabled;
    if (task->dpu_rdma.nrdma_cfg & ~MAKE_64BIT_MASK(1, 4)) {
        *reason = "dpu-nrdma-control";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (!rockchip_rknn_dpu_stage_is_supported(
            task->dpu.bn_cfg, stage->bn_mul_cfg, false,
            bn_rdma_available) ||
        !rockchip_rknn_dpu_ew_is_supported(task->dpu.ew_cfg,
                                         ew_rdma_available)) {
        *reason = "dpu-stage-mode";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (nrdma_data_use &&
        (!bn_rdma_available ||
         !rockchip_rknn_dpu_stage_uses_mul_source(
             task->dpu.bn_cfg, stage->bn_mul_cfg))) {
        *reason = "dpu-nrdma-unused";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (ew_rdma_available &&
        !rockchip_rknn_dpu_ew_uses_rdma(task->dpu.ew_cfg)) {
        *reason = "dpu-rdma-unused";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (task->dpu.output_precision == 0 && task->core.quantify &&
        task->enabled_blocks ==
            (dpu_blocks | ROCKCHIP_RKNN_BLOCK_DPU_RDMA) &&
        !(task->dpu.bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) &&
        !(task->dpu.bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_BYPASS) &&
        !(task->dpu.bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_MUL_BYPASS) &&
        !(task->dpu.bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_MUL_PRELU) &&
        (task->dpu.bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_SOURCE) &&
        (task->dpu.bs_cfg & ROCKCHIP_RKNN_DPU_STAGE_RELU_BYPASS) &&
        extract32(task->dpu.bs_cfg,
                  ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_SHIFT,
                  ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_LENGTH) == 2 &&
        brdma_data_use == 7 &&
        task->dpu_rdma.width == task->dpu.output.width &&
        task->dpu_rdma.height == task->dpu.output.height &&
        task->dpu_rdma.channels == task->dpu.output.channels &&
        rockchip_rknn_dpu_qd_bs_ow_is_supported(task) &&
        (stage->bs_mul_cfg & ROCKCHIP_RKNN_DPU_MUL_SOURCE) &&
        !(stage->bs_mul_cfg & ROCKCHIP_RKNN_DPU_MUL_TRUNCATE_SOURCE) &&
        (erdma_disabled ?
         rockchip_rknn_brdma_layout_is_supported(task) :
         rockchip_rknn_erdma_layout_is_supported(task))) {
        *reason = "dpu-int8-qd-brdma";
        return ROCKCHIP_RKNN_EXECUTION_DPU_INT8_QD_BRDMA;
    }
    if (brdma_data_use == 7) {
        if (task->dpu_rdma.width != task->dpu.output.width ||
            task->dpu_rdma.height != task->dpu.output.height ||
            task->dpu_rdma.channels != task->dpu.output.channels) {
            *reason = "dpu-qd-rdma-shape";
        } else if (!rockchip_rknn_dpu_qd_bs_ow_is_supported(task)) {
            *reason = "dpu-qd-bs-ow";
        } else if (!(stage->bs_mul_cfg & ROCKCHIP_RKNN_DPU_MUL_SOURCE) ||
                   (stage->bs_mul_cfg &
                    ROCKCHIP_RKNN_DPU_MUL_TRUNCATE_SOURCE)) {
            *reason = "dpu-qd-multiplier";
        } else if (!(erdma_disabled ?
                     rockchip_rknn_brdma_layout_is_supported(task) :
                     rockchip_rknn_erdma_layout_is_supported(task))) {
            *reason = "dpu-qd-rdma-layout";
        } else {
            *reason = "dpu-qd-control";
        }
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (!rockchip_rknn_dpu_stage_is_supported(
            task->dpu.bs_cfg, stage->bs_mul_cfg,
            bs_rdma_available, false)) {
        *reason = "dpu-bs-mode";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (task->dpu.output_precision == 0 && task->core.quantify &&
        task->enabled_blocks ==
            (dpu_blocks | ROCKCHIP_RKNN_BLOCK_DPU_RDMA) &&
        bs_rdma_available && brdma_data_use == 1 &&
        task->dpu.bs_ow_cfg ==
            (task->core.depthwise ?
             ROCKCHIP_RKNN_DPU_BS_OW_CFG_DEPTHWISE_NO_CPEND :
             ROCKCHIP_RKNN_DPU_BS_OW_CFG_CONV_NO_CPEND) &&
        task->dpu_rdma.width == task->dpu.output.width &&
        task->dpu_rdma.height == task->dpu.output.height &&
        task->dpu_rdma.channels == task->dpu.output.channels &&
        (erdma_disabled ?
         rockchip_rknn_brdma_layout_is_supported(task) :
         rockchip_rknn_erdma_layout_is_supported(task)) &&
        rockchip_rknn_strided_output_layout_valid(
            &task->dpu.output, sizeof(int8_t))) {
        *reason = "dpu-int8-brdma";
        return ROCKCHIP_RKNN_EXECUTION_DPU_INT8_BRDMA;
    }
    if (task->dpu.output_precision == 0 &&
        task->enabled_blocks == dpu_blocks) {
        uint32_t bs_ow_cfg = task->core.depthwise ?
            ROCKCHIP_RKNN_DPU_BS_OW_CFG_DEPTHWISE_NO_CPEND :
            ROCKCHIP_RKNN_DPU_BS_OW_CFG_CONV_NO_CPEND;

        if (task->dpu.bs_ow_cfg != bs_ow_cfg) {
            *reason = "dpu-int8-cpend";
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
        if (!rockchip_rknn_strided_output_layout_valid(
                &task->dpu.output, sizeof(int8_t))) {
            *reason = "dpu-int8-output-layout";
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
        *reason = "dpu-int8";
        return ROCKCHIP_RKNN_EXECUTION_DPU_INT8;
    }
    if (task->dpu.output_precision != 4 || task->core.quantify) {
        *reason = "dpu-output-mode";
        return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    }
    if (dpu_rdma_enabled) {
        if (task->dpu_rdma.width != task->dpu.output.width ||
            task->dpu_rdma.height != task->dpu.output.height ||
            task->dpu_rdma.channels != task->dpu.output.channels) {
            *reason = "dpu-rdma-layout";
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
        if (brdma_data_use != 0 && !bs_rdma_available) {
            *reason = "dpu-rdma-mode";
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
        if (bs_rdma_available &&
            !(erdma_disabled ?
              rockchip_rknn_brdma_layout_is_supported(task) :
              rockchip_rknn_erdma_layout_is_supported(task))) {
            *reason = "dpu-brdma-layout";
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
        if (!erdma_disabled &&
            !rockchip_rknn_erdma_layout_is_supported(task)) {
            *reason = "dpu-erdma-layout";
            return ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        }
    }
    *reason = "dpu-int32";
    return ROCKCHIP_RKNN_EXECUTION_DPU_INT32;
}

static bool rockchip_rknn_size_mul(size_t left, size_t right, size_t *result)
{
    return !__builtin_mul_overflow(left, right, result);
}

static bool rockchip_rknn_size_mul3(size_t first, size_t second,
                                    size_t third, size_t *result)
{
    size_t intermediate;

    return rockchip_rknn_size_mul(first, second, &intermediate) &&
           rockchip_rknn_size_mul(intermediate, third, result);
}

static bool rockchip_rknn_size_round_up(size_t value, size_t alignment,
                                        size_t *result)
{
    if (!alignment || value > SIZE_MAX - (alignment - 1)) {
        return false;
    }
    *result = ROUND_UP(value, alignment);
    return true;
}

static size_t rockchip_rknn_precision_bytes(uint8_t precision)
{
    switch (precision) {
    case 0:
        return 1;
    case 2:
        return 2;
    case 4:
    case 5:
        return 4;
    default:
        return 0;
    }
}

static bool rockchip_rknn_u64_mul(uint64_t left, uint64_t right,
                                  uint64_t *result)
{
    return !__builtin_mul_overflow(left, right, result);
}

static bool rockchip_rknn_u64_mul3(uint64_t first, uint64_t second,
                                   uint64_t third, uint64_t *result)
{
    uint64_t intermediate;

    return rockchip_rknn_u64_mul(first, second, &intermediate) &&
           rockchip_rknn_u64_mul(intermediate, third, result);
}

static bool rockchip_rknn_host_budget_add(const RockchipRKNNCoreState *s,
                                          uint64_t *used, size_t amount)
{
    if (amount > UINT64_MAX - *used) {
        return false;
    }
    *used += amount;
    return *used <= s->functional_max_host_bytes;
}

static bool rockchip_rknn_mac_budget_valid(const RockchipRKNNCoreState *s,
                                           const RockchipRKNNPipelineTask *task,
                                           bool int8_qd_brdma)
{
    uint64_t macs;
    uint64_t kernel_area;
    uint64_t extra = 0;
    uint32_t qd_channels = task->core.depthwise ?
        task->dpu.output_channels_valid :
        rockchip_rknn_cna_execution_channels(&task->cna);

    if (!rockchip_rknn_u64_mul(task->cna.kernel_width,
                               task->cna.kernel_height, &kernel_area) ||
        !rockchip_rknn_u64_mul3(task->core.width, task->core.height,
                                task->dpu.output_channels_valid, &macs) ||
        !rockchip_rknn_u64_mul(macs, kernel_area, &macs) ||
        (!task->core.depthwise &&
         !rockchip_rknn_u64_mul(
             macs, rockchip_rknn_cna_execution_channels(&task->cna),
                                &macs))) {
        return false;
    }
    if (int8_qd_brdma) {
        if (!rockchip_rknn_u64_mul3(task->core.width, task->core.height,
                                    qd_channels,
                                    &extra) ||
            !rockchip_rknn_u64_mul(extra, kernel_area, &extra)) {
            return false;
        }
    }
    return extra <= UINT64_MAX - macs &&
           macs + extra <= s->functional_max_mac_operations;
}

static bool rockchip_rknn_dpu_work_budget_valid(
    const RockchipRKNNCoreState *s, uint32_t width, uint32_t height,
    uint32_t channels)
{
    uint64_t work_items;

    return rockchip_rknn_u64_mul3(width, height, channels, &work_items) &&
           work_items <= s->functional_max_mac_operations;
}

static bool rockchip_rknn_dma_length_valid(size_t length)
{
    return length && length <= UINT32_MAX;
}

static bool rockchip_rknn_iova_length_valid(uint32_t iova, size_t length)
{
    return rockchip_rknn_dma_length_valid(length) &&
           length - 1 <= UINT32_MAX - iova;
}

static bool rockchip_rknn_strided_output_layout_valid(
    const RockchipRKNNTensorView *view, size_t element_bytes)
{
    uint64_t compact_surface_bytes;
    uint64_t surface_bytes;
    uint64_t surface_offset;
    uint64_t accessed_bytes;
    unsigned int surfaces;

    if (!element_bytes || !view->atom || !view->channels) {
        return false;
    }
    surfaces = DIV_ROUND_UP(view->channels, view->atom);
    if (!rockchip_rknn_u64_mul3(view->width, view->height, view->atom,
                                &compact_surface_bytes) ||
        !rockchip_rknn_u64_mul(compact_surface_bytes, element_bytes,
                               &compact_surface_bytes) ||
        !rockchip_rknn_u64_mul(view->surface_stride, 16, &surface_bytes) ||
        surface_bytes < compact_surface_bytes ||
        !rockchip_rknn_u64_mul(surfaces - 1, surface_bytes, &surface_offset) ||
        __builtin_add_overflow(surface_offset, compact_surface_bytes,
                               &accessed_bytes) ||
        accessed_bytes > SIZE_MAX ||
        !rockchip_rknn_iova_length_valid(view->iova, accessed_bytes)) {
        return false;
    }
    return true;
}

static bool rockchip_rknn_fp16_rdma_source_layout_valid(
    const RockchipRKNNDpuRdmaConfig *rdma, size_t width, size_t height,
    size_t storage_channels, size_t *compact_bytes)
{
    uint64_t line_atoms = width + extract32(rdma->src_dma_cfg, 19, 13);
    uint64_t surface_atoms = (uint64_t)width * height +
        (line_atoms - width) * (height - 1) + rdma->surface_notch;
    uint64_t compact_surface_bytes;
    uint64_t accessed;
    unsigned int surfaces = DIV_ROUND_UP(storage_channels, 8);

    if (!surfaces || !rockchip_rknn_u64_mul3(width, height, 8,
                                              &compact_surface_bytes) ||
        !rockchip_rknn_u64_mul(compact_surface_bytes, sizeof(uint16_t),
                               &compact_surface_bytes) ||
        !rockchip_rknn_u64_mul(surfaces - 1, surface_atoms, &accessed) ||
        __builtin_add_overflow(
            accessed, (height - 1) * line_atoms + width, &accessed) ||
        !rockchip_rknn_u64_mul(accessed, 16, &accessed) ||
        compact_surface_bytes > SIZE_MAX || accessed > SIZE_MAX ||
        !rockchip_rknn_iova_length_valid(rdma->src_iova, accessed)) {
        return false;
    }
    if (!rockchip_rknn_size_mul3(width, height, storage_channels,
                                 compact_bytes) ||
        !rockchip_rknn_size_mul(*compact_bytes, sizeof(uint16_t),
                                compact_bytes)) {
        return false;
    }
    return true;
}

static bool rockchip_rknn_read_fp16_rdma_source(
    RockchipRKNNCoreState *s, const RockchipRKNNDpuRdmaConfig *rdma,
    uint16_t *input, size_t width, size_t height, size_t storage_channels)
{
    uint64_t line_atoms = width + extract32(rdma->src_dma_cfg, 19, 13);
    uint64_t surface_atoms = (uint64_t)width * height +
        (line_atoms - width) * (height - 1) + rdma->surface_notch;
    size_t compact_surface_bytes = width * height * 16;
    size_t row_bytes = width * 16;
    unsigned int surfaces = DIV_ROUND_UP(storage_channels, 8);

    for (unsigned int surface = 0; surface < surfaces; surface++) {
        if (line_atoms == width) {
            uint64_t iova = (uint64_t)rdma->src_iova +
                            surface * surface_atoms * 16;
            size_t offset = surface * compact_surface_bytes /
                            sizeof(*input);

            if (iova > UINT32_MAX ||
                !rockchip_rknn_iommu_dma(
                    s, iova, input + offset, compact_surface_bytes, false)) {
                return false;
            }
            continue;
        }
        for (unsigned int row = 0; row < height; row++) {
            uint64_t iova = (uint64_t)rdma->src_iova +
                (surface * surface_atoms + row * line_atoms) * 16;
            size_t offset = ((size_t)surface * height + row) * width * 8;

            if (iova > UINT32_MAX ||
                !rockchip_rknn_iommu_dma(
                    s, iova, input + offset, row_bytes, false)) {
                return false;
            }
        }
    }
    return true;
}

static bool rockchip_rknn_read_fp16_erdma_operand(
    RockchipRKNNCoreState *s, const RockchipRKNNDpuRdmaConfig *rdma,
    uint16_t *operand, size_t width, size_t height,
    size_t storage_channels)
{
    uint64_t compact_surface_bytes = (uint64_t)width * height * 16;
    uint64_t surface_bytes = ((uint64_t)width * height +
                              rdma->ew_surface_notch) * 16;
    unsigned int surfaces = DIV_ROUND_UP(storage_channels, 8);

    for (unsigned int surface = 0; surface < surfaces; surface++) {
        bool ew_port = surface & 1;
        uint64_t port_surface = surface / 2;
        uint64_t offset;
        uint64_t iova;

        if (!rockchip_rknn_u64_mul(port_surface, surface_bytes, &offset) ||
            __builtin_add_overflow(ew_port ? rdma->ew_iova : rdma->src_iova,
                                   offset, &iova) ||
            iova > UINT32_MAX || compact_surface_bytes > SIZE_MAX ||
            !rockchip_rknn_iova_length_valid(iova,
                                              compact_surface_bytes) ||
            !rockchip_rknn_iommu_dma(
                s, iova,
                operand + (size_t)surface * width * height * 8,
                compact_surface_bytes, false)) {
            return false;
        }
    }
    return true;
}

static bool rockchip_rknn_depthwise_int32_layout(
    const RockchipRKNNPipelineTask *task,
    RockchipRKNNDepthwiseOutputLayout *layout)
{
    const RockchipRKNNTensorView *view = &task->dpu.output;
    const size_t planes = DIV_ROUND_UP(task->dpu.output_channels_valid,
                                       view->atom);
    const uint64_t surface_words =
        (uint64_t)extract32(task->dpu.surface_add, 4, 28) * 4;
    uint64_t row_words;
    uint64_t rows_per_surface;
    uint64_t last_surface;
    uint64_t last_surface_row;
    uint64_t last_block_words;
    uint64_t accessed_words;
    uint64_t output_bytes;
    size_t plane_atom;

    if (!planes || view->channels % planes ||
        !rockchip_rknn_u64_mul(view->width, view->channels, &row_words) ||
        !row_words || !surface_words || surface_words % row_words) {
        return false;
    }
    plane_atom = view->channels / planes;
    rows_per_surface = surface_words / row_words;
    if (plane_atom < view->atom || !rows_per_surface ||
        surface_words % planes) {
        return false;
    }
    last_surface = (task->core.height - 1) / rows_per_surface;
    last_surface_row = (task->core.height - 1) % rows_per_surface;
    if (!rockchip_rknn_u64_mul(last_surface, surface_words,
                               &last_block_words) ||
        __builtin_add_overflow(last_block_words,
                               (planes - 1) * (surface_words / planes),
                               &last_block_words) ||
        __builtin_add_overflow(
            last_block_words,
            (last_surface_row * view->width + view->width - 1) * plane_atom,
            &last_block_words) ||
        __builtin_add_overflow(last_block_words, plane_atom,
                               &accessed_words) ||
        !rockchip_rknn_u64_mul(accessed_words, sizeof(uint32_t),
                               &output_bytes) ||
        output_bytes > SIZE_MAX ||
        !rockchip_rknn_iova_length_valid(view->iova, output_bytes)) {
        return false;
    }
    if (layout) {
        *layout = (RockchipRKNNDepthwiseOutputLayout) {
            .planes = planes,
            .plane_atom = plane_atom,
            .surface_words = surface_words,
            .plane_words = surface_words / planes,
            .rows_per_surface = rows_per_surface,
        };
    }
    return true;
}

static bool rockchip_rknn_depthwise_int32_notched_output_is_supported(
    const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNTensorView *view = &task->dpu.output;
    if (!view->channels || view->channels % view->atom ||
        !task->dpu.output_channels_valid ||
        task->dpu.output_channels_valid > view->channels) {
        return false;
    }

    return task->core.depthwise && task->dpu.output_precision == 0 &&
           task->dpu.output_notch_0 == task->dpu.output_notch_1 &&
           (uint64_t)view->surface_stride * 16 >=
               (uint64_t)view->width * view->height * view->atom *
               sizeof(uint32_t);
}

static bool rockchip_rknn_depthwise_int32_flat_wdma_is_supported(
    const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNTensorView *view = &task->dpu.output;

    return rockchip_rknn_depthwise_int32_notched_output_is_supported(task) &&
           (uint64_t)task->dpu.wdma_width * task->dpu.wdma_height *
               task->dpu.wdma_channels ==
           (uint64_t)view->width * view->height * view->channels;
}

static size_t rockchip_rknn_feature_index(size_t width, size_t height,
                                          size_t atom, size_t channel,
                                          size_t row, size_t column)
{
    return (channel / atom) * height * width * atom +
           atom * (row * width + column) + channel % atom;
}

static size_t rockchip_rknn_weight_index(size_t channels,
                                         size_t outputs,
                                         size_t kernel_area,
                                         size_t output, size_t kernel,
                                         size_t channel)
{
    size_t input_group = channel / 32 * 32;
    size_t output_group = output / 32 * 32;
    size_t input_atom = MIN(channels - input_group, (size_t)32);
    size_t output_atom = MIN(outputs - output_group, (size_t)32);

    return output_group * channels * kernel_area +
           input_group * output_atom * kernel_area +
           kernel * output_atom * input_atom +
           (output - output_group) * input_atom +
           channel - input_group;
}

static size_t rockchip_rknn_depthwise_weight_index(size_t channels,
                                                   size_t kernel_area,
                                                   size_t kernel,
                                                   size_t channel)
{
    const size_t channel_group = channel / 64 * 64;
    const size_t channel_atom = MIN(channels - channel_group, (size_t)64);

    return channel_group * kernel_area + kernel * channel_atom +
           channel - channel_group;
}

static int32_t rockchip_rknn_dot_i8(const int8_t *input,
                                    const int8_t *weights,
                                    size_t count)
{
    int32_t accumulator = 0;

    for (size_t index = 0; index < count; index++) {
        accumulator += input[index] * weights[index];
    }
    return accumulator;
}

static int32_t rockchip_rknn_sum_i8(const int8_t *input, size_t count)
{
    int32_t sum = 0;

    for (size_t index = 0; index < count; index++) {
        sum += input[index];
    }
    return sum;
}

static size_t rockchip_rknn_fp16_weight_index(
    size_t channels, size_t kernel_area, size_t output, size_t kernel,
    size_t channel)
{
    const size_t input_groups = DIV_ROUND_UP(channels, 32);

    return ((((output / 16) * input_groups + channel / 32) * kernel_area +
             kernel) * 16 + output % 16) * 32 + channel % 32;
}

static Int128 rockchip_rknn_mul_s32(Int128 value, int32_t factor)
{
    return int128_makes64((int64_t)int128_getlo(value) * factor);
}

static Int128 rockchip_rknn_saturate_i32(Int128 value)
{
    Int128 minimum = int128_makes64(INT32_MIN);
    Int128 maximum = int128_makes64(INT32_MAX);

    if (int128_lt(value, minimum)) {
        return minimum;
    }
    if (int128_gt(value, maximum)) {
        return maximum;
    }
    return value;
}

static int8_t rockchip_rknn_saturate_i8(Int128 value)
{
    int64_t result = int128_getlo(value);

    return result < INT8_MIN ? INT8_MIN :
           result > INT8_MAX ? INT8_MAX : result;
}

static int16_t rockchip_rknn_saturate_i16(Int128 value)
{
    int64_t result = int128_getlo(value);

    return result < INT16_MIN ? INT16_MIN :
           result > INT16_MAX ? INT16_MAX : result;
}

static Int128 rockchip_rknn_round_shift(Int128 value, unsigned int shift,
                                        bool ties_away);

static Int128 rockchip_rknn_dpu_mul(Int128 value, int32_t factor,
                                    unsigned int positive_shift,
                                    unsigned int negative_shift)
{
    value = rockchip_rknn_mul_s32(value, factor);
    value = rockchip_rknn_round_shift(
        value, int128_nonneg(value) ? positive_shift : negative_shift,
        false);
    return rockchip_rknn_saturate_i32(value);
}

static Int128 rockchip_rknn_dpu_stage_apply(
    uint32_t cfg, int32_t alu_operand, uint32_t mul_cfg,
    uint32_t relux_cmp, bool rdma_alu_valid, int32_t rdma_alu,
    bool rdma_mul_valid, int16_t rdma_mul, unsigned int negative_shift,
    Int128 value)
{
    unsigned int algorithm;

    if (cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) {
        return value;
    }
    if (!(cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_BYPASS)) {
        int32_t operand = cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_SOURCE ?
                          rdma_alu : alu_operand;

        g_assert(!(cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_SOURCE) ||
                 rdma_alu_valid);
        algorithm = extract32(cfg,
                              ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_SHIFT,
                              ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_LENGTH);
        if (algorithm == 2) {
            value = int128_add(value, int128_makes64(operand));
        } else {
            value = int128_sub(value, int128_makes64(operand));
        }
    }
    if (!(cfg & ROCKCHIP_RKNN_DPU_STAGE_MUL_BYPASS) &&
        (!(cfg & ROCKCHIP_RKNN_DPU_STAGE_MUL_PRELU) ||
         !int128_nonneg(value))) {
        int16_t multiplier = mul_cfg & ROCKCHIP_RKNN_DPU_MUL_SOURCE ?
                             rdma_mul :
                             mul_cfg >> ROCKCHIP_RKNN_DPU_MUL_OPERAND_SHIFT;

        g_assert(!(mul_cfg & ROCKCHIP_RKNN_DPU_MUL_SOURCE) ||
                 rdma_mul_valid);
        value = rockchip_rknn_dpu_mul(
            value, multiplier,
            extract32(mul_cfg, ROCKCHIP_RKNN_DPU_MUL_SHIFT_SHIFT,
                      ROCKCHIP_RKNN_DPU_MUL_SHIFT_LENGTH),
            negative_shift);
    }
    if (!(cfg & ROCKCHIP_RKNN_DPU_STAGE_RELU_BYPASS)) {
        if (!int128_nonneg(value)) {
            value = int128_zero();
        }
        if ((cfg & ROCKCHIP_RKNN_DPU_STAGE_RELUX_ENABLE) &&
            int128_getlo(value) > relux_cmp) {
            value = int128_makes64(relux_cmp);
        }
    }
    return rockchip_rknn_saturate_i32(value);
}

static Int128 rockchip_rknn_out_cvt(const RockchipRKNNDPUConfig *dpu,
                                    const RockchipRKNNDPUStageSnapshot *stage,
                                    Int128 value)
{
    unsigned int shift = dpu->out_cvt_shift;

    if (dpu->out_cvt_type) {
        value = int128_add(value, int128_makes64(dpu->out_cvt_offset));
        value = rockchip_rknn_mul_s32(value, dpu->out_cvt_scale);
    } else if (shift >= 64) {
        return int128_makes64(dpu->out_cvt_offset);
    } else {
        value = rockchip_rknn_mul_s32(value, dpu->out_cvt_scale);
        value = int128_add(
            value, int128_lshift(int128_makes64(dpu->out_cvt_offset),
                                 shift));
    }
    return rockchip_rknn_saturate_i32(rockchip_rknn_round_shift(
        value, shift, stage->out_cvt_round));
}

static Int128 rockchip_rknn_round_shift(Int128 value, unsigned int shift,
                                        bool ties_away)
{
    bool negative = !int128_nonneg(value);
    Int128 magnitude = negative ? int128_neg(value) : value;
    Int128 quotient;
    Int128 remainder;
    Int128 half;

    if (!shift) {
        return value;
    }
    /* Register-selected shifts at or above the accumulator width yield zero. */
    if (shift >= 127) {
        return int128_zero();
    }
    quotient = int128_urshift(magnitude, shift);
    remainder = int128_and(
        magnitude, int128_sub(int128_lshift(int128_one(), shift),
                              int128_one()));
    half = int128_lshift(int128_one(), shift - 1);

    if (int128_gt(remainder, half) ||
        (int128_eq(remainder, half) &&
         (ties_away || (int128_getlo(quotient) & 1)))) {
        quotient = int128_add(quotient, int128_one());
    }

    return negative ? int128_neg(quotient) : quotient;
}

static Int128 rockchip_rknn_ew_operand_cvt(
    const RockchipRKNNDPUConfig *dpu,
    const RockchipRKNNDPUStageSnapshot *stage, int32_t operand)
{
    unsigned int shift = extract32(stage->ew_cvt_scale, 16, 6);
    Int128 value = int128_makes64(operand);

    if (dpu->ew_cfg & ROCKCHIP_RKNN_DPU_EW_CVT_TYPE) {
        value = int128_add(value,
                           int128_makes64(stage->ew_cvt_offset));
        value = rockchip_rknn_mul_s32(
            value, extract32(stage->ew_cvt_scale, 0, 16));
    } else {
        value = rockchip_rknn_mul_s32(
            value, extract32(stage->ew_cvt_scale, 0, 16));
        value = int128_add(
            value,
            int128_lshift(int128_makes64(stage->ew_cvt_offset), shift));
    }
    return rockchip_rknn_saturate_i32(rockchip_rknn_round_shift(
        value, shift, extract32(dpu->ew_cfg, 30, 1)));
}

static bool rockchip_rknn_fetch_pipeline_task(
    RockchipRKNNCoreState *s, uint32_t index, uint32_t iova,
    uint32_t command_count)
{
    g_autofree RockchipRKNNRegisterFile *file =
        g_new0(RockchipRKNNRegisterFile, 1);
    RockchipRKNNPipelineTask task = {};
    RockchipRKNNDPUStageSnapshot stage = {};
    uint32_t next_iova;
    uint32_t next_amount;
    const char *mode_reason;
    RockchipRKNNExecutionMode mode;
    bool decoded;

    trace_rockchip_rknn_task_fetch(s->core_index, index, iova,
                                   command_count);
    memset(s->pending_pipeline, 0, sizeof(*s->pending_pipeline));
    memset(&s->pending_dpu_stage, 0, sizeof(s->pending_dpu_stage));
    s->pending_pipeline_decoded = false;
    s->pending_domain_runtime_valid = false;
    memset(s->pending_register_writes, 0,
           sizeof(s->pending_register_writes));
    rockchip_rknn_register_file_init(file, &s->slave_file);
    if (!rockchip_rknn_fetch_register_file(s, file, iova, command_count)) {
        trace_rockchip_rknn_task_fetch_error(s->core_index, index,
                                              "regcmd-fetch");
        return false;
    }
    memcpy(s->pending_domain_runtime, file->runtime,
           sizeof(file->runtime));
    s->pending_file = *file;
    s->pending_domain_runtime_valid = true;
    task.enabled_blocks = file->enabled_blocks;
    decoded = rockchip_rknn_decode_pipeline(s, &task, &stage, file);
    *s->pending_pipeline = task;
    s->pending_dpu_stage = stage;
    s->pending_pipeline_decoded = decoded;
    if (decoded) {
        mode = rockchip_rknn_execution_mode(&task, &stage, &mode_reason);
    } else if (!file->enabled_blocks) {
        mode = ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        mode_reason = "control";
    } else {
        mode = ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        mode_reason = "decode-error";
    }
    trace_rockchip_rknn_pipeline_decode(
        s->core_index, index, file->enabled_blocks, decoded,
        mode != ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED, mode_reason,
        task.core.depthwise,
        task.dpu.output_precision);
    trace_rockchip_rknn_pipeline_decode_input(
        s->core_index, index,
        task.cna.input.width, task.cna.input.height,
        task.cna.input_channels_valid, task.cna.input.channels,
        task.cna.kernel_width, task.cna.kernel_height,
        task.cna.stride_x, task.cna.stride_y);
    trace_rockchip_rknn_pipeline_cna_layout(
        s->core_index, index, task.cna.output_width,
        task.cna.output_atomics,
        rockchip_rknn_cna_logical_width(&task.cna),
        rockchip_rknn_cna_logical_height(&task.cna),
        extract32(task.cna.fc_data_size1, 0, 16),
        task.core.width, task.core.height, task.core.channels);
    trace_rockchip_rknn_pipeline_cna_controls(
        s->core_index, index, task.cna.conv_mode,
        task.cna.cvt_con0, task.cna.per_channel_cvt, task.cna.argb_in,
        task.cna.input_channels_valid, task.cna.co_work_mode,
        task.cna.nonalign_dma,
        task.cna.group_line_off);
    trace_rockchip_rknn_pipeline_cna_storage(
        s->core_index, index, task.cna.weight_bytes,
        task.cna.weight_bytes_per_kernel, task.cna.pad_left,
        task.cna.pad_top, task.cna.pad_value, task.core.channels,
        task.core.quantify);
    trace_rockchip_rknn_pipeline_cna_addresses(
        s->core_index, index, task.cna.input.iova,
        task.cna.weight_iova, task.dpu.output.iova);
    trace_rockchip_rknn_pipeline_cna_geometry(
        s->core_index, index, task.cna.deconv,
        task.cna.deconv_stride_x, task.cna.deconv_stride_y,
        task.cna.atrous_x_dilation, task.cna.atrous_y_dilation,
        task.cna.nn_mode, task.cna.surface_mode);
    trace_rockchip_rknn_pipeline_decode_output(
        s->core_index, index,
        task.dpu.output.width, task.dpu.output.height,
        task.cna.weight_kernels, task.dpu.output_channels_valid,
        task.dpu.output.channels);
    trace_rockchip_rknn_pipeline_dpu_controls(
        s->core_index, index, task.dpu.feature_mode, task.dpu.data_format,
        task.dpu.dst_dma_cfg, task.dpu.bs_cfg, task.dpu.bs_ow_cfg,
        task.dpu.bs_ow_op, task.dpu.bn_cfg, task.dpu.ew_cfg);
    trace_rockchip_rknn_pipeline_dpu_output(
        s->core_index, index, task.dpu.out_cvt_offset,
        task.dpu.out_cvt_scale, task.dpu.out_cvt_shift,
        task.dpu.surface_add, task.dpu.output.surface_stride,
        task.dpu.output_notch_0, task.dpu.output_notch_1,
        task.dpu.minmax_ctl);
    trace_rockchip_rknn_pipeline_dpu_conversion(
        s->core_index, index, task.dpu.offset_pend,
        task.dpu.out_cvt_minus_exp,
        task.dpu.out_cvt_type, task.dpu.out_fp32_to_fp16,
        stage.out_cvt_round);
    trace_rockchip_rknn_pipeline_rdma_controls(
        s->core_index, index, task.dpu_rdma.feature_mode,
        task.dpu_rdma.brdma_cfg, task.dpu_rdma.erdma_cfg,
        task.dpu_rdma.src_dma_cfg, task.dpu_rdma.pad_cfg,
        task.dpu_rdma.surface_notch, task.dpu_rdma.weight,
        task.dpu_rdma.ew_surface_stride);
    trace_rockchip_rknn_pipeline_rdma_layout(
        s->core_index, index, task.dpu_rdma.width,
        task.dpu_rdma.height, task.dpu_rdma.channels,
        task.dpu.wdma_width, task.dpu.wdma_height,
        task.dpu.wdma_channels, task.dpu.wdma_size_c,
        task.dpu.wdma_tp_precision);
    trace_rockchip_rknn_pipeline_rdma_addresses(
        s->core_index, index, task.dpu_rdma.src_iova,
        task.dpu_rdma.bs_iova, task.dpu_rdma.nrdma_cfg,
        task.dpu_rdma.bn_iova,
        task.dpu_rdma.ew_iova,
        task.dpu_rdma.ew_surface_notch);
    trace_rockchip_rknn_pipeline_stage_controls(
        s->core_index, index, stage.bs_alu_operand, stage.bs_mul_cfg,
        stage.bn_alu_operand, stage.bn_mul_cfg,
        stage.ew_cvt_offset, stage.ew_cvt_scale, stage.out_cvt_round);
    trace_rockchip_rknn_pipeline_stage_relux(
        s->core_index, index, task.dpu.bs_relux_cmp,
        task.dpu.bn_relux_cmp, task.dpu.ew_relux_cmp);
    trace_rockchip_rknn_pipeline_lut_controls(
        s->core_index, index, task.dpu.lut_cfg, task.dpu.lut_info,
        task.dpu.lut_le_start, task.dpu.lut_le_end,
        task.dpu.lut_lo_start, task.dpu.lut_lo_end,
        task.dpu.lut_le_slope_scale, task.dpu.lut_lo_slope_scale);
    trace_rockchip_rknn_pipeline_lut_shifts(
        s->core_index, index, task.dpu.lut_le_slope_shift,
        task.dpu.lut_lo_slope_shift);
    trace_rockchip_rknn_pipeline_ppu_controls(
        s->core_index, index, task.ppu.mode, task.ppu.kernel,
        task.ppu.padding, task.ppu.data_format, task.ppu.misc_ctrl,
        task.ppu.rdma_data_format);
    trace_rockchip_rknn_pipeline_ppu_input(
        s->core_index, index, task.ppu.in_width, task.ppu.in_height,
        task.ppu.in_channels, task.ppu.rdma_in_width,
        task.ppu.rdma_in_height, task.ppu.rdma_in_channels);
    trace_rockchip_rknn_pipeline_ppu_output(
        s->core_index, index, task.ppu.out_width, task.ppu.out_height,
        task.ppu.out_channels,
        task.ppu.line_stride, task.ppu.surf_stride, task.ppu.dst_stride);
    trace_rockchip_rknn_pipeline_ppu_addresses(
        s->core_index, index, task.ppu.src_iova, task.ppu.dst_iova,
        task.ppu.padding_value_0, task.ppu.padding_value_1);

    if (index + 1 < s->pending_task_count) {
        if (!rockchip_rknn_register_read_present(
                file, ROCKCHIP_RKNN_DOMAIN_PC,
                A_PC_BASE_ADDRESS, &next_iova) ||
            !rockchip_rknn_register_read_present(
                file, ROCKCHIP_RKNN_DOMAIN_PC,
                A_PC_REGISTER_AMOUNTS, &next_amount)) {
            trace_rockchip_rknn_task_fetch_error(s->core_index, index,
                                                  "missing-next-task");
            return false;
        }
        s->pending_next_iova =
            next_iova & ROCKCHIP_RKNN_PC_BASE_ADDRESS_MASK;
        s->pending_next_command_count =
            ((next_amount & ROCKCHIP_RKNN_PC_REGISTER_AMOUNTS_MASK) + 1) * 2;
    }

    return true;
}

static void rockchip_rknn_prepare_pipeline(RockchipRKNNCoreState *s)
{
    uint32_t task_count = s->pc_regs[R_PC_TASK_CON] &
                          ROCKCHIP_RKNN_TASK_NUMBER_MASK;
    uint32_t iova = s->pc_regs[R_PC_BASE_ADDRESS] &
                    ROCKCHIP_RKNN_PC_BASE_ADDRESS_MASK;
    uint32_t command_count =
        ((s->pc_regs[R_PC_REGISTER_AMOUNTS] &
          ROCKCHIP_RKNN_PC_REGISTER_AMOUNTS_MASK) + 1) * 2;

    s->pending_task_count = 0;
    s->pending_task_index = 0;
    s->pending_next_iova = 0;
    s->pending_next_command_count = 0;
    memset(s->pending_pipeline, 0, sizeof(*s->pending_pipeline));
    s->pending_pipeline_decoded = false;
    memset(&s->pending_dpu_stage, 0, sizeof(s->pending_dpu_stage));
    s->pending_domain_runtime_valid = false;
    s->pending_fetch_error = false;
    s->pending_execution_error = false;
    s->pending_ppu_stage_attempted = false;
    s->pending_final_pipeline_attempted = false;
    s->pending_final_ppu_attempted = false;
    s->pending_final_ppu_success = false;
    s->pending_final_ppu_bank = 0;
    s->pending_dma_error_bits = 0;
    if (!task_count) {
        return;
    }

    s->pending_task_count = task_count;
    if (!rockchip_rknn_fetch_pipeline_task(s, 0, iova, command_count)) {
        s->pending_task_count = 0;
        s->pending_fetch_error = true;
    }
}

static void rockchip_rknn_prepare_slave_pipeline(RockchipRKNNCoreState *s,
                                                  uint32_t enabled_blocks)
{
    RockchipRKNNPipelineTask task = {};
    RockchipRKNNDPUStageSnapshot stage = {};

    s->pending_task_count = 1;
    s->pending_task_index = 0;
    s->pending_next_iova = 0;
    s->pending_next_command_count = 0;
    memset(s->pending_pipeline, 0, sizeof(*s->pending_pipeline));
    s->pending_pipeline_decoded = false;
    memset(&s->pending_dpu_stage, 0, sizeof(s->pending_dpu_stage));
    s->pending_domain_runtime_valid = false;
    s->pending_fetch_error = false;
    s->pending_execution_error = false;
    s->pending_ppu_stage_attempted = false;
    s->pending_final_pipeline_attempted = false;
    s->pending_final_ppu_attempted = false;
    s->pending_final_ppu_success = false;
    s->pending_final_ppu_bank = 0;
    s->pending_dma_error_bits = 0;
    s->slave_file.enabled_blocks = enabled_blocks & 0x7f;
    memset(s->pending_register_writes, 0,
           sizeof(s->pending_register_writes));
    s->pending_file = s->slave_file;
    memcpy(s->pending_domain_runtime, s->pending_file.runtime,
           sizeof(s->pending_file.runtime));
    s->pending_domain_runtime_valid = true;
    task.enabled_blocks = s->pending_file.enabled_blocks;
    *s->pending_pipeline = task;
    if (rockchip_rknn_decode_pipeline(s, &task, &stage, &s->pending_file)) {
        *s->pending_pipeline = task;
        s->pending_dpu_stage = stage;
        s->pending_pipeline_decoded = true;
    }
}

static int64_t rockchip_rknn_floor_div_pow2(int64_t value,
                                            unsigned int shift)
{
    uint64_t divisor;
    uint64_t magnitude;
    uint64_t quotient;

    if (!shift) {
        return value;
    }
    if (shift >= 63) {
        return value < 0 ? -1 : 0;
    }
    divisor = UINT64_C(1) << shift;
    if (value >= 0) {
        return value / divisor;
    }
    magnitude = -(uint64_t)value;
    quotient = magnitude / divisor;
    if (magnitude % divisor) {
        quotient++;
    }
    return -(int64_t)quotient;
}

static Int128 rockchip_rknn_lut_lookup(RockchipRKNNCoreState *s,
                                       const RockchipRKNNDPUConfig *dpu,
                                       Int128 value)
{
    int64_t input = int128_getlo(rockchip_rknn_saturate_i32(value));
    int64_t start, end;
    int64_t lower, upper, interpolated;
    uint64_t fraction;
    uint64_t offset;
    unsigned int table;
    unsigned int index;
    unsigned int index_shift;

    if (input < 0) {
        table = 0;
        start = (int32_t)dpu->lut_le_start;
        end = (int32_t)dpu->lut_le_end;
    } else {
        table = 1;
        start = (int32_t)dpu->lut_lo_start;
        end = (int32_t)dpu->lut_lo_end;
    }
    if (input <= start) {
        index = 0;
    } else if (input >= end) {
        index = ROCKCHIP_RKNN_LUT_ENTRIES - 1;
    } else {
        offset = (uint64_t)(input - start);
        index_shift = extract32(dpu->lut_info,
                                table ? 16 : 8, 8);
        index = index_shift >= 64 ? 0 : offset >> index_shift;
        if (index >= ROCKCHIP_RKNN_LUT_ENTRIES - 1) {
            index = ROCKCHIP_RKNN_LUT_ENTRIES - 1;
        } else {
            fraction = index_shift >= 64 ? offset :
                offset & ((UINT64_C(1) << index_shift) - 1);
            lower = (int16_t)s->execution_lut[table][index];
            upper = (int16_t)s->execution_lut[table][index + 1];
            interpolated = lower + rockchip_rknn_floor_div_pow2(
                (upper - lower) * (int64_t)fraction, index_shift);
            return int128_makes64(interpolated);
        }
    }
    if (table == 1 && input > end && dpu->lut_lo_slope_scale) {
        int64_t delta = input - end;
        int64_t endpoint = (int16_t)s->execution_lut[table][index];
        unsigned int scale = extract32(dpu->lut_lo_slope_scale, 16, 16);
        unsigned int shift = extract32(dpu->lut_lo_slope_shift, 5, 5);

        return int128_makes64(endpoint + ((delta * scale) >> shift));
    }
    return int128_makes64((int16_t)s->execution_lut[table][index]);
}

static Int128 rockchip_rknn_dpu_ew_apply(
    RockchipRKNNCoreState *s, const RockchipRKNNDPUConfig *dpu,
    const RockchipRKNNDPUStageSnapshot *stage, int32_t operand,
    Int128 value)
{
    uint32_t cfg = dpu->ew_cfg;
    unsigned int shift = 0;

    if (cfg & ROCKCHIP_RKNN_DPU_EW_BYPASS) {
        return value;
    }
    if (!(cfg & ROCKCHIP_RKNN_DPU_EW_OP_BYPASS)) {
        Int128 converted = int128_makes64(operand);

        if (!(cfg & ROCKCHIP_RKNN_DPU_EW_OP_CVT_BYPASS)) {
            converted = rockchip_rknn_ew_operand_cvt(
                dpu, stage, operand);
        }
        if (cfg & ROCKCHIP_RKNN_DPU_EW_OP_TYPE) {
            if (!(cfg & ROCKCHIP_RKNN_DPU_EW_MUL_PRELU) ||
                !int128_nonneg(value)) {
                value = rockchip_rknn_mul_s32(
                    value, (int32_t)int128_getlo(converted));
            }
        } else {
            value = int128_add(value, converted);
        }
    }
    if (!(cfg & ROCKCHIP_RKNN_DPU_EW_LUT_BYPASS)) {
        value = rockchip_rknn_lut_lookup(s, dpu, value);
    }
    if (!(cfg & ROCKCHIP_RKNN_DPU_EW_RELU_BYPASS)) {
        if (!int128_nonneg(value)) {
            value = int128_zero();
        }
        if ((cfg & ROCKCHIP_RKNN_DPU_EW_RELUX_ENABLE) &&
            int128_getlo(value) > dpu->ew_relux_cmp) {
            value = int128_makes64(dpu->ew_relux_cmp);
        }
    }
    if (int128_nonneg(value)) {
        shift = extract32(stage->ew_cvt_scale, 22, 10);
    } else {
        shift = extract32(dpu->data_format, 16, 10);
    }
    return rockchip_rknn_saturate_i32(
        rockchip_rknn_round_shift(value, shift, false));
}

static RockchipRKNNExecutionResult rockchip_rknn_ppu_dma_result(
    RockchipRKNNDMAResult result, bool write)
{
    if (result == ROCKCHIP_RKNN_DMA_IOMMU_FAULT) {
        return ROCKCHIP_RKNN_EXECUTION_IOMMU_FAULT;
    }
    if (result == ROCKCHIP_RKNN_DMA_BUS_ERROR) {
        return write ? ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT :
                       ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }
    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static float_status rockchip_rknn_fp16_status(void);

static unsigned int rockchip_rknn_ppu_element_bytes(
    const RockchipRKNNPPUConfig *ppu)
{
    return ppu->rdma_data_format == 2 ? 2 : 1;
}

static bool rockchip_rknn_ppu_budget_valid(
    const RockchipRKNNCoreState *s, const RockchipRKNNPPUConfig *ppu,
    RockchipRKNNExecutionMode mode, size_t *input_row_bytes,
    size_t *output_row_bytes, size_t *input_rows_bytes)
{
    const uint64_t bytes_per_pixel = 16;
    const uint64_t element_bytes = rockchip_rknn_ppu_element_bytes(ppu);
    const uint64_t surfaces = DIV_ROUND_UP(ppu->in_channels,
                                           bytes_per_pixel / element_bytes);
    const uint64_t kernel_width = extract32(
        ppu->kernel, ROCKCHIP_RKNN_PPU_KERNEL_WIDTH_SHIFT,
        ROCKCHIP_RKNN_PPU_KERNEL_FIELD_LENGTH) + 1;
    const uint64_t kernel_height = extract32(
        ppu->kernel, ROCKCHIP_RKNN_PPU_KERNEL_HEIGHT_SHIFT,
        ROCKCHIP_RKNN_PPU_KERNEL_FIELD_LENGTH) + 1;
    uint64_t input_row_bytes_u64;
    uint64_t output_row_bytes_u64;
    uint64_t input_buffer_bytes;
    uint64_t minimum_input_surface_bytes;
    uint64_t output_line_stride;
    uint64_t valid_row_bytes = 0;
    uint64_t host_bytes;
    uint64_t work_items;

    if (!ppu->out_height || ppu->dst_stride % ppu->out_height) {
        return false;
    }
    output_line_stride = ppu->dst_stride / ppu->out_height;
    if (!rockchip_rknn_u64_mul(ppu->in_width, bytes_per_pixel,
                               &input_row_bytes_u64) ||
        !rockchip_rknn_u64_mul(ppu->out_width, bytes_per_pixel,
                               &output_row_bytes_u64) ||
        !rockchip_rknn_u64_mul(ppu->line_stride, ppu->in_height,
                               &minimum_input_surface_bytes) ||
        ppu->line_stride < input_row_bytes_u64 ||
        ppu->surf_stride < minimum_input_surface_bytes ||
        output_line_stride < output_row_bytes_u64 ||
        input_row_bytes_u64 > SIZE_MAX || output_row_bytes_u64 > SIZE_MAX) {
        return false;
    }
    if (mode == ROCKCHIP_RKNN_EXECUTION_PPU_BYPASS) {
        if (!rockchip_rknn_u64_mul3(surfaces, ppu->in_height,
                                    input_row_bytes_u64, &work_items) ||
            !rockchip_rknn_u64_mul(input_row_bytes_u64, 2, &host_bytes)) {
            return false;
        }
        input_buffer_bytes = input_row_bytes_u64;
    } else if (mode == ROCKCHIP_RKNN_EXECUTION_PPU_INT8_MAX_POOL ||
               mode == ROCKCHIP_RKNN_EXECUTION_PPU_FP16_MAX_POOL) {
        if (!rockchip_rknn_u64_mul3(surfaces, ppu->out_height,
                                    ppu->out_width, &work_items) ||
            !rockchip_rknn_u64_mul(work_items, kernel_width, &work_items) ||
            !rockchip_rknn_u64_mul(work_items, kernel_height, &work_items) ||
            !rockchip_rknn_u64_mul(work_items, bytes_per_pixel,
                                   &work_items) ||
            !rockchip_rknn_u64_mul(kernel_height, input_row_bytes_u64,
                                   &input_buffer_bytes) ||
            !rockchip_rknn_u64_mul(kernel_height, sizeof(bool),
                                   &valid_row_bytes) ||
            input_buffer_bytes > UINT64_MAX - valid_row_bytes ||
            input_buffer_bytes + valid_row_bytes > UINT64_MAX -
                output_row_bytes_u64) {
            return false;
        }
        host_bytes = input_buffer_bytes + valid_row_bytes +
                     output_row_bytes_u64;
    } else {
        return false;
    }
    if (work_items > s->functional_max_ppu_work_items ||
        host_bytes > s->functional_max_host_bytes ||
        input_buffer_bytes > SIZE_MAX || output_row_bytes_u64 > SIZE_MAX) {
        return false;
    }
    *input_row_bytes = input_row_bytes_u64;
    *output_row_bytes = output_row_bytes_u64;
    *input_rows_bytes = input_buffer_bytes;
    return true;
}

static RockchipRKNNExecutionResult rockchip_rknn_execute_ppu(
    RockchipRKNNCoreState *s, const RockchipRKNNPPUConfig *ppu,
    RockchipRKNNExecutionMode mode)
{
    const unsigned int bytes_per_pixel = 16;
    const unsigned int element_bytes = rockchip_rknn_ppu_element_bytes(ppu);
    const unsigned int surfaces =
        DIV_ROUND_UP(ppu->in_channels, bytes_per_pixel / element_bytes);
    const unsigned int output_line_stride =
        ppu->dst_stride / ppu->out_height;
    const unsigned int top = extract32(ppu->padding, 4, 3);
    const unsigned int left = extract32(ppu->padding, 0, 3);
    const unsigned int kernel_width = extract32(
        ppu->kernel, ROCKCHIP_RKNN_PPU_KERNEL_WIDTH_SHIFT,
        ROCKCHIP_RKNN_PPU_KERNEL_FIELD_LENGTH) + 1;
    const unsigned int kernel_height = extract32(
        ppu->kernel, ROCKCHIP_RKNN_PPU_KERNEL_HEIGHT_SHIFT,
        ROCKCHIP_RKNN_PPU_KERNEL_FIELD_LENGTH) + 1;
    const unsigned int stride_width = extract32(
        ppu->kernel, ROCKCHIP_RKNN_PPU_KERNEL_STRIDE_WIDTH_SHIFT,
        ROCKCHIP_RKNN_PPU_KERNEL_FIELD_LENGTH) + 1;
    const unsigned int stride_height = extract32(
        ppu->kernel, ROCKCHIP_RKNN_PPU_KERNEL_STRIDE_HEIGHT_SHIFT,
        ROCKCHIP_RKNN_PPU_KERNEL_FIELD_LENGTH) + 1;
    size_t input_row_bytes;
    size_t output_row_bytes;
    g_autofree uint8_t *input_rows = NULL;
    g_autofree bool *input_row_valid = NULL;
    g_autofree uint8_t *output_row = NULL;

    size_t input_rows_bytes;

    if (!rockchip_rknn_ppu_budget_valid(s, ppu, mode, &input_row_bytes,
                                        &output_row_bytes,
                                        &input_rows_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    output_row = g_try_malloc(output_row_bytes);
    if (!output_row) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (mode == ROCKCHIP_RKNN_EXECUTION_PPU_BYPASS) {
        input_rows = g_try_malloc(input_rows_bytes);
        if (!input_rows) {
            return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
        }
        for (unsigned int surface = 0; surface < surfaces; surface++) {
            for (unsigned int row = 0; row < ppu->in_height; row++) {
                uint64_t src_iova = (uint64_t)ppu->src_iova +
                    (uint64_t)surface * ppu->surf_stride +
                    (uint64_t)row * ppu->line_stride;
                uint64_t dst_iova = (uint64_t)ppu->dst_iova +
                    (uint64_t)surface * ppu->dst_stride +
                    (uint64_t)row * output_line_stride;
                RockchipRKNNDMAResult dma_result;

                if (src_iova > UINT32_MAX || dst_iova > UINT32_MAX) {
                    return ROCKCHIP_RKNN_EXECUTION_IOMMU_FAULT;
                }
                dma_result = rockchip_rknn_iommu_dma_result(
                    s, src_iova, input_rows, input_row_bytes, false);
                if (dma_result != ROCKCHIP_RKNN_DMA_OK) {
                    return rockchip_rknn_ppu_dma_result(dma_result, false);
                }
                dma_result = rockchip_rknn_iommu_dma_result(
                    s, dst_iova, input_rows, output_row_bytes, true);
                if (dma_result != ROCKCHIP_RKNN_DMA_OK) {
                    return rockchip_rknn_ppu_dma_result(dma_result, true);
                }
            }
        }
        return ROCKCHIP_RKNN_EXECUTION_OK;
    }
    if (mode != ROCKCHIP_RKNN_EXECUTION_PPU_INT8_MAX_POOL &&
        mode != ROCKCHIP_RKNN_EXECUTION_PPU_FP16_MAX_POOL) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    input_rows = g_try_malloc(input_rows_bytes);
    input_row_valid = g_try_new(bool, kernel_height);
    if (!input_rows || !input_row_valid) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    for (unsigned int surface = 0; surface < surfaces; surface++) {
        for (unsigned int y = 0; y < ppu->out_height; y++) {
            for (unsigned int ky = 0; ky < kernel_height; ky++) {
                int input_y = (int)(y * stride_height) - (int)top +
                              (int)ky;

                input_row_valid[ky] =
                    input_y >= 0 && input_y < (int)ppu->in_height;
                if (input_row_valid[ky]) {
                    uint64_t src_iova = (uint64_t)ppu->src_iova +
                        (uint64_t)surface * ppu->surf_stride +
                        (uint64_t)input_y * ppu->line_stride;
                    RockchipRKNNDMAResult dma_result;

                    if (src_iova > UINT32_MAX) {
                        return ROCKCHIP_RKNN_EXECUTION_IOMMU_FAULT;
                    }
                    dma_result = rockchip_rknn_iommu_dma_result(
                        s, src_iova, input_rows + ky * input_row_bytes,
                        input_row_bytes, false);
                    if (dma_result != ROCKCHIP_RKNN_DMA_OK) {
                        return rockchip_rknn_ppu_dma_result(dma_result,
                                                            false);
                    }
                }
            }
            for (unsigned int x = 0; x < ppu->out_width; x++) {
                for (unsigned int byte = 0; byte < bytes_per_pixel;
                     byte += element_bytes) {
                    int best = INT8_MIN;
                    uint16_t best_fp16 = 0xfc00;
                    uint8_t best_byte = 0x80;

                    for (unsigned int ky = 0; ky < kernel_height; ky++) {
                        if (!input_row_valid[ky]) {
                            continue;
                        }
                        for (unsigned int kx = 0; kx < kernel_width; kx++) {
                            int ix = (int)(x * stride_width) - (int)left +
                                     (int)kx;
                            int value;

                            if (ix < 0 || ix >= (int)ppu->in_width) {
                                continue;
                            }
                            size_t input_offset = ky * input_row_bytes +
                                ix * bytes_per_pixel + byte;

                            if (element_bytes == 2) {
                                float_status status =
                                    rockchip_rknn_fp16_status();
                                uint16_t input_fp16 =
                                    lduw_le_p(input_rows + input_offset);
                                float32 input_value = float16_to_float32(
                                    make_float16(input_fp16), true, &status);
                                float32 best_value = float16_to_float32(
                                    make_float16(best_fp16), true, &status);

                                if (float32_lt(best_value, input_value,
                                               &status)) {
                                    best_fp16 = input_fp16;
                                }
                            } else {
                                uint8_t input_byte =
                                    input_rows[input_offset];

                                value = (int8_t)input_byte;
                                if (value > best) {
                                    best = value;
                                    best_byte = input_byte;
                                }
                            }
                        }
                    }
                    if (element_bytes == 2) {
                        stw_le_p(output_row + x * bytes_per_pixel + byte,
                                 best_fp16);
                    } else {
                        output_row[x * bytes_per_pixel + byte] = best_byte;
                    }
                }
            }
            RockchipRKNNDMAResult dma_result;
            uint64_t iova = (uint64_t)ppu->dst_iova +
                (uint64_t)surface * ppu->dst_stride +
                (uint64_t)y * output_line_stride;

            if (iova > UINT32_MAX) {
                return ROCKCHIP_RKNN_EXECUTION_IOMMU_FAULT;
            }
            dma_result = rockchip_rknn_iommu_dma_result(
                s, iova, output_row, output_row_bytes, true);
            if (dma_result != ROCKCHIP_RKNN_DMA_OK) {
                return rockchip_rknn_ppu_dma_result(dma_result, true);
            }
        }
    }
    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static bool rockchip_rknn_input_strides(
    const RockchipRKNNTensorView *view, uint64_t *line_bytes,
    uint64_t *surface_bytes)
{
    size_t element_bytes = rockchip_rknn_precision_bytes(view->precision);
    int64_t surface_units =
        (int64_t)sextract32(view->surface_stride, 0, 28) +
        view->line_stride;

    if (surface_units < 0 || !element_bytes) {
        return false;
    }
    *line_bytes = (uint64_t)view->line_stride * sizeof(uint32_t);
    *surface_bytes = surface_units * view->atom * element_bytes;
    return true;
}

static int8_t rockchip_rknn_cna_convert_input(
    const RockchipRKNNCNAConfig *cna, unsigned int channel, uint8_t raw)
{
    int32_t input = cna->cvt_con0 & ROCKCHIP_RKNN_CNA_CVT_DATA_SIGN ?
                    (int8_t)raw : raw;
    int16_t offset = extract32(cna->cvt_channel[channel], 0, 16);
    int16_t scale = extract32(cna->cvt_channel[channel], 16, 16);
    unsigned int truncate = extract32(cna->cvt_con0, 4 + channel * 6, 6);
    Int128 value = int128_add(int128_makes64(input),
                              int128_makes64(offset));

    value = rockchip_rknn_mul_s32(value, scale);
    value = rockchip_rknn_round_shift(
        value, truncate,
        cna->cvt_con0 & ROCKCHIP_RKNN_CNA_CVT_ROUND_TYPE);
    return rockchip_rknn_saturate_i8(value);
}

static int8_t rockchip_rknn_cna_padding_value(
    const RockchipRKNNCNAConfig *cna, unsigned int channel)
{
    uint8_t raw = cna->pad_value;

    return (int8_t)raw;
}

static bool rockchip_rknn_read_strided_input(
    RockchipRKNNCoreState *s, const RockchipRKNNCNAConfig *cna,
    uint8_t *input, uint64_t line_bytes, uint64_t surface_bytes)
{
    const RockchipRKNNTensorView *view = &cna->input;
    const size_t element_bytes =
        rockchip_rknn_precision_bytes(view->precision);
    const size_t row_bytes = view->width * view->atom * element_bytes;
    const unsigned int surfaces = DIV_ROUND_UP(view->channels, view->atom);

    if (!element_bytes) {
        return false;
    }

    if (rockchip_rknn_cna_interleaved_input(cna)) {
        const size_t interleaved_row_bytes =
            view->width * cna->input_channels_valid;
        g_autofree uint8_t *row_data = g_try_malloc(interleaved_row_bytes);

        if (!row_data) {
            return false;
        }
        for (unsigned int row = 0; row < view->height; row++) {
            uint64_t iova = (uint64_t)view->iova + row * line_bytes;

            if (iova > UINT32_MAX ||
                !rockchip_rknn_iommu_dma(s, iova, row_data,
                                         interleaved_row_bytes, false)) {
                return false;
            }
            for (unsigned int column = 0; column < view->width; column++) {
                for (unsigned int channel = 0;
                     channel < cna->input_channels_valid; channel++) {
                    size_t source =
                        column * cna->input_channels_valid + channel;
                    size_t destination = rockchip_rknn_feature_index(
                        view->width, view->height, view->atom, channel,
                        row, column);

                    input[destination * element_bytes] =
                        rockchip_rknn_cna_convert_input(
                        cna, channel, row_data[source]);
                }
            }
        }
        return true;
    }

    for (unsigned int surface = 0; surface < surfaces; surface++) {
        for (unsigned int row = 0; row < view->height; row++) {
            uint64_t iova = (uint64_t)view->iova +
                            surface * surface_bytes + row * line_bytes;
            size_t offset = (surface * view->height + row) *
                            view->width * view->atom;

            if (iova > UINT32_MAX ||
                !rockchip_rknn_iommu_dma(s, iova,
                                         input + offset * element_bytes,
                                         row_bytes, false)) {
                return false;
            }
            if (!(cna->cvt_con0 & ROCKCHIP_RKNN_CNA_CVT_BYPASS)) {
                for (unsigned int column = 0; column < view->width;
                     column++) {
                    for (unsigned int lane = 0; lane < view->atom; lane++) {
                        size_t index = offset + column * view->atom + lane;

                        if (cna->per_channel_cvt & BIT(lane)) {
                            input[index] = rockchip_rknn_cna_convert_input(
                                cna, lane % ARRAY_SIZE(cna->cvt_channel),
                                input[index]);
                        }
                    }
                }
            }
        }
    }
    return true;
}

static bool rockchip_rknn_write_strided_output(
    RockchipRKNNCoreState *s, const RockchipRKNNTensorView *view,
    unsigned int valid_channels, const void *output, size_t element_bytes)
{
    const uint8_t *bytes = output;
    const size_t compact_surface_bytes =
        view->width * view->height * view->atom * element_bytes;
    const uint64_t surface_bytes =
        (uint64_t)view->surface_stride * 16;
    const unsigned int surfaces = DIV_ROUND_UP(valid_channels, view->atom);
    (void)s;

    if (!valid_channels || valid_channels > view->channels ||
        !rockchip_rknn_strided_output_layout_valid(view, element_bytes)) {
        return false;
    }

    for (unsigned int surface = 0; surface < surfaces; surface++) {
        uint64_t iova = (uint64_t)view->iova + surface * surface_bytes;

        if (iova > UINT32_MAX ||
            !rockchip_rknn_iommu_dma(
                s, iova, (void *)(bytes + surface * compact_surface_bytes),
                compact_surface_bytes, true)) {
            return false;
        }
    }
    return true;
}

static RockchipRKNNExecutionResult rockchip_rknn_write_depthwise_int32_output(
    RockchipRKNNCoreState *s, const RockchipRKNNPipelineTask *task,
    const uint32_t *output)
{
    const RockchipRKNNTensorView *view = &task->dpu.output;
    RockchipRKNNDepthwiseOutputLayout layout;
    g_autofree uint32_t *block = NULL;

    if (!rockchip_rknn_depthwise_int32_layout(task, &layout)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    block = g_try_new0(uint32_t, layout.plane_atom);
    if (!block) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    for (unsigned int row = 0; row < task->core.height; row++) {
        uint64_t surface = row / layout.rows_per_surface;
        uint64_t surface_row = row % layout.rows_per_surface;

        for (unsigned int column = 0; column < task->core.width; column++) {
            for (size_t plane = 0; plane < layout.planes; plane++) {
                uint64_t block_offset = surface * layout.surface_words +
                    plane * layout.plane_words +
                    (surface_row * task->core.width + column) *
                    layout.plane_atom;
                uint64_t block_iova = view->iova + (block_offset << 2);

                memset(block, 0, layout.plane_atom * sizeof(*block));
                for (unsigned int lane = 0; lane < view->atom; lane++) {
                    size_t channel = plane * view->atom + lane;

                    if (channel < task->dpu.output_channels_valid) {
                        size_t index = rockchip_rknn_feature_index(
                            view->width, view->height, view->atom,
                            channel, row, column);

                        block[lane] = output[index];
                    }
                }
                if (block_iova > UINT32_MAX ||
                    !rockchip_rknn_iommu_dma(s, block_iova, block,
                                             layout.plane_atom *
                                             sizeof(*block),
                                             true)) {
                    return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
                }
            }
        }
    }
    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static float_status rockchip_rknn_fp16_status(void)
{
    float_status status = {};

    set_float_rounding_mode(float_round_nearest_even, &status);
    set_float_default_nan_pattern(0x40, &status);
    set_default_nan_mode(true, &status);
    set_float_detect_tininess(float_tininess_after_rounding, &status);
    return status;
}

static bool rockchip_rknn_read_fp16_interleaved_input(
    RockchipRKNNCoreState *s, const RockchipRKNNCNAConfig *cna,
    uint8_t *input, size_t width, size_t height)
{
    size_t row_values = width * cna->input_channels_valid;
    size_t row_bytes = row_values * sizeof(uint16_t);
    size_t source_stride = cna->input.line_stride *
                           cna->input_channels_valid * sizeof(uint16_t);

    for (size_t row = 0; row < height; row++) {
        uint64_t source = (uint64_t)cna->input.iova + row * source_stride;

        if (source > UINT32_MAX ||
            !rockchip_rknn_iommu_dma(s, source, input + row * row_bytes,
                                     row_bytes, false)) {
            return false;
        }
    }
    return true;
}

static float32 rockchip_rknn_dpu_fp16_stage_apply(
    uint32_t cfg, uint32_t alu_operand, uint32_t mul_cfg,
    uint32_t relux_cmp, bool rdma_alu_valid, uint32_t rdma_alu,
    float_status *status, float32 value)
{
    if (cfg & ROCKCHIP_RKNN_DPU_STAGE_BYPASS) {
        return value;
    }
    if (!(cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_BYPASS)) {
        float32 operand = make_float32(
            cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_SOURCE ? rdma_alu :
                                                       alu_operand);
        unsigned int algorithm = extract32(
            cfg, ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_SHIFT,
            ROCKCHIP_RKNN_DPU_STAGE_ALU_ALGO_LENGTH);

        g_assert(!(cfg & ROCKCHIP_RKNN_DPU_STAGE_ALU_SOURCE) ||
                 rdma_alu_valid);
        value = algorithm == 2 ? float32_add(value, operand, status) :
                                 float32_sub(value, operand, status);
    }
    if (!(cfg & ROCKCHIP_RKNN_DPU_STAGE_MUL_BYPASS) &&
        (!(cfg & ROCKCHIP_RKNN_DPU_STAGE_MUL_PRELU) ||
         float32_lt(value, float32_zero, status))) {
        float32 multiplier = float16_to_float32(
            make_float16(mul_cfg >> ROCKCHIP_RKNN_DPU_MUL_OPERAND_SHIFT),
            true, status);

        value = float32_mul(value, multiplier, status);
    }
    if (!(cfg & ROCKCHIP_RKNN_DPU_STAGE_RELU_BYPASS)) {
        if (float32_lt(value, float32_zero, status)) {
            value = float32_zero;
        }
        if ((cfg & ROCKCHIP_RKNN_DPU_STAGE_RELUX_ENABLE) &&
            float32_lt(make_float32(relux_cmp), value, status)) {
            value = make_float32(relux_cmp);
        }
    }
    return value;
}

static float32 rockchip_rknn_dpu_fp16_out_cvt(
    const RockchipRKNNDPUConfig *dpu, float_status *status, float32 value)
{
    float32 scale = int32_to_float32(dpu->out_cvt_scale, status);
    float32 offset = int32_to_float32(dpu->out_cvt_offset, status);

    value = float32_mul(value, scale, status);
    value = float32_add(value, offset, status);
    return float32_scalbn(value, -(int)dpu->out_cvt_minus_exp, status);
}

static uint16_t rockchip_rknn_dpu_fp16_lut_apply(
    RockchipRKNNCoreState *s, const RockchipRKNNDPUConfig *dpu,
    float_status *status, float32 value)
{
    int32_t fixed = float32_to_int32(value, status);
    int32_t start;
    int32_t end;
    int64_t offset;
    unsigned int table;
    unsigned int index_select;
    unsigned int lut_index;
    unsigned int fraction;
    uint64_t interpolated;
    uint64_t converted;
    float32 result;
    uint16_t bits;

    if (fixed < 0) {
        table = 0;
        start = dpu->lut_le_start;
        end = dpu->lut_le_end;
        index_select = extract32(dpu->lut_info, 8, 8);
    } else {
        table = 1;
        start = dpu->lut_lo_start;
        end = dpu->lut_lo_end;
        index_select = extract32(dpu->lut_info, 16, 8);
    }
    if (fixed <= start) {
        lut_index = 0;
        fraction = 0;
    } else if (fixed >= end) {
        lut_index = ROCKCHIP_RKNN_LUT_ENTRIES - 1;
        fraction = 0;
    } else {
        offset = (int64_t)fixed - start;
        lut_index = offset >> index_select;
        fraction = offset & ((1U << index_select) - 1);
    }
    if (lut_index >= ROCKCHIP_RKNN_LUT_ENTRIES - 1) {
        interpolated =
            s->execution_lut[table][ROCKCHIP_RKNN_LUT_ENTRIES - 1];
    } else {
        uint64_t denominator = 1U << index_select;
        uint64_t numerator =
            (uint64_t)s->execution_lut[table][lut_index] *
                (denominator - fraction) +
            (uint64_t)s->execution_lut[table][lut_index + 1] * fraction;

        interpolated = table == 0 ? numerator / denominator :
                                    numerator / denominator + 1;
    }
    converted = interpolated * dpu->out_cvt_scale;
    if (table == 0) {
        converted += dpu->out_cvt_offset;
    }
    result = int64_to_float32(converted, status);
    result = float32_scalbn(result, -(int)dpu->out_cvt_minus_exp, status);
    bits = float16_val(float32_to_float16(result, true, status));
    if (bits && bits < 0x400) {
        bits = 0x400;
    }
    return bits;
}

static RockchipRKNNExecutionResult rockchip_rknn_execute_fp16(
    RockchipRKNNCoreState *s, const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const size_t input_element_bytes = sizeof(uint16_t);
    const size_t output_element_bytes =
        task->dpu.output_precision == 2 ? sizeof(uint16_t) : sizeof(uint32_t);
    const size_t weight_element_bytes = sizeof(uint16_t);
    const size_t input_atom = task->cna.input.atom;
    const bool compact_input = rockchip_rknn_cna_fp16_compact(&task->cna);
    const bool interleaved_input =
        rockchip_rknn_cna_fp16_interleaved(&task->cna);
    const bool compact_weights = !task->core.quantify;
    const bool compact_output = compact_weights &&
                                !task->dpu.output.surface_stride;
    const bool bs_rdma = task->enabled_blocks &
                         ROCKCHIP_RKNN_BLOCK_DPU_RDMA;
    const bool ew_rdma = bs_rdma &&
                         rockchip_rknn_dpu_ew_uses_rdma(
                             task->dpu.ew_cfg);
    const bool lut = !(task->dpu.ew_cfg & ROCKCHIP_RKNN_DPU_EW_BYPASS) &&
                     !(task->dpu.ew_cfg & ROCKCHIP_RKNN_DPU_EW_LUT_BYPASS);
    const size_t weight_input_atom = 32;
    const size_t kernel_area = (size_t)task->cna.kernel_width *
                               task->cna.kernel_height;
    size_t rounded_input_channels;
    size_t rounded_output_channels;
    size_t input_values;
    size_t input_bytes;
    size_t weight_storage_channels;
    size_t rounded_weight_channels;
    size_t rounded_weight_kernels;
    size_t required_weight_bytes;
    size_t required_weight_values;
    size_t input_float_bytes;
    size_t weight_float_bytes;
    size_t input_channel_offset_bytes = 0;
    size_t output_values;
    size_t output_bytes;
    size_t bs_bytes = 0;
    size_t ew_bytes = 0;
    uint64_t input_line_bytes = 0;
    uint64_t input_surface_bytes = 0;
    const size_t logical_input_width =
        rockchip_rknn_cna_logical_width(&task->cna);
    const size_t logical_input_height =
        rockchip_rknn_cna_logical_height(&task->cna);
    const size_t execution_channels =
        rockchip_rknn_cna_execution_channels(&task->cna);
    uint64_t ignored_input_accessed;
    uint64_t host_bytes = 0;
    g_autofree uint8_t *input = NULL;
    g_autofree uint8_t *weights = NULL;
    g_autofree uint8_t *output = NULL;
    g_autofree uint8_t *bs_data = NULL;
    g_autofree uint16_t *ew_data = NULL;
    g_autofree float32 *input_fp32 = NULL;
    g_autofree float32 *weights_fp32 = NULL;
    g_autofree size_t *input_channel_offsets = NULL;

    if (!rockchip_rknn_mac_budget_valid(s, task, false) ||
        (interleaved_input ?
         !rockchip_rknn_size_mul3(logical_input_width,
                                  logical_input_height,
                                  task->cna.input_channels_valid,
                                  &input_values) :
         !rockchip_rknn_size_round_up(task->cna.input.channels, input_atom,
                                      &rounded_input_channels) ||
         !rockchip_rknn_size_mul3(task->cna.input.width,
                                  task->cna.input.height,
                                  rounded_input_channels, &input_values)) ||
        !rockchip_rknn_size_mul(input_values, input_element_bytes,
                                &input_bytes) ||
        !rockchip_rknn_size_mul(task->cna.weight_bytes_per_kernel,
                                1, &weight_storage_channels)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (compact_input) {
        if (!rockchip_rknn_iova_length_valid(task->cna.input.iova,
                                              input_bytes)) {
            return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
        }
    } else if (interleaved_input) {
        uint64_t source_line_bytes;
        uint64_t source_accessed;

        if (!rockchip_rknn_u64_mul(task->cna.input.line_stride,
                                   task->cna.input_channels_valid,
                                   &source_line_bytes) ||
            !rockchip_rknn_u64_mul(source_line_bytes, input_element_bytes,
                                   &source_line_bytes) ||
            !rockchip_rknn_u64_mul(logical_input_width,
                                   task->cna.input_channels_valid,
                                   &source_accessed) ||
            !rockchip_rknn_u64_mul(source_accessed, input_element_bytes,
                                   &source_accessed) ||
            !rockchip_rknn_u64_mul(
                logical_input_height - 1, source_line_bytes,
                &ignored_input_accessed) ||
            __builtin_add_overflow(ignored_input_accessed, source_accessed,
                                   &ignored_input_accessed) ||
            !rockchip_rknn_iova_length_valid(task->cna.input.iova,
                                              ignored_input_accessed)) {
            return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
        }
    } else if (!rockchip_rknn_input_strides(
                   &task->cna.input, &input_line_bytes,
                   &input_surface_bytes) ||
               !rockchip_rknn_u64_mul(
                   DIV_ROUND_UP(task->cna.input.channels, input_atom) - 1,
                   input_surface_bytes, &ignored_input_accessed) ||
               !rockchip_rknn_iova_length_valid(
                   task->cna.input.iova,
                   ignored_input_accessed +
                   input_line_bytes * (task->cna.input.height - 1) +
                   task->cna.input.width * input_atom *
                   input_element_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (!kernel_area ||
        task->cna.weight_bytes_per_kernel %
        (kernel_area * weight_element_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    weight_storage_channels = task->cna.weight_bytes_per_kernel /
                              (kernel_area * weight_element_bytes);
    if (weight_storage_channels < execution_channels) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (compact_weights) {
        if (!rockchip_rknn_size_mul(task->cna.weight_kernels,
                                    task->cna.weight_bytes_per_kernel,
                                    &required_weight_bytes)) {
            return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
        }
    } else if (!rockchip_rknn_size_round_up(weight_storage_channels,
                                            weight_input_atom,
                                            &rounded_weight_channels) ||
               !rockchip_rknn_size_round_up(task->cna.weight_kernels, 16,
                                            &rounded_weight_kernels) ||
               !rockchip_rknn_size_mul3(rounded_weight_kernels,
                                        rounded_weight_channels, kernel_area,
                                        &required_weight_bytes) ||
               !rockchip_rknn_size_mul(required_weight_bytes,
                                       weight_element_bytes,
                                       &required_weight_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (compact_output) {
        rounded_output_channels = task->dpu.wdma_channels;
        if (!rounded_output_channels) {
            return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
        }
    } else if (!rockchip_rknn_size_round_up(task->dpu.output.channels,
                                            task->dpu.output.atom,
                                            &rounded_output_channels)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (required_weight_bytes != task->cna.weight_bytes ||
        !rockchip_rknn_iova_length_valid(task->cna.weight_iova,
                                         required_weight_bytes) ||
        !rockchip_rknn_size_mul3(task->core.width, task->core.height,
                                 rounded_output_channels, &output_values) ||
        !rockchip_rknn_size_mul(output_values, output_element_bytes,
                                &output_bytes) ||
        (!compact_output && !rockchip_rknn_strided_output_layout_valid(
            &task->dpu.output, output_element_bytes)) ||
        (compact_output && !rockchip_rknn_iova_length_valid(
            task->dpu.output.iova, output_bytes)) ||
        (bs_rdma &&
         (!rockchip_rknn_size_mul(task->dpu_rdma.channels,
                                  sizeof(uint32_t), &bs_bytes) ||
          !rockchip_rknn_iova_length_valid(task->dpu_rdma.bs_iova,
                                           bs_bytes))) ||
        (ew_rdma &&
         !rockchip_rknn_size_mul(output_values, sizeof(*ew_data),
                                  &ew_bytes))) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    required_weight_values = required_weight_bytes / weight_element_bytes;
    if (!rockchip_rknn_size_mul(input_values, sizeof(*input_fp32),
                                &input_float_bytes) ||
        !rockchip_rknn_size_mul(required_weight_values,
                                sizeof(*weights_fp32),
                                &weight_float_bytes) ||
        (!interleaved_input &&
         !rockchip_rknn_size_mul(execution_channels,
                                 sizeof(*input_channel_offsets),
                                 &input_channel_offset_bytes)) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, input_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes,
                                       required_weight_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes,
                                       input_float_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes,
                                       weight_float_bytes) ||
        (!interleaved_input &&
         !rockchip_rknn_host_budget_add(
             s, &host_bytes, input_channel_offset_bytes)) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, output_bytes) ||
        (bs_rdma &&
         !rockchip_rknn_host_budget_add(s, &host_bytes, bs_bytes)) ||
        (ew_rdma &&
         !rockchip_rknn_host_budget_add(s, &host_bytes, ew_bytes))) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    input = g_try_malloc0(input_bytes);
    weights = g_try_malloc(required_weight_bytes);
    output = g_try_malloc0(output_bytes);
    if (bs_rdma) {
        bs_data = g_try_malloc(bs_bytes);
    }
    if (ew_rdma) {
        ew_data = g_try_malloc(ew_bytes);
    }
    input_fp32 = g_try_malloc(input_float_bytes);
    weights_fp32 = g_try_malloc(weight_float_bytes);
    if (!interleaved_input) {
        input_channel_offsets = g_try_malloc(input_channel_offset_bytes);
    }
    if (!input || !weights || !output || (bs_rdma && !bs_data) ||
        (ew_rdma && !ew_data) || !input_fp32 || !weights_fp32 ||
        (!interleaved_input && !input_channel_offsets)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (task->dpu.offset_pend) {
        for (unsigned int row = 0; row < task->core.height; row++) {
            for (unsigned int column = 0; column < task->core.width;
                 column++) {
                for (unsigned int channel =
                         task->dpu.output_channels_valid;
                     channel < task->dpu.output.channels; channel++) {
                    size_t index = rockchip_rknn_feature_index(
                        task->dpu.output.width, task->dpu.output.height,
                        task->dpu.output.atom, channel, row, column);

                    stw_le_p(output + index * output_element_bytes,
                             task->dpu.offset_pend);
                }
            }
        }
    }
    if ((compact_input ?
         !rockchip_rknn_iommu_dma(s, task->cna.input.iova, input,
                                  input_bytes, false) :
         interleaved_input ?
         !rockchip_rknn_read_fp16_interleaved_input(
             s, &task->cna, input, logical_input_width,
             logical_input_height) :
         !rockchip_rknn_read_strided_input(
             s, &task->cna, input, input_line_bytes,
             input_surface_bytes)) ||
        !rockchip_rknn_iommu_dma(s, task->cna.weight_iova, weights,
                                 required_weight_bytes, false) ||
        (bs_rdma &&
         !rockchip_rknn_iommu_dma(s, task->dpu_rdma.bs_iova, bs_data,
                                  bs_bytes, false)) ||
        (ew_rdma &&
         !rockchip_rknn_read_fp16_erdma_operand(
             s, &task->dpu_rdma, ew_data, task->core.width,
             task->core.height, rounded_output_channels))) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }
    float_status conversion_status = rockchip_rknn_fp16_status();

    for (size_t index = 0; index < input_values; index++) {
        input_fp32[index] = float16_to_float32(
            make_float16(lduw_le_p(input + index * input_element_bytes)),
            true, &conversion_status);
    }
    for (size_t index = 0; index < required_weight_values; index++) {
        weights_fp32[index] = float16_to_float32(
            make_float16(lduw_le_p(
                weights + index * weight_element_bytes)), true,
            &conversion_status);
    }
    g_clear_pointer(&input, g_free);
    g_clear_pointer(&weights, g_free);
    for (size_t channel = 0;
         !interleaved_input && channel < execution_channels; channel++) {
        input_channel_offsets[channel] =
            (channel / input_atom) * task->cna.input.height *
                task->cna.input.width * input_atom + channel % input_atom;
    }

    float_status status = rockchip_rknn_fp16_status();

    for (unsigned int row = 0; row < task->core.height; row++) {
        for (unsigned int column = 0; column < task->core.width; column++) {
            for (unsigned int out = 0;
                 out < task->dpu.output_channels_valid; out++) {
                float32 value = float32_zero;
                for (unsigned int kernel_row = 0;
                     kernel_row < task->cna.kernel_height; kernel_row++) {
                    int input_row;
                    bool input_row_valid = true;

                    if (task->cna.deconv) {
                        int numerator = row + task->cna.kernel_height - 1 -
                                        task->cna.pad_top - kernel_row;
                        unsigned int stride =
                            task->cna.deconv_stride_y + 1;

                        input_row_valid = numerator >= 0 &&
                                          numerator % stride == 0;
                        input_row = input_row_valid ? numerator / stride : 0;
                    } else {
                        input_row = (int)row * task->cna.stride_y -
                                    task->cna.pad_top + kernel_row;
                    }

                    for (unsigned int kernel_column = 0;
                         kernel_column < task->cna.kernel_width;
                         kernel_column++) {
                        int input_column;
                        bool input_column_valid = true;
                        unsigned int kernel =
                            kernel_row * task->cna.kernel_width +
                            kernel_column;

                        if (task->cna.deconv) {
                            int numerator =
                                column + task->cna.kernel_width - 1 -
                                task->cna.pad_left - kernel_column;
                            unsigned int stride =
                                task->cna.deconv_stride_x + 1;

                            input_column_valid = numerator >= 0 &&
                                numerator % stride == 0;
                            input_column = input_column_valid ?
                                numerator / stride : 0;
                        } else {
                            input_column =
                                (int)column * task->cna.stride_x -
                                task->cna.pad_left + kernel_column;
                        }

                        unsigned int first_channel =
                            task->core.depthwise ? out : 0;
                        unsigned int channel_count =
                            task->core.depthwise ? 1 :
                            execution_channels;

                        for (unsigned int channel = first_channel;
                             channel < first_channel + channel_count;
                             channel++) {
                            float32 input_value = float32_zero;
                            float32 weight_value;
                            size_t weight_index =
                                compact_weights ?
                                (task->core.depthwise ?
                                 ((channel / weight_input_atom) *
                                      weight_input_atom * kernel_area +
                                  kernel * MIN(weight_input_atom,
                                               weight_storage_channels -
                                               (channel / weight_input_atom) *
                                                   weight_input_atom) +
                                  channel % weight_input_atom) :
                                 (out * kernel_area + kernel) *
                                     weight_storage_channels + channel) :
                                rockchip_rknn_fp16_weight_index(
                                    weight_storage_channels, kernel_area,
                                    out, kernel, channel);

                            if (weight_index >= required_weight_values) {
                                return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
                            }

                            if (input_row_valid && input_column_valid &&
                                input_row >= 0 &&
                                input_row < logical_input_height &&
                                input_column >= 0 &&
                                input_column < logical_input_width) {
                                size_t input_index = interleaved_input ?
                                    (input_row * logical_input_width +
                                     input_column) *
                                    task->cna.input_channels_valid + channel :
                                    input_channel_offsets[channel] +
                                    input_atom *
                                        (input_row * task->cna.input.width +
                                         input_column);
                                input_value = input_fp32[input_index];
                            }
                            weight_value = weights_fp32[weight_index];
                            value = float32_add(
                                value, float32_mul(input_value, weight_value,
                                                   &status), &status);
                        }
                    }
                }

                value = rockchip_rknn_dpu_fp16_stage_apply(
                    task->dpu.bs_cfg, stage->bs_alu_operand,
                    stage->bs_mul_cfg, task->dpu.bs_relux_cmp,
                    bs_rdma, bs_rdma ? ldl_le_p(bs_data + out * 4) : 0,
                    &status, value);
                value = rockchip_rknn_dpu_fp16_stage_apply(
                    task->dpu.bn_cfg, stage->bn_alu_operand,
                    stage->bn_mul_cfg, task->dpu.bn_relux_cmp,
                    false, 0, &status, value);

                size_t output_index = rockchip_rknn_feature_index(
                    task->dpu.output.width, task->dpu.output.height,
                    task->dpu.output.atom, out, row, column);
                if (ew_rdma) {
                    float32 operand = float16_to_float32(
                        make_float16(le16_to_cpu(ew_data[output_index])),
                        true, &status);

                    value = float32_add(value, operand, &status);
                }
                if (!(task->dpu.ew_cfg &
                      ROCKCHIP_RKNN_DPU_EW_RELU_BYPASS) &&
                    float32_lt(value, float32_zero, &status)) {
                    value = float32_zero;
                }
                if (lut) {
                    stw_le_p(output + output_index * output_element_bytes,
                             rockchip_rknn_dpu_fp16_lut_apply(
                                 s, &task->dpu, &status, value));
                } else if (task->dpu.output_precision == 2) {
                    value = rockchip_rknn_dpu_fp16_out_cvt(
                        &task->dpu, &status, value);
                    stw_le_p(output + output_index * output_element_bytes,
                             float16_val(float32_to_float16(
                                 value, true, &status)));
                } else {
                    value = rockchip_rknn_dpu_fp16_out_cvt(
                        &task->dpu, &status, value);
                    stl_le_p(output + output_index * output_element_bytes,
                             float32_val(value));
                }
            }
        }
    }
    if (compact_output ?
        !rockchip_rknn_iommu_dma(s, task->dpu.output.iova, output,
                                 output_bytes, true) :
        !rockchip_rknn_write_strided_output(
            s, &task->dpu.output, task->dpu.output_channels_valid,
            output, output_element_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
    }
    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static RockchipRKNNExecutionResult
rockchip_rknn_execute_dpu_rdma_int8_pipeline(
    RockchipRKNNCoreState *s, const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    const RockchipRKNNTensorView *output_view = &task->dpu.output;
    g_autofree uint8_t *input = NULL;
    g_autofree uint8_t *ew_input = NULL;
    g_autofree uint8_t *output = NULL;
    size_t input_storage_channels;
    size_t output_storage_channels;
    size_t input_bytes;
    size_t ew_input_bytes = 0;
    uint64_t ew_input_accessed = 0;
    size_t output_bytes;
    uint64_t host_bytes = 0;
    bool ew_rdma = rockchip_rknn_dpu_ew_uses_rdma(task->dpu.ew_cfg);
    bool reshape = rockchip_rknn_dpu_rdma_int8_reshape_is_supported(task);
    uint64_t input_line_atoms = rdma->width +
        extract32(rdma->src_dma_cfg, 19, 13);
    int64_t signed_input_surface_atoms =
        (int64_t)rdma->width * rdma->height +
        (int64_t)(input_line_atoms - rdma->width) *
            (rdma->height - 1) +
        sextract32(rdma->surface_notch, 0, 28);
    uint64_t input_surface_atoms;
    uint64_t input_accessed;
    unsigned int input_surfaces;

    if (!rockchip_rknn_dpu_work_budget_valid(
            s, output_view->width, output_view->height,
            task->dpu.output_channels_valid) ||
        !rockchip_rknn_size_round_up(rdma->channels, 16,
                                     &input_storage_channels) ||
        !rockchip_rknn_size_round_up(output_view->channels,
                                     output_view->atom,
                                     &output_storage_channels) ||
        !rockchip_rknn_size_mul3(rdma->width, rdma->height,
                                 input_storage_channels, &input_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    input_surfaces = DIV_ROUND_UP(input_storage_channels, 16);
    if (!input_surfaces || signed_input_surface_atoms <= 0) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    input_surface_atoms = signed_input_surface_atoms;
    if (!rockchip_rknn_u64_mul(input_surfaces - 1, input_surface_atoms,
                               &input_accessed) ||
        __builtin_add_overflow(
            input_accessed,
            (uint64_t)(rdma->height - 1) * input_line_atoms + rdma->width,
            &input_accessed) ||
        !rockchip_rknn_u64_mul(input_accessed, 16, &input_accessed) ||
        input_accessed > SIZE_MAX ||
        (ew_rdma &&
         !rockchip_rknn_size_mul3(rdma->width, rdma->height,
                                  input_storage_channels,
                                  &ew_input_bytes)) ||
        (ew_rdma &&
         (!rockchip_rknn_u64_mul(input_surfaces - 1,
                                 rdma->ew_surface_stride,
                                 &ew_input_accessed) ||
          __builtin_add_overflow(
              ew_input_accessed,
              (uint64_t)rdma->width * rdma->height,
              &ew_input_accessed) ||
          !rockchip_rknn_u64_mul(ew_input_accessed, 16,
                                 &ew_input_accessed))) ||
        !rockchip_rknn_size_mul3(output_view->width, output_view->height,
                                 output_storage_channels, &output_bytes) ||
        !rockchip_rknn_iova_length_valid(rdma->src_iova, input_accessed) ||
        (ew_rdma &&
         !rockchip_rknn_iova_length_valid(rdma->ew_iova,
                                           ew_input_accessed)) ||
        (reshape ?
         !rockchip_rknn_iova_length_valid(output_view->iova,
                                           output_bytes) :
         !rockchip_rknn_strided_output_layout_valid(output_view, 1)) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, input_bytes) ||
        (ew_rdma &&
         !rockchip_rknn_host_budget_add(s, &host_bytes, ew_input_bytes)) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, output_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    input = g_try_malloc(input_bytes);
    if (ew_rdma) {
        ew_input = g_try_malloc(ew_input_bytes);
    }
    output = g_try_malloc0(output_bytes);
    if (!input || (ew_rdma && !ew_input) || !output) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    memset(output, task->dpu.offset_pend, output_bytes);
    for (unsigned int surface = 0; surface < input_surfaces; surface++) {
        for (unsigned int row = 0; row < rdma->height; row++) {
            uint64_t iova = (uint64_t)rdma->src_iova +
                (surface * input_surface_atoms + row * input_line_atoms) *
                16;
            size_t offset = ((size_t)surface * rdma->height + row) *
                            rdma->width * 16;

            if (iova > UINT32_MAX ||
                !rockchip_rknn_iommu_dma(
                    s, iova, input + offset, rdma->width * 16, false)) {
                return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
            }
        }
    }
    if (ew_rdma) {
        size_t surface_bytes = (size_t)rdma->width * rdma->height * 16;

        for (unsigned int surface = 0; surface < input_surfaces; surface++) {
            uint64_t iova = (uint64_t)rdma->ew_iova +
                (uint64_t)surface * rdma->ew_surface_stride * 16;

            if (iova > UINT32_MAX ||
                !rockchip_rknn_iommu_dma(
                    s, iova, ew_input + surface * surface_bytes,
                    surface_bytes, false)) {
                return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
            }
        }
    }

    for (unsigned int row = 0; row < output_view->height; row++) {
        for (unsigned int column = 0; column < output_view->width; column++) {
            uint64_t spatial_index =
                (uint64_t)row * output_view->width + column;
            unsigned int input_row = reshape ?
                spatial_index / rdma->width : row % rdma->height;
            unsigned int input_column = reshape ?
                spatial_index % rdma->width : column % rdma->width;

            for (unsigned int channel = 0;
                 channel < task->dpu.output_channels_valid; channel++) {
                size_t input_index = rockchip_rknn_feature_index(
                    rdma->width, rdma->height, 16, channel,
                    input_row, input_column);
                size_t output_index = rockchip_rknn_feature_index(
                    output_view->width, output_view->height,
                    output_view->atom, channel, row, column);
                Int128 value = int128_makes64((int8_t)input[input_index]);

                value = rockchip_rknn_dpu_stage_apply(
                    task->dpu.bs_cfg, stage->bs_alu_operand,
                    stage->bs_mul_cfg, task->dpu.bs_relux_cmp,
                    false, 0, false, 0,
                    extract32(task->dpu.data_format, 4, 6), value);
                value = rockchip_rknn_dpu_stage_apply(
                    task->dpu.bn_cfg, stage->bn_alu_operand,
                    stage->bn_mul_cfg, task->dpu.bn_relux_cmp,
                    false, 0, false, 0,
                    extract32(task->dpu.data_format, 10, 6), value);
                value = rockchip_rknn_dpu_ew_apply(
                    s, &task->dpu, stage,
                    ew_rdma ? (int8_t)ew_input[input_index] :
                    stage->ew_operand[
                        channel % ARRAY_SIZE(stage->ew_operand)], value);
                value = rockchip_rknn_out_cvt(&task->dpu, stage, value);
                output[output_index] = rockchip_rknn_saturate_i8(value);
            }
        }
    }

    if (reshape) {
        size_t row_bytes = output_view->width * 16;

        for (unsigned int row = 0; row < output_view->height; row++) {
            for (unsigned int surface = 0;
                 surface < DIV_ROUND_UP(output_storage_channels, 16);
                 surface++) {
                uint64_t iova = (uint64_t)output_view->iova +
                    ((uint64_t)row *
                         DIV_ROUND_UP(output_storage_channels, 16) +
                     surface) * row_bytes;
                size_t offset =
                    ((size_t)surface * output_view->height + row) *
                    row_bytes;

                if (iova > UINT32_MAX ||
                    !rockchip_rknn_iommu_dma(
                        s, iova, output + offset, row_bytes, true)) {
                    return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
                }
            }
        }
    } else if (!rockchip_rknn_write_strided_output(
                   s, output_view, task->dpu.output_channels_valid,
                   output, 1)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
    }
    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static RockchipRKNNExecutionResult
rockchip_rknn_execute_dpu_rdma_int16_unpool(
    RockchipRKNNCoreState *s, const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    const RockchipRKNNTensorView *output_view = &task->dpu.output;
    g_autofree uint8_t *input = NULL;
    g_autofree uint8_t *output = NULL;
    size_t input_storage_channels;
    size_t output_storage_channels;
    size_t output_height;
    size_t input_bytes;
    size_t output_bytes;
    uint64_t work_items;
    uint64_t host_bytes = 0;

    if (!rockchip_rknn_size_round_up(rdma->channels, 8,
                                     &input_storage_channels) ||
        !rockchip_rknn_size_round_up(output_view->channels, 16,
                                     &output_storage_channels)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (!rockchip_rknn_size_mul(output_view->height, 2, &output_height) ||
        !rockchip_rknn_u64_mul3(
            output_view->width, output_height,
            task->dpu.output_channels_valid, &work_items) ||
        work_items > s->functional_max_mac_operations ||
        input_storage_channels > SIZE_MAX / 2 ||
        output_storage_channels != input_storage_channels * 2 ||
        !rockchip_rknn_size_mul3(rdma->width, rdma->height,
                                 input_storage_channels * 2, &input_bytes) ||
        !rockchip_rknn_size_mul3(output_view->width, output_height,
                                 output_storage_channels, &output_bytes) ||
        output_bytes != task->dpu.surface_add ||
        !rockchip_rknn_iova_length_valid(rdma->src_iova, input_bytes) ||
        !rockchip_rknn_iova_length_valid(output_view->iova, output_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, input_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, output_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (!rockchip_rknn_iommu_range_mapped(s, rdma->src_iova,
                                           input_bytes, false)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }
    if (!rockchip_rknn_iommu_range_mapped(s, output_view->iova,
                                           output_bytes, true)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
    }

    input = g_try_malloc(input_bytes);
    output = g_try_malloc0(output_bytes);
    if (!input || !output) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (!rockchip_rknn_iommu_dma(s, rdma->src_iova, input,
                                 input_bytes, false)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }

    for (unsigned int row = 0; row < output_height; row++) {
        for (unsigned int column = 0; column < output_view->width; column++) {
            for (unsigned int channel = 0;
                 channel < output_view->channels; channel++) {
                size_t input_index = rockchip_rknn_feature_index(
                    rdma->width, rdma->height, 8, channel / 2,
                    row / 2, column / 2) * 2 + channel % 2;
                size_t output_index = rockchip_rknn_feature_index(
                    output_view->width, output_height, 16,
                    channel, row, column);

                output[output_index] = input[input_index];
            }
        }
    }

    if (!rockchip_rknn_iommu_dma(s, output_view->iova, output,
                                 output_bytes, true)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
    }
    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static RockchipRKNNExecutionResult
rockchip_rknn_execute_dpu_rdma_int8_to_fp16(
    RockchipRKNNCoreState *s, const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    const RockchipRKNNTensorView *output_view = &task->dpu.output;
    g_autofree int8_t *input = NULL;
    g_autofree uint16_t *output = NULL;
    size_t input_storage_channels;
    size_t output_storage_channels;
    size_t input_values;
    size_t output_values;
    size_t input_bytes;
    size_t output_bytes;
    uint64_t host_bytes = 0;

    if (!rockchip_rknn_dpu_work_budget_valid(
            s, output_view->width, output_view->height,
            task->dpu.output_channels_valid) ||
        !rockchip_rknn_size_round_up(rdma->channels, 16,
                                     &input_storage_channels) ||
        !rockchip_rknn_size_round_up(output_view->channels,
                                     output_view->atom,
                                     &output_storage_channels) ||
        !rockchip_rknn_size_mul3(rdma->width, rdma->height,
                                 input_storage_channels, &input_values) ||
        !rockchip_rknn_size_mul3(output_view->width, output_view->height,
                                 output_storage_channels, &output_values) ||
        !rockchip_rknn_size_mul(input_values, sizeof(*input), &input_bytes) ||
        !rockchip_rknn_size_mul(output_values, sizeof(*output),
                                &output_bytes) ||
        !rockchip_rknn_iova_length_valid(rdma->src_iova, input_bytes) ||
        !rockchip_rknn_strided_output_layout_valid(
            output_view, sizeof(*output)) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, input_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, output_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }

    if (!rockchip_rknn_iommu_range_mapped(s, rdma->src_iova,
                                           input_bytes, false)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }

    input = g_try_malloc(input_bytes);
    output = g_try_malloc0(output_bytes);
    if (!input || !output) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (!rockchip_rknn_iommu_dma(s, rdma->src_iova, input,
                                 input_bytes, false)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }

    for (unsigned int row = 0; row < output_view->height; row++) {
        for (unsigned int column = 0; column < output_view->width; column++) {
            for (unsigned int channel = 0;
                 channel < task->dpu.output_channels_valid; channel++) {
                size_t input_index = rockchip_rknn_feature_index(
                    rdma->width, rdma->height, 16, channel, row, column);
                size_t output_index = rockchip_rknn_feature_index(
                    output_view->width, output_view->height,
                    output_view->atom, channel, row, column);
                float_status status = rockchip_rknn_fp16_status();
                float32 value = int32_to_float32(input[input_index], &status);

                output[output_index] = cpu_to_le16(float16_val(
                    float32_to_float16(value, true, &status)));
            }
        }
    }

    if (!rockchip_rknn_write_strided_output(
            s, output_view, task->dpu.output_channels_valid,
            output, sizeof(*output))) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
    }
    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static RockchipRKNNExecutionResult rockchip_rknn_execute_dpu_rdma_fp16_pipeline(
    RockchipRKNNCoreState *s, const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    const RockchipRKNNTensorView *view = &task->dpu.output;
    size_t channels;
    size_t source_width = rdma->width - extract32(rdma->pad_cfg, 0, 3);
    size_t source_height = rdma->height - extract32(rdma->pad_cfg, 4, 3);
    size_t input_values;
    size_t output_values;
    size_t input_bytes;
    size_t output_bytes;
    uint64_t host_bytes = 0;
    g_autofree uint16_t *input = NULL;
    g_autofree uint16_t *operand = NULL;
    g_autofree uint16_t *output = NULL;
    RockchipRKNNDpuRdmaFP16EWMode ew_mode =
        rockchip_rknn_dpu_fp16_ew_mode(&task->dpu, stage);
    bool ew_enabled = ew_mode != ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_BYPASS;
    bool ew_dual_port = extract32(
        task->dpu.ew_cfg, ROCKCHIP_RKNN_DPU_EW_DATA_MODE_SHIFT,
        ROCKCHIP_RKNN_DPU_EW_DATA_MODE_LENGTH) == 1;

    if (!rockchip_rknn_dpu_work_budget_valid(
            s, view->width, view->height, task->dpu.output_channels_valid) ||
        !rockchip_rknn_size_round_up(rdma->channels, 8, &channels) ||
        !rockchip_rknn_size_mul3(source_width, source_height, channels,
                                 &input_values) ||
        !rockchip_rknn_size_mul3(view->width, view->height, channels,
                                 &output_values) ||
        !rockchip_rknn_size_mul(input_values, sizeof(*input),
                                &input_bytes) ||
        !rockchip_rknn_fp16_rdma_source_layout_valid(
            rdma, source_width, source_height, channels,
            &input_bytes) ||
        !rockchip_rknn_size_mul(output_values, sizeof(*output),
                                &output_bytes) ||
        (ew_enabled && !ew_dual_port &&
         !rockchip_rknn_iova_length_valid(rdma->ew_iova, output_bytes)) ||
        !rockchip_rknn_iova_length_valid(view->iova, output_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, input_bytes) ||
        (ew_enabled &&
         !rockchip_rknn_host_budget_add(s, &host_bytes, output_bytes)) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, output_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (ew_enabled && !ew_dual_port &&
         !rockchip_rknn_iommu_range_mapped(
             s, rdma->ew_iova, output_bytes, false)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }
    input = g_try_new(uint16_t, input_values);
    if (ew_enabled) {
        operand = g_try_new(uint16_t, output_values);
    }
    output = g_try_new(uint16_t, output_values);
    if (!input || (ew_enabled && !operand) || !output) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    for (size_t index = 0; index < output_values; index++) {
        stw_le_p(&output[index], task->dpu.offset_pend);
    }
    if (!rockchip_rknn_read_fp16_rdma_source(
            s, rdma, input, source_width, source_height, channels) ||
        (ew_enabled &&
         !(ew_dual_port ?
           rockchip_rknn_read_fp16_erdma_operand(
               s, rdma, operand, view->width, view->height, channels) :
           rockchip_rknn_iommu_dma(
               s, rdma->ew_iova, operand, output_bytes, false)))) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }

    for (unsigned int row = 0; row < view->height; row++) {
        for (unsigned int column = 0; column < view->width; column++) {
            for (unsigned int channel = 0;
                 channel < task->dpu.output_channels_valid; channel++) {
                size_t output_index = rockchip_rknn_feature_index(
                    view->width, view->height, 8, channel, row, column);
                float_status status = rockchip_rknn_fp16_status();
                unsigned int pad_left = extract32(rdma->pad_cfg, 0, 3);
                unsigned int pad_top = extract32(rdma->pad_cfg, 4, 3);
                uint16_t input_value;
                float32 value;

                if (row < pad_top || column < pad_left) {
                    input_value = extract32(rdma->pad_cfg, 16, 16);
                } else {
                    unsigned int source_row = source_height == 1 ? 0 :
                                              row - pad_top;
                    unsigned int source_column = source_width == 1 ? 0 :
                                                 column - pad_left;
                    size_t input_index = rockchip_rknn_feature_index(
                        source_width, source_height, 8, channel,
                        source_row, source_column);

                    input_value = le16_to_cpu(input[input_index]);
                }
                value = float16_to_float32(
                    make_float16(input_value), true, &status);

                value = rockchip_rknn_dpu_fp16_stage_apply(
                    task->dpu.bs_cfg, stage->bs_alu_operand,
                    stage->bs_mul_cfg, task->dpu.bs_relux_cmp,
                    false, 0, &status, value);
                if (ew_enabled) {
                    float32 rhs = float16_to_float32(
                        make_float16(le16_to_cpu(operand[output_index])), true,
                        &status);

                    if (ew_mode == ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_ADD) {
                        value = float32_add(value, rhs, &status);
                    } else if (ew_mode ==
                               ROCKCHIP_RKNN_DPU_RDMA_FP16_EW_DIVIDE) {
                        uint32_t bits;
                        unsigned int exponent;

                        value = float32_div(value, rhs, &status);
                        bits = float32_val(value);
                        exponent = extract32(bits, 23, 8);
                        if (exponent && exponent != 0xff) {
                            bits &= ~MAKE_64BIT_MASK(0, 10);
                            value = make_float32(bits);
                        }
                    } else {
                        value = float32_sub(value, rhs, &status);
                    }
                }
                output[output_index] = cpu_to_le16(float16_val(
                    float32_to_float16(value, true, &status)));
            }
        }
    }
    if (!rockchip_rknn_write_strided_output(
            s, view, task->dpu.output_channels_valid,
            output, sizeof(*output))) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
    }
    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static RockchipRKNNExecutionResult
rockchip_rknn_execute_dpu_rdma_fp16_to_int8_combine(
    RockchipRKNNCoreState *s, const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    const RockchipRKNNTensorView *view = &task->dpu.output;
    size_t spatial;
    size_t surfaces = view->channels / 16;
    size_t port_values;
    size_t port_bytes;
    size_t output_values;
    size_t output_bytes;
    uint64_t surface_atoms;
    uint64_t accessed_atoms;
    uint64_t accessed_bytes;
    uint64_t host_bytes = 0;
    g_autofree uint16_t *main_input = NULL;
    g_autofree uint16_t *ew_input = NULL;
    g_autofree int8_t *output = NULL;

    if (!rockchip_rknn_dpu_work_budget_valid(
            s, view->width, view->height,
            task->dpu.output_channels_valid) ||
        !rockchip_rknn_size_mul(view->width, view->height, &spatial) ||
        !surfaces ||
        !rockchip_rknn_size_mul3(spatial, surfaces, 8, &port_values) ||
        !rockchip_rknn_size_mul(port_values, sizeof(*main_input),
                                &port_bytes) ||
        !rockchip_rknn_size_mul3(spatial, view->channels,
                                 sizeof(*output), &output_bytes) ||
        !rockchip_rknn_size_mul(spatial, view->channels, &output_values) ||
        !rockchip_rknn_u64_mul(surfaces - 1,
                               spatial + rdma->surface_notch,
                               &accessed_atoms) ||
        __builtin_add_overflow(accessed_atoms, spatial, &accessed_atoms) ||
        !rockchip_rknn_u64_mul(accessed_atoms, 16, &accessed_bytes) ||
        accessed_bytes > SIZE_MAX ||
        !rockchip_rknn_iova_length_valid(rdma->src_iova, accessed_bytes) ||
        !rockchip_rknn_iova_length_valid(rdma->ew_iova, accessed_bytes) ||
        !rockchip_rknn_strided_output_layout_valid(view, sizeof(*output)) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, port_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, port_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, output_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (!rockchip_rknn_iommu_range_mapped(
            s, rdma->src_iova, accessed_bytes, false) ||
        !rockchip_rknn_iommu_range_mapped(
            s, rdma->ew_iova, accessed_bytes, false)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }
    main_input = g_try_new(uint16_t, port_values);
    ew_input = g_try_new(uint16_t, port_values);
    output = g_try_new0(int8_t, output_values);
    if (!main_input || !ew_input || !output) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }

    surface_atoms = spatial + rdma->surface_notch;
    for (size_t surface = 0; surface < surfaces; surface++) {
        for (size_t row = 0; row < view->height; row++) {
            uint64_t atom_offset = surface * surface_atoms +
                                   row * view->width;
            uint64_t main_iova = rdma->src_iova + atom_offset * 16;
            uint64_t ew_iova = rdma->ew_iova + atom_offset * 16;
            size_t offset = (surface * view->height + row) *
                            view->width * 8;
            size_t row_bytes = view->width * 16;

            if (main_iova > UINT32_MAX || ew_iova > UINT32_MAX ||
                !rockchip_rknn_iommu_dma(
                    s, main_iova, main_input + offset, row_bytes, false) ||
                !rockchip_rknn_iommu_dma(
                    s, ew_iova, ew_input + offset, row_bytes, false)) {
                return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
            }
        }
    }

    for (size_t row = 0; row < view->height; row++) {
        for (size_t column = 0; column < view->width; column++) {
            for (size_t channel = 0;
                 channel < task->dpu.output_channels_valid; channel++) {
                size_t surface = channel / 16;
                size_t lane = channel % 16;
                size_t input_index = surface * spatial * 8 +
                    (row * view->width + column) * 8 + lane % 8;
                size_t output_index = rockchip_rknn_feature_index(
                    view->width, view->height, 16, channel, row, column);
                float_status status = rockchip_rknn_fp16_status();
                uint16_t input_bits = le16_to_cpu(
                    lane < 8 ? main_input[input_index] :
                               ew_input[input_index]);
                float32 value = float16_to_float32(
                    make_float16(input_bits), true, &status);
                int32_t fixed;

                value = rockchip_rknn_dpu_fp16_stage_apply(
                    task->dpu.bs_cfg, stage->bs_alu_operand,
                    stage->bs_mul_cfg, task->dpu.bs_relux_cmp,
                    false, 0, &status, value);
                value = rockchip_rknn_dpu_fp16_stage_apply(
                    task->dpu.bn_cfg, stage->bn_alu_operand,
                    stage->bn_mul_cfg, task->dpu.bn_relux_cmp,
                    false, 0, &status, value);
                fixed = float32_to_int32(value, &status);
                output[output_index] = rockchip_rknn_saturate_i8(
                    int128_makes64(fixed));
            }
        }
    }
    if (!rockchip_rknn_write_strided_output(
            s, view, task->dpu.output_channels_valid,
            output, sizeof(*output))) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
    }
    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static RockchipRKNNExecutionResult rockchip_rknn_execute_dpu_rdma_fp16_lut(
    RockchipRKNNCoreState *s, const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    const RockchipRKNNTensorView *output_view = &task->dpu.output;
    size_t storage_channels;
    size_t source_width = rdma->width - extract32(rdma->pad_cfg, 0, 3);
    size_t source_height = rdma->height - extract32(rdma->pad_cfg, 4, 3);
    size_t input_values;
    size_t output_values;
    size_t input_bytes;
    size_t output_bytes;
    uint64_t host_bytes = 0;
    g_autofree uint16_t *input = NULL;
    g_autofree uint16_t *output = NULL;

    if (!rockchip_rknn_dpu_work_budget_valid(
            s, output_view->width, output_view->height,
            task->dpu.output_channels_valid) ||
        !rockchip_rknn_size_round_up(output_view->channels, 8,
                                     &storage_channels) ||
        !rockchip_rknn_size_mul3(source_width, source_height,
                                 storage_channels, &input_values) ||
        !rockchip_rknn_size_mul3(output_view->width, output_view->height,
                                 storage_channels, &output_values) ||
        !rockchip_rknn_size_mul(input_values, sizeof(*input),
                                &input_bytes) ||
        !rockchip_rknn_fp16_rdma_source_layout_valid(
            rdma, source_width, source_height, storage_channels,
            &input_bytes) ||
        !rockchip_rknn_size_mul(output_values, sizeof(*output),
                                &output_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, input_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, output_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    input = g_try_new(uint16_t, input_values);
    output = g_try_new0(uint16_t, output_values);
    if (!input || !output) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (!rockchip_rknn_read_fp16_rdma_source(
            s, rdma, input, source_width, source_height,
            storage_channels)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }

    for (unsigned int row = 0; row < output_view->height; row++) {
        for (unsigned int column = 0; column < output_view->width; column++) {
            for (unsigned int channel = 0;
                 channel < task->dpu.output_channels_valid; channel++) {
                size_t output_index = rockchip_rknn_feature_index(
                    output_view->width, output_view->height, 8,
                    channel, row, column);
                unsigned int pad_left = extract32(rdma->pad_cfg, 0, 3);
                unsigned int pad_top = extract32(rdma->pad_cfg, 4, 3);
                uint16_t input_bits;
                float_status status = rockchip_rknn_fp16_status();
                float32 value;

                if (row < pad_top || column < pad_left) {
                    input_bits = extract32(rdma->pad_cfg, 16, 16);
                } else {
                    size_t input_index = rockchip_rknn_feature_index(
                        source_width, source_height, 8, channel,
                        row - pad_top, column - pad_left);

                    input_bits = le16_to_cpu(input[input_index]);
                }
                value = float16_to_float32(
                    make_float16(input_bits), true, &status);
                float32 multiplier = float16_to_float32(
                    make_float16(stage->bn_mul_cfg >> 16), true, &status);
                int32_t fixed;
                int32_t start;
                int32_t end;
                int64_t offset;
                unsigned int table;
                unsigned int index_select;
                unsigned int lut_index;
                unsigned int fraction;
                uint64_t interpolated;
                uint64_t converted;
                float32 result;
                uint16_t bits;

                value = float32_add(
                    float32_mul(value, multiplier, &status),
                    make_float32(stage->bn_alu_operand), &status);
                fixed = float32_to_int32(value, &status);
                if (fixed < 0) {
                    table = 0;
                    start = task->dpu.lut_le_start;
                    end = task->dpu.lut_le_end;
                    index_select = extract32(task->dpu.lut_info, 8, 8);
                } else {
                    table = 1;
                    start = task->dpu.lut_lo_start;
                    end = task->dpu.lut_lo_end;
                    index_select = extract32(task->dpu.lut_info, 16, 8);
                }
                if (fixed <= start) {
                    lut_index = 0;
                    fraction = 0;
                } else if (fixed >= end) {
                    lut_index = ROCKCHIP_RKNN_LUT_ENTRIES - 1;
                    fraction = 0;
                } else {
                    offset = (int64_t)fixed - start;
                    lut_index = offset >> index_select;
                    fraction = offset & ((1U << index_select) - 1);
                }
                if (lut_index >= ROCKCHIP_RKNN_LUT_ENTRIES - 1) {
                    interpolated = s->execution_lut[table]
                                         [ROCKCHIP_RKNN_LUT_ENTRIES - 1];
                } else {
                    uint64_t denominator = 1U << index_select;
                    uint64_t numerator =
                        (uint64_t)s->execution_lut[table][lut_index] *
                            (denominator - fraction) +
                        (uint64_t)s->execution_lut[table][lut_index + 1] *
                        fraction;

                    interpolated = table == 0 ?
                        numerator / denominator :
                        numerator / denominator + 1;
                }
                converted = interpolated * task->dpu.out_cvt_scale;
                if (table == 0) {
                    converted += task->dpu.out_cvt_offset;
                }
                result = int64_to_float32(converted, &status);
                result = float32_scalbn(
                    result, -(int)task->dpu.out_cvt_minus_exp, &status);
                bits = float16_val(float32_to_float16(result, true, &status));
                if (bits && bits < 0x400) {
                    bits = 0x400;
                }
                output[output_index] = cpu_to_le16(bits);
            }
        }
    }
    if (!rockchip_rknn_write_strided_output(
            s, output_view, task->dpu.output_channels_valid,
            output, sizeof(*output))) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
    }
    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static RockchipRKNNExecutionResult rockchip_rknn_execute_dpu_rdma_fp16_minmax(
    RockchipRKNNCoreState *s, const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    size_t storage_channels;
    size_t input_values;
    size_t input_bytes;
    g_autofree uint16_t *input = NULL;
    uint16_t extremum;
    unsigned int extremum_index = 0;
    uint8_t result[16] = {};
    bool select_max = task->dpu.minmax_ctl & BIT(1);
    uint64_t host_bytes = 0;

    if (!rockchip_rknn_size_round_up(rdma->channels, 8,
                                     &storage_channels) ||
        !rockchip_rknn_size_mul3(rdma->width, rdma->height,
                                 storage_channels, &input_values) ||
        !rockchip_rknn_size_mul(input_values, sizeof(*input),
                                &input_bytes) ||
        !rockchip_rknn_fp16_rdma_source_layout_valid(
            rdma, rdma->width, rdma->height, storage_channels,
            &input_bytes) ||
        (!(task->dpu.minmax_ctl & BIT(2)) && input_values > UINT16_MAX) ||
        !rockchip_rknn_dpu_work_budget_valid(
            s, rdma->width, rdma->height, rdma->channels) ||
        !rockchip_rknn_iova_length_valid(task->dpu.output.iova,
                                        sizeof(result)) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, input_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, sizeof(result))) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    input = g_try_new(uint16_t, input_values);
    if (!input) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (!rockchip_rknn_read_fp16_rdma_source(
            s, rdma, input, rdma->width, rdma->height,
            storage_channels)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }
    extremum = le16_to_cpu(input[0]);
    for (unsigned int row = 0; row < rdma->height; row++) {
        for (unsigned int column = 0; column < rdma->width; column++) {
            for (unsigned int channel = 0; channel < rdma->channels;
                 channel++) {
                size_t index = rockchip_rknn_feature_index(
                    rdma->width, rdma->height, 8, channel, row, column);
                uint16_t value;
                float_status status;
                float32 lhs;
                float32 rhs;
                bool replace;

                if (!index) {
                    continue;
                }
                value = le16_to_cpu(input[index]);
                status = rockchip_rknn_fp16_status();
                lhs = float16_to_float32(make_float16(value), true, &status);
                rhs = float16_to_float32(make_float16(extremum), true,
                                         &status);
                replace = select_max ? float32_lt(rhs, lhs, &status) :
                                       float32_lt(lhs, rhs, &status);
                if (replace) {
                    extremum = value;
                    extremum_index = index;
                }
            }
        }
    }
    stw_le_p(result, extremum);
    if (!(task->dpu.minmax_ctl & BIT(2))) {
        stw_le_p(result + sizeof(uint16_t), extremum_index);
    }
    if (!rockchip_rknn_iommu_dma(s, task->dpu.output.iova, result,
                                 sizeof(result), true)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
    }
    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static bool rockchip_rknn_read_raw16_rdma_source(
    RockchipRKNNCoreState *s, const RockchipRKNNDpuRdmaConfig *rdma,
    uint16_t *input, size_t storage_channels)
{
    uint64_t line_atoms = rdma->width +
        extract32(rdma->src_dma_cfg, 19, 13);
    int64_t surface_atoms = (uint64_t)rdma->width * rdma->height +
        (line_atoms - rdma->width) * (rdma->height - 1) +
        sextract32(rdma->surface_notch, 0, 28);
    size_t row_bytes = rdma->width * 8 * sizeof(*input);
    unsigned int surfaces = DIV_ROUND_UP(storage_channels, 8);

    if (surface_atoms <= 0) {
        return false;
    }
    for (unsigned int surface = 0; surface < surfaces; surface++) {
        for (unsigned int row = 0; row < rdma->height; row++) {
            uint64_t iova = (uint64_t)rdma->src_iova +
                (surface * surface_atoms + row * line_atoms) * 16;
            size_t offset = ((size_t)surface * rdma->height + row) *
                            rdma->width * 8;

            if (iova > UINT32_MAX ||
                !rockchip_rknn_iommu_dma(
                    s, iova, input + offset, row_bytes, false)) {
                return false;
            }
        }
    }
    return true;
}

static bool rockchip_rknn_write_raw16_output(
    RockchipRKNNCoreState *s, const RockchipRKNNTensorView *view,
    uint32_t line_notch, const uint16_t *output)
{
    uint64_t line_atoms = view->width + line_notch;
    uint64_t surface_atoms = view->surface_stride;
    size_t row_bytes = view->width * view->atom * sizeof(*output);
    unsigned int surfaces = DIV_ROUND_UP(view->channels, view->atom);

    for (unsigned int surface = 0; surface < surfaces; surface++) {
        for (unsigned int row = 0; row < view->height; row++) {
            uint64_t iova = (uint64_t)view->iova +
                (surface * surface_atoms + row * line_atoms) * 16;
            size_t offset = ((size_t)surface * view->height + row) *
                            view->width * view->atom;

            if (iova > UINT32_MAX ||
                !rockchip_rknn_iommu_dma(
                    s, iova, (void *)(output + offset), row_bytes, true)) {
                return false;
            }
        }
    }
    return true;
}

static RockchipRKNNExecutionResult rockchip_rknn_execute_dpu_rdma_raw16_tensor(
    RockchipRKNNCoreState *s, const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const RockchipRKNNDpuRdmaConfig *rdma = &task->dpu_rdma;
    const RockchipRKNNTensorView *view = &task->dpu.output;
    size_t storage_channels;
    size_t values;
    size_t input_bytes;
    size_t output_bytes;
    size_t source_accessed;
    size_t output_accessed;
    int64_t source_surface_atoms;
    uint64_t source_last_atoms;
    uint64_t output_last_atoms;
    uint32_t source_line_notch = extract32(rdma->src_dma_cfg, 19, 13);
    bool compact_reorder =
        (task->dpu.feature_mode & ROCKCHIP_RKNN_DPU_FEATURE_MODE_TP_EN) ||
        extract32(task->dpu.feature_mode,
                  ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_SHIFT,
                  ROCKCHIP_RKNN_DPU_FEATURE_MODE_SURF_LEN_LENGTH);
    uint64_t host_bytes = 0;
    g_autofree uint16_t *input = NULL;
    g_autofree uint16_t *output = NULL;

    source_surface_atoms =
        (int64_t)rdma->width * rdma->height +
        (int64_t)source_line_notch * (rdma->height - 1) +
        sextract32(rdma->surface_notch, 0, 28);
    if (!rockchip_rknn_dpu_work_budget_valid(
            s, view->width, view->height,
            task->dpu.output_channels_valid) ||
        !rockchip_rknn_size_round_up(view->channels, view->atom,
                                     &storage_channels) ||
        !rockchip_rknn_size_mul3(view->width, view->height,
                                 storage_channels, &values) ||
        !rockchip_rknn_size_mul(values, sizeof(*input), &input_bytes) ||
        !rockchip_rknn_size_mul(values, sizeof(*output), &output_bytes) ||
        source_surface_atoms <= 0 ||
        !rockchip_rknn_u64_mul(DIV_ROUND_UP(storage_channels, 8) - 1,
                               source_surface_atoms,
                               &source_last_atoms) ||
        __builtin_add_overflow(
            source_last_atoms,
            (uint64_t)(rdma->height - 1) *
                (rdma->width + source_line_notch) + rdma->width,
            &source_last_atoms) ||
        !rockchip_rknn_u64_mul(source_last_atoms, 16,
                               &source_last_atoms) ||
        source_last_atoms > SIZE_MAX) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    source_accessed = source_last_atoms;
    if (!source_accessed ||
        !rockchip_rknn_iova_length_valid(rdma->src_iova,
                                         source_accessed)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (compact_reorder) {
        output_accessed = output_bytes;
    } else {
        if (!rockchip_rknn_u64_mul(
                DIV_ROUND_UP(storage_channels, 8) - 1,
                view->surface_stride, &output_last_atoms) ||
            __builtin_add_overflow(
                output_last_atoms,
                (uint64_t)(view->height - 1) *
                    (view->width + task->dpu.output_notch_0) + view->width,
                &output_last_atoms) ||
            !rockchip_rknn_u64_mul(output_last_atoms, 16,
                                   &output_last_atoms) ||
            output_last_atoms > SIZE_MAX) {
            return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
        }
        output_accessed = output_last_atoms;
    }
    if (!output_accessed ||
        !rockchip_rknn_iova_length_valid(view->iova, output_accessed) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, input_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, output_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    input = g_try_new(uint16_t, values);
    output = g_try_new(uint16_t, values);
    if (!input || !output) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (!rockchip_rknn_read_raw16_rdma_source(
            s, rdma, input, storage_channels)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }

    for (unsigned int row = 0; row < view->height; row++) {
        for (unsigned int column = 0; column < view->width; column++) {
            for (unsigned int channel = 0; channel < storage_channels;
                 channel++) {
                size_t index = rockchip_rknn_feature_index(
                    view->width, view->height, view->atom,
                    channel, row, column);
                int16_t result = task->dpu.offset_pend;

                if (channel < task->dpu.output_channels_valid) {
                    Int128 value = int128_makes64(
                        (int16_t)le16_to_cpu(input[index]));

                    value = rockchip_rknn_dpu_stage_apply(
                        task->dpu.bs_cfg, stage->bs_alu_operand,
                        stage->bs_mul_cfg, task->dpu.bs_relux_cmp,
                        false, 0, false, 0, 0, value);
                    value = rockchip_rknn_dpu_stage_apply(
                        task->dpu.bn_cfg, stage->bn_alu_operand,
                        stage->bn_mul_cfg, task->dpu.bn_relux_cmp,
                        false, 0, false, 0, 0, value);
                    value = rockchip_rknn_out_cvt(&task->dpu, stage, value);
                    result = rockchip_rknn_saturate_i16(value);
                }
                output[index] = cpu_to_le16(result);
            }
        }
    }
    if (compact_reorder ?
        !rockchip_rknn_iommu_dma(s, view->iova, output,
                                 output_bytes, true) :
        !rockchip_rknn_write_raw16_output(
            s, view, task->dpu.output_notch_0, output)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
    }
    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static RockchipRKNNExecutionResult rockchip_rknn_execute_dpu_rdma_raw16_tail(
    RockchipRKNNCoreState *s, const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNTensorView *view = &task->dpu.output;
    size_t spatial;
    size_t surfaces;
    size_t output_plane_bytes;
    size_t input_bytes;
    size_t output_bytes;
    uint64_t host_bytes = 0;
    bool vector_layout = view->surface_stride == 1 &&
                         task->dpu.surface_add == 16;
    g_autofree uint16_t *input = NULL;
    g_autofree uint16_t *output = NULL;

    surfaces = DIV_ROUND_UP(view->channels, 8);
    if (!rockchip_rknn_size_mul(view->width, view->height, &spatial) ||
        !surfaces ||
        !rockchip_rknn_size_mul(view->surface_stride, 16,
                                &output_plane_bytes) ||
        !rockchip_rknn_size_mul(spatial, 16, &input_bytes) ||
        !rockchip_rknn_size_mul(surfaces, input_bytes,
                                &input_bytes) ||
        !rockchip_rknn_size_mul(vector_layout ? 1 : surfaces,
                                task->dpu.surface_add,
                                &output_bytes) ||
        !rockchip_rknn_iova_length_valid(task->dpu_rdma.src_iova,
                                         input_bytes) ||
        !rockchip_rknn_iova_length_valid(view->iova, output_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, input_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, output_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (!rockchip_rknn_iommu_range_mapped(s, task->dpu_rdma.src_iova,
                                          input_bytes, false)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }
    if (!rockchip_rknn_iommu_range_mapped(s, view->iova, output_bytes, true)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
    }
    input = g_try_new(uint16_t, input_bytes / sizeof(*input));
    output = g_try_new0(uint16_t, output_bytes / sizeof(*output));
    if (!input || !output) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (!rockchip_rknn_iommu_dma(s, task->dpu_rdma.src_iova, input,
                                 input_bytes, false)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }
    if (vector_layout) {
        for (size_t index = 0; index < 8; index++) {
            stw_le_p(&output[index], index == 0 ?
                     le16_to_cpu(input[index]) : task->dpu.offset_pend);
        }
        if (!rockchip_rknn_iommu_dma(s, view->iova, output,
                                     output_bytes, true)) {
            return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
        }
        return ROCKCHIP_RKNN_EXECUTION_OK;
    }
    for (size_t surface = 0; surface < surfaces; surface++) {
        for (size_t channel = 0; channel < 8; channel++) {
            size_t global_channel = surface * 8 + channel;
            size_t input_base = surface * spatial * 8 + channel * spatial;
            size_t output_base = surface * task->dpu.surface_add / 2 +
                                 (spatial == 1 ? channel :
                                  channel * output_plane_bytes / 2);

            for (size_t index = 0; index < spatial; index++) {
                uint16_t value = task->dpu.offset_pend;

                if (global_channel < task->dpu.output_channels_valid) {
                    value = le16_to_cpu(input[input_base + index]);
                }
                stw_le_p(&output[output_base + index], value);
            }
        }
    }
    if (!rockchip_rknn_iommu_dma(s, view->iova, output, output_bytes, true)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
    }
    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static RockchipRKNNExecutionResult
rockchip_rknn_execute_dpu_rdma_raw16_compact_u8(
    RockchipRKNNCoreState *s, const RockchipRKNNPipelineTask *task)
{
    const RockchipRKNNTensorView *view = &task->dpu.output;
    size_t spatial;
    size_t storage_channels;
    size_t input_values;
    size_t input_bytes;
    size_t output_bytes;
    uint64_t host_bytes = 0;
    g_autofree uint16_t *input = NULL;
    g_autofree uint8_t *output = NULL;

    if (!rockchip_rknn_size_mul(view->width, view->height, &spatial) ||
        !rockchip_rknn_size_round_up(view->channels, 8,
                                     &storage_channels) ||
        !rockchip_rknn_size_mul(spatial, storage_channels,
                                &input_values) ||
        !rockchip_rknn_size_mul(input_values, sizeof(*input),
                                &input_bytes) ||
        !rockchip_rknn_size_mul(spatial, storage_channels / 8,
                                &output_bytes) ||
        !rockchip_rknn_iova_length_valid(task->dpu_rdma.src_iova,
                                         input_bytes) ||
        !rockchip_rknn_iova_length_valid(view->iova, output_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, input_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, output_bytes) ||
        !rockchip_rknn_dpu_work_budget_valid(
            s, view->width, view->height, storage_channels / 8)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    input = g_try_new(uint16_t, input_values);
    output = g_try_new(uint8_t, output_bytes);
    if (!input || !output) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (!rockchip_rknn_read_raw16_rdma_source(
            s, &task->dpu_rdma, input, storage_channels)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }
    for (unsigned int surface = 0; surface < storage_channels / 8;
         surface++) {
        unsigned int channel = surface * 8;

        for (unsigned int row = 0; row < view->height; row++) {
            for (unsigned int column = 0; column < view->width; column++) {
                size_t input_index = rockchip_rknn_feature_index(
                    view->width, view->height, 8,
                    channel, row, column);
                size_t output_index =
                    (surface * spatial + row * view->width + column);

                output[output_index] = le16_to_cpu(input[input_index]);
            }
        }
    }
    if (!rockchip_rknn_iommu_dma(s, view->iova, output,
                                 output_bytes, true)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
    }
    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static RockchipRKNNExecutionResult rockchip_rknn_execute_pipeline(
    RockchipRKNNCoreState *s, const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage,
    RockchipRKNNExecutionMode mode)
{
    if (task->enabled_blocks == (ROCKCHIP_RKNN_BLOCK_PPU |
                                 ROCKCHIP_RKNN_BLOCK_PPU_RDMA)) {
        return rockchip_rknn_execute_ppu(s, &task->ppu, mode);
    }
    if (mode == ROCKCHIP_RKNN_EXECUTION_DPU_FP16) {
        return rockchip_rknn_execute_fp16(s, task, stage);
    }
    if (mode == ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_INT8_PIPELINE) {
        return rockchip_rknn_execute_dpu_rdma_int8_pipeline(
            s, task, stage);
    }
    if (mode == ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_INT16_UNPOOL) {
        return rockchip_rknn_execute_dpu_rdma_int16_unpool(s, task);
    }
    if (mode == ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_INT8_TO_FP16) {
        return rockchip_rknn_execute_dpu_rdma_int8_to_fp16(s, task);
    }
    if (mode == ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_FP16_TO_INT8_COMBINE) {
        return rockchip_rknn_execute_dpu_rdma_fp16_to_int8_combine(
            s, task, stage);
    }
    if (mode == ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_FP16_PIPELINE) {
        return rockchip_rknn_execute_dpu_rdma_fp16_pipeline(
            s, task, stage);
    }
    if (mode == ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_FP16_MINMAX) {
        return rockchip_rknn_execute_dpu_rdma_fp16_minmax(s, task);
    }
    if (mode == ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_RAW16_TENSOR) {
        return rockchip_rknn_execute_dpu_rdma_raw16_tensor(
            s, task, stage);
    }
    if (mode == ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_RAW16_COMPACT_U8) {
        return rockchip_rknn_execute_dpu_rdma_raw16_compact_u8(s, task);
    }
    if (mode == ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_RAW16_TAIL) {
        return rockchip_rknn_execute_dpu_rdma_raw16_tail(s, task);
    }
    if (mode == ROCKCHIP_RKNN_EXECUTION_DPU_RDMA_FP16_LUT) {
        return rockchip_rknn_execute_dpu_rdma_fp16_lut(s, task, stage);
    }
    const bool int8_qd_brdma =
        mode == ROCKCHIP_RKNN_EXECUTION_DPU_INT8_QD_BRDMA;
    const bool int8_qd_cpend = int8_qd_brdma &&
        rockchip_rknn_dpu_qd_uses_cpend(task);
    const bool int8_writeback =
        mode == ROCKCHIP_RKNN_EXECUTION_DPU_INT8 || int8_qd_brdma ||
        mode == ROCKCHIP_RKNN_EXECUTION_DPU_INT8_BRDMA;
    const bool int8_weight_offset = int8_writeback && !int8_qd_brdma;
    const bool mc_surf_out = task->dpu.data_format & BIT(3);
    const bool bs_rdma = !int8_qd_brdma &&
        rockchip_rknn_dpu_stage_uses_alu_source(task->dpu.bs_cfg);
    const bool bn_rdma = rockchip_rknn_dpu_stage_uses_mul_source(
        task->dpu.bn_cfg, stage->bn_mul_cfg);
    const bool ew_rdma = rockchip_rknn_dpu_ew_uses_rdma(task->dpu.ew_cfg);
    g_autofree int8_t *input = NULL;
    g_autofree int8_t *weights = NULL;
    g_autofree uint8_t *bs_data = NULL;
    g_autofree uint16_t *bn_data = NULL;
    g_autofree int8_t *ew_src = NULL;
    g_autofree int8_t *ew_data = NULL;
    g_autofree uint32_t *output = NULL;
    g_autofree int8_t *int8_output = NULL;
    g_autofree int8_t *padding_values = NULL;
    g_autofree size_t *input_channel_offsets = NULL;
    g_autofree size_t *output_channel_offsets = NULL;
    size_t input_bytes;
    size_t weight_bytes;
    size_t required_weight_bytes;
    size_t logical_weight_bytes;
    size_t output_values;
    size_t output_bytes;
    size_t weight_storage_channels;
    size_t weight_kernel_area;
    size_t output_storage_channels;
    size_t rounded_input_channels;
    size_t input_channel_offset_bytes;
    size_t output_channel_offset_bytes;
    size_t depthwise_block_bytes = 0;
    size_t input_staging_bytes = 0;
    uint64_t input_line_bytes;
    uint64_t input_surface_bytes;
    uint64_t input_accessed_bytes;
    uint64_t input_active_row_bytes;
    uint64_t host_bytes = 0;

    if (!rockchip_rknn_mac_budget_valid(s, task, int8_qd_brdma)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }

    if (!rockchip_rknn_size_round_up(task->cna.input.channels,
                                    task->cna.input.atom,
                                    &rounded_input_channels) ||
        !rockchip_rknn_size_mul3(task->cna.input.width,
                                 task->cna.input.height,
                                 rounded_input_channels, &input_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (!rockchip_rknn_input_strides(&task->cna.input,
                                     &input_line_bytes,
                                     &input_surface_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (rockchip_rknn_cna_interleaved_input(&task->cna)) {
        if (task->cna.input.line_stride < task->cna.input.width ||
            !rockchip_rknn_u64_mul(task->cna.input.line_stride,
                                   task->cna.input_channels_valid,
                                   &input_line_bytes) ||
            !rockchip_rknn_u64_mul(task->cna.input.width,
                                   task->cna.input_channels_valid,
                                   &input_active_row_bytes) ||
            !rockchip_rknn_u64_mul(task->cna.input.height - 1,
                                   input_line_bytes,
                                   &input_accessed_bytes) ||
            __builtin_add_overflow(input_accessed_bytes,
                                   input_active_row_bytes,
                                   &input_accessed_bytes) ||
            input_active_row_bytes > SIZE_MAX) {
            return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
        }
        input_surface_bytes = 0;
        input_staging_bytes = input_active_row_bytes;
    } else {
        uint64_t input_surfaces =
            DIV_ROUND_UP(task->cna.input.channels, task->cna.input.atom);

        if (!rockchip_rknn_u64_mul(input_surfaces - 1,
                                   input_surface_bytes,
                                   &input_accessed_bytes) ||
            __builtin_add_overflow(
                input_accessed_bytes,
                (uint64_t)(task->cna.input.height - 1) * input_line_bytes,
                &input_accessed_bytes) ||
            __builtin_add_overflow(
                input_accessed_bytes,
                (uint64_t)task->cna.input.width * task->cna.input.atom,
                &input_accessed_bytes)) {
            return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
        }
    }
    if (input_accessed_bytes > SIZE_MAX ||
        !rockchip_rknn_iova_length_valid(task->cna.input.iova,
                                         input_accessed_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    weight_kernel_area = (size_t)task->cna.kernel_width *
                         task->cna.kernel_height;
    if (!task->cna.weight_bytes_per_kernel ||
        task->cna.weight_bytes_per_kernel % weight_kernel_area) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    weight_storage_channels = task->cna.weight_bytes_per_kernel /
                              weight_kernel_area;
    if (weight_storage_channels <
        rockchip_rknn_cna_execution_channels(&task->cna)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (!rockchip_rknn_size_mul(task->cna.weight_kernels,
                                task->cna.weight_bytes_per_kernel,
                                &logical_weight_bytes) ||
        logical_weight_bytes != task->cna.weight_bytes) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    required_weight_bytes = logical_weight_bytes;
    if (!rockchip_rknn_iova_length_valid(task->cna.weight_iova,
                                         required_weight_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    weight_bytes = required_weight_bytes;
    if (!rockchip_rknn_size_round_up(
            task->dpu.output.channels,
            mc_surf_out ? 32 : task->dpu.output.atom,
            &output_storage_channels) ||
        !rockchip_rknn_size_mul3(task->core.width, task->core.height,
                                 output_storage_channels, &output_values) ||
        !rockchip_rknn_size_mul(
            output_values, int8_writeback ? sizeof(*int8_output) :
                                           sizeof(*output),
            &output_bytes) ||
        !rockchip_rknn_iova_length_valid(task->dpu.output.iova,
                                         output_bytes)) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    if (task->core.depthwise && !int8_writeback) {
        RockchipRKNNDepthwiseOutputLayout layout;

        if (!rockchip_rknn_depthwise_int32_layout(task, &layout) ||
            !rockchip_rknn_size_mul(layout.plane_atom, sizeof(*output),
                                    &depthwise_block_bytes)) {
            return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
        }
    }
    if (!rockchip_rknn_host_budget_add(s, &host_bytes, input_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes,
                                       input_staging_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, weight_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes, output_bytes) ||
        !rockchip_rknn_host_budget_add(
            s, &host_bytes,
            rockchip_rknn_cna_execution_channels(&task->cna)) ||
        !rockchip_rknn_size_mul(
            rockchip_rknn_cna_execution_channels(&task->cna),
            sizeof(*input_channel_offsets), &input_channel_offset_bytes) ||
        !rockchip_rknn_size_mul(
            task->dpu.output_channels_valid,
            sizeof(*output_channel_offsets), &output_channel_offset_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes,
                                       input_channel_offset_bytes) ||
        !rockchip_rknn_host_budget_add(s, &host_bytes,
                                       output_channel_offset_bytes) ||
        (task->core.depthwise && !int8_writeback &&
         !rockchip_rknn_host_budget_add(s, &host_bytes,
                                        depthwise_block_bytes))) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    input = g_try_new(int8_t, input_bytes);
    weights = g_try_new(int8_t, weight_bytes);
    padding_values = g_try_malloc(
        rockchip_rknn_cna_execution_channels(&task->cna));
    input_channel_offsets = g_try_malloc(input_channel_offset_bytes);
    output_channel_offsets = g_try_malloc(output_channel_offset_bytes);
    if (!input || !weights || !padding_values || !input_channel_offsets ||
        !output_channel_offsets) {
        return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
    }
    for (unsigned int channel = 0;
         channel < rockchip_rknn_cna_execution_channels(&task->cna);
         channel++) {
        padding_values[channel] =
            rockchip_rknn_cna_padding_value(&task->cna, channel);
        input_channel_offsets[channel] =
            (channel / task->cna.input.atom) *
                task->cna.input.height * task->cna.input.width *
                task->cna.input.atom +
            channel % task->cna.input.atom;
    }
    for (unsigned int channel = 0;
         channel < task->dpu.output_channels_valid; channel++) {
        output_channel_offsets[channel] =
            (channel / task->dpu.output.atom) *
                task->dpu.output.height * task->dpu.output.width *
                task->dpu.output.atom +
            channel % task->dpu.output.atom;
    }
    if (int8_writeback) {
        int8_output = g_try_new0(int8_t, output_values);
        if (!int8_output) {
            return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
        }
    } else {
        output = g_try_new0(uint32_t, output_values);
        if (!output) {
            return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
        }
    }
    if (!rockchip_rknn_read_strided_input(
            s, &task->cna, (uint8_t *)input, input_line_bytes,
            input_surface_bytes) ||
        !rockchip_rknn_iommu_dma(s, task->cna.weight_iova, weights,
                                 weight_bytes, false)) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
    }
    if (task->enabled_blocks & ROCKCHIP_RKNN_BLOCK_DPU_RDMA) {
        if (int8_qd_brdma || bs_rdma) {
            size_t bs_bytes;
            size_t bs_units = int8_qd_brdma ?
                DIV_ROUND_UP((size_t)task->dpu.output.channels, 8) :
                task->dpu.output.channels;

            if (!rockchip_rknn_size_mul(
                    bs_units, int8_qd_brdma ? 0x40 : sizeof(uint32_t),
                    &bs_bytes) ||
                !rockchip_rknn_iova_length_valid(task->dpu_rdma.bs_iova,
                                                  bs_bytes)) {
                return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
            }
            if (!rockchip_rknn_host_budget_add(s, &host_bytes, bs_bytes)) {
                return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
            }
            bs_data = g_try_malloc(bs_bytes);
            if (!bs_data) {
                return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
            }
            if (!rockchip_rknn_iommu_dma(s, task->dpu_rdma.bs_iova,
                                         bs_data, bs_bytes, false)) {
                return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
            }
        }
        if (bn_rdma) {
            size_t bn_bytes;

            if (!rockchip_rknn_size_mul(task->dpu.output.channels,
                                        sizeof(*bn_data), &bn_bytes) ||
                !rockchip_rknn_iova_length_valid(task->dpu_rdma.bn_iova,
                                                  bn_bytes) ||
                !rockchip_rknn_host_budget_add(s, &host_bytes, bn_bytes)) {
                return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
            }
            bn_data = g_try_malloc(bn_bytes);
            if (!bn_data) {
                return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
            }
            if (!rockchip_rknn_iommu_dma(s, task->dpu_rdma.bn_iova,
                                         bn_data, bn_bytes, false)) {
                return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
            }
        }
        if (ew_rdma) {
            size_t channel_groups =
                DIV_ROUND_UP(task->dpu.output.channels, 32);
            size_t spatial;
            size_t group_bytes;
            size_t operand_bytes;
            size_t operand_host_bytes;

            if (!rockchip_rknn_size_mul(task->core.width,
                                        task->core.height, &spatial) ||
                !rockchip_rknn_size_mul(spatial, 16, &group_bytes) ||
                !rockchip_rknn_size_mul(channel_groups, group_bytes,
                                        &operand_bytes) ||
                !rockchip_rknn_iova_length_valid(
                    task->dpu_rdma.src_iova, operand_bytes) ||
                !rockchip_rknn_iova_length_valid(
                    task->dpu_rdma.ew_iova, operand_bytes)) {
                return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
            }
            if (!rockchip_rknn_size_mul(operand_bytes, 2,
                                        &operand_host_bytes) ||
                !rockchip_rknn_host_budget_add(s, &host_bytes,
                                               operand_host_bytes)) {
                return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
            }

            ew_src = g_try_new(int8_t, operand_bytes);
            ew_data = g_try_new(int8_t, operand_bytes);
            if (!ew_src || !ew_data) {
                return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
            }
            for (unsigned int group = 0; group < channel_groups; group++) {
                uint64_t src_surface_offset =
                    (uint64_t)group *
                    (spatial + task->dpu_rdma.surface_notch) * 16;
                uint64_t ew_surface_offset =
                    (uint64_t)group *
                    (spatial + task->dpu_rdma.ew_surface_notch) * 16;
                size_t buffer_offset = group * group_bytes;
                uint64_t src_iova = task->dpu_rdma.src_iova +
                                    src_surface_offset;
                uint64_t ew_iova = task->dpu_rdma.ew_iova +
                                   ew_surface_offset;

                if (src_iova > UINT32_MAX || ew_iova > UINT32_MAX ||
                    !rockchip_rknn_iommu_dma(
                        s, (uint32_t)src_iova, ew_src + buffer_offset,
                        group_bytes, false) ||
                    !rockchip_rknn_iommu_dma(
                        s, (uint32_t)ew_iova, ew_data + buffer_offset,
                        group_bytes, false)) {
                    return ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT;
                }
            }
        }
    }

    for (unsigned int row = 0; row < task->core.height; row++) {
        for (unsigned int column = 0; column < task->core.width; column++) {
            int64_t qd_sum = 0;

            if (int8_qd_brdma && !task->core.depthwise) {
                for (unsigned int kernel_row = 0;
                     kernel_row < task->cna.kernel_height; kernel_row++) {
                    int input_row;
                    bool input_row_valid = true;

                    if (task->cna.deconv) {
                        int numerator =
                            row + task->cna.kernel_height - 1 -
                            task->cna.pad_top - kernel_row;
                        unsigned int stride =
                            task->cna.deconv_stride_y + 1;

                        input_row_valid = numerator >= 0 &&
                                          numerator % stride == 0;
                        input_row = input_row_valid ? numerator / stride : 0;
                    } else {
                        input_row = row * task->cna.stride_y -
                                    task->cna.pad_top + kernel_row;
                    }
                    for (unsigned int kernel_column = 0;
                         kernel_column < task->cna.kernel_width;
                         kernel_column++) {
                        int input_column;
                        bool input_column_valid = true;

                        if (task->cna.deconv) {
                            int numerator =
                                column + task->cna.kernel_width - 1 -
                                task->cna.pad_left - kernel_column;
                            unsigned int stride =
                                task->cna.deconv_stride_x + 1;

                            input_column_valid = numerator >= 0 &&
                                numerator % stride == 0;
                            input_column = input_column_valid ?
                                numerator / stride : 0;
                        } else {
                            input_column = column * task->cna.stride_x -
                                           task->cna.pad_left +
                                           kernel_column;
                        }
                        bool input_valid = input_row_valid &&
                            input_column_valid && input_row >= 0 &&
                            input_row < task->cna.input.height &&
                            input_column >= 0 &&
                            input_column < task->cna.input.width;
                        size_t input_spatial = input_valid ?
                            ((size_t)input_row * task->cna.input.width +
                             input_column) * task->cna.input.atom : 0;

                        for (unsigned int channel = 0;
                             channel < rockchip_rknn_cna_execution_channels(
                                           &task->cna);) {
                            size_t chunk = MIN(
                                rockchip_rknn_cna_execution_channels(
                                    &task->cna) - channel,
                                task->cna.input.atom -
                                    channel % task->cna.input.atom);
                            const int8_t *input_chunk = input_valid ?
                                input + input_channel_offsets[channel] +
                                    input_spatial :
                                padding_values + channel;

                            chunk = MIN(chunk, 32 - channel % 32);
                            qd_sum += rockchip_rknn_sum_i8(input_chunk, chunk);
                            channel += chunk;
                        }
                    }
                }
            }
            for (unsigned int out = 0;
                 out < task->dpu.output_channels_valid; out++) {
                int64_t accumulator = 0;
                int64_t output_qd_sum = qd_sum;
                int32_t bs_operand = 0;
                int32_t ew_operand = stage->ew_operand[
                    out % ARRAY_SIZE(stage->ew_operand)];

                for (unsigned int kernel_row = 0;
                     kernel_row < task->cna.kernel_height; kernel_row++) {
                    int input_row;
                    bool input_row_valid = true;

                    if (task->cna.deconv) {
                        int numerator =
                            row + task->cna.kernel_height - 1 -
                            task->cna.pad_top - kernel_row;
                        unsigned int stride =
                            task->cna.deconv_stride_y + 1;

                        input_row_valid = numerator >= 0 &&
                                          numerator % stride == 0;
                        input_row = input_row_valid ? numerator / stride : 0;
                    } else {
                        input_row = row * task->cna.stride_y -
                                    task->cna.pad_top + kernel_row;
                    }
                    for (unsigned int kernel_column = 0;
                         kernel_column < task->cna.kernel_width;
                         kernel_column++) {
                        int input_column;
                        bool input_column_valid = true;

                        if (task->cna.deconv) {
                            int numerator =
                                column + task->cna.kernel_width - 1 -
                                task->cna.pad_left - kernel_column;
                            unsigned int stride =
                                task->cna.deconv_stride_x + 1;

                            input_column_valid = numerator >= 0 &&
                                numerator % stride == 0;
                            input_column = input_column_valid ?
                                numerator / stride : 0;
                        } else {
                            input_column = column * task->cna.stride_x -
                                           task->cna.pad_left +
                                           kernel_column;
                        }
                        bool input_valid = input_row_valid &&
                            input_column_valid && input_row >= 0 &&
                            input_row < task->cna.input.height &&
                            input_column >= 0 &&
                            input_column < task->cna.input.width;
                        size_t input_spatial = input_valid ?
                            ((size_t)input_row * task->cna.input.width +
                             input_column) * task->cna.input.atom : 0;
                        unsigned int kernel =
                            kernel_row * task->cna.kernel_width +
                            kernel_column;

                        if (!task->core.depthwise) {
                            unsigned int execution_channels =
                                rockchip_rknn_cna_execution_channels(
                                    &task->cna);

                            for (unsigned int channel = 0;
                                 channel < execution_channels;) {
                                size_t chunk = MIN(
                                    execution_channels - channel,
                                    task->cna.input.atom -
                                        channel % task->cna.input.atom);
                                size_t weight_index =
                                    rockchip_rknn_weight_index(
                                        weight_storage_channels,
                                        task->cna.weight_kernels,
                                        weight_kernel_area, out, kernel,
                                        channel);
                                const int8_t *input_chunk = input_valid ?
                                    input + input_channel_offsets[channel] +
                                        input_spatial :
                                    padding_values + channel;

                                chunk = MIN(chunk, 32 - channel % 32);
                                if (weight_index > weight_bytes ||
                                    chunk > weight_bytes - weight_index) {
                                    return
                                        ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
                                }
                                accumulator += rockchip_rknn_dot_i8(
                                    input_chunk, weights + weight_index,
                                    chunk);
                                if (int8_weight_offset) {
                                    accumulator +=
                                        rockchip_rknn_sum_i8(input_chunk,
                                                             chunk) *
                                        (int16_t)task->dpu.bs_ow_op;
                                }
                                channel += chunk;
                            }
                        } else {
                            unsigned int channel = out;
                            int8_t input_value =
                                padding_values[channel];
                            size_t weight_index =
                                rockchip_rknn_depthwise_weight_index(
                                    weight_storage_channels,
                                    weight_kernel_area, kernel, channel);
                            int32_t weight_value;

                            if (weight_index >= weight_bytes) {
                                return ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR;
                            }
                            if (int8_weight_offset) {
                                weight_value = weights[weight_index] +
                                    (int16_t)task->dpu.bs_ow_op;
                            } else {
                                weight_value = weights[weight_index];
                            }

                            if (input_valid) {
                                input_value = input[
                                    input_channel_offsets[channel] +
                                    input_spatial];
                            }
                            accumulator += input_value * weight_value;
                            if (int8_qd_brdma && task->core.depthwise) {
                                output_qd_sum += input_value;
                            }
                        }
                    }
                }
                Int128 value = int128_makes64(accumulator);

                if (task->core.clip_truncate) {
                    value = rockchip_rknn_round_shift(
                        value, task->core.clip_truncate, false);
                }
                if (int8_qd_brdma) {
                    const unsigned int group = out / 8;
                    const unsigned int lane = out % 8;
                    const uint8_t *coefficients = bs_data + group * 0x40;
                    int32_t alu = ldl_le_p(coefficients + lane * 4);
                    int16_t cpend = int8_qd_cpend ?
                        lduw_le_p(coefficients + 0x20 + lane * 2) : 0;
                    int16_t mul = lduw_le_p(
                        coefficients + 0x30 + lane * 2);

                    value = int128_add(
                        value, rockchip_rknn_mul_s32(
                            int128_makes64(output_qd_sum), cpend));
                    value = int128_add(value, int128_makes64(alu));
                    value = rockchip_rknn_dpu_mul(
                        value, mul, extract32(stage->bs_mul_cfg, 8, 6),
                        extract32(task->dpu.data_format, 4, 6));
                } else {
                    if (bs_rdma) {
                        bs_operand = ldl_le_p(bs_data + out * 4);
                    }
                    value = rockchip_rknn_dpu_stage_apply(
                        task->dpu.bs_cfg, stage->bs_alu_operand,
                        stage->bs_mul_cfg, task->dpu.bs_relux_cmp,
                        bs_rdma, bs_operand, false, 0,
                        extract32(task->dpu.data_format, 4, 6), value);
                }
                value = rockchip_rknn_saturate_i32(value);
                value = rockchip_rknn_dpu_stage_apply(
                    task->dpu.bn_cfg, stage->bn_alu_operand,
                    stage->bn_mul_cfg, task->dpu.bn_relux_cmp,
                    false, 0, bn_rdma,
                    bn_rdma ? (int16_t)le16_to_cpu(bn_data[out]) : 0,
                    extract32(task->dpu.data_format, 10, 6), value);
                if (ew_rdma) {
                    unsigned int channel_group = out / 32;
                    unsigned int channel_in_group = out % 32;
                    const int8_t *operand =
                        channel_in_group < 16 ? ew_src : ew_data;
                    size_t operand_index =
                        channel_group * task->core.width *
                            task->core.height * 16 +
                        (row * task->core.width + column) * 16 +
                        channel_in_group % 16;

                    ew_operand = operand[operand_index];
                }
                value = rockchip_rknn_dpu_ew_apply(
                    s, &task->dpu, stage, ew_operand, value);
                value = rockchip_rknn_out_cvt(&task->dpu, stage, value);
                if (int8_writeback) {
                    int8_output[output_channel_offsets[out] +
                        (row * task->dpu.output.width + column) *
                            task->dpu.output.atom] =
                        rockchip_rknn_saturate_i8(value);
                } else {
                    output[output_channel_offsets[out] +
                        (row * task->dpu.output.width + column) *
                            task->dpu.output.atom] =
                        cpu_to_le32(int128_getlo(value));
                }
            }
        }
    }

    if (int8_writeback) {
        if (!rockchip_rknn_write_strided_output(
                s, &task->dpu.output, task->dpu.output_channels_valid,
                int8_output, sizeof(*int8_output))) {
            return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
        }
    } else if (rockchip_rknn_depthwise_int32_notched_output_is_supported(
                   task)) {
        if (!rockchip_rknn_write_strided_output(
                s, &task->dpu.output, task->dpu.output_channels_valid,
                output, sizeof(*output))) {
            return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
        }
    } else if (task->core.depthwise) {
        return rockchip_rknn_write_depthwise_int32_output(s, task, output);
    } else if (mc_surf_out) {
        const unsigned int channels = task->dpu.output.channels;
        const unsigned int width = task->core.width;
        const unsigned int spatial = width * task->core.height;
        const unsigned int channel_blocks = DIV_ROUND_UP(channels, 32);
        const uint64_t surface_words =
            extract32(task->dpu.surface_add, 4, 28) * 4;
        uint32_t block_data[32];
        unsigned int blocks = spatial * channel_blocks;

        for (unsigned int block = 0; block < blocks; block++) {
            unsigned int position = block % spatial;
            unsigned int row = position / width;
            unsigned int column = position % width;
            unsigned int channel_base = block / spatial * 32;
            uint64_t block_offset = block / 2 * surface_words +
                                    block % 2 * 32;
            uint64_t block_iova = task->dpu.output.iova +
                                  block_offset * sizeof(uint32_t);

            if (block_iova > UINT32_MAX) {
                return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
            }

            for (unsigned int channel = 0; channel < 32; channel++) {
                size_t index = rockchip_rknn_feature_index(
                    task->dpu.output.width, task->dpu.output.height,
                    task->dpu.output.atom, channel_base + channel, row,
                    column);

                block_data[channel] = output[index];
            }
            if (!rockchip_rknn_iommu_dma(
                    s, block_iova,
                    block_data, sizeof(block_data), true)) {
                return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
            }
        }
    } else if (!rockchip_rknn_write_strided_output(
                   s, &task->dpu.output,
                   task->dpu.output_channels_valid,
                   output, sizeof(*output))) {
        return ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT;
    }

    return ROCKCHIP_RKNN_EXECUTION_OK;
}

static uint32_t rockchip_rknn_encode_pointer_state(
    const RockchipRKNNDomainRuntimeState *state)
{
    uint32_t value = state->pointer_value;

    value = deposit32(value, 0, 1, state->pointer_bank);
    value = deposit32(value, 16, 1, state->executor_bank);
    return value & ~(ROCKCHIP_RKNN_POINTER_PP_CLEAR |
                     ROCKCHIP_RKNN_EXECUTOR_PP_CLEAR);
}

static uint32_t rockchip_rknn_encode_core_pointer_state(
    RockchipRKNNCoreState *s, const RockchipRKNNDomainRuntimeState *state)
{
    return deposit32(rockchip_rknn_encode_pointer_state(state),
                     ROCKCHIP_RKNN_CORE_TAG_SHIFT, 2, s->core_index);
}

static void rockchip_rknn_commit_domain_runtime(
    RockchipRKNNDomainRuntimeState *state)
{
    if (state->executor_pingpong) {
        state->executor_bank ^= 1;
    }
    state->pointer_value = rockchip_rknn_encode_pointer_state(state);
}

static void rockchip_rknn_set_ppu_status(RockchipRKNNCoreState *s,
                                         uint32_t status)
{
    unsigned int ppu_bank =
        s->pending_domain_runtime[ROCKCHIP_RKNN_DOMAIN_PPU].pointer_bank;
    unsigned int rdma_bank =
        s->pending_domain_runtime[ROCKCHIP_RKNN_DOMAIN_PPU_RDMA].pointer_bank;

    s->ppu_regs[R_PPU_S_STATUS] = ppu_bank ? 0 : status;
    s->ppu_rdma_regs[R_PPU_RDMA_S_STATUS] = rdma_bank ? 0 : status;
}

static void rockchip_rknn_commit_runtime(RockchipRKNNCoreState *s)
{
    static const uint32_t domain_blocks[ROCKCHIP_RKNN_DOMAIN_COUNT] = {
        [ROCKCHIP_RKNN_DOMAIN_CNA] = ROCKCHIP_RKNN_BLOCK_CNA,
        [ROCKCHIP_RKNN_DOMAIN_CORE] = ROCKCHIP_RKNN_BLOCK_CORE,
        [ROCKCHIP_RKNN_DOMAIN_DPU] = ROCKCHIP_RKNN_BLOCK_DPU,
        [ROCKCHIP_RKNN_DOMAIN_DPU_RDMA] = ROCKCHIP_RKNN_BLOCK_DPU_RDMA,
        [ROCKCHIP_RKNN_DOMAIN_PPU] = ROCKCHIP_RKNN_BLOCK_PPU,
        [ROCKCHIP_RKNN_DOMAIN_PPU_RDMA] = ROCKCHIP_RKNN_BLOCK_PPU_RDMA,
    };
    uint32_t enabled_blocks = s->pending_pipeline->enabled_blocks;

    for (unsigned int domain = 0;
         domain < ROCKCHIP_RKNN_DOMAIN_COUNT; domain++) {
        for (unsigned int bank = 0; bank < 2; bank++) {
            RockchipRKNNRegisterBank *destination =
                &s->slave_file.domain[domain].bank[bank];
            const RockchipRKNNRegisterBank *source =
                &s->pending_file.domain[domain].bank[bank];

            for (unsigned int index = 0;
                 index < ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX; index++) {
                uint32_t bit = BIT(index % 32);
                size_t word = ((domain * 2 + bank) *
                               ROCKCHIP_RKNN_PRESENT_R_MAX) + index / 32;

                if (s->pending_register_writes[word] & bit) {
                    destination->regs[index] = source->regs[index];
                    destination->present[index / 32] |= bit;
                }
            }
        }
    }
    for (unsigned int i = 0; i < ROCKCHIP_RKNN_DOMAIN_COUNT; i++) {
        RockchipRKNNDomainRuntimeState committed =
            s->pending_domain_runtime[i];
        uint32_t pointer_mask = 0;

        if (i != ROCKCHIP_RKNN_DOMAIN_PC) {
            unsigned int pointer_index = A_CNA_S_POINTER / sizeof(uint32_t);

            for (unsigned int bank = 0; bank < 2; bank++) {
                size_t word = ((i * 2 + bank) *
                               ROCKCHIP_RKNN_PRESENT_R_MAX) +
                              pointer_index / 32;

                pointer_mask |= s->pending_register_writes[word] &
                                BIT(pointer_index % 32);
            }
        }
        if (enabled_blocks & domain_blocks[i]) {
            rockchip_rknn_commit_domain_runtime(&committed);
        }
        if (pointer_mask) {
            s->domain_runtime[i] = committed;
        } else {
            s->domain_runtime[i] = s->slave_file.runtime[i];
            if (enabled_blocks & domain_blocks[i]) {
                s->domain_runtime[i].executor_bank =
                    committed.executor_bank;
            }
            s->domain_runtime[i].pointer_value =
                rockchip_rknn_encode_pointer_state(&s->domain_runtime[i]);
        }
    }

    s->cna_regs[R_CNA_S_POINTER] = rockchip_rknn_encode_core_pointer_state(
        s, &s->domain_runtime[ROCKCHIP_RKNN_DOMAIN_CNA]);
    s->core_regs[R_CORE_S_POINTER] = rockchip_rknn_encode_core_pointer_state(
        s, &s->domain_runtime[ROCKCHIP_RKNN_DOMAIN_CORE]);
    s->ppu_regs[R_PPU_S_POINTER] = rockchip_rknn_encode_pointer_state(
        &s->domain_runtime[ROCKCHIP_RKNN_DOMAIN_PPU]);
    s->ppu_rdma_regs[R_PPU_RDMA_S_POINTER] =
        rockchip_rknn_encode_pointer_state(
            &s->domain_runtime[ROCKCHIP_RKNN_DOMAIN_PPU_RDMA]);
    memcpy(s->slave_file.runtime, s->domain_runtime,
           sizeof(s->slave_file.runtime));
    for (unsigned int i = 0; i < ROCKCHIP_RKNN_DOMAIN_COUNT; i++) {
        s->slave_file.domain[i].write_bank =
            i == ROCKCHIP_RKNN_DOMAIN_PC ? 0 :
            rockchip_rknn_domain_write_bank(&s->slave_file.runtime[i]);
    }
}

static const char *rockchip_rknn_regcmd_unhandled_kind(uint64_t raw,
                                                       uint32_t target)
{
    if (raw == 0) {
        return "zero";
    }
    if (target == 0) {
        return "target-zero";
    }
    if (target == 0x0041) {
        return "pre-op-enable";
    }
    if (target == 0x0081) {
        return "block-op-enable";
    }

    return NULL;
}

static void rockchip_rknn_update_irq(RockchipRKNNCoreState *s)
{
    bool old_level = s->irq_level;

    s->pc_regs[R_PC_INTERRUPT_STATUS] =
        s->pc_regs[R_PC_INTERRUPT_RAW_STATUS] &
        s->pc_regs[R_PC_INTERRUPT_MASK] &
        ROCKCHIP_RKNN_INTERRUPT_VALID_BITS;
    s->irq_level = s->pc_regs[R_PC_INTERRUPT_STATUS] != 0;
    if (s->irq_level != old_level) {
        trace_rockchip_rknn_irq(s->core_index, s->irq_level,
                                s->pc_regs[R_PC_INTERRUPT_RAW_STATUS],
                                s->pc_regs[R_PC_INTERRUPT_MASK],
                                s->pc_regs[R_PC_INTERRUPT_STATUS]);
    }
    qemu_set_irq(s->irq, s->irq_level);
}

static void rockchip_rknn_complete(void *opaque);
static void rockchip_rknn_try_complete_rendezvous(RockchipRKNNCoreState *s);

static RockchipRKNNCoreState *rockchip_rknn_get_peer(
    RockchipRKNNCoreState *s, unsigned int core_index)
{
    Object *parent = OBJECT(s)->parent;
    g_autofree char *name = g_strdup_printf("rknn%u", core_index);
    Object *peer;

    if (!parent) {
        return NULL;
    }
    peer = object_resolve_path_component(parent, name);
    if (!peer || !object_dynamic_cast(peer, TYPE_ROCKCHIP_RKNN_CORE)) {
        return NULL;
    }

    return ROCKCHIP_RKNN_CORE(peer);
}

static int rockchip_rknn_execution_worker(void *opaque)
{
    RockchipRKNNCoreState *s = opaque;
    int ret;

    ret = rockchip_rknn_execute_pipeline(
        s, s->pending_pipeline, &s->pending_dpu_stage, s->execution_mode);
    qemu_mutex_lock(&s->execution_lock);
    s->execution_worker_done = true;
    qemu_cond_signal(&s->execution_cond);
    qemu_mutex_unlock(&s->execution_lock);
    return ret;
}

static void rockchip_rknn_execution_done(void *opaque, int ret)
{
    RockchipRKNNCoreState *s = opaque;

    s->execution_aiocb = NULL;
    if (s->execution_discard) {
        return;
    }
    s->execution_result = ret < 0 ? ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR : ret;
    s->execution_result_ready = true;
    rockchip_rknn_try_complete_rendezvous(s);
}

static unsigned int rockchip_rknn_participating_core_count(
    RockchipRKNNCoreState *s)
{
    if (!s->pending_pipeline_decoded ||
        !(s->pending_pipeline->enabled_blocks & ROCKCHIP_RKNN_BLOCK_CNA)) {
        return 1;
    }

    return rockchip_rknn_co_work_core_count(
        s->pending_pipeline->cna.co_work_mode);
}

static void rockchip_rknn_try_complete_rendezvous(RockchipRKNNCoreState *s)
{
    RockchipRKNNCoreState *ready[ROCKCHIP_RKNN_CORE_COUNT];
    unsigned int core_count = rockchip_rknn_participating_core_count(s);
    unsigned int index = s->pending_task_index;

    if (!core_count) {
        rockchip_rknn_complete(s);
        return;
    }
    if (core_count == 1) {
        rockchip_rknn_complete(s);
        return;
    }
    if (core_count > ROCKCHIP_RKNN_CORE_COUNT ||
        s->core_index >= core_count) {
        return;
    }

    for (unsigned int i = 0; i < core_count; i++) {
        RockchipRKNNCoreState *peer = rockchip_rknn_get_peer(s, i);
        unsigned int peer_core_count = peer ?
            rockchip_rknn_participating_core_count(peer) : 0;

        trace_rockchip_rknn_rendezvous_peer(
            s->core_index, index, core_count, i,
            peer && peer->busy, peer ? peer->pending_task_count : 0,
            peer ? peer->pending_task_index : 0, peer_core_count,
            peer && peer->execution_result_ready);

        if (!peer || !peer->busy || peer_core_count != core_count ||
            !peer->execution_result_ready) {
            return;
        }
        ready[i] = peer;
    }

    for (unsigned int i = 0; i < core_count; i++) {
        rockchip_rknn_complete(ready[i]);
    }
}

static void rockchip_rknn_start_execution(RockchipRKNNCoreState *s)
{
    assert(!s->execution_aiocb && !s->execution_result_ready);

    qemu_mutex_lock(&s->execution_lock);
    s->execution_worker_done = false;
    qemu_mutex_unlock(&s->execution_lock);
    s->execution_aiocb = thread_pool_submit_aio(
        rockchip_rknn_execution_worker, s,
        rockchip_rknn_execution_done, s);
}

static void rockchip_rknn_drain_execution(RockchipRKNNCoreState *s,
                                           bool discard)
{
    bool waited;

    if (!s->execution_aiocb) {
        return;
    }

    assert(bql_locked());
    s->execution_discard = discard;
    qemu_mutex_lock(&s->execution_lock);
    waited = !s->execution_worker_done;
    if (waited) {
        bql_unlock();
        while (!s->execution_worker_done) {
            qemu_cond_wait(&s->execution_cond, &s->execution_lock);
        }
    }
    qemu_mutex_unlock(&s->execution_lock);
    if (waited) {
        bql_lock();
    }
    AIO_WAIT_WHILE(NULL, s->execution_aiocb != NULL);
    s->execution_discard = false;
    if (discard) {
        s->execution_result_ready = false;
    }
}

static void rockchip_rknn_drain_executions(RockchipRKNNCoreState *s)
{
    for (unsigned int i = 0; i < ROCKCHIP_RKNN_CORE_COUNT; i++) {
        RockchipRKNNCoreState *peer = rockchip_rknn_get_peer(s, i);

        if (peer) {
            rockchip_rknn_drain_execution(peer, false);
        }
    }
}

static void rockchip_rknn_complete(void *opaque)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(opaque);
    bool slave_submission = s->pending_slave;
    bool task_finished = false;
    bool completed_success;
    bool final_pipeline_attempted;
    bool final_ppu_attempted;
    bool final_ppu_success;
    bool ppu_stage_attempted;
    bool depthwise_stage_attempted;
    unsigned int final_ppu_bank;
    uint32_t dma_error_bits;

    if (s->pending_task_index < s->pending_task_count &&
        !s->pending_fetch_error && !s->pending_execution_error) {
        RockchipRKNNExecutionMode mode =
            ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
        RockchipRKNNExecutionResult result = ROCKCHIP_RKNN_EXECUTION_OK;
        const char *mode_reason = "control";
        const uint32_t final_blocks = ROCKCHIP_RKNN_BLOCK_CNA |
            ROCKCHIP_RKNN_BLOCK_CORE | ROCKCHIP_RKNN_BLOCK_DPU |
            ROCKCHIP_RKNN_BLOCK_DPU_RDMA;
        uint32_t pipeline_blocks = s->pending_pipeline->enabled_blocks;

        s->pending_final_pipeline_attempted = false;
        s->pending_final_ppu_attempted = false;
        s->pending_final_ppu_success = false;
        s->pending_final_ppu_bank = 0;
        s->pending_final_ppu_attempted = s->pending_pipeline_decoded &&
            pipeline_blocks == (ROCKCHIP_RKNN_BLOCK_PPU |
                                ROCKCHIP_RKNN_BLOCK_PPU_RDMA);
        if (s->pending_final_ppu_attempted) {
            s->pending_ppu_stage_attempted = true;
            s->pending_final_ppu_bank =
                s->pending_domain_runtime[ROCKCHIP_RKNN_DOMAIN_PPU]
                    .pointer_bank;
            rockchip_rknn_set_ppu_status(s, 0);
        }
        if (s->pending_pipeline_decoded) {
            mode = rockchip_rknn_execution_mode(
                s->pending_pipeline, &s->pending_dpu_stage, &mode_reason);
            if (mode == ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED) {
                s->pending_execution_error = true;
                trace_rockchip_rknn_execution_error(
                    s->core_index, s->pending_task_index, mode_reason);
                if (s->pending_final_ppu_attempted) {
                    rockchip_rknn_set_ppu_status(
                        s, ROCKCHIP_RKNN_PPU_STATUS_FAULT);
                }
            } else {
                s->pending_final_pipeline_attempted =
                    pipeline_blocks == (final_blocks &
                                        ~ROCKCHIP_RKNN_BLOCK_DPU_RDMA) ||
                    pipeline_blocks == final_blocks;
                if (!s->execution_result_ready) {
                    memcpy(s->execution_lut, s->lut,
                           sizeof(s->execution_lut));
                    s->execution_mode = mode;
                    rockchip_rknn_start_execution(s);
                    return;
                }
                result = s->execution_result;
                s->execution_result_ready = false;
                if (result == ROCKCHIP_RKNN_EXECUTION_DMA_READ_FAULT) {
                    s->pending_dma_error_bits |= ROCKCHIP_RKNN_DMA_READ_ERROR;
                } else if (result == ROCKCHIP_RKNN_EXECUTION_DMA_WRITE_FAULT) {
                    s->pending_dma_error_bits |= ROCKCHIP_RKNN_DMA_WRITE_ERROR;
                } else if (result == ROCKCHIP_RKNN_EXECUTION_MODEL_ERROR) {
                    s->pending_execution_error = true;
                    trace_rockchip_rknn_execution_error(
                        s->core_index, s->pending_task_index,
                        "host-resource-or-internal-layout");
                }
                if (s->pending_final_ppu_attempted) {
                    s->pending_final_ppu_success =
                        result == ROCKCHIP_RKNN_EXECUTION_OK;
                    rockchip_rknn_set_ppu_status(
                        s,
                        s->pending_final_ppu_success ?
                            ROCKCHIP_RKNN_PPU_STATUS_SUCCESS :
                            ROCKCHIP_RKNN_PPU_STATUS_FAULT);
                }
            }
        } else if (pipeline_blocks) {
            s->pending_execution_error = true;
            trace_rockchip_rknn_execution_error(
                s->core_index, s->pending_task_index, "decode-error");
        }
        if (s->pending_domain_runtime_valid) {
            rockchip_rknn_commit_runtime(s);
        }
        s->pending_task_index++;
        if (!s->pending_execution_error &&
            s->pending_task_index < s->pending_task_count &&
            !rockchip_rknn_fetch_pipeline_task(
                s, s->pending_task_index, s->pending_next_iova,
                s->pending_next_command_count)) {
            s->pending_fetch_error = true;
        }
        task_finished = s->pending_fetch_error ||
            s->pending_execution_error ||
            s->pending_task_index >= s->pending_task_count;
        if (!task_finished) {
            timer_mod(&s->complete_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      ROCKCHIP_RKNN_COMPLETE_DELAY_NS);
            return;
        }
    } else {
        task_finished = true;
    }

    if (s->pending_fetch_error || s->pending_execution_error) {
        s->pc_regs[R_PC_TASK_STATUS] =
            ROCKCHIP_RKNN_TASK_STATUS_FETCH_ERROR |
            s->pending_task_index;
    } else if (slave_submission) {
        s->pc_regs[R_PC_TASK_STATUS] = 0x00005000;
    } else if (s->pending_final_ppu_attempted) {
        s->pc_regs[R_PC_TASK_STATUS] = ROCKCHIP_RKNN_PPU_TASK_STATUS;
    } else if (s->pending_task_count) {
        s->pc_regs[R_PC_TASK_STATUS] = ROCKCHIP_RKNN_TASK_STATUS_SUCCESS;
    } else {
        s->pc_regs[R_PC_TASK_STATUS] = 0;
    }
    completed_success = !s->pending_fetch_error &&
        !s->pending_execution_error && task_finished &&
        s->pending_task_count;
    final_pipeline_attempted = s->pending_final_pipeline_attempted;
    final_ppu_attempted = s->pending_final_ppu_attempted;
    final_ppu_success = s->pending_final_ppu_success;
    ppu_stage_attempted = s->pending_ppu_stage_attempted;
    depthwise_stage_attempted = final_pipeline_attempted &&
        s->pending_pipeline->core.depthwise;
    final_ppu_bank = s->pending_final_ppu_bank;
    dma_error_bits = s->pending_dma_error_bits;
    s->pending_task_count = 0;
    s->pending_task_index = 0;
    s->pending_next_iova = 0;
    s->pending_next_command_count = 0;
    memset(s->pending_pipeline, 0, sizeof(*s->pending_pipeline));
    s->pending_pipeline_decoded = false;
    memset(&s->pending_dpu_stage, 0, sizeof(s->pending_dpu_stage));
    s->pending_domain_runtime_valid = false;
    s->pending_fetch_error = false;
    s->pending_execution_error = false;
    s->pending_ppu_stage_attempted = false;
    s->pending_final_pipeline_attempted = false;
    s->pending_final_ppu_attempted = false;
    s->pending_final_ppu_success = false;
    s->pending_final_ppu_bank = 0;
    s->pending_dma_error_bits = 0;
    memset(s->pending_register_writes, 0,
           sizeof(s->pending_register_writes));
    s->pending_slave = false;
    s->busy = false;
    s->pc_regs[R_PC_OPERATION_ENABLE] &= ~R_PC_OPERATION_ENABLE_OP_EN_MASK;
    if (completed_success) {
        uint32_t interrupt_bits = ROCKCHIP_RKNN_DPU_INTERRUPT_BITS;

        if (final_pipeline_attempted) {
            interrupt_bits =
                s->domain_runtime[ROCKCHIP_RKNN_DOMAIN_DPU].executor_bank ?
                ROCKCHIP_RKNN_PIPELINE_BANK1_INTERRUPT :
                ROCKCHIP_RKNN_PIPELINE_BANK0_INTERRUPT;
        } else if (final_ppu_attempted) {
            interrupt_bits = ROCKCHIP_RKNN_STAGE_RAW_STATUS_BITS;
            if (final_ppu_success) {
                interrupt_bits |= final_ppu_bank ?
                    ROCKCHIP_RKNN_PPU_BANK1_INTERRUPT :
                    ROCKCHIP_RKNN_PPU_BANK0_INTERRUPT;
            }
        }
        if (ppu_stage_attempted || depthwise_stage_attempted) {
            interrupt_bits |= ROCKCHIP_RKNN_STAGE_RAW_STATUS_BITS;
        }
        s->pc_regs[R_PC_INTERRUPT_RAW_STATUS] |= interrupt_bits;
    }
    s->pc_regs[R_PC_INTERRUPT_RAW_STATUS] |= dma_error_bits;
    rockchip_rknn_update_irq(s);
    trace_rockchip_rknn_complete(s->core_index,
                                 s->pc_regs[R_PC_INTERRUPT_RAW_STATUS],
                                 s->pc_regs[R_PC_INTERRUPT_STATUS]);
}

static void rockchip_rknn_clear_regcmd_shadow(RockchipRKNNCoreState *s)
{
    memset(s->regcmd_shadow_pc, 0, sizeof(s->regcmd_shadow_pc));
    memset(s->regcmd_shadow_cna, 0, sizeof(s->regcmd_shadow_cna));
    memset(s->regcmd_shadow_core, 0, sizeof(s->regcmd_shadow_core));
    memset(s->regcmd_shadow_dpu, 0, sizeof(s->regcmd_shadow_dpu));
    memset(s->regcmd_shadow_dpu_rdma, 0, sizeof(s->regcmd_shadow_dpu_rdma));
    memset(s->regcmd_shadow_ppu, 0, sizeof(s->regcmd_shadow_ppu));
    memset(s->regcmd_shadow_ppu_rdma, 0,
           sizeof(s->regcmd_shadow_ppu_rdma));
}

static uint32_t rockchip_rknn_regcmd_shadow_read(const uint32_t shadow[],
                                                 uint32_t rel)
{
    return shadow[rel / sizeof(uint32_t)];
}

static bool rockchip_rknn_regcmd_trace_summary_enabled(void)
{
    return trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SUMMARY_CNA_IO) ||
           trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SUMMARY_CNA_FC) ||
           trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SUMMARY_CORE) ||
           trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SUMMARY_DPU) ||
           trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SUMMARY_RDMA_IO) ||
           trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SUMMARY_RDMA_SHAPE);
}

static bool rockchip_rknn_regcmd_trace_ingest_enabled(void)
{
    return trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_INGEST) ||
           trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_INGEST_PPU) ||
           trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SHADOW_WRITE) ||
           trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_UNHANDLED) ||
           rockchip_rknn_regcmd_trace_summary_enabled();
}

static bool rockchip_rknn_shadow_write_domain(uint32_t reg, uint32_t value,
                                              uint32_t base,
                                              uint32_t shadow[],
                                              const char **domain,
                                              const char *domain_name,
                                              uint32_t *rel)
{
    if (reg < base || reg >= base + ROCKCHIP_RKNN_WINDOW_SIZE) {
        return false;
    }

    *rel = reg - base;
    shadow[*rel / sizeof(uint32_t)] = value;
    *domain = domain_name;
    return true;
}

static bool rockchip_rknn_regcmd_shadow_write(RockchipRKNNCoreState *s,
                                              uint32_t target, uint32_t reg,
                                              uint32_t value,
                                              const char **domain,
                                              uint32_t *rel)
{
    *domain = NULL;
    *rel = 0;

    switch (target) {
    case ROCKCHIP_RKNN_REGCMD_TARGET_PC:
        if (reg >= sizeof(s->regcmd_shadow_pc)) {
            return false;
        }
        s->regcmd_shadow_pc[reg / sizeof(uint32_t)] = value;
        *domain = "PC";
        *rel = reg;
        return true;
    case ROCKCHIP_RKNN_REGCMD_TARGET_CNA:
        return rockchip_rknn_shadow_write_domain(
            reg, value, ROCKCHIP_RKNN_REGCMD_CNA_BASE,
            s->regcmd_shadow_cna, domain, "CNA", rel);
    case ROCKCHIP_RKNN_REGCMD_TARGET_CORE:
        return rockchip_rknn_shadow_write_domain(
            reg, value, ROCKCHIP_RKNN_REGCMD_CORE_BASE,
            s->regcmd_shadow_core, domain, "CORE", rel);
    case ROCKCHIP_RKNN_REGCMD_TARGET_DPU:
        return rockchip_rknn_shadow_write_domain(
            reg, value, ROCKCHIP_RKNN_REGCMD_DPU_BASE,
            s->regcmd_shadow_dpu, domain, "DPU", rel);
    case ROCKCHIP_RKNN_REGCMD_TARGET_DPU_RDMA:
        return rockchip_rknn_shadow_write_domain(
            reg, value, ROCKCHIP_RKNN_REGCMD_DPU_RDMA_BASE,
            s->regcmd_shadow_dpu_rdma, domain, "DPU_RDMA", rel);
    case ROCKCHIP_RKNN_REGCMD_TARGET_PPU:
        return rockchip_rknn_shadow_write_domain(
            reg, value, ROCKCHIP_RKNN_REGCMD_PPU_BASE,
            s->regcmd_shadow_ppu, domain, "PPU", rel);
    case ROCKCHIP_RKNN_REGCMD_TARGET_PPU_RDMA:
        return rockchip_rknn_shadow_write_domain(
            reg, value, ROCKCHIP_RKNN_REGCMD_PPU_RDMA_BASE,
            s->regcmd_shadow_ppu_rdma, domain, "PPU_RDMA", rel);
    default:
        return false;
    }
}

static void rockchip_rknn_trace_regcmd_summary(RockchipRKNNCoreState *s,
                                               unsigned int bank)
{
    if (trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SUMMARY_CNA_IO)) {
        uint32_t data_size0 = rockchip_rknn_regcmd_shadow_read(
            s->regcmd_shadow_cna, ROCKCHIP_RKNN_CNA_DATA_SIZE0);
        uint32_t data_size1 = rockchip_rknn_regcmd_shadow_read(
            s->regcmd_shadow_cna, ROCKCHIP_RKNN_CNA_DATA_SIZE1);

        trace_rockchip_rknn_regcmd_summary_cna_io(
            s->core_index, bank,
            rockchip_rknn_regcmd_shadow_read(
                s->regcmd_shadow_cna, ROCKCHIP_RKNN_CNA_FEATURE_DATA_ADDR),
            extract32(data_size0, 16, 11), extract32(data_size0, 0, 11),
            extract32(data_size1, 0, 16),
            extract32(rockchip_rknn_regcmd_shadow_read(
                          s->regcmd_shadow_cna,
                          ROCKCHIP_RKNN_CNA_DMA_CON1), 0, 28),
            extract32(rockchip_rknn_regcmd_shadow_read(
                          s->regcmd_shadow_cna,
                          ROCKCHIP_RKNN_CNA_DMA_CON2), 0, 28),
            rockchip_rknn_regcmd_shadow_read(
                s->regcmd_shadow_cna, ROCKCHIP_RKNN_CNA_DCOMP_ADDR0));
    }

    if (trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SUMMARY_CNA_FC)) {
        uint32_t fc_data_size0 = rockchip_rknn_regcmd_shadow_read(
            s->regcmd_shadow_cna, ROCKCHIP_RKNN_CNA_FC_DATA_SIZE0);

        trace_rockchip_rknn_regcmd_summary_cna_fc(
            s->core_index, bank,
            extract32(fc_data_size0, 16, 14), extract32(fc_data_size0, 0, 11),
            extract32(rockchip_rknn_regcmd_shadow_read(
                          s->regcmd_shadow_cna,
                          ROCKCHIP_RKNN_CNA_FC_DATA_SIZE1), 0, 16));
    }

    if (trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SUMMARY_CORE)) {
        uint32_t dataout_size0 = rockchip_rknn_regcmd_shadow_read(
            s->regcmd_shadow_core, ROCKCHIP_RKNN_CORE_DATAOUT_SIZE_0);

        trace_rockchip_rknn_regcmd_summary_core(
            s->core_index, bank,
            rockchip_rknn_regcmd_shadow_read(
                s->regcmd_shadow_core, ROCKCHIP_RKNN_CORE_MISC_CFG),
            extract32(dataout_size0, 0, 16), extract32(dataout_size0, 16, 16),
            extract32(rockchip_rknn_regcmd_shadow_read(
                          s->regcmd_shadow_core,
                          ROCKCHIP_RKNN_CORE_DATAOUT_SIZE_1), 0, 16),
            extract32(rockchip_rknn_regcmd_shadow_read(
                          s->regcmd_shadow_core,
                          ROCKCHIP_RKNN_CORE_CLIP_TRUNCATE), 0, 5));
    }

    if (trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SUMMARY_DPU)) {
        uint32_t cube_channel = rockchip_rknn_regcmd_shadow_read(
            s->regcmd_shadow_dpu, ROCKCHIP_RKNN_DPU_DATA_CUBE_CHANNEL);

        trace_rockchip_rknn_regcmd_summary_dpu(
            s->core_index, bank,
            rockchip_rknn_regcmd_shadow_read(
                s->regcmd_shadow_dpu, ROCKCHIP_RKNN_DPU_DST_BASE_ADDR),
            extract32(rockchip_rknn_regcmd_shadow_read(
                          s->regcmd_shadow_dpu,
                          ROCKCHIP_RKNN_DPU_DST_SURF_STRIDE), 4, 28),
            extract32(rockchip_rknn_regcmd_shadow_read(
                          s->regcmd_shadow_dpu,
                          ROCKCHIP_RKNN_DPU_DATA_CUBE_WIDTH), 0, 13),
            extract32(rockchip_rknn_regcmd_shadow_read(
                          s->regcmd_shadow_dpu,
                          ROCKCHIP_RKNN_DPU_DATA_CUBE_HEIGHT), 0, 13),
            extract32(cube_channel, 0, 13), extract32(cube_channel, 16, 13),
            rockchip_rknn_regcmd_shadow_read(
                s->regcmd_shadow_dpu, ROCKCHIP_RKNN_DPU_FEATURE_MODE_CFG),
            rockchip_rknn_regcmd_shadow_read(
                s->regcmd_shadow_dpu, ROCKCHIP_RKNN_DPU_DATA_FORMAT));
    }

    if (trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SUMMARY_RDMA_IO)) {
        trace_rockchip_rknn_regcmd_summary_rdma_io(
            s->core_index, bank,
            rockchip_rknn_regcmd_shadow_read(
                s->regcmd_shadow_dpu_rdma,
                ROCKCHIP_RKNN_DPU_RDMA_SRC_BASE_ADDR),
            rockchip_rknn_regcmd_shadow_read(
                s->regcmd_shadow_dpu_rdma,
                ROCKCHIP_RKNN_DPU_RDMA_BS_BASE_ADDR),
            rockchip_rknn_regcmd_shadow_read(
                s->regcmd_shadow_dpu_rdma,
                ROCKCHIP_RKNN_DPU_RDMA_BN_BASE_ADDR),
            rockchip_rknn_regcmd_shadow_read(
                s->regcmd_shadow_dpu_rdma,
                ROCKCHIP_RKNN_DPU_RDMA_EW_BASE_ADDR));
    }

    if (trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SUMMARY_RDMA_SHAPE)) {
        trace_rockchip_rknn_regcmd_summary_rdma_shape(
            s->core_index, bank,
            extract32(rockchip_rknn_regcmd_shadow_read(
                          s->regcmd_shadow_dpu_rdma,
                          ROCKCHIP_RKNN_DPU_RDMA_DATA_CUBE_WIDTH), 0, 13),
            extract32(rockchip_rknn_regcmd_shadow_read(
                          s->regcmd_shadow_dpu_rdma,
                          ROCKCHIP_RKNN_DPU_RDMA_DATA_CUBE_HEIGHT), 0, 13),
            extract32(rockchip_rknn_regcmd_shadow_read(
                          s->regcmd_shadow_dpu_rdma,
                          ROCKCHIP_RKNN_DPU_RDMA_DATA_CUBE_CHANNEL), 0, 13),
            extract32(rockchip_rknn_regcmd_shadow_read(
                          s->regcmd_shadow_dpu_rdma,
                          ROCKCHIP_RKNN_DPU_RDMA_EW_SURF_STRIDE), 4, 28),
            rockchip_rknn_regcmd_shadow_read(
                s->regcmd_shadow_dpu_rdma,
                ROCKCHIP_RKNN_DPU_RDMA_FEATURE_MODE_CFG),
            rockchip_rknn_regcmd_shadow_read(
                s->regcmd_shadow_dpu_rdma,
                ROCKCHIP_RKNN_DPU_RDMA_ERDMA_CFG));
    }
}

static void rockchip_rknn_ingest_regcmd(RockchipRKNNCoreState *s,
                                        unsigned int bank,
                                        const uint8_t *sample,
                                        uint32_t sample_commands,
                                        uint32_t command_count)
{
    RockchipRKNNRegcmdStats stats = { 0 };
    bool trace_shadow_write =
        trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SHADOW_WRITE);
    bool trace_unhandled =
        trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_UNHANDLED);

    rockchip_rknn_clear_regcmd_shadow(s);

    for (uint32_t i = 0; i < sample_commands; i++) {
        uint64_t raw = ldq_le_p(sample + i * sizeof(uint64_t));
        uint32_t reg = raw & 0xffff;
        uint32_t value = (raw >> 16) & 0xffffffff;
        uint32_t target = (raw >> 48) & 0xffff;
        const char *domain = NULL;
        const char *kind;
        uint32_t rel = 0;

        if (!rockchip_rknn_regcmd_shadow_write(s, target, reg, value,
                                               &domain, &rel)) {
            kind = rockchip_rknn_regcmd_unhandled_kind(raw, target);
            if (kind) {
                stats.raw++;
            } else {
                kind = "unknown";
                stats.unknown++;
            }
            if (trace_unhandled) {
                trace_rockchip_rknn_regcmd_unhandled(s->core_index, bank, i,
                                                     kind, target, reg, value,
                                                     raw);
            }
            continue;
        }

        if (trace_shadow_write) {
            trace_rockchip_rknn_regcmd_shadow_write(s->core_index, bank, i,
                                                    domain, rel, value);
        }

        if (g_str_equal(domain, "PC")) {
            stats.pc++;
        } else if (g_str_equal(domain, "CNA")) {
            stats.cna++;
        } else if (g_str_equal(domain, "CORE")) {
            stats.core++;
        } else if (g_str_equal(domain, "DPU")) {
            stats.dpu++;
        } else if (g_str_equal(domain, "DPU_RDMA")) {
            stats.dpu_rdma++;
        } else if (g_str_equal(domain, "PPU")) {
            stats.ppu++;
        } else if (g_str_equal(domain, "PPU_RDMA")) {
            stats.ppu_rdma++;
        }
    }

    stats.raw += command_count - sample_commands;
    trace_rockchip_rknn_regcmd_ingest(s->core_index, bank, command_count,
                                      sample_commands, stats.pc, stats.cna,
                                      stats.core, stats.dpu + stats.dpu_rdma,
                                      stats.raw, stats.unknown);
    trace_rockchip_rknn_regcmd_ingest_ppu(s->core_index, bank, stats.ppu,
                                          stats.ppu_rdma);
    rockchip_rknn_trace_regcmd_summary(s, bank);
}

static void rockchip_rknn_trace_regcmd_sample(RockchipRKNNCoreState *s)
{
    uint32_t iova = s->pc_regs[R_PC_BASE_ADDRESS] &
                    ROCKCHIP_RKNN_PC_BASE_ADDRESS_MASK;
    uint32_t amounts = s->pc_regs[R_PC_REGISTER_AMOUNTS] &
                       ROCKCHIP_RKNN_PC_REGISTER_AMOUNTS_MASK;
    uint32_t command_count = (amounts + 1) * 2;
    uint32_t command_bytes = command_count * sizeof(uint64_t);
    uint32_t sample_bytes;
    uint32_t sample_commands;
    bool trace_sample =
        trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SAMPLE);
    bool trace_words = trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_WORD);
    bool trace_ingest = rockchip_rknn_regcmd_trace_ingest_enabled();
    IOMMUMemoryRegion *iommu;
    IOMMUMemoryRegionClass *imrc;
    IOMMUTLBEntry entry;
    unsigned int bank;
    int iommu_idx;
    hwaddr phys = 0;
    uint8_t sample[ROCKCHIP_RKNN_REGCMD_SAMPLE_BYTES_MAX] = { 0 };

    iommu = memory_region_get_iommu(s->dma_mr);
    if (!iommu) {
        trace_rockchip_rknn_regcmd_sample_error(s->core_index, iova,
                                                "no-iommu-link");
        return;
    }

    imrc = memory_region_get_iommu_class_nocheck(iommu);
    iommu_idx = memory_region_iommu_attrs_to_index(
        iommu, MEMTXATTRS_UNSPECIFIED);
    entry = imrc->translate(iommu, iova, IOMMU_RO, iommu_idx);
    if (!entry.target_as || !(entry.perm & IOMMU_RO)) {
        trace_rockchip_rknn_regcmd_sample_error(s->core_index, iova,
                                                "translation-failed");
        return;
    }
    if (!rockchip_iommu_find_translation_bank(iommu, iova, IOMMU_RO,
                                               &bank)) {
        trace_rockchip_rknn_regcmd_sample_error(s->core_index, iova,
                                                "bank-lookup-failed");
        return;
    }
    phys = (entry.translated_addr & ~entry.addr_mask) |
           (iova & entry.addr_mask);

    if (!trace_sample && !trace_words && !trace_ingest) {
        return;
    }

    sample_bytes = MIN(command_bytes, trace_words || trace_ingest ?
                       ROCKCHIP_RKNN_REGCMD_SAMPLE_BYTES_MAX :
                       ROCKCHIP_RKNN_REGCMD_SUMMARY_BYTES);
    if (entry.addr_mask != HWADDR_MAX) {
        sample_bytes = MIN(sample_bytes,
                           (uint32_t)(entry.addr_mask -
                                      (iova & entry.addr_mask) + 1));
    }
    sample_commands = trace_words || trace_ingest ?
                      sample_bytes / sizeof(uint64_t) : 0;

    if (dma_memory_read(s->dma_as, iova, sample, sample_bytes,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        trace_rockchip_rknn_regcmd_sample_error(s->core_index, iova,
                                                "sample-read-failed");
        return;
    }

    if (trace_sample) {
        uint32_t summary_bytes = MIN(sample_bytes,
                                     ROCKCHIP_RKNN_REGCMD_SUMMARY_BYTES);

        trace_rockchip_rknn_regcmd_sample(s->core_index, iova, phys,
                                          command_bytes, summary_bytes,
                                          ldl_le_p(sample),
                                          ldl_le_p(sample + 4),
                                          ldl_le_p(sample + 8),
                                          ldl_le_p(sample + 12));
    }

    for (uint32_t i = 0; i < sample_commands; i++) {
        uint64_t raw = ldq_le_p(sample + i * sizeof(uint64_t));
        uint32_t reg = raw & 0xffff;
        uint32_t value = (raw >> 16) & 0xffffffff;
        uint32_t target = (raw >> 48) & 0xffff;

        trace_rockchip_rknn_regcmd_word(s->core_index, bank, i, target, reg,
                                        value, raw);
    }

    if (trace_ingest) {
        rockchip_rknn_ingest_regcmd(s, bank, sample, sample_commands,
                                    command_count);
    }
}

static void rockchip_rknn_start(RockchipRKNNCoreState *s)
{
    if (s->busy) {
        return;
    }

    s->busy = true;
    s->pc_regs[R_PC_TASK_STATUS] = 0;
    trace_rockchip_rknn_start(s->core_index,
                              s->pc_regs[R_PC_BASE_ADDRESS],
                              s->pc_regs[R_PC_REGISTER_AMOUNTS],
                              s->pc_regs[R_PC_TASK_CON],
                              s->pc_regs[R_PC_TASK_DMA_BASE_ADDR],
                              s->cna_regs[R_CNA_S_POINTER],
                              s->core_regs[R_CORE_S_POINTER]);
    if (trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SAMPLE) ||
        trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_WORD) ||
        rockchip_rknn_regcmd_trace_ingest_enabled() ||
        trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SAMPLE_ERROR)) {
        rockchip_rknn_trace_regcmd_sample(s);
    }
    rockchip_rknn_prepare_pipeline(s);
    timer_mod(&s->complete_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              ROCKCHIP_RKNN_COMPLETE_DELAY_NS);
}

static void rockchip_rknn_start_slave(RockchipRKNNCoreState *s,
                                      uint32_t enabled_blocks)
{
    if (s->busy ||
        !(s->pc_regs[R_PC_BASE_ADDRESS] & ROCKCHIP_RKNN_PC_SLAVE_MODE)) {
        return;
    }

    s->busy = true;
    s->pending_slave = true;
    s->pc_regs[R_PC_TASK_STATUS] = 0;
    rockchip_rknn_prepare_slave_pipeline(s, enabled_blocks);
    timer_mod(&s->complete_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              ROCKCHIP_RKNN_COMPLETE_DELAY_NS);
}

static void rockchip_rknn_operation_enable_postw(RegisterInfo *reg,
                                                 uint64_t val)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(reg->opaque);

    if (val & R_PC_OPERATION_ENABLE_OP_EN_MASK) {
        rockchip_rknn_start(s);
    }
}

static void rockchip_rknn_pointer_postw(RegisterInfo *reg, uint64_t val,
                                        RockchipRKNNRegcmdDomain domain)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(reg->opaque);
    uint32_t encoded;

    rockchip_rknn_set_pointer_state(
        &s->domain_runtime[domain], val,
        domain == ROCKCHIP_RKNN_DOMAIN_CNA ||
        domain == ROCKCHIP_RKNN_DOMAIN_CORE);
    encoded = domain == ROCKCHIP_RKNN_DOMAIN_CNA ||
              domain == ROCKCHIP_RKNN_DOMAIN_CORE ?
        rockchip_rknn_encode_core_pointer_state(
            s, &s->domain_runtime[domain]) :
        rockchip_rknn_encode_pointer_state(&s->domain_runtime[domain]);

    switch (domain) {
    case ROCKCHIP_RKNN_DOMAIN_CNA:
        s->cna_regs[R_CNA_S_POINTER] = encoded;
        break;
    case ROCKCHIP_RKNN_DOMAIN_CORE:
        s->core_regs[R_CORE_S_POINTER] = encoded;
        break;
    case ROCKCHIP_RKNN_DOMAIN_PPU:
        s->ppu_regs[R_PPU_S_POINTER] = encoded;
        break;
    case ROCKCHIP_RKNN_DOMAIN_PPU_RDMA:
        s->ppu_rdma_regs[R_PPU_RDMA_S_POINTER] = encoded;
        break;
    default:
        g_assert_not_reached();
    }
}

static void rockchip_rknn_cna_pointer_postw(RegisterInfo *reg, uint64_t val)
{
    rockchip_rknn_pointer_postw(reg, val, ROCKCHIP_RKNN_DOMAIN_CNA);
}

static void rockchip_rknn_core_pointer_postw(RegisterInfo *reg, uint64_t val)
{
    rockchip_rknn_pointer_postw(reg, val, ROCKCHIP_RKNN_DOMAIN_CORE);
}

static void rockchip_rknn_ppu_pointer_postw(RegisterInfo *reg, uint64_t val)
{
    rockchip_rknn_pointer_postw(reg, val, ROCKCHIP_RKNN_DOMAIN_PPU);
}

static void rockchip_rknn_ppu_rdma_pointer_postw(RegisterInfo *reg,
                                                  uint64_t val)
{
    rockchip_rknn_pointer_postw(reg, val, ROCKCHIP_RKNN_DOMAIN_PPU_RDMA);
}

static void rockchip_rknn_interrupt_mask_postw(RegisterInfo *reg,
                                               uint64_t val)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(reg->opaque);

    rockchip_rknn_update_irq(s);
}

static uint64_t rockchip_rknn_interrupt_clear_prew(RegisterInfo *reg,
                                                   uint64_t val)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(reg->opaque);

    s->pc_regs[R_PC_INTERRUPT_RAW_STATUS] &=
        ~((uint32_t)val & ROCKCHIP_RKNN_INTERRUPT_VALID_BITS);
    rockchip_rknn_update_irq(s);
    return 0;
}

static const RegisterAccessInfo rockchip_rknn_pc_regs_info[] = {
    {   .name = "PC_VERSION", .addr = A_PC_VERSION,
        .reset = ROCKCHIP_RKNN_PC_VERSION,
        .ro = UINT32_MAX,
    }, { .name = "PC_VERSION_NUM", .addr = A_PC_VERSION_NUM,
        .reset = ROCKCHIP_RKNN_PC_VERSION_NUM,
        .ro = UINT32_MAX,
    }, { .name = "PC_OPERATION_ENABLE", .addr = A_PC_OPERATION_ENABLE,
        .post_write = rockchip_rknn_operation_enable_postw,
    }, { .name = "PC_BASE_ADDRESS", .addr = A_PC_BASE_ADDRESS,
    }, { .name = "PC_REGISTER_AMOUNTS", .addr = A_PC_REGISTER_AMOUNTS,
    }, { .name = "PC_INTERRUPT_MASK", .addr = A_PC_INTERRUPT_MASK,
        .reset = ROCKCHIP_RKNN_INTERRUPT_VALID_BITS,
        .rsvd = ROCKCHIP_RKNN_INTERRUPT_RESERVED_BITS,
        .post_write = rockchip_rknn_interrupt_mask_postw,
    }, { .name = "PC_INTERRUPT_CLEAR", .addr = A_PC_INTERRUPT_CLEAR,
        .rsvd = ROCKCHIP_RKNN_INTERRUPT_RESERVED_BITS,
        .pre_write = rockchip_rknn_interrupt_clear_prew,
    }, { .name = "PC_INTERRUPT_STATUS", .addr = A_PC_INTERRUPT_STATUS,
        .ro = UINT32_MAX,
    }, { .name = "PC_INTERRUPT_RAW_STATUS", .addr = A_PC_INTERRUPT_RAW_STATUS,
        .ro = UINT32_MAX,
    }, { .name = "PC_TASK_CON", .addr = A_PC_TASK_CON,
    }, { .name = "PC_TASK_DMA_BASE_ADDR", .addr = A_PC_TASK_DMA_BASE_ADDR,
    }, { .name = "PC_TASK_STATUS", .addr = A_PC_TASK_STATUS,
        .ro = UINT32_MAX,
    },
};

static const RegisterAccessInfo rockchip_rknn_cna_regs_info[] = {
    {   .name = "CNA_S_POINTER", .addr = A_CNA_S_POINTER,
        .ro = ROCKCHIP_RKNN_CORE_TAG_MASK,
        .post_write = rockchip_rknn_cna_pointer_postw,
    },
};

static const RegisterAccessInfo rockchip_rknn_core_regs_info[] = {
    {   .name = "CORE_S_POINTER", .addr = A_CORE_S_POINTER,
        .ro = ROCKCHIP_RKNN_CORE_TAG_MASK,
        .post_write = rockchip_rknn_core_pointer_postw,
    },
};

static const RegisterAccessInfo rockchip_rknn_ppu_regs_info[] = {
    {   .name = "PPU_S_STATUS", .addr = A_PPU_S_STATUS,
        .ro = UINT32_MAX,
    }, { .name = "PPU_S_POINTER", .addr = A_PPU_S_POINTER,
        .post_write = rockchip_rknn_ppu_pointer_postw,
    },
};

static const RegisterAccessInfo rockchip_rknn_ppu_rdma_regs_info[] = {
    {   .name = "PPU_RDMA_S_STATUS", .addr = A_PPU_RDMA_S_STATUS,
        .ro = UINT32_MAX,
    }, { .name = "PPU_RDMA_S_POINTER", .addr = A_PPU_RDMA_S_POINTER,
        .post_write = rockchip_rknn_ppu_rdma_pointer_postw,
    },
};

static uint64_t rockchip_rknn_slave_domain_read(
    RockchipRKNNCoreState *s, RockchipRKNNRegcmdDomain domain,
    uint32_t rel)
{
    uint32_t value;

    return rockchip_rknn_register_read(&s->slave_file, domain, rel, &value) ?
           value : 0;
}

static void rockchip_rknn_slave_domain_write(
    RockchipRKNNCoreState *s, RockchipRKNNRegcmdDomain domain,
    uint32_t rel, uint32_t value)
{
    static const uint32_t targets[ROCKCHIP_RKNN_DOMAIN_COUNT] = {
        [ROCKCHIP_RKNN_DOMAIN_PC] = ROCKCHIP_RKNN_REGCMD_TARGET_PC,
        [ROCKCHIP_RKNN_DOMAIN_CNA] = ROCKCHIP_RKNN_REGCMD_TARGET_CNA,
        [ROCKCHIP_RKNN_DOMAIN_CORE] = ROCKCHIP_RKNN_REGCMD_TARGET_CORE,
        [ROCKCHIP_RKNN_DOMAIN_DPU] = ROCKCHIP_RKNN_REGCMD_TARGET_DPU,
        [ROCKCHIP_RKNN_DOMAIN_DPU_RDMA] =
            ROCKCHIP_RKNN_REGCMD_TARGET_DPU_RDMA,
        [ROCKCHIP_RKNN_DOMAIN_PPU] = ROCKCHIP_RKNN_REGCMD_TARGET_PPU,
        [ROCKCHIP_RKNN_DOMAIN_PPU_RDMA] =
            ROCKCHIP_RKNN_REGCMD_TARGET_PPU_RDMA,
    };
    static const uint32_t bases[ROCKCHIP_RKNN_DOMAIN_COUNT] = {
        [ROCKCHIP_RKNN_DOMAIN_PC] = 0,
        [ROCKCHIP_RKNN_DOMAIN_CNA] = ROCKCHIP_RKNN_REGCMD_CNA_BASE,
        [ROCKCHIP_RKNN_DOMAIN_CORE] = ROCKCHIP_RKNN_REGCMD_CORE_BASE,
        [ROCKCHIP_RKNN_DOMAIN_DPU] = ROCKCHIP_RKNN_REGCMD_DPU_BASE,
        [ROCKCHIP_RKNN_DOMAIN_DPU_RDMA] =
            ROCKCHIP_RKNN_REGCMD_DPU_RDMA_BASE,
        [ROCKCHIP_RKNN_DOMAIN_PPU] = ROCKCHIP_RKNN_REGCMD_PPU_BASE,
        [ROCKCHIP_RKNN_DOMAIN_PPU_RDMA] =
            ROCKCHIP_RKNN_REGCMD_PPU_RDMA_BASE,
    };

    if (domain == ROCKCHIP_RKNN_DOMAIN_DPU) {
        rockchip_rknn_lut_write(s, rel, value);
    }
    rockchip_rknn_register_write(&s->slave_file, targets[domain],
                                 bases[domain] + rel, value, NULL);
}

static uint64_t rockchip_rknn_cna_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    RegisterInfoArray *array = opaque;
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(
        register_array_get_owner(array));

    if (addr == A_CNA_S_POINTER) {
        return register_read_memory(opaque, addr, size);
    }
    return rockchip_rknn_slave_domain_read(s, ROCKCHIP_RKNN_DOMAIN_CNA,
                                           addr);
}

static void rockchip_rknn_cna_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    RegisterInfoArray *array = opaque;
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(
        register_array_get_owner(array));

    if (addr == A_CNA_S_POINTER) {
        register_write_memory(opaque, addr, value, size);
    }
    rockchip_rknn_slave_domain_write(s, ROCKCHIP_RKNN_DOMAIN_CNA, addr,
                                     value);
}

static uint64_t rockchip_rknn_core_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    RegisterInfoArray *array = opaque;
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(
        register_array_get_owner(array));

    if (addr == A_CORE_S_POINTER) {
        return register_read_memory(opaque, addr, size);
    }
    return rockchip_rknn_slave_domain_read(s, ROCKCHIP_RKNN_DOMAIN_CORE,
                                           addr);
}

static void rockchip_rknn_core_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size)
{
    RegisterInfoArray *array = opaque;
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(
        register_array_get_owner(array));

    if (addr == A_CORE_S_POINTER) {
        register_write_memory(opaque, addr, value, size);
    }
    rockchip_rknn_slave_domain_write(s, ROCKCHIP_RKNN_DOMAIN_CORE, addr,
                                     value);
}

static uint64_t rockchip_rknn_ppu_domain_read(
    RegisterInfoArray *array, RockchipRKNNRegcmdDomain domain,
    hwaddr addr, unsigned size)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(
        register_array_get_owner(array));

    if (addr == 0 || addr == 4) {
        return register_read_memory(array, addr, size);
    }
    return rockchip_rknn_slave_domain_read(s, domain, addr);
}

static void rockchip_rknn_ppu_domain_write(
    RegisterInfoArray *array, RockchipRKNNRegcmdDomain domain,
    hwaddr addr, uint64_t value, unsigned size)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(
        register_array_get_owner(array));

    if (addr == 0) {
        register_write_memory(array, addr, value, size);
        return;
    }
    if (addr == 4) {
        register_write_memory(array, addr, value, size);
    }
    rockchip_rknn_slave_domain_write(s, domain, addr, value);
}

static uint64_t rockchip_rknn_ppu_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    return rockchip_rknn_ppu_domain_read(
        opaque, ROCKCHIP_RKNN_DOMAIN_PPU, addr, size);
}

static void rockchip_rknn_ppu_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    rockchip_rknn_ppu_domain_write(
        opaque, ROCKCHIP_RKNN_DOMAIN_PPU, addr, value, size);
}

static uint64_t rockchip_rknn_ppu_rdma_read(void *opaque, hwaddr addr,
                                            unsigned size)
{
    return rockchip_rknn_ppu_domain_read(
        opaque, ROCKCHIP_RKNN_DOMAIN_PPU_RDMA, addr, size);
}

static void rockchip_rknn_ppu_rdma_write(void *opaque, hwaddr addr,
                                         uint64_t value, unsigned size)
{
    rockchip_rknn_ppu_domain_write(
        opaque, ROCKCHIP_RKNN_DOMAIN_PPU_RDMA, addr, value, size);
}

static uint64_t rockchip_rknn_dpu_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    RockchipRKNNCoreState *s = opaque;

    return rockchip_rknn_slave_domain_read(s, ROCKCHIP_RKNN_DOMAIN_DPU,
                                           addr);
}

static void rockchip_rknn_dpu_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    RockchipRKNNCoreState *s = opaque;

    rockchip_rknn_slave_domain_write(s, ROCKCHIP_RKNN_DOMAIN_DPU, addr,
                                     value);
}

static uint64_t rockchip_rknn_global_read(void *opaque, hwaddr addr,
                                          unsigned size)
{
    return 0;
}

static void rockchip_rknn_global_write(void *opaque, hwaddr addr,
                                       uint64_t value, unsigned size)
{
    RockchipRKNNCoreState *s = opaque;

    if (addr == A_GLOBAL_OPERATION_ENABLE && value) {
        rockchip_rknn_start_slave(s, value);
    }
}

static const MemoryRegionOps rockchip_rknn_reg_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const MemoryRegionOps rockchip_rknn_cna_ops = {
    .read = rockchip_rknn_cna_read,
    .write = rockchip_rknn_cna_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4, },
    .valid = { .min_access_size = 4, .max_access_size = 4, },
};

static const MemoryRegionOps rockchip_rknn_core_ops = {
    .read = rockchip_rknn_core_read,
    .write = rockchip_rknn_core_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4, },
    .valid = { .min_access_size = 4, .max_access_size = 4, },
};

static const MemoryRegionOps rockchip_rknn_ppu_ops = {
    .read = rockchip_rknn_ppu_read,
    .write = rockchip_rknn_ppu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4, },
    .valid = { .min_access_size = 4, .max_access_size = 4, },
};

static const MemoryRegionOps rockchip_rknn_ppu_rdma_ops = {
    .read = rockchip_rknn_ppu_rdma_read,
    .write = rockchip_rknn_ppu_rdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4, },
    .valid = { .min_access_size = 4, .max_access_size = 4, },
};

static const MemoryRegionOps rockchip_rknn_dpu_ops = {
    .read = rockchip_rknn_dpu_read,
    .write = rockchip_rknn_dpu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4, },
    .valid = { .min_access_size = 4, .max_access_size = 4, },
};

static const MemoryRegionOps rockchip_rknn_global_ops = {
    .read = rockchip_rknn_global_read,
    .write = rockchip_rknn_global_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4, },
    .valid = { .min_access_size = 4, .max_access_size = 4, },
};

static void rockchip_rknn_reset(DeviceState *dev)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(dev);

    rockchip_rknn_drain_execution(s, true);
    timer_del(&s->complete_timer);
    s->pending_task_count = 0;
    s->pending_task_index = 0;
    s->pending_next_iova = 0;
    s->pending_next_command_count = 0;
    memset(s->pending_pipeline, 0, sizeof(*s->pending_pipeline));
    s->pending_pipeline_decoded = false;
    memset(&s->pending_dpu_stage, 0, sizeof(s->pending_dpu_stage));
    s->pending_domain_runtime_valid = false;
    s->pending_fetch_error = false;
    s->pending_execution_error = false;
    s->pending_ppu_stage_attempted = false;
    s->pending_final_pipeline_attempted = false;
    s->pending_final_ppu_attempted = false;
    s->pending_final_ppu_success = false;
    s->pending_final_ppu_bank = 0;
    s->pending_dma_error_bits = 0;
    s->pending_slave = false;
    s->busy = false;
    s->execution_mode = ROCKCHIP_RKNN_EXECUTION_UNSUPPORTED;
    s->execution_result = ROCKCHIP_RKNN_EXECUTION_OK;
    s->execution_result_ready = false;
    rockchip_rknn_clear_regcmd_shadow(s);
    memset(s->domain_runtime, 0, sizeof(s->domain_runtime));
    memset(s->pending_domain_runtime, 0, sizeof(s->pending_domain_runtime));
    memset(&s->slave_file, 0, sizeof(s->slave_file));
    memset(&s->pending_file, 0, sizeof(s->pending_file));
    memset(s->pending_register_writes, 0,
           sizeof(s->pending_register_writes));
    memset(s->lut, 0, sizeof(s->lut));
    s->lut_access_cfg = 0;
    s->lut_cfg = 0;
    s->lut_info = 0;
    s->lut_le_start = 0;
    s->lut_le_end = 0;
    s->lut_lo_start = 0;
    s->lut_lo_end = 0;
    s->lut_le_slope_scale = 0;
    s->lut_le_slope_shift = 0;
    s->lut_lo_slope_scale = 0;
    s->lut_lo_slope_shift = 0;

    s->pc_regs[R_PC_INTERRUPT_RAW_STATUS] = 0;
    rockchip_rknn_update_irq(s);
    for (unsigned int i = 0; i < ARRAY_SIZE(s->pc_regs_info); i++) {
        register_reset(&s->pc_regs_info[i]);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(s->cna_regs_info); i++) {
        register_reset(&s->cna_regs_info[i]);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(s->core_regs_info); i++) {
        register_reset(&s->core_regs_info[i]);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(s->ppu_regs_info); i++) {
        register_reset(&s->ppu_regs_info[i]);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(s->ppu_rdma_regs_info); i++) {
        register_reset(&s->ppu_rdma_regs_info[i]);
    }

    rockchip_rknn_update_irq(s);

    for (unsigned int i = 0; i < ROCKCHIP_RKNN_CORE_COUNT; i++) {
        RockchipRKNNCoreState *peer = rockchip_rknn_get_peer(s, i);

        if (peer && peer->execution_result_ready) {
            rockchip_rknn_try_complete_rendezvous(peer);
        }
    }
}

static int rockchip_rknn_post_load(void *opaque, int version_id)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(opaque);
    uint32_t cna_conv2;

    if (s->pending_task_index > s->pending_task_count) {
        return -EINVAL;
    }
    if (version_id < 2) {
        s->execution_result_ready = false;
        s->execution_result = ROCKCHIP_RKNN_EXECUTION_OK;
    }
    /* co_work_mode is derived from the migrated register file. */
    if (s->pending_pipeline_decoded && s->pending_pipeline &&
        (s->pending_pipeline->enabled_blocks & ROCKCHIP_RKNN_BLOCK_CNA)) {
        rockchip_rknn_register_read(&s->pending_file,
                                    ROCKCHIP_RKNN_DOMAIN_CNA,
                                    ROCKCHIP_RKNN_CNA_CONV_CON2,
                                    &cna_conv2);
        s->pending_pipeline->cna.co_work_mode = extract32(cna_conv2, 28, 3);
    }
    s->ppu_regs[R_PPU_S_POINTER] = rockchip_rknn_encode_pointer_state(
        &s->domain_runtime[ROCKCHIP_RKNN_DOMAIN_PPU]);
    s->ppu_rdma_regs[R_PPU_RDMA_S_POINTER] =
        rockchip_rknn_encode_pointer_state(
            &s->domain_runtime[ROCKCHIP_RKNN_DOMAIN_PPU_RDMA]);
    rockchip_rknn_update_irq(s);
    if (s->execution_result_ready) {
        rockchip_rknn_try_complete_rendezvous(s);
    }
    return 0;
}

static bool rockchip_rknn_execution_active(Object *obj, Error **errp)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(obj);

    return s->execution_aiocb || s->execution_result_ready;
}

static int rockchip_rknn_pre_save(void *opaque)
{
    RockchipRKNNCoreState *s = opaque;

    rockchip_rknn_drain_executions(s);
    for (unsigned int i = 0; i < ROCKCHIP_RKNN_CORE_COUNT; i++) {
        RockchipRKNNCoreState *peer = rockchip_rknn_get_peer(s, i);

        if (peer && peer->execution_aiocb) {
            return -EBUSY;
        }
    }
    return 0;
}

static void rockchip_rknn_vm_state_change(void *opaque, bool running,
                                          RunState state)
{
    RockchipRKNNCoreState *s = opaque;

    if (!running && state == RUN_STATE_FINISH_MIGRATE &&
        s->core_index == 0) {
        rockchip_rknn_drain_executions(s);
    }
}

static void rockchip_rknn_reset_input(void *opaque, int n, int level)
{
    RockchipRKNNCoreState *s = opaque;
    bool asserted = level;

    (void)n;
    if (asserted && !s->reset_asserted) {
        s->reset_asserted = true;
        device_cold_reset(DEVICE(s));
    } else {
        s->reset_asserted = asserted;
    }
}

static void rockchip_rknn_init(Object *obj)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(obj);
    DeviceState *dev = DEVICE(obj);

    s->pc_reg_array =
        register_init_block32(dev, rockchip_rknn_pc_regs_info,
                              ARRAY_SIZE(rockchip_rknn_pc_regs_info),
                              s->pc_regs_info, s->pc_regs,
                              &rockchip_rknn_reg_ops, false,
                              ROCKCHIP_RKNN_WINDOW_SIZE);
    s->cna_reg_array =
        register_init_block32(dev, rockchip_rknn_cna_regs_info,
                              ARRAY_SIZE(rockchip_rknn_cna_regs_info),
                              s->cna_regs_info, s->cna_regs,
                              &rockchip_rknn_cna_ops, false,
                              ROCKCHIP_RKNN_WINDOW_SIZE);
    s->core_reg_array =
        register_init_block32(dev, rockchip_rknn_core_regs_info,
                              ARRAY_SIZE(rockchip_rknn_core_regs_info),
                              s->core_regs_info, s->core_regs,
                              &rockchip_rknn_core_ops, false,
                              ROCKCHIP_RKNN_WINDOW_SIZE);
    s->ppu_reg_array =
        register_init_block32(dev, rockchip_rknn_ppu_regs_info,
                              ARRAY_SIZE(rockchip_rknn_ppu_regs_info),
                              s->ppu_regs_info, s->ppu_regs,
                              &rockchip_rknn_ppu_ops, false,
                              ROCKCHIP_RKNN_WINDOW_SIZE);
    s->ppu_rdma_reg_array =
        register_init_block32(dev, rockchip_rknn_ppu_rdma_regs_info,
                              ARRAY_SIZE(rockchip_rknn_ppu_rdma_regs_info),
                              s->ppu_rdma_regs_info, s->ppu_rdma_regs,
                              &rockchip_rknn_ppu_rdma_ops, false,
                              ROCKCHIP_RKNN_WINDOW_SIZE);

    memory_region_init_io(&s->dpu_reg_array, obj, &rockchip_rknn_dpu_ops, s,
                          TYPE_ROCKCHIP_RKNN_CORE ".dpu",
                          ROCKCHIP_RKNN_WINDOW_SIZE);
    memory_region_init_io(&s->global_reg_array, obj, &rockchip_rknn_global_ops,
                          s, TYPE_ROCKCHIP_RKNN_CORE ".global",
                          ROCKCHIP_RKNN_WINDOW_SIZE);

    timer_init_ns(&s->complete_timer, QEMU_CLOCK_VIRTUAL,
                  rockchip_rknn_complete, s);
    qemu_mutex_init(&s->execution_lock);
    qemu_cond_init(&s->execution_cond);
    object_property_add_bool(obj, "x-execution-active",
                             rockchip_rknn_execution_active, NULL);
    qdev_init_gpio_in_named(dev, rockchip_rknn_reset_input, "reset", 1);
    s->pending_pipeline = g_new0(RockchipRKNNPipelineTask, 1);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->pc_reg_array->mem);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->cna_reg_array->mem);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->core_reg_array->mem);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->dpu_reg_array);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->global_reg_array);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->ppu_reg_array->mem);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->ppu_rdma_reg_array->mem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void rockchip_rknn_finalize(Object *obj)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(obj);

    qemu_cond_destroy(&s->execution_cond);
    qemu_mutex_destroy(&s->execution_lock);
    g_clear_pointer(&s->pending_pipeline, g_free);
}

static void rockchip_rknn_realize(DeviceState *dev, Error **errp)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(dev);
    IOMMUMemoryRegion *iommu;

    if (s->core_index >= ROCKCHIP_RKNN_CORE_COUNT) {
        error_setg(errp, TYPE_ROCKCHIP_RKNN_CORE
                   " invalid core-index %u", s->core_index);
        return;
    }
    if (!s->dma_mr) {
        error_setg(errp, TYPE_ROCKCHIP_RKNN_CORE " 'dma' link not set");
        return;
    }
    iommu = memory_region_get_iommu(s->dma_mr);
    if (!iommu || MEMORY_REGION(iommu) != s->dma_mr) {
        error_setg(errp, TYPE_ROCKCHIP_RKNN_CORE
                   " 'dma' link is not a direct IOMMU memory region");
        return;
    }

    s->dma_as = g_new0(AddressSpace, 1);
    address_space_init(s->dma_as, s->dma_mr, "rk3588-rknpu-dma");
    s->vmstate = qdev_add_vm_change_state_handler(
        dev, rockchip_rknn_vm_state_change, NULL, s);
}

static void rockchip_rknn_unrealize(DeviceState *dev)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(dev);

    qemu_del_vm_change_state_handler(s->vmstate);
    s->vmstate = NULL;
    rockchip_rknn_drain_execution(s, true);
    if (s->dma_as) {
        address_space_destroy_free(s->dma_as);
        s->dma_as = NULL;
    }
}

static const VMStateDescription vmstate_rockchip_rknn_tensor = {
    .name = "rockchip-rknn-tensor",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(iova, RockchipRKNNTensorView),
        VMSTATE_UINT32(width, RockchipRKNNTensorView),
        VMSTATE_UINT32(height, RockchipRKNNTensorView),
        VMSTATE_UINT32(channels, RockchipRKNNTensorView),
        VMSTATE_UINT32(line_stride, RockchipRKNNTensorView),
        VMSTATE_UINT32(surface_stride, RockchipRKNNTensorView),
        VMSTATE_UINT8(precision, RockchipRKNNTensorView),
        VMSTATE_UINT8(atom, RockchipRKNNTensorView),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rockchip_rknn_cna = {
    .name = "rockchip-rknn-cna",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(input, RockchipRKNNCNAConfig, 0,
                       vmstate_rockchip_rknn_tensor, RockchipRKNNTensorView),
        VMSTATE_UINT32(weight_iova, RockchipRKNNCNAConfig),
        VMSTATE_UINT32(weight_bytes, RockchipRKNNCNAConfig),
        VMSTATE_UINT32(weight_bytes_per_kernel, RockchipRKNNCNAConfig),
        VMSTATE_UINT32(output_atomics, RockchipRKNNCNAConfig),
        VMSTATE_UINT32(cvt_con0, RockchipRKNNCNAConfig),
        VMSTATE_UINT32_ARRAY(cvt_channel, RockchipRKNNCNAConfig, 4),
        VMSTATE_UINT32(per_channel_cvt, RockchipRKNNCNAConfig),
        VMSTATE_UINT32(pad_value, RockchipRKNNCNAConfig),
        VMSTATE_UINT32(fc_data_size0, RockchipRKNNCNAConfig),
        VMSTATE_UINT32(fc_data_size1, RockchipRKNNCNAConfig),
        VMSTATE_UINT16(input_channels_valid, RockchipRKNNCNAConfig),
        VMSTATE_UINT16(output_width, RockchipRKNNCNAConfig),
        VMSTATE_UINT16(weight_kernels, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(kernel_groups, RockchipRKNNCNAConfig),
        VMSTATE_UINT16(feature_grains, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(nn_mode, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(atrous_x_dilation, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(atrous_y_dilation, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(deconv_stride_x, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(deconv_stride_y, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(surface_mode, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(kernel_width, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(kernel_height, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(conv_mode, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(argb_in, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(input_precision, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(process_precision, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(stride_x, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(stride_y, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(pad_left, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(pad_top, RockchipRKNNCNAConfig),
        VMSTATE_BOOL(nonalign_dma, RockchipRKNNCNAConfig),
        VMSTATE_BOOL(group_line_off, RockchipRKNNCNAConfig),
        VMSTATE_BOOL(deconv, RockchipRKNNCNAConfig),
        VMSTATE_BOOL(csc_weight_output_disable, RockchipRKNNCNAConfig),
        VMSTATE_BOOL(csc_data_output_disable, RockchipRKNNCNAConfig),
        VMSTATE_BOOL(cmd_fifo_soft_reset, RockchipRKNNCNAConfig),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rockchip_rknn_core = {
    .name = "rockchip-rknn-core-config",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(width, RockchipRKNNCoreConfig),
        VMSTATE_UINT32(height, RockchipRKNNCoreConfig),
        VMSTATE_UINT32(channels, RockchipRKNNCoreConfig),
        VMSTATE_UINT8(process_precision, RockchipRKNNCoreConfig),
        VMSTATE_UINT8(clip_truncate, RockchipRKNNCoreConfig),
        VMSTATE_BOOL(depthwise, RockchipRKNNCoreConfig),
        VMSTATE_BOOL(quantify, RockchipRKNNCoreConfig),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rockchip_rknn_dpu = {
    .name = "rockchip-rknn-dpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(output, RockchipRKNNDPUConfig, 0,
                       vmstate_rockchip_rknn_tensor, RockchipRKNNTensorView),
        VMSTATE_UINT32(feature_mode, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(data_format, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(offset_pend, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(output_notch_0, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(output_notch_1, RockchipRKNNDPUConfig),
        VMSTATE_UINT8(minmax_ctl, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(bs_cfg, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(bs_ow_cfg, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(bs_ow_op, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(bs_relux_cmp, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(dst_dma_cfg, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(bn_cfg, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(bn_relux_cmp, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(ew_cfg, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(ew_relux_cmp, RockchipRKNNDPUConfig),
        VMSTATE_INT32(out_cvt_offset, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(out_cvt_scale, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(out_cvt_shift, RockchipRKNNDPUConfig),
        VMSTATE_UINT8(out_cvt_minus_exp, RockchipRKNNDPUConfig),
        VMSTATE_BOOL(out_cvt_type, RockchipRKNNDPUConfig),
        VMSTATE_BOOL(out_fp32_to_fp16, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(surface_add, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(output_channels_valid, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(wdma_channels, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(wdma_width, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(wdma_height, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(wdma_size_c, RockchipRKNNDPUConfig),
        VMSTATE_BOOL(wdma_tp_precision, RockchipRKNNDPUConfig),
        VMSTATE_UINT8(input_precision, RockchipRKNNDPUConfig),
        VMSTATE_UINT8(process_precision, RockchipRKNNDPUConfig),
        VMSTATE_UINT8(output_precision, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(lut_cfg, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(lut_info, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(lut_le_start, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(lut_le_end, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(lut_lo_start, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(lut_lo_end, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(lut_le_slope_scale, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(lut_le_slope_shift, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(lut_lo_slope_scale, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(lut_lo_slope_shift, RockchipRKNNDPUConfig),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rockchip_rknn_dpu_rdma = {
    .name = "rockchip-rknn-dpu-rdma",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(src_iova, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(bs_iova, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(bn_iova, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(ew_iova, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(width, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(height, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(channels, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(brdma_cfg, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(nrdma_cfg, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(erdma_cfg, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(ew_surface_stride, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(feature_mode, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(src_dma_cfg, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(surface_notch, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(pad_cfg, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(weight, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(ew_surface_notch, RockchipRKNNDpuRdmaConfig),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rockchip_rknn_ppu = {
    .name = "rockchip-rknn-ppu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(src_iova, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(dst_iova, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(in_width, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(in_height, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(in_channels, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(rdma_in_width, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(rdma_in_height, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(rdma_in_channels, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(out_width, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(out_height, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(out_channels, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(mode, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(kernel, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(reciprocal_width, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(reciprocal_height, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(padding, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(padding_value_0, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(padding_value_1, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(dst_stride, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(line_stride, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(surf_stride, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(data_format, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(rdma_data_format, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(misc_ctrl, RockchipRKNNPPUConfig),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rockchip_rknn_pipeline = {
    .name = "rockchip-rknn-pipeline",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(cna, RockchipRKNNPipelineTask, 0,
                       vmstate_rockchip_rknn_cna, RockchipRKNNCNAConfig),
        VMSTATE_STRUCT(core, RockchipRKNNPipelineTask, 0,
                       vmstate_rockchip_rknn_core, RockchipRKNNCoreConfig),
        VMSTATE_STRUCT(dpu, RockchipRKNNPipelineTask, 0,
                       vmstate_rockchip_rknn_dpu, RockchipRKNNDPUConfig),
        VMSTATE_STRUCT(dpu_rdma, RockchipRKNNPipelineTask, 0,
                       vmstate_rockchip_rknn_dpu_rdma,
                       RockchipRKNNDpuRdmaConfig),
        VMSTATE_STRUCT(ppu, RockchipRKNNPipelineTask, 0,
                       vmstate_rockchip_rknn_ppu, RockchipRKNNPPUConfig),
        VMSTATE_UINT32(task_dma_base, RockchipRKNNPipelineTask),
        VMSTATE_UINT32(enabled_blocks, RockchipRKNNPipelineTask),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rockchip_rknn_dpu_stage = {
    .name = "rockchip-rknn-dpu-stage",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(bs_alu_operand, RockchipRKNNDPUStageSnapshot),
        VMSTATE_UINT32(bs_mul_cfg, RockchipRKNNDPUStageSnapshot),
        VMSTATE_INT32(bn_alu_operand, RockchipRKNNDPUStageSnapshot),
        VMSTATE_UINT32(bn_mul_cfg, RockchipRKNNDPUStageSnapshot),
        VMSTATE_INT32(ew_cvt_offset, RockchipRKNNDPUStageSnapshot),
        VMSTATE_UINT32(ew_cvt_scale, RockchipRKNNDPUStageSnapshot),
        VMSTATE_INT32_ARRAY(ew_operand, RockchipRKNNDPUStageSnapshot, 8),
        VMSTATE_BOOL(out_cvt_round, RockchipRKNNDPUStageSnapshot),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rockchip_rknn_domain_runtime = {
    .name = "rockchip-rknn-domain-runtime",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pointer_value, RockchipRKNNDomainRuntimeState),
        VMSTATE_UINT8(pointer_bank, RockchipRKNNDomainRuntimeState),
        VMSTATE_UINT8(executor_bank, RockchipRKNNDomainRuntimeState),
        VMSTATE_BOOL(pointer_pingpong, RockchipRKNNDomainRuntimeState),
        VMSTATE_BOOL(executor_pingpong, RockchipRKNNDomainRuntimeState),
        VMSTATE_BOOL(pingpong_mode, RockchipRKNNDomainRuntimeState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rockchip_rknn_register_bank = {
    .name = "rockchip-rknn-register-bank",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RockchipRKNNRegisterBank,
                             ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX),
        VMSTATE_UINT32_ARRAY(present, RockchipRKNNRegisterBank,
                             ROCKCHIP_RKNN_PRESENT_R_MAX),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rockchip_rknn_domain_state = {
    .name = "rockchip-rknn-domain-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(bank, RockchipRKNNDomainState, 2, 0,
                             vmstate_rockchip_rknn_register_bank,
                             RockchipRKNNRegisterBank),
        VMSTATE_UINT8(write_bank, RockchipRKNNDomainState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rockchip_rknn_register_file = {
    .name = "rockchip-rknn-register-file",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(domain, RockchipRKNNRegisterFile,
                             ROCKCHIP_RKNN_REGCMD_DOMAIN_COUNT, 0,
                             vmstate_rockchip_rknn_domain_state,
                             RockchipRKNNDomainState),
        VMSTATE_STRUCT_ARRAY(runtime, RockchipRKNNRegisterFile,
                             ROCKCHIP_RKNN_REGCMD_DOMAIN_COUNT, 0,
                             vmstate_rockchip_rknn_domain_runtime,
                             RockchipRKNNDomainRuntimeState),
        VMSTATE_UINT32(enabled_blocks, RockchipRKNNRegisterFile),
        VMSTATE_BOOL(pre_enable, RockchipRKNNRegisterFile),
        VMSTATE_BOOL(block_enable, RockchipRKNNRegisterFile),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rockchip_rknn = {
    .name = TYPE_ROCKCHIP_RKNN_CORE,
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_save = rockchip_rknn_pre_save,
    .post_load = rockchip_rknn_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(pc_regs, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_PC_R_MAX),
        VMSTATE_UINT32_ARRAY(cna_regs, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_CNA_R_MAX),
        VMSTATE_UINT32_ARRAY(core_regs, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_CORE_R_MAX),
        VMSTATE_UINT32_ARRAY(ppu_regs, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_PPU_R_MAX),
        VMSTATE_UINT32_ARRAY(ppu_rdma_regs, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_PPU_RDMA_R_MAX),
        VMSTATE_TIMER(complete_timer, RockchipRKNNCoreState),
        VMSTATE_UINT32(core_index, RockchipRKNNCoreState),
        VMSTATE_BOOL(busy, RockchipRKNNCoreState),
        VMSTATE_BOOL(irq_level, RockchipRKNNCoreState),
        VMSTATE_STRUCT_ARRAY(domain_runtime, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_REGCMD_DOMAIN_COUNT, 0,
                             vmstate_rockchip_rknn_domain_runtime,
                             RockchipRKNNDomainRuntimeState),
        VMSTATE_STRUCT_POINTER(pending_pipeline, RockchipRKNNCoreState,
                               vmstate_rockchip_rknn_pipeline,
                               RockchipRKNNPipelineTask),
        VMSTATE_STRUCT_ARRAY(pending_domain_runtime, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_REGCMD_DOMAIN_COUNT, 0,
                             vmstate_rockchip_rknn_domain_runtime,
                             RockchipRKNNDomainRuntimeState),
        VMSTATE_BOOL(pending_pipeline_decoded, RockchipRKNNCoreState),
        VMSTATE_BOOL(pending_domain_runtime_valid, RockchipRKNNCoreState),
        VMSTATE_BOOL(pending_fetch_error, RockchipRKNNCoreState),
        VMSTATE_BOOL(pending_execution_error, RockchipRKNNCoreState),
        VMSTATE_BOOL(pending_ppu_stage_attempted, RockchipRKNNCoreState),
        VMSTATE_BOOL(pending_final_pipeline_attempted, RockchipRKNNCoreState),
        VMSTATE_BOOL(pending_final_ppu_attempted, RockchipRKNNCoreState),
        VMSTATE_BOOL(pending_final_ppu_success, RockchipRKNNCoreState),
        VMSTATE_UINT8(pending_final_ppu_bank, RockchipRKNNCoreState),
        VMSTATE_UINT32(pending_dma_error_bits, RockchipRKNNCoreState),
        VMSTATE_UINT32(pending_next_iova, RockchipRKNNCoreState),
        VMSTATE_UINT32(pending_next_command_count, RockchipRKNNCoreState),
        VMSTATE_UINT16(pending_task_count, RockchipRKNNCoreState),
        VMSTATE_UINT16(pending_task_index, RockchipRKNNCoreState),
        VMSTATE_STRUCT(slave_file, RockchipRKNNCoreState, 0,
                       vmstate_rockchip_rknn_register_file,
                       RockchipRKNNRegisterFile),
        VMSTATE_STRUCT(pending_file, RockchipRKNNCoreState, 0,
                       vmstate_rockchip_rknn_register_file,
                       RockchipRKNNRegisterFile),
        VMSTATE_UINT32_ARRAY(pending_register_writes,
                             RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_PENDING_WRITE_R_MAX),
        VMSTATE_BOOL(pending_slave, RockchipRKNNCoreState),
        VMSTATE_BOOL_V(execution_result_ready, RockchipRKNNCoreState, 2),
        VMSTATE_INT32_V(execution_result, RockchipRKNNCoreState, 2),
        VMSTATE_UINT16_2DARRAY(lut, RockchipRKNNCoreState, 2,
                               ROCKCHIP_RKNN_LUT_ENTRIES),
        VMSTATE_UINT32(lut_access_cfg, RockchipRKNNCoreState),
        VMSTATE_UINT32(lut_cfg, RockchipRKNNCoreState),
        VMSTATE_UINT32(lut_info, RockchipRKNNCoreState),
        VMSTATE_UINT32(lut_le_start, RockchipRKNNCoreState),
        VMSTATE_UINT32(lut_le_end, RockchipRKNNCoreState),
        VMSTATE_UINT32(lut_lo_start, RockchipRKNNCoreState),
        VMSTATE_UINT32(lut_lo_end, RockchipRKNNCoreState),
        VMSTATE_UINT32(lut_le_slope_scale, RockchipRKNNCoreState),
        VMSTATE_UINT32(lut_le_slope_shift, RockchipRKNNCoreState),
        VMSTATE_UINT32(lut_lo_slope_scale, RockchipRKNNCoreState),
        VMSTATE_UINT32(lut_lo_slope_shift, RockchipRKNNCoreState),
        VMSTATE_STRUCT(pending_dpu_stage, RockchipRKNNCoreState, 0,
                       vmstate_rockchip_rknn_dpu_stage,
                       RockchipRKNNDPUStageSnapshot),
        VMSTATE_END_OF_LIST()
    },
};

static const Property rockchip_rknn_properties[] = {
    DEFINE_PROP_LINK("dma", RockchipRKNNCoreState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT32("core-index", RockchipRKNNCoreState, core_index, 0),
    DEFINE_PROP_UINT64("functional-max-host-bytes", RockchipRKNNCoreState,
                       functional_max_host_bytes,
                       ROCKCHIP_RKNN_DEFAULT_MAX_HOST_BYTES),
    DEFINE_PROP_UINT64("functional-max-mac-operations", RockchipRKNNCoreState,
                       functional_max_mac_operations,
                       ROCKCHIP_RKNN_DEFAULT_MAX_MAC_OPERATIONS),
    DEFINE_PROP_UINT64("functional-max-ppu-work-items", RockchipRKNNCoreState,
                       functional_max_ppu_work_items,
                       ROCKCHIP_RKNN_DEFAULT_MAX_PPU_WORK_ITEMS),
};

static void rockchip_rknn_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, rockchip_rknn_reset);
    dc->realize = rockchip_rknn_realize;
    dc->unrealize = rockchip_rknn_unrealize;
    dc->vmsd = &vmstate_rockchip_rknn;
    device_class_set_props(dc, rockchip_rknn_properties);
    dc->user_creatable = false;
}

static const TypeInfo rockchip_rknn_info = {
    .name = TYPE_ROCKCHIP_RKNN_CORE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipRKNNCoreState),
    .instance_init = rockchip_rknn_init,
    .instance_finalize = rockchip_rknn_finalize,
    .class_init = rockchip_rknn_class_init,
};

static void rockchip_rknn_register_types(void)
{
    type_register_static(&rockchip_rknn_info);
}

type_init(rockchip_rknn_register_types)
