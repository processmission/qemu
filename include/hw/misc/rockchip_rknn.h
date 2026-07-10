/*
 * Rockchip RK3588 RKNN/RKNPU core
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_ROCKCHIP_RKNN_H
#define HW_MISC_ROCKCHIP_RKNN_H

#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "hw/misc/rockchip_iommu.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_RKNN_CORE "rockchip.rk3588-rknn-core"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipRKNNCoreState, ROCKCHIP_RKNN_CORE)

#define ROCKCHIP_RKNN_WINDOW_SIZE 0x1000
#define ROCKCHIP_RKNN_PC_R_MAX (0x40 / 4)
#define ROCKCHIP_RKNN_CNA_R_MAX (0x8 / 4)
#define ROCKCHIP_RKNN_CORE_R_MAX (0x8 / 4)
#define ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX (0x1000 / 4)

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

    QEMUTimer complete_timer;
    qemu_irq irq;
    RockchipIOMMUState *iommu;
    uint32_t core_index;
    uint32_t regcmd_shadow_pc[ROCKCHIP_RKNN_PC_R_MAX];
    uint32_t regcmd_shadow_cna[ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX];
    uint32_t regcmd_shadow_core[ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX];
    uint32_t regcmd_shadow_dpu[ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX];
    uint32_t regcmd_shadow_dpu_rdma[ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX];
    uint32_t regcmd_shadow_ppu[ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX];
    uint32_t regcmd_shadow_ppu_rdma[ROCKCHIP_RKNN_REGCMD_DOMAIN_R_MAX];
    bool busy;
    bool irq_level;
};

#endif /* HW_MISC_ROCKCHIP_RKNN_H */
