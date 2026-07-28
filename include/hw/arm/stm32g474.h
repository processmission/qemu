/*
 * STM32G474 microcontroller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_STM32G474_H
#define HW_ARM_STM32G474_H

#include "qemu/units.h"
#include "system/memory.h"
#include "hw/arm/armv7m.h"
#include "hw/char/stm32g474_usart.h"
#include "hw/core/clock.h"
#include "hw/core/or-irq.h"
#include "hw/gpio/stm32g474_gpio.h"
#include "hw/misc/stm32g474_exti.h"
#include "hw/misc/stm32g474_flash.h"
#include "hw/misc/stm32g474_pwr.h"
#include "hw/misc/stm32g474_rcc.h"
#include "hw/misc/stm32g474_syscfg.h"
#include "hw/net/stm32g474_fdcan.h"
#include "hw/usb/stm32g474_usbfs.h"
#include "net/can_emu.h"
#include "qom/object.h"

#define TYPE_STM32G474 "stm32g474"
OBJECT_DECLARE_SIMPLE_TYPE(STM32G474State, STM32G474)

#define STM32G474_FLASH_BASE        0x08000000
#define STM32G474_SRAM1_BASE        0x20000000
#define STM32G474_SRAM1_SIZE        (80 * KiB)
#define STM32G474_SRAM2_BASE        0x20014000
#define STM32G474_SRAM2_SIZE        (16 * KiB)
#define STM32G474_CCM_SRAM_BASE     0x10000000
#define STM32G474_CCM_SRAM_ALIAS    0x20018000
#define STM32G474_CCM_SRAM_SIZE     (32 * KiB)
#define STM32G474_USART1_BASE       0x40013800
#define STM32G474_USART2_BASE       0x40004400
#define STM32G474_UART4_BASE        0x40004c00
#define STM32G474_EXTI0_IRQ         6
#define STM32G474_EXTI9_5_IRQ       23
#define STM32G474_EXTI15_10_IRQ     40
#define STM32G474_USART1_IRQ        37
#define STM32G474_USART2_IRQ        38
#define STM32G474_UART4_IRQ         52

struct STM32G474State {
    SysBusDevice parent_obj;

    ARMv7MState armv7m;
    DeviceState *rcc;
    DeviceState *pwr;
    DeviceState *flash;
    DeviceState *syscfg;
    DeviceState *exti;
    OrIRQState exti_9_5_or;
    OrIRQState exti_15_10_or;
    DeviceState *usart1;
    DeviceState *usart2;
    DeviceState *uart4;
    DeviceState *fdcan;
    DeviceState *usbfs;
    DeviceState *gpio[STM32G474_GPIO_NUM_PORTS];

    Clock *hsi16;
    Clock *hsi48;
    Clock *lsi;
    CanBusState *canbus[STM32G474_FDCAN_NUM_CHANNELS];

    MemoryRegion flash_alias;
    MemoryRegion sram1;
    MemoryRegion sram2;
    MemoryRegion ccm_sram;
    MemoryRegion ccm_sram_alias;
};

#endif
