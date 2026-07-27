/*
 * STM32G474 microcontroller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_STM32G474_H
#define HW_ARM_STM32G474_H

#include "qemu/units.h"
#include "system/memory.h"
#include "hw/arm/armv7m.h"
#include "hw/core/clock.h"
#include "hw/misc/stm32g474_rcc.h"
#include "qom/object.h"

#define TYPE_STM32G474 "stm32g474"
OBJECT_DECLARE_SIMPLE_TYPE(STM32G474State, STM32G474)

#define STM32G474_FLASH_BASE        0x08000000
#define STM32G474_FLASH_SIZE        (512 * KiB)
#define STM32G474_SRAM1_BASE        0x20000000
#define STM32G474_SRAM1_SIZE        (80 * KiB)
#define STM32G474_SRAM2_BASE        0x20014000
#define STM32G474_SRAM2_SIZE        (16 * KiB)
#define STM32G474_CCM_SRAM_BASE     0x10000000
#define STM32G474_CCM_SRAM_ALIAS    0x20018000
#define STM32G474_CCM_SRAM_SIZE     (32 * KiB)

struct STM32G474State {
    SysBusDevice parent_obj;

    ARMv7MState armv7m;
    Stm32g474RccState rcc;

    Clock *hsi16;
    Clock *hsi48;
    Clock *lsi;

    MemoryRegion flash;
    MemoryRegion flash_alias;
    MemoryRegion sram1;
    MemoryRegion sram2;
    MemoryRegion ccm_sram;
    MemoryRegion ccm_sram_alias;
};

#endif
