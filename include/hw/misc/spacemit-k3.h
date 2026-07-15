/*
 * SpacemiT K3 clock and boot controls
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SPACEMIT_K3_H
#define HW_MISC_SPACEMIT_K3_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SPACEMIT_K3_APMU "spacemit.k3.apmu"
OBJECT_DECLARE_SIMPLE_TYPE(SpacemitK3APMUState, SPACEMIT_K3_APMU)

#define TYPE_SPACEMIT_K3_CIU "spacemit.k3.ciu"
OBJECT_DECLARE_SIMPLE_TYPE(SpacemitK3CIUState, SPACEMIT_K3_CIU)

#define SPACEMIT_K3_APMU_MMIO_SIZE 0x400
#define SPACEMIT_K3_CIU_MMIO_SIZE  0x400

struct SpacemitK3APMUState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t sdh0_ctrl;
};

struct SpacemitK3CIUState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
};

#endif /* HW_MISC_SPACEMIT_K3_H */
