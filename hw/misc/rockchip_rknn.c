/*
 * Rockchip RK3588 RKNN/RKNPU core
 *
 * This is a register-level fake-completion model for the Linux mainline
 * rocket driver path. It accepts task submission writes and completes them
 * with the DPU interrupt bits. An optional experimental mode executes one
 * hardware-validated INT8 Matmul command stream.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "hw/misc/rockchip_rknn.h"
#include "migration/vmstate.h"
#include "qemu/int128.h"
#include "qemu/module.h"
#include "system/dma.h"
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

#define ROCKCHIP_RKNN_PC_VERSION 0x00000100
#define ROCKCHIP_RKNN_PC_VERSION_NUM 0x00003588
#define ROCKCHIP_RKNN_DPU_INTERRUPT_BITS 0x00000300
#define ROCKCHIP_RKNN_PIPELINE_BANK0_INTERRUPT 0x000002aa
#define ROCKCHIP_RKNN_PIPELINE_BANK1_INTERRUPT 0x00000155
#define ROCKCHIP_RKNN_DMA_READ_ERROR 0x00001000
#define ROCKCHIP_RKNN_DMA_WRITE_ERROR 0x00002000
#define ROCKCHIP_RKNN_TASK_STATUS_SUCCESS 0x0000f000
#define ROCKCHIP_RKNN_TASK_STATUS_FETCH_ERROR 0x0000a000
#define ROCKCHIP_RKNN_COMPLETE_DELAY_NS (100 * 1000)
#define ROCKCHIP_RKNN_REGCMD_SUMMARY_BYTES 16
#define ROCKCHIP_RKNN_REGCMD_SAMPLE_COMMANDS_MAX 256
#define ROCKCHIP_RKNN_REGCMD_SAMPLE_BYTES_MAX \
    (ROCKCHIP_RKNN_REGCMD_SAMPLE_COMMANDS_MAX * sizeof(uint64_t))
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
#define ROCKCHIP_RKNN_DPU_DST_BASE_ADDR 0x020
#define ROCKCHIP_RKNN_DPU_DST_SURF_STRIDE 0x024
#define ROCKCHIP_RKNN_DPU_DATA_CUBE_WIDTH 0x030
#define ROCKCHIP_RKNN_DPU_DATA_CUBE_HEIGHT 0x034
#define ROCKCHIP_RKNN_DPU_DATA_CUBE_CHANNEL 0x03c
#define ROCKCHIP_RKNN_DPU_WDMA_SIZE_0 0x058
#define ROCKCHIP_RKNN_DPU_RDMA_DATA_CUBE_WIDTH 0x00c
#define ROCKCHIP_RKNN_DPU_RDMA_DATA_CUBE_HEIGHT 0x010
#define ROCKCHIP_RKNN_DPU_RDMA_DATA_CUBE_CHANNEL 0x014
#define ROCKCHIP_RKNN_DPU_RDMA_SRC_BASE_ADDR 0x018
#define ROCKCHIP_RKNN_DPU_RDMA_BS_BASE_ADDR 0x020
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
#define ROCKCHIP_RKNN_REGCMD_COMMANDS_MAX 4096
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
#define ROCKCHIP_RKNN_DPU_BS_CFG 0x040
#define ROCKCHIP_RKNN_DPU_BS_ALU_CFG 0x044
#define ROCKCHIP_RKNN_DPU_BS_MUL_CFG 0x048
#define ROCKCHIP_RKNN_DPU_DST_DMA_CFG 0x050
#define ROCKCHIP_RKNN_DPU_BN_CFG 0x060
#define ROCKCHIP_RKNN_DPU_BN_ALU_CFG 0x064
#define ROCKCHIP_RKNN_DPU_BN_MUL_CFG 0x068
#define ROCKCHIP_RKNN_DPU_EW_CFG 0x070
#define ROCKCHIP_RKNN_DPU_EW_CVT_OFFSET_VALUE 0x074
#define ROCKCHIP_RKNN_DPU_EW_CVT_SCALE_VALUE 0x078
#define ROCKCHIP_RKNN_DPU_OUT_CVT_OFFSET 0x080
#define ROCKCHIP_RKNN_DPU_OUT_CVT_SCALE 0x084
#define ROCKCHIP_RKNN_DPU_OUT_CVT_SHIFT 0x088
#define ROCKCHIP_RKNN_DPU_EW_OP_VALUE_0 0x090
#define ROCKCHIP_RKNN_DPU_SURFACE_ADD 0x0c0
#define ROCKCHIP_RKNN_POINTER_BANK BIT(0)
#define ROCKCHIP_RKNN_POINTER_PP_EN BIT(1)
#define ROCKCHIP_RKNN_EXECUTOR_PP_EN BIT(2)
#define ROCKCHIP_RKNN_POINTER_PP_MODE BIT(3)
#define ROCKCHIP_RKNN_POINTER_PP_CLEAR BIT(4)
#define ROCKCHIP_RKNN_EXECUTOR_PP_CLEAR BIT(5)
#define ROCKCHIP_RKNN_EXECUTOR_BANK BIT(16)
#define ROCKCHIP_RKNN_CORE_TAG_MASK (BIT(28) | BIT(29))
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

typedef struct RockchipRKNNCNAConfig {
    RockchipRKNNTensorView input;
    uint32_t weight_iova;
    uint32_t weight_bytes;
    uint32_t weight_bytes_per_kernel;
    uint32_t output_atomics;
    uint32_t cvt_con0;
    uint32_t fc_data_size0;
    uint32_t fc_data_size1;
    uint16_t input_channels_valid;
    uint16_t output_width;
    uint16_t weight_kernels;
    uint8_t kernel_groups;
    uint16_t feature_grains;
    uint8_t kernel_width;
    uint8_t kernel_height;
    uint8_t conv_mode;
    uint8_t input_precision;
    uint8_t process_precision;
    uint8_t stride_x;
    uint8_t stride_y;
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
    uint32_t bs_cfg;
    uint32_t dst_dma_cfg;
    uint32_t bn_cfg;
    uint32_t ew_cfg;
    int32_t out_cvt_offset;
    uint16_t out_cvt_scale;
    uint16_t out_cvt_shift;
    bool out_cvt_type;
    uint32_t surface_add;
    uint16_t output_channels_valid;
    uint16_t wdma_channels;
    uint8_t input_precision;
    uint8_t process_precision;
    uint8_t output_precision;
} RockchipRKNNDPUConfig;

typedef struct RockchipRKNNDpuRdmaConfig {
    uint32_t src_iova;
    uint32_t ew_iova;
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t erdma_cfg;
    uint32_t ew_surface_stride;
    uint32_t feature_mode;
    uint32_t src_dma_cfg;
    uint32_t surface_notch;
    uint32_t pad_cfg;
    uint32_t weight;
    uint32_t ew_surface_notch;
} RockchipRKNNDpuRdmaConfig;

struct RockchipRKNNPipelineTask {
    RockchipRKNNCNAConfig cna;
    RockchipRKNNCoreConfig core;
    RockchipRKNNDPUConfig dpu;
    RockchipRKNNDpuRdmaConfig dpu_rdma;
    uint32_t task_dma_base;
    uint32_t enabled_blocks;
};

static uint32_t rockchip_rknn_regcmd_shadow_read(const uint32_t shadow[],
                                                 uint32_t rel);

static bool rockchip_rknn_iommu_range_mapped(RockchipRKNNCoreState *s,
                                             uint32_t iova, size_t length,
                                             bool write)
{
    while (length) {
        unsigned int bank;
        const char *reason;
        hwaddr phys;
        size_t chunk;

        if (!rockchip_iommu_translate(s->iommu, iova, write, &phys, &bank,
                                     &reason)) {
            return false;
        }
        chunk = MIN(length, 0x1000 - (size_t)(phys & 0xfff));
        if (chunk < length && iova > UINT32_MAX - chunk) {
            return false;
        }
        iova += chunk;
        length -= chunk;
    }

    return true;
}

static bool rockchip_rknn_iommu_dma(RockchipRKNNCoreState *s, uint32_t iova,
                                    void *buffer, size_t length, bool write)
{
    uint8_t *bytes = buffer;

    if (!rockchip_rknn_iommu_range_mapped(s, iova, length, write)) {
        return false;
    }

    while (length) {
        unsigned int bank;
        const char *reason;
        hwaddr phys;
        size_t chunk;
        MemTxResult result;

        if (!rockchip_iommu_translate(s->iommu, iova, write, &phys, &bank,
                                     &reason)) {
            return false;
        }

        chunk = MIN(length, 0x1000 - (size_t)(phys & 0xfff));
        if (write) {
            result = dma_memory_write(&address_space_memory, phys, bytes,
                                      chunk, MEMTXATTRS_UNSPECIFIED);
        } else {
            result = dma_memory_read(&address_space_memory, phys, bytes,
                                     chunk, MEMTXATTRS_UNSPECIFIED);
        }
        if (result != MEMTX_OK) {
            return false;
        }

        if (chunk < length && iova > UINT32_MAX - chunk) {
            return false;
        }
        iova += chunk;
        bytes += chunk;
        length -= chunk;
    }

    return true;
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
    RockchipRKNNDomainRuntimeState *runtime, uint32_t value)
{
    runtime->pointer_value = value & ROCKCHIP_RKNN_POINTER_WRITABLE_MASK;
    runtime->pointer_bank = extract32(value, 0, 1);
    runtime->pointer_pingpong = extract32(value, 1, 1);
    runtime->executor_pingpong = extract32(value, 2, 1);
    runtime->pingpong_mode = extract32(value, 3, 1);

}

static bool rockchip_rknn_register_write(RockchipRKNNRegisterFile *file,
                                         uint32_t target, uint32_t reg,
                                         uint32_t value)
{
    RockchipRKNNRegcmdDomain domain;
    RockchipRKNNDomainState *state;
    RockchipRKNNRegisterBank *bank;
    uint32_t base;
    uint32_t rel;
    uint32_t index;

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

        rockchip_rknn_set_pointer_state(runtime, value);
        state->write_bank = runtime->pointer_bank;
        value = runtime->pointer_value;
    }
    bank = &state->bank[domain == ROCKCHIP_RKNN_DOMAIN_PC ? 0 :
                       state->write_bank];
    bank->regs[index] = value;
    bank->present[index / 32] |= BIT(index % 32);
    return true;
}

static void rockchip_rknn_register_file_init(
    RockchipRKNNRegisterFile *file,
    const RockchipRKNNDomainRuntimeState runtime[ROCKCHIP_RKNN_DOMAIN_COUNT])
{
    for (unsigned int i = 0; i < ROCKCHIP_RKNN_DOMAIN_COUNT; i++) {
        file->runtime[i] = runtime[i];
        file->domain[i].write_bank = i == ROCKCHIP_RKNN_DOMAIN_PC ? 0 :
                                     file->runtime[i].pointer_bank;
    }
}

static bool rockchip_rknn_fetch_register_file(RockchipRKNNCoreState *s,
                                              RockchipRKNNRegisterFile *file,
                                              uint32_t iova,
                                              uint32_t command_count)
{
    g_autofree uint64_t *commands = NULL;

    if (!s->iommu || command_count > ROCKCHIP_RKNN_REGCMD_COMMANDS_MAX) {
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
        if (!rockchip_rknn_register_write(file, target, reg, value)) {
            return false;
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

static bool rockchip_rknn_register_read(const RockchipRKNNRegisterFile *file,
                                        RockchipRKNNRegcmdDomain domain,
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

static bool rockchip_rknn_add_task_base(uint32_t base, uint32_t offset,
                                        uint32_t *iova)
{
    if (base > UINT32_MAX - offset) {
        return false;
    }
    *iova = base + offset;
    return true;
}

static bool rockchip_rknn_decode_pipeline(RockchipRKNNCoreState *s,
                                          RockchipRKNNPipelineTask *task,
                                          RockchipRKNNDPUStageSnapshot *stage,
                                          RockchipRKNNRegisterFile *file)
{
    uint32_t cna_conv1, cna_conv2, cna_conv3, data0, data1, data2, data3;
    uint32_t weight0, weight1, weight2, cna_cvt, cna_dma1, cna_dma2;
    uint32_t cna_fc0, cna_fc1, core_misc, core_size0, core_size1, core_clip;
    uint32_t dpu_feature, dpu_format, dpu_stride, dpu_width, dpu_height;
    uint32_t dpu_channel, dpu_bs, dpu_dma, dpu_wdma0, dpu_bn, dpu_ew;
    uint32_t dpu_ew_cvt_offset, dpu_ew_cvt_scale;
    uint32_t dpu_bs_alu, dpu_bs_mul, dpu_bn_alu, dpu_bn_mul;
    uint32_t dpu_cvt_offset, dpu_cvt_scale, dpu_cvt_shift, dpu_surface_add;
    uint32_t rdma_width = 0, rdma_height = 0, rdma_channels = 0;
    uint32_t rdma_src = 0, rdma_erdma = 0, rdma_ew = 0, rdma_ew_stride = 0;
    uint32_t rdma_feature = 0, rdma_src_dma = 0, rdma_notch = 0;
    uint32_t rdma_pad = 0, rdma_weight = 0, rdma_ew_notch = 0;
    uint32_t input_offset, weight_offset, output_offset;

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
                                     ROCKCHIP_RKNN_DPU_DATA_CUBE_CHANNEL,
                                     &dpu_channel) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BS_CFG, &dpu_bs) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BS_ALU_CFG,
                                     &dpu_bs_alu) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BS_MUL_CFG,
                                     &dpu_bs_mul) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_DST_DMA_CFG,
                                     &dpu_dma) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_WDMA_SIZE_0,
                                     &dpu_wdma0) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BN_CFG, &dpu_bn) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BN_ALU_CFG,
                                     &dpu_bn_alu) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_BN_MUL_CFG,
                                     &dpu_bn_mul) ||
        !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_DPU,
                                     ROCKCHIP_RKNN_DPU_EW_CFG, &dpu_ew) ||
        !rockchip_rknn_register_read(
            file, ROCKCHIP_RKNN_DOMAIN_DPU,
            ROCKCHIP_RKNN_DPU_EW_CVT_OFFSET_VALUE, &dpu_ew_cvt_offset) ||
        !rockchip_rknn_register_read(
            file, ROCKCHIP_RKNN_DOMAIN_DPU,
            ROCKCHIP_RKNN_DPU_EW_CVT_SCALE_VALUE, &dpu_ew_cvt_scale) ||
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
                ROCKCHIP_RKNN_DPU_RDMA_SRC_BASE_ADDR, &rdma_src) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_ERDMA_CFG, &rdma_erdma) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_EW_BASE_ADDR, &rdma_ew) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_EW_SURF_STRIDE, &rdma_ew_stride) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_FEATURE_MODE_CFG, &rdma_feature) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_SRC_DMA_CFG, &rdma_src_dma) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_SURF_NOTCH, &rdma_notch) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_PAD_CFG, &rdma_pad) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_WEIGHT, &rdma_weight) ||
            !rockchip_rknn_register_read(
                file, ROCKCHIP_RKNN_DOMAIN_DPU_RDMA,
                ROCKCHIP_RKNN_DPU_RDMA_EW_SURF_NOTCH, &rdma_ew_notch)) {
            return false;
        }
    }

    task->task_dma_base = s->pc_regs[R_PC_TASK_DMA_BASE_ADDR] &
                          ROCKCHIP_RKNN_PC_BASE_ADDRESS_MASK;
    task->enabled_blocks = file->enabled_blocks;
    task->cna.conv_mode = extract32(cna_conv1, 0, 2);
    task->cna.input_precision = extract32(cna_conv1, 4, 3);
    task->cna.process_precision = extract32(cna_conv1, 7, 3);
    task->cna.stride_x = extract32(cna_conv3, 0, 3);
    task->cna.stride_y = extract32(cna_conv3, 3, 3);
    task->cna.input.width = extract32(data0, 16, 11);
    task->cna.input.height = extract32(data0, 0, 11);
    task->cna.input.channels = extract32(data1, 0, 16);
    task->cna.input_channels_valid = extract32(data1, 16, 14) + 1;
    task->cna.output_width = extract32(data2, 0, 11);
    task->cna.output_atomics = extract32(data3, 0, 22);
    task->cna.input.precision = task->cna.input_precision;
    task->cna.input.atom = 16;
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
    task->dpu.output_precision = extract32(dpu_format, 29, 3);
    task->dpu.input_precision = extract32(dpu_format, 26, 3);
    task->dpu.process_precision = extract32(dpu_format, 0, 3);
    task->dpu.output.width = extract32(dpu_width, 0, 13) + 1;
    task->dpu.output.height = extract32(dpu_height, 0, 13) + 1;
    task->dpu.output.channels = extract32(dpu_channel, 0, 13) + 1;
    task->dpu.output_channels_valid = extract32(dpu_channel, 16, 13) + 1;
    task->dpu.output.precision = task->dpu.output_precision;
    task->dpu.output.atom = 4;
    task->dpu.output.surface_stride = extract32(dpu_stride, 4, 28);
    task->dpu.bs_cfg = dpu_bs;
    task->dpu.dst_dma_cfg = dpu_dma;
    task->dpu.wdma_channels = extract32(dpu_wdma0, 0, 13) + 1;
    task->dpu.bn_cfg = dpu_bn;
    task->dpu.ew_cfg = dpu_ew;
    task->dpu.out_cvt_offset = dpu_cvt_offset;
    task->dpu.out_cvt_scale = extract32(dpu_cvt_scale, 0, 16);
    task->dpu.out_cvt_shift = extract32(dpu_cvt_shift, 0, 12);
    task->dpu.out_cvt_type = extract32(dpu_cvt_shift, 31, 1);
    task->dpu.surface_add = dpu_surface_add;
    task->dpu_rdma.width = extract32(rdma_width, 0, 13) + 1;
    task->dpu_rdma.height = extract32(rdma_height, 0, 13) + 1;
    task->dpu_rdma.channels = extract32(rdma_channels, 0, 13) + 1;
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

    if (dpu_cvt_scale & ~0xffffU ||
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
             rockchip_rknn_add_task_base(task->task_dma_base, rdma_ew,
                                         &task->dpu_rdma.ew_iova)));
}

static bool rockchip_rknn_pipeline_shape_is_well_formed(
    const RockchipRKNNPipelineTask *task)
{
    uint32_t m = task->cna.input.height;
    uint32_t k_valid = task->cna.input_channels_valid;
    uint32_t k_storage = task->cna.input.channels;
    uint32_t n_weight = task->cna.weight_kernels;
    uint32_t n_valid = task->dpu.output_channels_valid;
    uint32_t n_cube = task->dpu.output.channels;

    return m && k_valid && k_valid <= k_storage && !(k_storage % 8) &&
           n_valid && n_valid <= n_weight && n_weight <= n_cube;
}

static bool rockchip_rknn_pipeline_is_covered_shape(
    const RockchipRKNNPipelineTask *task)
{
    uint32_t m = task->cna.input.height;
    uint32_t k = task->cna.input.channels;
    uint32_t n = task->dpu.output.channels;

    /* Partial-channel writeback uses an unmodeled compact DPU layout. */
    if (task->cna.input_channels_valid != k ||
        task->cna.weight_kernels != n ||
        task->dpu.output_channels_valid != n) {
        return false;
    }

    /* Independent board scans establish these current execution ranges. */
    return m <= 64 && k >= 32 && k <= 128 && !(k % 32) &&
           n >= 32 && n <= 96 && !(n % 32);
}

static bool rockchip_rknn_pipeline_is_captured_profile(
    RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const uint32_t blocks = ROCKCHIP_RKNN_BLOCK_CNA |
                            ROCKCHIP_RKNN_BLOCK_CORE |
                            ROCKCHIP_RKNN_BLOCK_DPU;
    const bool dpu_rdma =
        task->enabled_blocks & ROCKCHIP_RKNN_BLOCK_DPU_RDMA;
    uint32_t m = task->cna.input.height;
    uint32_t k_storage = task->cna.input.channels;
    uint32_t n_weight = task->cna.weight_kernels;
    uint32_t n_cube = task->dpu.output.channels;
    int32_t input_surface_stride;

    if (!rockchip_rknn_pipeline_shape_is_well_formed(task)) {
        return false;
    }

    /* This gates tensor DMA, not guest-visible hardware acceptance. */
    if (!rockchip_rknn_pipeline_is_covered_shape(task)) {
        return false;
    }
    input_surface_stride = (int32_t)m - 4;

    if ((task->enabled_blocks != blocks &&
         task->enabled_blocks != (blocks | ROCKCHIP_RKNN_BLOCK_DPU_RDMA)) ||
        task->cna.conv_mode != 0 ||
        task->cna.input_precision != 0 || task->cna.process_precision != 0 ||
        task->cna.stride_x != 1 || task->cna.stride_y != 1 ||
        task->cna.input.width != 1 ||
        task->cna.output_width != 1 ||
        task->cna.output_atomics != m ||
        task->cna.kernel_width != 1 || task->cna.kernel_height != 1 ||
        task->cna.kernel_groups != 1 ||
        task->cna.feature_grains != m + 1 ||
        task->cna.csc_weight_output_disable ||
        task->cna.csc_data_output_disable || task->cna.cmd_fifo_soft_reset ||
        task->cna.weight_bytes != n_weight * k_storage ||
        task->cna.weight_bytes_per_kernel != k_storage ||
        task->cna.cvt_con0 != 0xb || task->cna.input.line_stride != 4 ||
        task->cna.input.surface_stride !=
            ((uint32_t)input_surface_stride & 0x0fffffff) ||
        task->cna.fc_data_size0 != ((1 << 16) | m) ||
        task->cna.fc_data_size1 != k_storage ||
        task->core.width != 1 ||
        task->core.height != m || task->core.channels != n_cube ||
        task->core.process_precision != 0 || task->core.depthwise ||
        task->core.quantify ||
        task->dpu.output.width != 1 ||
        task->dpu.output.height != m ||
        task->dpu.wdma_channels != n_cube ||
        task->dpu.input_precision != 0 ||
        task->dpu.process_precision != 0 ||
        task->dpu.output_precision != 4 || task->dpu.feature_mode != 0x1e4 ||
        task->dpu.output.surface_stride != m ||
        (task->dpu.bs_cfg != 0x53 && task->dpu.bs_cfg != 0x20050 &&
         task->dpu.bs_cfg != 0x40050 && task->dpu.bs_cfg != 0x42 &&
         task->dpu.bs_cfg != 0x12) ||
        (task->dpu.bs_cfg == 0x42 &&
         (stage->bs_mul_cfg & ~(0xffff0000U | 0x3f00U))) ||
        task->dpu.dst_dma_cfg != 0x7fe ||
        (task->dpu.bn_cfg != 0x53 && task->dpu.bn_cfg != 0x20050 &&
         task->dpu.bn_cfg != 0x42) ||
        (task->dpu.bn_cfg == 0x42 &&
         (stage->bn_mul_cfg & ~(0xffff0000U | 0x3f00U))) ||
        (task->dpu.ew_cfg != 0x383 && task->dpu.ew_cfg != 0x20380 &&
         task->dpu.ew_cfg != 0x384 && task->dpu.ew_cfg != 0x104203c0 &&
         task->dpu.ew_cfg != 0x104003c4 &&
         task->dpu.ew_cfg != 0x104202c0 &&
         task->dpu.ew_cfg != 0x504202c0) ||
        ((task->dpu.ew_cfg & BIT(8)) &&
         extract32(stage->ew_cvt_scale, 0, 22) != 1) ||
        (dpu_rdma &&
         (task->dpu.ew_cfg != 0x104203c0 &&
          task->dpu.ew_cfg != 0x104003c4 &&
          task->dpu.ew_cfg != 0x104202c0 &&
          task->dpu.ew_cfg != 0x504202c0)) ||
        (dpu_rdma &&
         (task->dpu_rdma.width != 1 || task->dpu_rdma.height != m ||
          task->dpu_rdma.channels != n_cube ||
          task->dpu_rdma.erdma_cfg != 0x40000004 ||
          task->dpu_rdma.ew_surface_stride != m ||
          task->dpu_rdma.feature_mode != 0x7d00 ||
          task->dpu_rdma.src_dma_cfg ||
          task->dpu_rdma.surface_notch != m || task->dpu_rdma.pad_cfg ||
          task->dpu_rdma.weight != 0x01010101 ||
          task->dpu_rdma.ew_surface_notch != m)) ||
        (!dpu_rdma && (task->dpu.ew_cfg == 0x104203c0 ||
                       task->dpu.ew_cfg == 0x104003c4 ||
                       task->dpu.ew_cfg == 0x104202c0 ||
                       task->dpu.ew_cfg == 0x504202c0)) ||
        (!(task->dpu.data_format & BIT(3)) &&
         task->dpu.surface_add != (m * 8) << 4)) {
        return false;
    }

    return true;
}

static size_t rockchip_rknn_feature_index(unsigned int width,
                                          unsigned int height,
                                          unsigned int atom,
                                          unsigned int channel,
                                          unsigned int row,
                                          unsigned int column)
{
    return (channel / atom) * height * width * atom +
           atom * (row * width + column) + channel % atom;
}

static size_t rockchip_rknn_weight_index(unsigned int channels,
                                         unsigned int output,
                                         unsigned int channel)
{
    return (channel / 32 * 32) * 32 +
           (output / 32) * 32 * channels + channel % 32 +
           (output % 32) * 32;
}

static Int128 rockchip_rknn_mul_s32(Int128 value, int32_t factor)
{
    uint32_t magnitude = factor < 0 ? -(int64_t)factor : factor;
    Int128 result = int128_zero();

    while (magnitude) {
        if (magnitude & 1) {
            result = int128_add(result, value);
        }
        magnitude >>= 1;
        if (magnitude) {
            value = int128_add(value, value);
        }
    }

    return factor < 0 ? int128_neg(result) : result;
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
    /* The covered K range and factor widths keep magnitude below 2^110. */
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
    const RockchipRKNNDPUStageSnapshot *stage, int8_t operand)
{
    unsigned int shift = extract32(stage->ew_cvt_scale, 16, 6);
    Int128 value = rockchip_rknn_mul_s32(
        int128_makes64(operand), extract32(stage->ew_cvt_scale, 0, 16));

    value = int128_add(
        value, int128_lshift(int128_makes64(stage->ew_cvt_offset), shift));
    return rockchip_rknn_saturate_i32(rockchip_rknn_round_shift(
        value, shift, extract32(dpu->ew_cfg, 30, 1)));
}

static bool rockchip_rknn_fetch_pipeline_task(
    RockchipRKNNCoreState *s, uint32_t index, uint32_t iova,
    uint32_t command_count,
    const RockchipRKNNDomainRuntimeState runtime[
        ROCKCHIP_RKNN_REGCMD_DOMAIN_COUNT])
{
    g_autofree RockchipRKNNRegisterFile *file =
        g_new0(RockchipRKNNRegisterFile, 1);
    RockchipRKNNPipelineTask task = {};
    RockchipRKNNDPUStageSnapshot stage = {};
    uint32_t next_iova;
    uint32_t next_amount;

    trace_rockchip_rknn_task_fetch(s->core_index, index, iova,
                                   command_count);
    rockchip_rknn_register_file_init(file, runtime);
    if (!rockchip_rknn_fetch_register_file(s, file, iova, command_count)) {
        trace_rockchip_rknn_task_fetch_error(s->core_index, index,
                                              "regcmd-fetch");
        return false;
    }
    memcpy(s->pending_domain_runtime[index], file->runtime,
           sizeof(file->runtime));
    s->pending_domain_runtime_valid[index] = true;
    s->pending_pipeline_valid[index] = false;
    if (rockchip_rknn_decode_pipeline(s, &task, &stage, file) &&
        rockchip_rknn_pipeline_is_captured_profile(&task, &stage)) {
        s->pending_pipeline[index] = task;
        s->pending_dpu_stage[index] = stage;
        s->pending_pipeline_valid[index] = true;
    }

    if (index + 1 < s->pending_task_count) {
        if (!rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PC,
                                         A_PC_BASE_ADDRESS, &next_iova) ||
            !rockchip_rknn_register_read(file, ROCKCHIP_RKNN_DOMAIN_PC,
                                         A_PC_REGISTER_AMOUNTS,
                                         &next_amount)) {
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
    memset(s->pending_pipeline_valid, 0, sizeof(s->pending_pipeline_valid));
    memset(s->pending_dpu_stage, 0, sizeof(s->pending_dpu_stage));
    memset(s->pending_domain_runtime_valid, 0,
           sizeof(s->pending_domain_runtime_valid));
    if (!task_count || task_count > ROCKCHIP_RKNN_TASKS_MAX) {
        return;
    }

    s->pending_task_count = task_count;
    if (!rockchip_rknn_fetch_pipeline_task(s, 0, iova, command_count,
                                            s->domain_runtime)) {
        s->pending_task_count = 0;
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
    memset(s->pending_pipeline_valid, 0, sizeof(s->pending_pipeline_valid));
    memset(s->pending_dpu_stage, 0, sizeof(s->pending_dpu_stage));
    memset(s->pending_domain_runtime_valid, 0,
           sizeof(s->pending_domain_runtime_valid));
    s->slave_file.enabled_blocks = enabled_blocks & 0x7f;
    memcpy(s->pending_domain_runtime[0], s->slave_file.runtime,
           sizeof(s->slave_file.runtime));
    s->pending_domain_runtime_valid[0] = true;
    if (rockchip_rknn_decode_pipeline(s, &task, &stage, &s->slave_file) &&
        rockchip_rknn_pipeline_is_captured_profile(&task, &stage)) {
        s->pending_pipeline[0] = task;
        s->pending_dpu_stage[0] = stage;
        s->pending_pipeline_valid[0] = true;
    }
}

static uint32_t rockchip_rknn_execute_pipeline(
    RockchipRKNNCoreState *s, const RockchipRKNNPipelineTask *task,
    const RockchipRKNNDPUStageSnapshot *stage)
{
    const bool mc_surf_out = task->dpu.data_format & BIT(3);
    g_autofree int8_t *input = NULL;
    g_autofree int8_t *weights = NULL;
    g_autofree int8_t *ew_src = NULL;
    g_autofree int8_t *ew_data = NULL;
    g_autofree uint32_t *output = NULL;
    size_t input_bytes;
    size_t weight_bytes;
    size_t output_values;
    size_t output_bytes;
    bool output_write_ok;
    uint32_t weight_channels;

    input_bytes = task->cna.input.width * task->cna.input.height *
                  task->cna.input.channels;
    weight_channels = ROUND_UP(task->cna.input.channels, 32);
    weight_bytes = ROUND_UP(task->cna.weight_kernels, 32) * weight_channels;
    output_values = task->core.width * task->core.height *
                    task->dpu.output.channels;
    input = g_new(int8_t, input_bytes);
    weights = g_new(int8_t, weight_bytes);
    output = g_new0(uint32_t, output_values);
    output_bytes = output_values;
    output_bytes *= sizeof(*output);
    if (!rockchip_rknn_iommu_dma(s, task->cna.input.iova, input,
                                 input_bytes, false) ||
        !rockchip_rknn_iommu_dma(s, task->cna.weight_iova, weights,
                                 weight_bytes, false)) {
        return ROCKCHIP_RKNN_DMA_READ_ERROR;
    }
    if (task->enabled_blocks & ROCKCHIP_RKNN_BLOCK_DPU_RDMA) {
        size_t channel_groups = task->dpu.output.channels / 32;
        size_t group_bytes = task->core.height * 16;
        size_t operand_bytes = channel_groups * group_bytes;

        ew_src = g_new(int8_t, operand_bytes);
        ew_data = g_new(int8_t, operand_bytes);
        for (unsigned int group = 0; group < channel_groups; group++) {
            size_t surface_offset = group * task->core.height * 32;
            size_t buffer_offset = group * group_bytes;
            uint64_t src_iova = task->dpu_rdma.src_iova + surface_offset;
            uint64_t ew_iova = task->dpu_rdma.ew_iova + surface_offset;

            if (src_iova > UINT32_MAX || ew_iova > UINT32_MAX ||
                !rockchip_rknn_iommu_dma(
                    s, (uint32_t)src_iova, ew_src + buffer_offset,
                    group_bytes, false) ||
                !rockchip_rknn_iommu_dma(
                    s, (uint32_t)ew_iova, ew_data + buffer_offset,
                    group_bytes, false)) {
                return ROCKCHIP_RKNN_DMA_READ_ERROR;
            }
        }
    }

    for (unsigned int row = 0; row < task->core.height; row++) {
        for (unsigned int column = 0; column < task->core.width; column++) {
            for (unsigned int out = 0;
                 out < task->dpu.output_channels_valid; out++) {
                Int128 value = int128_zero();
                unsigned int ew_shift;

                for (unsigned int channel = 0;
                     channel < task->cna.input_channels_valid; channel++) {
                    size_t input_index = rockchip_rknn_feature_index(
                        task->cna.input.width, task->cna.input.height,
                        task->cna.input.atom, channel, row, column);
                    size_t weight_index = rockchip_rknn_weight_index(
                        weight_channels, out, channel);

                    value = int128_add(
                        value, int128_makes64(input[input_index] *
                                             weights[weight_index]));
                }
                if (task->core.clip_truncate) {
                    value = rockchip_rknn_round_shift(
                        value, task->core.clip_truncate, false);
                }
                switch (task->dpu.bs_cfg) {
                case 0x20050:
                    value = int128_add(
                        value, int128_makes64(stage->bs_alu_operand));
                    break;
                case 0x40050:
                    value = int128_sub(
                        value, int128_makes64(stage->bs_alu_operand));
                    break;
                case 0x42:
                    value = rockchip_rknn_dpu_mul(
                        value, (int16_t)(stage->bs_mul_cfg >> 16),
                        extract32(stage->bs_mul_cfg, 8, 6),
                        extract32(task->dpu.data_format, 4, 6));
                    break;
                case 0x12:
                    if (!int128_nonneg(value)) {
                        value = int128_zero();
                    }
                    break;
                }
                value = rockchip_rknn_saturate_i32(value);
                switch (task->dpu.bn_cfg) {
                case 0x20050:
                    value = int128_add(
                        value, int128_makes64(stage->bn_alu_operand));
                    break;
                case 0x42:
                    value = rockchip_rknn_dpu_mul(
                        value, (int16_t)(stage->bn_mul_cfg >> 16),
                        extract32(stage->bn_mul_cfg, 8, 6),
                        extract32(task->dpu.data_format, 10, 6));
                    break;
                }
                value = rockchip_rknn_saturate_i32(value);
                if (task->enabled_blocks & ROCKCHIP_RKNN_BLOCK_DPU_RDMA) {
                    unsigned int channel_group = out / 32;
                    unsigned int channel_in_group = out % 32;
                    const int8_t *operand =
                        channel_in_group < 16 ? ew_src : ew_data;
                    size_t operand_index =
                        channel_group * task->core.height * 16 + row * 16 +
                        channel_in_group % 16;
                    Int128 ew_operand =
                        int128_makes64(operand[operand_index]);

                    if (!(task->dpu.ew_cfg & BIT(8))) {
                        ew_operand = rockchip_rknn_ew_operand_cvt(
                            &task->dpu, stage, operand[operand_index]);
                    }
                    if (task->dpu.ew_cfg & BIT(2)) {
                        value = rockchip_rknn_mul_s32(
                            value, (int32_t)int128_getlo(ew_operand));
                    } else {
                        value = int128_add(value, ew_operand);
                    }
                } else if (task->dpu.ew_cfg == 0x20380) {
                    value = int128_add(
                        value, int128_makes64(stage->ew_operand[
                            out % ARRAY_SIZE(stage->ew_operand)]));
                } else if (task->dpu.ew_cfg == 0x384) {
                    value = rockchip_rknn_mul_s32(
                        value, (int16_t)stage->ew_operand[
                            out % ARRAY_SIZE(stage->ew_operand)]);
                }
                ew_shift = 0;
                if (task->dpu.ew_cfg != 0x383) {
                    if (int128_nonneg(value)) {
                        ew_shift = extract32(stage->ew_cvt_scale, 22, 10);
                    } else {
                        ew_shift = extract32(task->dpu.data_format, 16, 10);
                    }
                }
                value = rockchip_rknn_round_shift(value, ew_shift, false);
                value = rockchip_rknn_saturate_i32(value);
                value = rockchip_rknn_out_cvt(&task->dpu, stage, value);
                output[rockchip_rknn_feature_index(
                    task->dpu.output.width, task->dpu.output.height,
                    task->dpu.output.atom, out, row, column)] =
                    cpu_to_le32(int128_getlo(value));
            }
        }
    }

    if (mc_surf_out) {
        const unsigned int channels = task->dpu.output.channels;
        const unsigned int rows = task->core.height;
        const unsigned int channel_blocks = channels / 32;
        const uint64_t surface_words =
            extract32(task->dpu.surface_add, 4, 28) * 4;
        uint32_t block_data[32];
        unsigned int blocks = rows * channel_blocks;

        for (unsigned int block = 0; block < MIN(blocks, 8); block++) {
            unsigned int row = block % rows;
            unsigned int channel_base = block / rows * 32;
            uint64_t block_offset = block / 2 * surface_words +
                                    block % 2 * 32;
            uint64_t block_iova = task->dpu.output.iova +
                                  block_offset * sizeof(uint32_t);

            if (block_iova > UINT32_MAX) {
                return ROCKCHIP_RKNN_DMA_WRITE_ERROR;
            }

            for (unsigned int channel = 0; channel < 32; channel++) {
                size_t index = rockchip_rknn_feature_index(
                    task->dpu.output.width, task->dpu.output.height,
                    task->dpu.output.atom, channel_base + channel, row, 0);

                block_data[channel] = output[index];
            }
            if (!rockchip_rknn_iommu_dma(
                    s, block_iova,
                    block_data, sizeof(block_data), true)) {
                return ROCKCHIP_RKNN_DMA_WRITE_ERROR;
            }
        }
    } else {
        output_write_ok = rockchip_rknn_iommu_dma(
            s, task->dpu.output.iova, output, output_bytes, true);
        if (!output_write_ok) {
            return ROCKCHIP_RKNN_DMA_WRITE_ERROR;
        }
    }

    return 0;
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

static void rockchip_rknn_commit_domain_runtime(
    RockchipRKNNDomainRuntimeState *state)
{
    if (state->executor_pingpong) {
        state->executor_bank ^= 1;
    }
    state->pointer_value = rockchip_rknn_encode_pointer_state(state);
}

static void rockchip_rknn_commit_runtime(RockchipRKNNCoreState *s,
                                         unsigned int task_index)
{
    memcpy(s->domain_runtime, s->pending_domain_runtime[task_index],
           sizeof(s->domain_runtime));

    for (unsigned int i = 0; i < ROCKCHIP_RKNN_DOMAIN_COUNT; i++) {
        if (i != ROCKCHIP_RKNN_DOMAIN_PC) {
            rockchip_rknn_commit_domain_runtime(&s->domain_runtime[i]);
        }
    }

    s->cna_regs[R_CNA_S_POINTER] = rockchip_rknn_encode_pointer_state(
        &s->domain_runtime[ROCKCHIP_RKNN_DOMAIN_CNA]);
    s->core_regs[R_CORE_S_POINTER] = rockchip_rknn_encode_pointer_state(
        &s->domain_runtime[ROCKCHIP_RKNN_DOMAIN_CORE]);
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
        s->pc_regs[R_PC_INTERRUPT_MASK];
    s->irq_level = s->pc_regs[R_PC_INTERRUPT_STATUS] != 0;
    if (s->irq_level != old_level) {
        trace_rockchip_rknn_irq(s->core_index, s->irq_level,
                                s->pc_regs[R_PC_INTERRUPT_RAW_STATUS],
                                s->pc_regs[R_PC_INTERRUPT_MASK],
                                s->pc_regs[R_PC_INTERRUPT_STATUS]);
    }
    qemu_set_irq(s->irq, s->irq_level);
}

static void rockchip_rknn_complete(void *opaque)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(opaque);
    bool slave_submission = s->pending_slave;
    bool fetch_error = false;
    bool final_pipeline_attempted = false;
    uint32_t dma_error_bits = 0;

    if (s->functional) {
        while (s->pending_task_index < s->pending_task_count) {
            unsigned int task_index = s->pending_task_index;

            final_pipeline_attempted =
                s->pending_pipeline_valid[task_index];
            if (s->pending_pipeline_valid[task_index]) {
                dma_error_bits |= rockchip_rknn_execute_pipeline(
                    s, &s->pending_pipeline[task_index],
                    &s->pending_dpu_stage[task_index]);
            }
            if (s->pending_domain_runtime_valid[task_index]) {
                rockchip_rknn_commit_runtime(s, task_index);
            }
            s->pending_task_index++;
            if (s->pending_task_index < s->pending_task_count &&
                !rockchip_rknn_fetch_pipeline_task(
                    s, s->pending_task_index, s->pending_next_iova,
                    s->pending_next_command_count, s->domain_runtime)) {
                fetch_error = true;
                break;
            }
        }
        if (fetch_error) {
            s->pc_regs[R_PC_TASK_STATUS] =
                ROCKCHIP_RKNN_TASK_STATUS_FETCH_ERROR |
                s->pending_task_index;
        } else if (slave_submission) {
            s->pc_regs[R_PC_TASK_STATUS] = 0x00005000;
        } else if (s->pending_task_count) {
            s->pc_regs[R_PC_TASK_STATUS] =
                ROCKCHIP_RKNN_TASK_STATUS_SUCCESS;
        } else {
            s->pc_regs[R_PC_TASK_STATUS] = 0;
        }
    } else {
        s->pc_regs[R_PC_TASK_STATUS] = slave_submission ? 0x00005000 :
            s->pc_regs[R_PC_TASK_CON] & ROCKCHIP_RKNN_TASK_NUMBER_MASK;
    }
    if (slave_submission) {
        memcpy(s->slave_file.runtime, s->domain_runtime,
               sizeof(s->slave_file.runtime));
        for (unsigned int i = 0; i < ROCKCHIP_RKNN_DOMAIN_COUNT; i++) {
            s->slave_file.domain[i].write_bank =
                s->slave_file.runtime[i].pointer_bank;
        }
    }
    s->pending_task_count = 0;
    s->pending_task_index = 0;
    s->pending_next_iova = 0;
    s->pending_next_command_count = 0;
    memset(s->pending_pipeline_valid, 0, sizeof(s->pending_pipeline_valid));
    memset(s->pending_dpu_stage, 0, sizeof(s->pending_dpu_stage));
    memset(s->pending_domain_runtime_valid, 0,
           sizeof(s->pending_domain_runtime_valid));
    s->pending_slave = false;
    s->busy = false;
    s->pc_regs[R_PC_OPERATION_ENABLE] &= ~R_PC_OPERATION_ENABLE_OP_EN_MASK;
    if (!fetch_error) {
        uint32_t interrupt_bits = ROCKCHIP_RKNN_DPU_INTERRUPT_BITS;

        if (final_pipeline_attempted) {
            interrupt_bits =
                s->domain_runtime[ROCKCHIP_RKNN_DOMAIN_DPU].executor_bank ?
                ROCKCHIP_RKNN_PIPELINE_BANK1_INTERRUPT :
                ROCKCHIP_RKNN_PIPELINE_BANK0_INTERRUPT;
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
    unsigned int bank = 0;
    const char *reason = NULL;
    hwaddr phys = 0;
    uint8_t sample[ROCKCHIP_RKNN_REGCMD_SAMPLE_BYTES_MAX] = { 0 };

    if (!s->iommu) {
        trace_rockchip_rknn_regcmd_sample_error(s->core_index, iova,
                                                "no-iommu-link");
        return;
    }

    if (!rockchip_iommu_iova_to_phys(s->iommu, iova, &phys, &bank, &reason)) {
        trace_rockchip_rknn_regcmd_sample_error(s->core_index, iova, reason);
        return;
    }

    if (!trace_sample && !trace_words && !trace_ingest) {
        return;
    }

    sample_bytes = MIN(command_bytes, trace_words || trace_ingest ?
                       ROCKCHIP_RKNN_REGCMD_SAMPLE_BYTES_MAX :
                       ROCKCHIP_RKNN_REGCMD_SUMMARY_BYTES);
    sample_bytes = MIN(sample_bytes, 0x1000 - (uint32_t)(phys & 0xfff));
    sample_commands = trace_words || trace_ingest ?
                      sample_bytes / sizeof(uint64_t) : 0;

    if (dma_memory_read(&address_space_memory, phys, sample, sample_bytes,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        trace_rockchip_rknn_regcmd_sample_error(s->core_index, iova,
                                                "sample-read-failed");
        return;
    }

    if (trace_sample) {
        uint32_t summary_bytes = MIN(sample_bytes,
                                     ROCKCHIP_RKNN_REGCMD_SUMMARY_BYTES);

        trace_rockchip_rknn_regcmd_sample(s->core_index, bank, iova, phys,
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
    if (s->functional) {
        rockchip_rknn_prepare_pipeline(s);
    }
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
    if (s->functional) {
        rockchip_rknn_prepare_slave_pipeline(s, enabled_blocks);
    }
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

    rockchip_rknn_set_pointer_state(&s->domain_runtime[domain], val);
    encoded = rockchip_rknn_encode_pointer_state(&s->domain_runtime[domain]);

    switch (domain) {
    case ROCKCHIP_RKNN_DOMAIN_CNA:
        s->cna_regs[R_CNA_S_POINTER] = encoded;
        break;
    case ROCKCHIP_RKNN_DOMAIN_CORE:
        s->core_regs[R_CORE_S_POINTER] = encoded;
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

    s->pc_regs[R_PC_INTERRUPT_RAW_STATUS] &= ~((uint32_t)val);
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
        .post_write = rockchip_rknn_interrupt_mask_postw,
    }, { .name = "PC_INTERRUPT_CLEAR", .addr = A_PC_INTERRUPT_CLEAR,
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
        .post_write = rockchip_rknn_cna_pointer_postw,
    },
};

static const RegisterAccessInfo rockchip_rknn_core_regs_info[] = {
    {   .name = "CORE_S_POINTER", .addr = A_CORE_S_POINTER,
        .post_write = rockchip_rknn_core_pointer_postw,
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

    rockchip_rknn_register_write(&s->slave_file, targets[domain],
                                 bases[domain] + rel, value);
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

    timer_del(&s->complete_timer);
    s->pending_task_count = 0;
    s->pending_task_index = 0;
    s->pending_next_iova = 0;
    s->pending_next_command_count = 0;
    memset(s->pending_pipeline_valid, 0, sizeof(s->pending_pipeline_valid));
    memset(s->pending_dpu_stage, 0, sizeof(s->pending_dpu_stage));
    memset(s->pending_domain_runtime_valid, 0,
           sizeof(s->pending_domain_runtime_valid));
    s->pending_slave = false;
    s->busy = false;
    rockchip_rknn_clear_regcmd_shadow(s);
    memset(s->domain_runtime, 0, sizeof(s->domain_runtime));
    memset(s->pending_domain_runtime, 0, sizeof(s->pending_domain_runtime));
    memset(&s->slave_file, 0, sizeof(s->slave_file));

    for (unsigned int i = 0; i < ARRAY_SIZE(s->pc_regs_info); i++) {
        register_reset(&s->pc_regs_info[i]);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(s->cna_regs_info); i++) {
        register_reset(&s->cna_regs_info[i]);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(s->core_regs_info); i++) {
        register_reset(&s->core_regs_info[i]);
    }

    rockchip_rknn_update_irq(s);
}

static int rockchip_rknn_post_load(void *opaque, int version_id)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(opaque);

    (void)version_id;
    rockchip_rknn_update_irq(s);
    return 0;
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

    memory_region_init_io(&s->dpu_reg_array, obj, &rockchip_rknn_dpu_ops, s,
                          TYPE_ROCKCHIP_RKNN_CORE ".dpu",
                          ROCKCHIP_RKNN_WINDOW_SIZE);
    memory_region_init_io(&s->global_reg_array, obj, &rockchip_rknn_global_ops,
                          s, TYPE_ROCKCHIP_RKNN_CORE ".global",
                          ROCKCHIP_RKNN_WINDOW_SIZE);

    timer_init_ns(&s->complete_timer, QEMU_CLOCK_VIRTUAL,
                  rockchip_rknn_complete, s);
    s->pending_pipeline = g_new0(RockchipRKNNPipelineTask,
                                 ROCKCHIP_RKNN_TASKS_MAX);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->pc_reg_array->mem);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->cna_reg_array->mem);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->core_reg_array->mem);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->dpu_reg_array);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->global_reg_array);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void rockchip_rknn_finalize(Object *obj)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(obj);

    g_clear_pointer(&s->pending_pipeline, g_free);
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
        VMSTATE_UINT32(fc_data_size0, RockchipRKNNCNAConfig),
        VMSTATE_UINT32(fc_data_size1, RockchipRKNNCNAConfig),
        VMSTATE_UINT16(input_channels_valid, RockchipRKNNCNAConfig),
        VMSTATE_UINT16(output_width, RockchipRKNNCNAConfig),
        VMSTATE_UINT16(weight_kernels, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(kernel_groups, RockchipRKNNCNAConfig),
        VMSTATE_UINT16(feature_grains, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(kernel_width, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(kernel_height, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(conv_mode, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(input_precision, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(process_precision, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(stride_x, RockchipRKNNCNAConfig),
        VMSTATE_UINT8(stride_y, RockchipRKNNCNAConfig),
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
        VMSTATE_UINT32(bs_cfg, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(dst_dma_cfg, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(bn_cfg, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(ew_cfg, RockchipRKNNDPUConfig),
        VMSTATE_INT32(out_cvt_offset, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(out_cvt_scale, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(out_cvt_shift, RockchipRKNNDPUConfig),
        VMSTATE_BOOL(out_cvt_type, RockchipRKNNDPUConfig),
        VMSTATE_UINT32(surface_add, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(output_channels_valid, RockchipRKNNDPUConfig),
        VMSTATE_UINT16(wdma_channels, RockchipRKNNDPUConfig),
        VMSTATE_UINT8(input_precision, RockchipRKNNDPUConfig),
        VMSTATE_UINT8(process_precision, RockchipRKNNDPUConfig),
        VMSTATE_UINT8(output_precision, RockchipRKNNDPUConfig),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rockchip_rknn_dpu_rdma = {
    .name = "rockchip-rknn-dpu-rdma",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(src_iova, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(ew_iova, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(width, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(height, RockchipRKNNDpuRdmaConfig),
        VMSTATE_UINT32(channels, RockchipRKNNDpuRdmaConfig),
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
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rockchip_rknn_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(pc_regs, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_PC_R_MAX),
        VMSTATE_UINT32_ARRAY(cna_regs, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_CNA_R_MAX),
        VMSTATE_UINT32_ARRAY(core_regs, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_CORE_R_MAX),
        VMSTATE_TIMER(complete_timer, RockchipRKNNCoreState),
        VMSTATE_UINT32(core_index, RockchipRKNNCoreState),
        VMSTATE_BOOL(busy, RockchipRKNNCoreState),
        VMSTATE_BOOL(irq_level, RockchipRKNNCoreState),
        VMSTATE_STRUCT_ARRAY(domain_runtime, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_REGCMD_DOMAIN_COUNT, 0,
                             vmstate_rockchip_rknn_domain_runtime,
                             RockchipRKNNDomainRuntimeState),
        VMSTATE_STRUCT_VARRAY_POINTER_KNOWN(
            pending_pipeline, RockchipRKNNCoreState,
            ROCKCHIP_RKNN_TASKS_MAX, 0,
            vmstate_rockchip_rknn_pipeline, RockchipRKNNPipelineTask),
        VMSTATE_STRUCT_2DARRAY(pending_domain_runtime, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_TASKS_MAX,
                             ROCKCHIP_RKNN_REGCMD_DOMAIN_COUNT, 0,
                             vmstate_rockchip_rknn_domain_runtime,
                             RockchipRKNNDomainRuntimeState),
        VMSTATE_BOOL_ARRAY(pending_pipeline_valid, RockchipRKNNCoreState,
                           ROCKCHIP_RKNN_TASKS_MAX),
        VMSTATE_BOOL_ARRAY(pending_domain_runtime_valid,
                           RockchipRKNNCoreState, ROCKCHIP_RKNN_TASKS_MAX),
        VMSTATE_UINT32(pending_next_iova, RockchipRKNNCoreState),
        VMSTATE_UINT32(pending_next_command_count, RockchipRKNNCoreState),
        VMSTATE_UINT16(pending_task_count, RockchipRKNNCoreState),
        VMSTATE_UINT16(pending_task_index, RockchipRKNNCoreState),
        VMSTATE_STRUCT(slave_file, RockchipRKNNCoreState, 0,
                       vmstate_rockchip_rknn_register_file,
                       RockchipRKNNRegisterFile),
        VMSTATE_BOOL(pending_slave, RockchipRKNNCoreState),
        VMSTATE_STRUCT_ARRAY(pending_dpu_stage, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_TASKS_MAX, 0,
                             vmstate_rockchip_rknn_dpu_stage,
                             RockchipRKNNDPUStageSnapshot),
        VMSTATE_END_OF_LIST()
    },
};

static const Property rockchip_rknn_properties[] = {
    DEFINE_PROP_LINK("iommu", RockchipRKNNCoreState, iommu,
                     TYPE_ROCKCHIP_IOMMU, RockchipIOMMUState *),
    DEFINE_PROP_UINT32("core-index", RockchipRKNNCoreState, core_index, 0),
    DEFINE_PROP_BOOL("functional", RockchipRKNNCoreState, functional, false),
};

static void rockchip_rknn_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, rockchip_rknn_reset);
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
