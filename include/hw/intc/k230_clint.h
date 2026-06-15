/*
 * K230 T-Head C908 S-mode CLINT extension
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INTC_K230_CLINT_H
#define HW_INTC_K230_CLINT_H

#include "hw/core/sysbus.h"

#define TYPE_K230_CLINT_SMODE "riscv.k230.clint-smode"
OBJECT_DECLARE_SIMPLE_TYPE(K230ClintSModeState, K230_CLINT_SMODE)

struct K230ClintSModeState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint32_t hartid_base;
    uint32_t cpu_index_base;
    uint32_t num_harts;
};

DeviceState *k230_clint_smode_create_in(MemoryRegion *mem, hwaddr addr,
                                        uint32_t hartid_base,
                                        uint32_t cpu_index_base,
                                        uint32_t num_harts);

#endif /* HW_INTC_K230_CLINT_H */
