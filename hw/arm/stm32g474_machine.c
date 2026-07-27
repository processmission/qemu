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

#define TYPE_STM32G474_MACHINE MACHINE_TYPE_NAME("stm32g474")
OBJECT_DECLARE_SIMPLE_TYPE(Stm32g474MachineState, STM32G474_MACHINE)

struct Stm32g474MachineState {
    MachineState parent_obj;

    CanBusState *canbus[STM32G474_FDCAN_NUM_CHANNELS];
};

static void stm32g474_machine_init(MachineState *machine)
{
    Stm32g474MachineState *s = STM32G474_MACHINE(machine);
    DeviceState *mcu;

    mcu = qdev_new(TYPE_STM32G474);
    object_property_add_child(OBJECT(machine), "mcu", OBJECT(mcu));
    for (unsigned int i = 0;
         i < STM32G474_FDCAN_NUM_CHANNELS; i++) {
        g_autofree char *bus_name = g_strdup_printf("canbus%u", i);

        object_property_set_link(OBJECT(mcu), bus_name,
                                 OBJECT(s->canbus[i]), &error_fatal);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(mcu), &error_fatal);

    armv7m_load_kernel(STM32G474(mcu)->armv7m.cpu,
                       machine->kernel_filename, STM32G474_FLASH_BASE,
                       STM32G474_FLASH_SIZE);
}

static void stm32g474_machine_instance_init(Object *obj)
{
    Stm32g474MachineState *s = STM32G474_MACHINE(obj);

    object_property_add_link(obj, "canbus0", TYPE_CAN_BUS,
                             (Object **)&s->canbus[0],
                             object_property_allow_set_link, 0);
    object_property_add_link(obj, "canbus1", TYPE_CAN_BUS,
                             (Object **)&s->canbus[1],
                             object_property_allow_set_link, 0);
    object_property_add_link(obj, "canbus2", TYPE_CAN_BUS,
                             (Object **)&s->canbus[2],
                             object_property_allow_set_link, 0);
}

static void stm32g474_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m4"),
        NULL,
    };

    mc->desc = "STMicroelectronics STM32G474VE (Cortex-M4F)";
    mc->init = stm32g474_machine_init;
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_size = 0;
}

static const TypeInfo stm32g474_machine_types[] = {
    {
        .name = TYPE_STM32G474_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(Stm32g474MachineState),
        .instance_init = stm32g474_machine_instance_init,
        .class_init = stm32g474_machine_class_init,
        .interfaces = arm_machine_interfaces,
    },
};

DEFINE_TYPES(stm32g474_machine_types)
