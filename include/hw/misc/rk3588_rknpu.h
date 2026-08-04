/*
 * Rockchip RK3588 RKNPU core
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RK3588_RKNPU_H
#define HW_MISC_RK3588_RKNPU_H

#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qemu/aiocb.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/memory.h"

#define TYPE_ROCKCHIP_RKNN_CORE "rockchip.rk3588-rknn-core"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipRKNNCoreState, ROCKCHIP_RKNN_CORE)

typedef struct RockchipRKNNPipelineTask RockchipRKNNPipelineTask;

#define ROCKCHIP_RKNN_WINDOW_SIZE 0x1000
#define ROCKCHIP_RKNN_DPU_OFFSET 0x4000
#define ROCKCHIP_RKNN_PPU_OFFSET 0x6000
#define ROCKCHIP_RKNN_PPU_RDMA_OFFSET 0x7000
#define ROCKCHIP_RKNN_GLOBAL_OFFSET 0xf000
#define ROCKCHIP_RKNN_PC_R_MAX (0x40 / 4)
#define ROCKCHIP_RKNN_CNA_R_MAX (0x8 / 4)
#define ROCKCHIP_RKNN_CORE_R_MAX (0x8 / 4)
#define ROCKCHIP_RKNN_PPU_R_MAX (0x8 / 4)
#define ROCKCHIP_RKNN_PPU_RDMA_R_MAX (0x8 / 4)
#define ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX (0x1000 / 4)
#define ROCKCHIP_RKNN_REGCMD_DOMAIN_COUNT 7
#define ROCKCHIP_RKNN_PRESENT_R_MAX 32
#define ROCKCHIP_RKNN_CORE_COUNT 3
#define ROCKCHIP_RKNN_PENDING_WRITE_R_MAX \
    (ROCKCHIP_RKNN_REGCMD_DOMAIN_COUNT * 2 * \
     ROCKCHIP_RKNN_PRESENT_R_MAX)

typedef struct RockchipRKNNDomainRuntimeState {
    uint32_t pointer_value;
    uint8_t pointer_bank;
    uint8_t executor_bank;
    bool pointer_pingpong;
    bool executor_pingpong;
    bool pingpong_mode;
} RockchipRKNNDomainRuntimeState;

typedef struct RockchipRKNNRegisterBank {
    uint32_t regs[ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX];
    uint32_t present[ROCKCHIP_RKNN_PRESENT_R_MAX];
} RockchipRKNNRegisterBank;

typedef struct RockchipRKNNDomainState {
    RockchipRKNNRegisterBank bank[2];
    uint8_t write_bank;
} RockchipRKNNDomainState;

typedef struct RockchipRKNNRegisterFile {
    RockchipRKNNDomainState domain[ROCKCHIP_RKNN_REGCMD_DOMAIN_COUNT];
    RockchipRKNNDomainRuntimeState runtime[
        ROCKCHIP_RKNN_REGCMD_DOMAIN_COUNT];
    uint32_t enabled_blocks;
    bool pre_enable;
    bool block_enable;
} RockchipRKNNRegisterFile;

typedef struct RockchipRKNNDPUStageSnapshot {
    int32_t bs_alu_operand;
    uint32_t bs_mul_cfg;
    int32_t bn_alu_operand;
    uint32_t bn_mul_cfg;
    int32_t ew_cvt_offset;
    uint32_t ew_cvt_scale;
    int32_t ew_operand[8];
    bool out_cvt_round;
} RockchipRKNNDPUStageSnapshot;

struct RockchipRKNNCoreState {
    SysBusDevice parent_obj;

    RegisterInfoArray *pc_reg_array;
    RegisterInfo pc_regs_info[ROCKCHIP_RKNN_PC_R_MAX];
    uint32_t pc_regs[ROCKCHIP_RKNN_PC_R_MAX];

    RegisterInfoArray *cna_reg_array;
    RegisterInfo cna_regs_info[ROCKCHIP_RKNN_CNA_R_MAX];
    uint32_t cna_regs[ROCKCHIP_RKNN_CNA_R_MAX];

    RegisterInfoArray *core_reg_array;
    RegisterInfo core_regs_info[ROCKCHIP_RKNN_CORE_R_MAX];
    uint32_t core_regs[ROCKCHIP_RKNN_CORE_R_MAX];

    RegisterInfoArray *ppu_reg_array;
    RegisterInfo ppu_regs_info[ROCKCHIP_RKNN_PPU_R_MAX];
    uint32_t ppu_regs[ROCKCHIP_RKNN_PPU_R_MAX];

    RegisterInfoArray *ppu_rdma_reg_array;
    RegisterInfo ppu_rdma_regs_info[ROCKCHIP_RKNN_PPU_RDMA_R_MAX];
    uint32_t ppu_rdma_regs[ROCKCHIP_RKNN_PPU_RDMA_R_MAX];

    MemoryRegion dpu_reg_array;
    MemoryRegion global_reg_array;
    RockchipRKNNRegisterFile slave_file;
    RockchipRKNNRegisterFile pending_file;
    uint32_t pending_register_writes[ROCKCHIP_RKNN_PENDING_WRITE_R_MAX];

    QEMUTimer complete_timer;
    VMChangeStateEntry *vmstate;
    BlockAIOCB *execution_aiocb;
    QemuMutex execution_lock;
    QemuCond execution_cond;
    qemu_irq irq;
    MemoryRegion *dma_mr;
    AddressSpace *dma_as;
    uint32_t core_index;
    uint32_t regcmd_shadow_pc[ROCKCHIP_RKNN_PC_R_MAX];
    uint32_t regcmd_shadow_cna[ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX];
    uint32_t regcmd_shadow_core[ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX];
    uint32_t regcmd_shadow_dpu[ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX];
    uint32_t regcmd_shadow_dpu_rdma[ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX];
    uint32_t regcmd_shadow_ppu[ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX];
    uint32_t regcmd_shadow_ppu_rdma[ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX];
    RockchipRKNNDomainRuntimeState domain_runtime[
        ROCKCHIP_RKNN_REGCMD_DOMAIN_COUNT];
    RockchipRKNNDomainRuntimeState pending_domain_runtime[
        ROCKCHIP_RKNN_REGCMD_DOMAIN_COUNT];
    RockchipRKNNPipelineTask *pending_pipeline;
    RockchipRKNNDPUStageSnapshot pending_dpu_stage;
    uint32_t pending_next_iova;
    uint32_t pending_next_command_count;
    uint16_t pending_task_count;
    uint16_t pending_task_index;
    bool pending_pipeline_decoded;
    bool pending_domain_runtime_valid;
    bool pending_fetch_error;
    bool pending_execution_error;
    bool pending_ppu_stage_attempted;
    bool pending_final_pipeline_attempted;
    bool pending_final_ppu_attempted;
    bool pending_final_ppu_success;
    uint8_t pending_final_ppu_bank;
    uint32_t pending_dma_error_bits;
    bool pending_slave;
    bool busy;
    bool reset_asserted;
    bool irq_level;
    bool execution_result_ready;
    bool execution_discard;
    bool execution_worker_done;
    int execution_mode;
    int execution_result;
    uint64_t functional_max_host_bytes;
    uint64_t functional_max_mac_operations;
    uint64_t functional_max_ppu_work_items;
    uint16_t lut[2][513];
    uint16_t execution_lut[2][513];
    uint32_t lut_access_cfg;
    uint32_t lut_cfg;
    uint32_t lut_info;
    uint32_t lut_le_start;
    uint32_t lut_le_end;
    uint32_t lut_lo_start;
    uint32_t lut_lo_end;
    uint32_t lut_le_slope_scale;
    uint32_t lut_le_slope_shift;
    uint32_t lut_lo_slope_scale;
    uint32_t lut_lo_slope_shift;
};

#endif /* HW_MISC_RK3588_RKNPU_H */
