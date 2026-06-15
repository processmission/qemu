/*
 * K230 KPU/GNNE engine
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_KPU_H
#define HW_MISC_K230_KPU_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_K230_KPU "riscv.k230.kpu"
OBJECT_DECLARE_SIMPLE_TYPE(K230KpuState, K230_KPU)

#define K230_KPU_SIZE 0x800

struct K230KpuState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;
    QEMUTimer complete_timer;
    bool busy;
    bool irq_level;
    uint8_t regs[K230_KPU_SIZE];
    uint8_t *gnne_rdata_shadow;
    uint64_t gnne_rdata_shadow_base;
    uint64_t gnne_rdata_shadow_size;
    bool gnne_rdata_shadow_valid;
};

#endif /* HW_MISC_K230_KPU_H */
