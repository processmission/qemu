/*
 * K230 RX CSI and video input registers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_RX_CSI_H
#define HW_MISC_K230_RX_CSI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_RX_CSI "riscv.k230.rx-csi"
OBJECT_DECLARE_SIMPLE_TYPE(K230RxCsiState, K230_RX_CSI)

#define K230_RX_CSI_SIZE 0x10000

struct K230RxCsiState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_RX_CSI_SIZE];
};

#endif /* HW_MISC_K230_RX_CSI_H */
