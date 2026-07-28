/*
 * Mercedes-Benz ARDEP V2 machine
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
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/split-irq.h"
#include "hw/misc/led.h"

#define ARDEP_V2_HSE_FREQ_HZ 16000000
#define ARDEP_V2_LED_PIN 3
#define ARDEP_V2_GREEN_LED_PORT 0
#define ARDEP_V2_RED_LED_PORT 2

#define TYPE_ARDEP_V2_MACHINE MACHINE_TYPE_NAME("ardep-v2")
OBJECT_DECLARE_SIMPLE_TYPE(ArdepV2MachineState, ARDEP_V2_MACHINE)

struct ArdepV2MachineState {
    MachineState parent_obj;

    STM32G474State mcu;
    CanBusState *canbus[2];
    SplitIRQ red_led_splitter;
    SplitIRQ green_led_splitter;
    Clock *hse;
};

static void ardep_v2_connect_led(ArdepV2MachineState *s,
                                 SplitIRQ *splitter, LEDState *led,
                                 unsigned int port)
{
    DeviceState *gpio = s->mcu.gpio[port];
    DeviceState *splitter_dev = DEVICE(splitter);
    unsigned int gpio_input = port * STM32G474_GPIO_NUM_PINS
                              + ARDEP_V2_LED_PIN;

    qdev_connect_gpio_out(
        splitter_dev, 0,
        qdev_get_gpio_in_named(DEVICE(&s->mcu), "gpio-in", gpio_input));
    qdev_connect_gpio_out(splitter_dev, 1,
                          qdev_get_gpio_in(DEVICE(led), 0));
    qdev_connect_gpio_out_named(
        gpio, "pin-out", ARDEP_V2_LED_PIN,
        qdev_get_gpio_in(splitter_dev, 0));
}

static void ardep_v2_machine_init(MachineState *machine)
{
    ArdepV2MachineState *s = ARDEP_V2_MACHINE(machine);
    DeviceState *mcu;
    DeviceState *red_led_splitter;
    DeviceState *green_led_splitter;
    LEDState *red_led;
    LEDState *green_led;

    object_initialize_child(OBJECT(machine), "mcu", &s->mcu,
                            TYPE_STM32G474);
    mcu = DEVICE(&s->mcu);
    object_property_set_link(OBJECT(mcu), "canbus0",
                             OBJECT(s->canbus[0]), &error_fatal);
    object_property_set_link(OBJECT(mcu), "canbus1",
                             OBJECT(s->canbus[1]), &error_fatal);
    s->hse = clock_new(OBJECT(machine), "hse");
    clock_set_hz(s->hse, ARDEP_V2_HSE_FREQ_HZ);
    qdev_connect_clock_in(mcu, "hse", s->hse);
    sysbus_realize(SYS_BUS_DEVICE(mcu), &error_fatal);

    red_led = led_create_simple(OBJECT(machine), GPIO_POLARITY_ACTIVE_LOW,
                                LED_COLOR_RED, "Red LED");
    green_led = led_create_simple(OBJECT(machine), GPIO_POLARITY_ACTIVE_LOW,
                                  LED_COLOR_GREEN, "Green LED");

    object_initialize_child(OBJECT(machine), "red-led-splitter",
                            &s->red_led_splitter, TYPE_SPLIT_IRQ);
    red_led_splitter = DEVICE(&s->red_led_splitter);
    qdev_prop_set_uint16(red_led_splitter, "num-lines", 2);
    qdev_realize(red_led_splitter, NULL, &error_fatal);

    object_initialize_child(OBJECT(machine), "green-led-splitter",
                            &s->green_led_splitter, TYPE_SPLIT_IRQ);
    green_led_splitter = DEVICE(&s->green_led_splitter);
    qdev_prop_set_uint16(green_led_splitter, "num-lines", 2);
    qdev_realize(green_led_splitter, NULL, &error_fatal);

    ardep_v2_connect_led(s, &s->red_led_splitter, red_led,
                         ARDEP_V2_RED_LED_PORT);
    ardep_v2_connect_led(s, &s->green_led_splitter, green_led,
                         ARDEP_V2_GREEN_LED_PORT);

    armv7m_load_kernel(s->mcu.armv7m.cpu, machine->kernel_filename,
                       STM32G474_FLASH_BASE, STM32G474_FLASH_SIZE);
}

static void ardep_v2_machine_instance_init(Object *obj)
{
    ArdepV2MachineState *s = ARDEP_V2_MACHINE(obj);

    object_property_add_link(obj, "canbus0", TYPE_CAN_BUS,
                             (Object **)&s->canbus[0],
                             object_property_allow_set_link, 0);
    object_property_add_link(obj, "canbus1", TYPE_CAN_BUS,
                             (Object **)&s->canbus[1],
                             object_property_allow_set_link, 0);
}

static void ardep_v2_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m4"),
        NULL,
    };

    mc->desc = "Mercedes-Benz ARDEP V2 (STM32G474VE)";
    mc->init = ardep_v2_machine_init;
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_size = 0;
}

static const TypeInfo ardep_v2_machine_types[] = {
    {
        .name = TYPE_ARDEP_V2_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(ArdepV2MachineState),
        .instance_init = ardep_v2_machine_instance_init,
        .class_init = ardep_v2_machine_class_init,
        .interfaces = arm_machine_interfaces,
    },
};

DEFINE_TYPES(ardep_v2_machine_types)
