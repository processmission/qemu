/*
 * STM32G474 USB full-speed device controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_USB_STM32G474_USBFS_H
#define HW_USB_STM32G474_USBFS_H

#include "qom/object.h"

#define TYPE_STM32G474_USBFS "stm32g474-usbfs"
OBJECT_DECLARE_SIMPLE_TYPE(Stm32g474UsbFsState, STM32G474_USBFS)

#define STM32G474_USBFS_BASE            0x40005c00
#define STM32G474_USBFS_MMIO_SIZE       0x400
#define STM32G474_USBFS_PMA_BASE        0x40006000
#define STM32G474_USBFS_PMA_SIZE        0x400
#define STM32G474_USBFS_HP_IRQ          19
#define STM32G474_USBFS_LP_IRQ          20

#endif
