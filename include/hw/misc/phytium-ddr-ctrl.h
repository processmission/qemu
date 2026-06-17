/*
 * Phytium DDR controller shim
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_PHYTIUM_DDR_CTRL_H
#define HW_MISC_PHYTIUM_DDR_CTRL_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PHYTIUM_DDR_CTRL "phytium-ddr-ctrl"
OBJECT_DECLARE_SIMPLE_TYPE(PhytiumDdrCtrlState, PHYTIUM_DDR_CTRL)

#define PHYTIUM_DDR_CTRL_MMIO_SIZE 0x1000
#define PHYTIUM_DDR_INDEX_SPACE_SIZE 0x10000

struct PhytiumDdrCtrlState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[PHYTIUM_DDR_CTRL_MMIO_SIZE / sizeof(uint32_t)];
    uint32_t index_regs[PHYTIUM_DDR_INDEX_SPACE_SIZE / sizeof(uint32_t)];
};

#endif /* HW_MISC_PHYTIUM_DDR_CTRL_H */
