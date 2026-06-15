/*
 * K230 UGZIP decompressor
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_UGZIP_H
#define HW_MISC_K230_UGZIP_H

#include "hw/core/sysbus.h"
#include "hw/dma/k230_gsdma.h"
#include "qom/object.h"

#define TYPE_K230_UGZIP "riscv.k230.ugzip"
OBJECT_DECLARE_SIMPLE_TYPE(K230UgzipState, K230_UGZIP)

#define K230_UGZIP_SIZE 0x4000

struct K230UgzipState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_UGZIP_SIZE];
    K230GsdmaState *gsdma;
};

#endif /* HW_MISC_K230_UGZIP_H */
