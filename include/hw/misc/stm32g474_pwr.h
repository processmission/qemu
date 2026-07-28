/*
 * STM32G474 power control
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_STM32G474_PWR_H
#define HW_MISC_STM32G474_PWR_H

#include "qom/object.h"

#define TYPE_STM32G474_PWR "stm32g474-pwr"
OBJECT_DECLARE_SIMPLE_TYPE(Stm32g474PwrState, STM32G474_PWR)

#define STM32G474_PWR_BASE  0x40007000
#define STM32G474_PWR_SIZE  0x400
#endif
