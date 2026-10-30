/*
 * Sony PlayStation 2 Emotion Engine interrupt controller (INTC)
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INTC_PS2_INTC_H
#define HW_INTC_PS2_INTC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PS2_INTC "ps2-intc"
OBJECT_DECLARE_SIMPLE_TYPE(PS2INTCState, PS2_INTC)

/* INTC input lines, also the bit numbers of INTC_STAT and INTC_MASK. */
enum {
    PS2_INTC_GS = 0,
    PS2_INTC_SBUS = 1,
    PS2_INTC_VBLANK_START = 2,
    PS2_INTC_VBLANK_END = 3,
    PS2_INTC_VIF0 = 4,
    PS2_INTC_VIF1 = 5,
    PS2_INTC_VU0 = 6,
    PS2_INTC_VU1 = 7,
    PS2_INTC_IPU = 8,
    PS2_INTC_TIMER0 = 9,
    PS2_INTC_TIMER1 = 10,
    PS2_INTC_TIMER2 = 11,
    PS2_INTC_TIMER3 = 12,
    PS2_INTC_SFIFO = 13,
    PS2_INTC_VU0WD = 14,
    PS2_INTC_PGPU = 15,
    PS2_INTC_NUM_IRQS = 16,
};

#define PS2_INTC_STAT   0x00
#define PS2_INTC_MASK   0x10
#define PS2_INTC_SIZE   0x20

struct PS2INTCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t stat;
    uint32_t mask;
    uint32_t level;
};

#endif
