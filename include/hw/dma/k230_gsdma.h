/*
 * K230 GSDMA controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DMA_K230_GSDMA_H
#define HW_DMA_K230_GSDMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_GSDMA "riscv.k230.gsdma"
OBJECT_DECLARE_SIMPLE_TYPE(K230GsdmaState, K230_GSDMA)

#define K230_GSDMA_SIZE 0x4000

#define K230_GSDMA_UGZIP_RD_CH 0
#define K230_GSDMA_UGZIP_WR_CH 1

struct K230GsdmaState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;
    uint8_t regs[K230_GSDMA_SIZE];
};

uint32_t k230_gsdma_get_llt_saddr(K230GsdmaState *s, unsigned int ch);
void k230_gsdma_ugzip_complete(K230GsdmaState *s);

#endif /* HW_DMA_K230_GSDMA_H */
