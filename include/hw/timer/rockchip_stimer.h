/*
 * Rockchip secure timer stub
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_ROCKCHIP_STIMER_H
#define HW_TIMER_ROCKCHIP_STIMER_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_STIMER "rockchip-stimer"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipSTimerState, ROCKCHIP_STIMER)

#define ROCKCHIP_STIMER_SIZE 0x1000

struct RockchipSTimerState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[ROCKCHIP_STIMER_SIZE / 4];
};

#endif /* HW_TIMER_ROCKCHIP_STIMER_H */
