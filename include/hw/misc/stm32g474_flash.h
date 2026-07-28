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
#include "qom/object.h"

#define TYPE_STM32G474_FLASH "stm32g474-flash"
OBJECT_DECLARE_SIMPLE_TYPE(Stm32g474FlashState, STM32G474_FLASH)

#define STM32G474_FLASH_IF_BASE   0x40022000
#define STM32G474_FLASH_IF_SIZE   0x400
#define STM32G474_FLASH_IRQ       4
#define STM32G474_FLASH_SIZE      (512 * KiB)
#define STM32G474_FLASH_SIZE_BASE 0x1fff75e0
#define STM32G474_FLASH_SIZE_WORD 0x00000200

#endif
