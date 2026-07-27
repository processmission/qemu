/*
 * STMicroelectronics STM32G474VE generic machine
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/stm32g474.h"
#include "hw/core/boards.h"

static void stm32g474_machine_init(MachineState *machine)
{
    DeviceState *mcu;

    mcu = qdev_new(TYPE_STM32G474);
    object_property_add_child(OBJECT(machine), "mcu", OBJECT(mcu));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(mcu), &error_fatal);

    armv7m_load_kernel(STM32G474(mcu)->armv7m.cpu,
                       machine->kernel_filename, STM32G474_FLASH_BASE,
                       STM32G474_FLASH_SIZE);
}

static void stm32g474_machine_class_init(MachineClass *mc)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m4"),
        NULL,
    };

    mc->desc = "STMicroelectronics STM32G474VE (Cortex-M4F)";
    mc->init = stm32g474_machine_init;
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_size = 0;
}

DEFINE_MACHINE_ARM("stm32g474", stm32g474_machine_class_init)
