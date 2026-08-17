/*
 * RK3588 DMAC (pl330) stub
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RK3588_PL330_H
#define HW_MISC_RK3588_PL330_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RK3588_PL330 "rk3588-pl330"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588PL330State, RK3588_PL330)

#define RK3588_PL330_MMIO_SIZE 0x4000

struct RK3588PL330State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq[2];

    uint8_t storage[RK3588_PL330_MMIO_SIZE];
};

#endif /* HW_MISC_RK3588_PL330_H */
