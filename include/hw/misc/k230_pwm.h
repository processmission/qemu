/*
 * K230 PWM register block
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_PWM_H
#define HW_MISC_K230_PWM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_PWM "riscv.k230.pwm"
OBJECT_DECLARE_SIMPLE_TYPE(K230PwmState, K230_PWM)

#define K230_PWM_SIZE 0x1000

struct K230PwmState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_PWM_SIZE];
};

#endif /* HW_MISC_K230_PWM_H */
