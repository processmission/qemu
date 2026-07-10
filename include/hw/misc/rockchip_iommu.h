/*
 * Rockchip IOMMU
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_ROCKCHIP_IOMMU_H
#define HW_MISC_ROCKCHIP_IOMMU_H

#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_IOMMU "rockchip.iommu"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipIOMMUState, ROCKCHIP_IOMMU)

#define ROCKCHIP_IOMMU_WINDOW_SIZE 0x100
#define ROCKCHIP_IOMMU_MAX_MMU 2
#define ROCKCHIP_IOMMU_R_MAX (0x28 / 4)

struct RockchipIOMMUState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array[ROCKCHIP_IOMMU_MAX_MMU];
    RegisterInfo regs_info[ROCKCHIP_IOMMU_MAX_MMU][ROCKCHIP_IOMMU_R_MAX];
    uint32_t regs[ROCKCHIP_IOMMU_MAX_MMU][ROCKCHIP_IOMMU_R_MAX];

    uint32_t num_mmu;
    uint32_t core_index;
};

bool rockchip_iommu_iova_to_phys(RockchipIOMMUState *s, uint32_t iova,
                                 hwaddr *phys, unsigned int *bank,
                                 const char **reason);

#endif /* HW_MISC_ROCKCHIP_IOMMU_H */
