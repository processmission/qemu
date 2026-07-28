/*
 * STM32G474 system configuration controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_STM32G474_SYSCFG_H
#define HW_MISC_STM32G474_SYSCFG_H

#include "qom/object.h"

#define TYPE_STM32G474_SYSCFG "stm32g474-syscfg"
OBJECT_DECLARE_SIMPLE_TYPE(Stm32g474SyscfgState, STM32G474_SYSCFG)

#define STM32G474_SYSCFG_BASE 0x40010000
#define STM32G474_SYSCFG_SIZE 0x30
#define STM32G474_SYSCFG_NUM_PORTS 7
#define STM32G474_SYSCFG_NUM_LINES 16

#endif
