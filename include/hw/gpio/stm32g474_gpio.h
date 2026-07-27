/*
 * STM32G474 general-purpose I/O ports
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_STM32G474_GPIO_H
#define HW_GPIO_STM32G474_GPIO_H

#include "hw/core/clock.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_STM32G474_GPIO "stm32g474-gpio"
#define TYPE_STM32G474_GPIO_A "stm32g474-gpio-a"
#define TYPE_STM32G474_GPIO_B "stm32g474-gpio-b"
#define TYPE_STM32G474_GPIO_CG "stm32g474-gpio-cg"
OBJECT_DECLARE_TYPE(Stm32g474GpioState, Stm32g474GpioClass,
                    STM32G474_GPIO)

#define STM32G474_GPIO_NUM_PORTS 7
#define STM32G474_GPIO_NUM_PINS 16
#define STM32G474_GPIO_NUM_REGS 11
#define STM32G474_GPIO_MMIO_SIZE 0x400

#define STM32G474_GPIOA_BASE 0x48000000
#define STM32G474_GPIOB_BASE 0x48000400
#define STM32G474_GPIOC_BASE 0x48000800
#define STM32G474_GPIOD_BASE 0x48000c00
#define STM32G474_GPIOE_BASE 0x48001000
#define STM32G474_GPIOF_BASE 0x48001400
#define STM32G474_GPIOG_BASE 0x48001800

typedef struct Stm32g474GpioVariant Stm32g474GpioVariant;

struct Stm32g474GpioState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[STM32G474_GPIO_NUM_REGS];
    uint32_t regs[STM32G474_GPIO_NUM_REGS];

    Clock *clk;
    qemu_irq pin_out[STM32G474_GPIO_NUM_PINS];

    uint16_t external_driven;
    uint16_t external_level;
    uint16_t lock_candidate;
    uint16_t locked_mask;
    uint16_t resolved_cache;
    uint8_t lock_phase;
    uint8_t current_access_size;
    uint8_t current_access_lane;
    bool peripheral_reset_asserted;
    bool resetting;
    bool resolved_cache_valid;
};

struct Stm32g474GpioClass {
    SysBusDeviceClass parent_class;

    const Stm32g474GpioVariant *variant;
};

#endif
