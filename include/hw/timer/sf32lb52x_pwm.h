/*
 * SiFli SF32LB52X advanced timer
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_SF32LB52X_PWM_H
#define HW_TIMER_SF32LB52X_PWM_H

#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_PWM "sifli.sf32lb52x-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XPWMState, SF32LB52X_PWM)

#define SF32LB52X_PWM_SIZE 0x1000

struct SF32LB52XPWMState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;
    /* false: 32-bit ATIM, true: 16-bit GPTIM. */
    bool gptim;
    uint32_t regs[0x400];
    /* Counter snapshot for the free-running virtual-clock model. */
    uint64_t base_ns;
    uint32_t base_count;
};

#endif /* HW_TIMER_SF32LB52X_PWM_H */
