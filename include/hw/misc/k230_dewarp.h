/*
 * K230 DW200 dewarp engine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_DEWARP_H
#define HW_MISC_K230_DEWARP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_DEWARP "riscv.k230.dewarp"
OBJECT_DECLARE_SIMPLE_TYPE(K230DewarpState, K230_DEWARP)

#define K230_DEWARP_SIZE 0x1000

struct K230DewarpState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq dwe_irq;
    qemu_irq fe_irq;
    qemu_irq vse_irq;
    bool dwe_irq_level;
    bool fe_irq_level;
    bool vse_irq_level;
    uint8_t regs[K230_DEWARP_SIZE];
};

#endif /* HW_MISC_K230_DEWARP_H */
