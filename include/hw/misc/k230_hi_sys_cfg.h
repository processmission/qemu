/*
 * K230 high-speed system config registers
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_HI_SYS_CFG_H
#define HW_MISC_K230_HI_SYS_CFG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_HI_SYS_CFG "riscv.k230.hi-sys-cfg"
OBJECT_DECLARE_SIMPLE_TYPE(K230HiSysCfgState, K230_HI_SYS_CFG)

#define K230_HI_SYS_CFG_SIZE 0x400

struct K230HiSysCfgState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_HI_SYS_CFG_SIZE];
};

#endif /* HW_MISC_K230_HI_SYS_CFG_H */
