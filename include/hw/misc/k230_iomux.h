/*
 * K230 IOMUX register block
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_IOMUX_H
#define HW_MISC_K230_IOMUX_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_IOMUX "riscv.k230.iomux"
OBJECT_DECLARE_SIMPLE_TYPE(K230IomuxState, K230_IOMUX)

#define K230_IOMUX_SIZE 0x800

struct K230IomuxState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_IOMUX_SIZE];
};

#endif /* HW_MISC_K230_IOMUX_H */
