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
#include "system/memory.h"

#define TYPE_ROCKCHIP_IOMMU "rockchip.iommu"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipIOMMUState, ROCKCHIP_IOMMU)

#define TYPE_ROCKCHIP_IOMMU_MEMORY_REGION \
    "rockchip-iommu-memory-region"
#define ROCKCHIP_IOMMU_WINDOW_SIZE 0x100
#define ROCKCHIP_IOMMU_MAX_MMU 2
#define ROCKCHIP_IOMMU_R_MAX (0x28 / 4)

struct RockchipIOMMUState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array[ROCKCHIP_IOMMU_MAX_MMU];
    RegisterInfo regs_info[ROCKCHIP_IOMMU_MAX_MMU][ROCKCHIP_IOMMU_R_MAX];
    uint32_t regs[ROCKCHIP_IOMMU_MAX_MMU][ROCKCHIP_IOMMU_R_MAX];
    IOMMUMemoryRegion iommu_mr;
    qemu_irq irq;

    uint32_t num_mmu;
    uint32_t core_index;
};

MemoryRegion *rockchip_iommu_get_memory_region(RockchipIOMMUState *s);
bool rockchip_iommu_find_translation_bank(IOMMUMemoryRegion *iommu,
                                           hwaddr addr,
                                           IOMMUAccessFlags flag,
                                           unsigned int *bank);

#endif /* HW_MISC_ROCKCHIP_IOMMU_H */
