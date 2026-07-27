/*
 * STM32G474 extended interrupts and events controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_STM32G474_EXTI_H
#define HW_MISC_STM32G474_EXTI_H

#include "hw/core/clock.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_STM32G474_EXTI "stm32g474-exti"
OBJECT_DECLARE_SIMPLE_TYPE(Stm32g474ExtiState, STM32G474_EXTI)

#define STM32G474_EXTI_BASE 0x40010400
#define STM32G474_EXTI_SIZE 0x400
#define STM32G474_EXTI_NUM_LINES 44
#define STM32G474_EXTI_NUM_BANKS 2
#define STM32G474_EXTI_NUM_REGS 14

struct Stm32g474ExtiState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[STM32G474_EXTI_NUM_REGS];
    uint32_t regs[STM32G474_EXTI_NUM_REGS];

    Clock *clk;
    qemu_irq irq[STM32G474_EXTI_NUM_LINES];
    qemu_irq event[STM32G474_EXTI_NUM_LINES];

    uint32_t input_levels[STM32G474_EXTI_NUM_BANKS];
    uint32_t swier_rising[STM32G474_EXTI_NUM_BANKS];
    uint32_t raw_pr_write;
    hwaddr raw_pr_addr;
    bool raw_pr_valid;
    bool resetting;
};

#endif
