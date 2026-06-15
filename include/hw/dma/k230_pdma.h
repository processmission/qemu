/*
 * K230 PDMA controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DMA_K230_PDMA_H
#define HW_DMA_K230_PDMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_PDMA "riscv.k230.pdma"
OBJECT_DECLARE_SIMPLE_TYPE(K230PdmaState, K230_PDMA)

#define K230_PDMA_SIZE 0x4000

struct K230PdmaState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_PDMA_SIZE];
};

#endif /* HW_DMA_K230_PDMA_H */
