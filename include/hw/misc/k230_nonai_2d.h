/*
 * K230 non-AI 2D engine
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_NONAI_2D_H
#define HW_MISC_K230_NONAI_2D_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_NONAI_2D "riscv.k230.nonai-2d"
OBJECT_DECLARE_SIMPLE_TYPE(K230NonAI2DState, K230_NONAI_2D)

#define K230_NONAI_2D_SIZE 0x4000

struct K230NonAI2DState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;
    bool irq_level;
    uint8_t regs[K230_NONAI_2D_SIZE];
};

#endif /* HW_MISC_K230_NONAI_2D_H */
