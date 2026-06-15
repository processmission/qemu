/*
 * K230 display controller register blocks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_K230_DISPLAY_H
#define HW_DISPLAY_K230_DISPLAY_H

#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/memory.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "ui/console.h"

#define TYPE_K230_VO "riscv.k230.vo"
OBJECT_DECLARE_SIMPLE_TYPE(K230VoState, K230_VO)

#define TYPE_K230_DSI "riscv.k230.dsi"
OBJECT_DECLARE_SIMPLE_TYPE(K230DsiState, K230_DSI)

#define K230_VO_SIZE  0x10000
#define K230_DSI_SIZE 0x1000

struct K230VoState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    MemoryRegionSection fbsection;
    QemuConsole *con;
    qemu_irq irq;
    QEMUTimer *vblank_timer;
    hwaddr fbdev_base;
    hwaddr fb_base;
    uint32_t fbdev_width;
    uint32_t fbdev_height;
    uint32_t fbdev_stride;
    uint32_t src_width;
    uint32_t cols;
    uint32_t rows;
    bool fbdev_compat;
    bool invalidate;
    uint8_t regs[K230_VO_SIZE];
};

struct K230DsiState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_DSI_SIZE];
};

#endif /* HW_DISPLAY_K230_DISPLAY_H */
