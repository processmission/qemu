/*
 * STM32G474 USART and UART
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_STM32G474_USART_H
#define HW_CHAR_STM32G474_USART_H

#include "qom/object.h"

#define TYPE_STM32G474_USART_BASE "stm32g474-usart-base"
#define TYPE_STM32G474_USART "stm32g474-usart"
#define TYPE_STM32G474_UART "stm32g474-uart"
OBJECT_DECLARE_TYPE(Stm32g474UsartState, Stm32g474UsartClass,
                    STM32G474_USART_BASE)

#endif
