/*
 * K230 PDMA controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DMA_K230_PDMA_H
#define HW_DMA_K230_PDMA_H

#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_PDMA "riscv.k230.pdma"
OBJECT_DECLARE_SIMPLE_TYPE(K230PdmaState, K230_PDMA)

#define K230_PDMA_SIZE 0x400
#define K230_PDMA_MMIO_SIZE 0x4000
#define K230_PDMA_LEGACY_SIZE K230_PDMA_MMIO_SIZE
#define K230_PDMA_R_MAX (K230_PDMA_SIZE / sizeof(uint32_t))

struct K230PdmaState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;
    RegisterInfoArray *reg_array;
    bool irq_level;
    RegisterInfo regs_info[K230_PDMA_R_MAX];
    uint32_t regs[K230_PDMA_R_MAX];
    uint8_t legacy_regs[K230_PDMA_LEGACY_SIZE];
};

#endif /* HW_DMA_K230_PDMA_H */
