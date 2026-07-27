/*
 * STM32G474 USART and UART
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_STM32G474_USART_H
#define HW_CHAR_STM32G474_USART_H

#include "chardev/char-fe.h"
#include "hw/core/clock.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_STM32G474_USART_BASE "stm32g474-usart-base"
#define TYPE_STM32G474_USART "stm32g474-usart"
#define TYPE_STM32G474_UART "stm32g474-uart"
OBJECT_DECLARE_TYPE(Stm32g474UsartState, Stm32g474UsartClass,
                    STM32G474_USART_BASE)

#define STM32G474_USART_NUM_REGS 12

typedef struct Stm32g474UsartVariant Stm32g474UsartVariant;

struct Stm32g474UsartState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[STM32G474_USART_NUM_REGS];
    uint32_t regs[STM32G474_USART_NUM_REGS];

    Clock *clk;
    CharFrontend chr;
    qemu_irq irq;
    guint watch_tag;
    VMChangeStateEntry *resume_entry;

    bool tx_pending;
    bool tdr_write_accepted;
    bool resetting;
    bool peripheral_reset_asserted;
    bool handlers_installed;
    bool host_io_blocked;
};

struct Stm32g474UsartClass {
    SysBusDeviceClass parent_class;

    const Stm32g474UsartVariant *variant;
};

#endif
