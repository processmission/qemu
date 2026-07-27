/*
 * STM32G474 flash memory interface
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_STM32G474_FLASH_H
#define HW_MISC_STM32G474_FLASH_H

#include "qemu/units.h"
#include "system/memory.h"
#include "hw/core/clock.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_STM32G474_FLASH "stm32g474-flash"
OBJECT_DECLARE_SIMPLE_TYPE(Stm32g474FlashState, STM32G474_FLASH)

#define STM32G474_FLASH_IF_BASE   0x40022000
#define STM32G474_FLASH_IF_SIZE   0x400
#define STM32G474_FLASH_R_MAX     (0x78 / 4)
#define STM32G474_FLASH_IRQ       4
#define STM32G474_FLASH_SIZE      (512 * KiB)
#define STM32G474_FLASH_SIZE_BASE 0x1fff75e0
#define STM32G474_FLASH_SIZE_WORD 0x00000200

struct Stm32g474FlashState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    uint32_t regs[STM32G474_FLASH_R_MAX];
    RegisterInfo regs_info[STM32G474_FLASH_R_MAX];

    Clock *clk;
    qemu_irq irq;
    MemoryRegion main_flash;
    MemoryRegion flash_size;
    uint8_t *storage;

    bool peripheral_reset_asserted;
    bool resetting;
};

#endif
