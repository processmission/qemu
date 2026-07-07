/*
 * NXP Low Power UART
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_NXP_LPUART_H
#define HW_CHAR_NXP_LPUART_H

#include "chardev/char-fe.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_NXP_LPUART "nxp.lpuart"
OBJECT_DECLARE_SIMPLE_TYPE(NXPLPUARTState, NXP_LPUART)

/*
 * Register storage slots for the contiguous 32-bit LPUART register subset
 * from offsets 0x00 through 0x2c.
 */
#define NXP_LPUART_R_MAX (0x30 / 4)

struct NXPLPUARTState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[NXP_LPUART_R_MAX];
    uint32_t regs[NXP_LPUART_R_MAX];

    CharFrontend chr;
    qemu_irq irq;
};

#endif
