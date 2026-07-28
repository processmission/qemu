/*
 * STM32G474 extended interrupts and events controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_STM32G474_EXTI_H
#define HW_MISC_STM32G474_EXTI_H

#include "qom/object.h"

#define TYPE_STM32G474_EXTI "stm32g474-exti"
OBJECT_DECLARE_SIMPLE_TYPE(Stm32g474ExtiState, STM32G474_EXTI)

#define STM32G474_EXTI_BASE 0x40010400
#define STM32G474_EXTI_SIZE 0x400
#define STM32G474_EXTI_NUM_LINES 44

#endif
