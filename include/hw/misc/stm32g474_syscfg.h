/*
 * STM32G474 system configuration controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_STM32G474_SYSCFG_H
#define HW_MISC_STM32G474_SYSCFG_H

#include "hw/core/clock.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_STM32G474_SYSCFG "stm32g474-syscfg"
OBJECT_DECLARE_SIMPLE_TYPE(Stm32g474SyscfgState, STM32G474_SYSCFG)

#define STM32G474_SYSCFG_BASE 0x40010000
#define STM32G474_SYSCFG_SIZE 0x30
#define STM32G474_SYSCFG_NUM_REGS 10
#define STM32G474_SYSCFG_NUM_PORTS 7
#define STM32G474_SYSCFG_NUM_LINES 16

typedef enum Stm32g474SyscfgKeyPhase {
    STM32G474_SYSCFG_KEY_LOCKED,
    STM32G474_SYSCFG_KEY_HAVE_CA,
    STM32G474_SYSCFG_KEY_UNLOCKED,
} Stm32g474SyscfgKeyPhase;

struct Stm32g474SyscfgState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[STM32G474_SYSCFG_NUM_REGS];
    uint32_t regs[STM32G474_SYSCFG_NUM_REGS];

    Clock *clk;
    qemu_irq exti_out[STM32G474_SYSCFG_NUM_LINES];

    uint16_t gpio_levels[STM32G474_SYSCFG_NUM_PORTS];
    uint16_t output_cache;
    uint32_t raw_exticr;
    uint8_t key_phase;
    bool peripheral_reset_asserted;
    bool resetting;
    bool output_cache_valid;
    bool raw_exticr_valid;
};

#endif
