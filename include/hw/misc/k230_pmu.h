/*
 * K230 power management unit
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_PMU_H
#define HW_MISC_K230_PMU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_PMU "riscv.k230.pmu"
OBJECT_DECLARE_SIMPLE_TYPE(K230PmuState, K230_PMU)

#define K230_PMU_MMIO_SIZE 0xc00

struct K230PmuState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_PMU_MMIO_SIZE];
    uint8_t poweroff_step;
};

#endif /* HW_MISC_K230_PMU_H */
