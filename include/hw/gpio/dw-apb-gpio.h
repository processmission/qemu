/*
 * Synopsys DesignWare APB GPIO
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_DW_APB_GPIO_H
#define HW_GPIO_DW_APB_GPIO_H

#include "hw/core/irq.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"

#define TYPE_DW_APB_GPIO "dw-apb-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(DWAPBGPIOState, DW_APB_GPIO)

#define DW_APB_GPIO_NR_PINS          32
#define DW_APB_GPIO_REG_SIZE         0x54
#define DW_APB_GPIO_NR_REGS          (DW_APB_GPIO_REG_SIZE / sizeof(uint32_t))
#define DW_APB_GPIO_MMIO_SIZE        0x400

typedef struct DWAPBGPIOState {
    SysBusDevice parent;

    uint32_t regs[DW_APB_GPIO_NR_REGS];
    RegisterInfo regs_info[DW_APB_GPIO_NR_REGS];
    RegisterInfoArray *reg_array;
    qemu_irq output[DW_APB_GPIO_NR_PINS];
    uint32_t input;
} DWAPBGPIOState;

#endif /* HW_GPIO_DW_APB_GPIO_H */
