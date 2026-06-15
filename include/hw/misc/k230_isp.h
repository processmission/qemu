/*
 * K230 ISP media pipeline registers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_ISP_H
#define HW_MISC_K230_ISP_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_K230_ISP "riscv.k230.isp"
OBJECT_DECLARE_SIMPLE_TYPE(K230IspState, K230_ISP)

#define K230_ISP_SIZE 0x8000

struct K230IspState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq isp_irq;
    qemu_irq mi_irq;
    qemu_irq fe_irq;
    QEMUTimer mcm_frame_timer;
    bool isp_irq_level;
    bool mi_irq_level;
    bool fe_irq_level;
    uint8_t regs[K230_ISP_SIZE];
};

#endif /* HW_MISC_K230_ISP_H */
