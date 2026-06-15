/*
 * Rockchip RAM-backed syscon register bank
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_ROCKCHIP_SYSCON_H
#define HW_MISC_ROCKCHIP_SYSCON_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_SYSCON "rockchip-syscon"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipSysconState, ROCKCHIP_SYSCON)

struct RockchipSysconState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t *regs;
    uint32_t size;
};

void rockchip_syscon_set_u32(RockchipSysconState *s, hwaddr offset,
                             uint32_t value);
uint32_t rockchip_syscon_get_u32(RockchipSysconState *s, hwaddr offset);

#endif /* HW_MISC_ROCKCHIP_SYSCON_H */
