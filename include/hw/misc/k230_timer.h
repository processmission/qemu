/*
 * K230 low-speed hardware timers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_TIMER_H
#define HW_MISC_K230_TIMER_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_TIMER "riscv.k230.timer"
OBJECT_DECLARE_SIMPLE_TYPE(K230TimerState, K230_TIMER)

#define K230_TIMER_SIZE 0x800
#define K230_TIMER_COUNT 6

struct K230TimerState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_TIMER_SIZE];
    int64_t start_ns[K230_TIMER_COUNT];
};

#endif /* HW_MISC_K230_TIMER_H */
