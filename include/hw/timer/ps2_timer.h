/*
 * Sony PlayStation 2 Emotion Engine timers
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_PS2_TIMER_H
#define HW_TIMER_PS2_TIMER_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PS2_TIMER "ps2-timer"
OBJECT_DECLARE_SIMPLE_TYPE(PS2TimerState, PS2_TIMER)

#define PS2_TIMER_NUM           4
#define PS2_TIMER_STRIDE        0x800
#define PS2_TIMER_SIZE          (PS2_TIMER_NUM * PS2_TIMER_STRIDE)

#define PS2_TIMER_COUNT         0x00
#define PS2_TIMER_MODE          0x10
#define PS2_TIMER_COMP          0x20
#define PS2_TIMER_HOLD          0x30

#define PS2_TIMER_MODE_CLKS     0x0003
#define PS2_TIMER_MODE_ZRET     (1 << 6)
#define PS2_TIMER_MODE_CUE      (1 << 7)
#define PS2_TIMER_MODE_CMPE     (1 << 8)
#define PS2_TIMER_MODE_OVFE     (1 << 9)
#define PS2_TIMER_MODE_EQUF     (1 << 10)
#define PS2_TIMER_MODE_OVFF     (1 << 11)
#define PS2_TIMER_MODE_FLAGS    (PS2_TIMER_MODE_EQUF | PS2_TIMER_MODE_OVFF)

/* The bus clock is half the R5900 clock. */
#define PS2_BUS_CLOCK_HZ        147456000
/* NTSC horizontal blank rate, for the HBLNK clock source. */
#define PS2_HBLANK_HZ           15734

typedef struct PS2TimerChannel {
    PS2TimerState *parent;
    QEMUTimer *timer;
    qemu_irq irq;

    uint32_t count;
    uint32_t mode;
    uint32_t comp;
    uint32_t hold;

    /* Counting runs from base_ns; ticks already folded into count. */
    int64_t base_ns;
    uint64_t ticks_seen;
} PS2TimerChannel;

struct PS2TimerState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    PS2TimerChannel ch[PS2_TIMER_NUM];
};

#endif
