/*
 * K230 temperature sensor
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_TSENSOR_H
#define HW_MISC_K230_TSENSOR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_TSENSOR "riscv.k230.tsensor"
OBJECT_DECLARE_SIMPLE_TYPE(K230TSensorState, K230_TSENSOR)

#define K230_TSENSOR_SIZE 0x800

struct K230TSensorState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_TSENSOR_SIZE];
};

#endif /* HW_MISC_K230_TSENSOR_H */
