/*
 * STM32G474 USB full-speed device controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_USB_STM32G474_USBFS_H
#define HW_USB_STM32G474_USBFS_H

#include "hw/core/clock.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_STM32G474_USBFS "stm32g474-usbfs"
OBJECT_DECLARE_SIMPLE_TYPE(Stm32g474UsbFsState, STM32G474_USBFS)

#define STM32G474_USBFS_BASE            0x40005c00
#define STM32G474_USBFS_MMIO_SIZE       0x400
#define STM32G474_USBFS_PMA_BASE        0x40006000
#define STM32G474_USBFS_PMA_SIZE        0x400
#define STM32G474_USBFS_NUM_REGS        (0x5c / sizeof(uint32_t))
#define STM32G474_USBFS_NUM_BDT_REGS    32

struct Stm32g474UsbFsState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[STM32G474_USBFS_NUM_REGS];
    uint32_t regs[STM32G474_USBFS_NUM_REGS];

    MemoryRegion pma_mr;
    uint16_t pma[STM32G474_USBFS_PMA_SIZE / sizeof(uint16_t)];
    RegisterInfo bdt_regs[STM32G474_USBFS_NUM_BDT_REGS];

    Clock *pclk;
    Clock *usb;
    qemu_irq hp_irq;
    qemu_irq lp_irq;

    bool resetting;
    bool peripheral_reset_asserted;
    bool fres_active;
    bool fres_reset_pending;
};

#endif
