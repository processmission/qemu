/*
 * K230 GPIO register block
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_GPIO_H
#define HW_MISC_K230_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_GPIO "riscv.k230.gpio"
OBJECT_DECLARE_SIMPLE_TYPE(K230GpioState, K230_GPIO)

#define K230_GPIO_SIZE 0x1000

struct K230GpioState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_GPIO_SIZE];
};

#endif /* HW_MISC_K230_GPIO_H */
