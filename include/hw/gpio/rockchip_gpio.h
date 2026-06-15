/*
 * Rockchip GPIO bank emulation
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_ROCKCHIP_GPIO_H
#define HW_GPIO_ROCKCHIP_GPIO_H

#include "hw/core/irq.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_GPIO "rockchip-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipGPIOState, ROCKCHIP_GPIO)

#define ROCKCHIP_GPIO_PINS 32
#define ROCKCHIP_GPIO_MMIO_SIZE 0x100
#define ROCKCHIP_GPIO_NR_REGS (ROCKCHIP_GPIO_MMIO_SIZE / sizeof(uint32_t))

struct RockchipGPIOState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[ROCKCHIP_GPIO_NR_REGS];
    uint32_t regs[ROCKCHIP_GPIO_NR_REGS];

    qemu_irq irq;
    qemu_irq output[ROCKCHIP_GPIO_PINS];
    uint32_t input_level;
};

#endif /* HW_GPIO_ROCKCHIP_GPIO_H */
