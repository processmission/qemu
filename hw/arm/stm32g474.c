/*
 * STM32G474 microcontroller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "hw/arm/stm32g474.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"

#define STM32G474_NUM_IRQS 102
#define STM32G474_NUM_PRIO_BITS 4
#define STM32G474_HSI16_FREQ_HZ 16000000

static void stm32g474_init(Object *obj)
{
    STM32G474State *s = STM32G474(obj);

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);

    /* Fixed-frequency clocks do not need migration state. */
    s->hsi16 = clock_new(obj, "hsi16");
    clock_set_hz(s->hsi16, STM32G474_HSI16_FREQ_HZ);
    s->cortex_refclk = clock_new(obj, "cortex-refclk");
    clock_set_mul_div(s->cortex_refclk, 8, 1);
    clock_set_source(s->cortex_refclk, s->hsi16);
}

static void stm32g474_realize(DeviceState *dev, Error **errp)
{
    STM32G474State *s = STM32G474(dev);
    MemoryRegion *system_memory = get_system_memory();
    DeviceState *armv7m = DEVICE(&s->armv7m);

    if (!memory_region_init_rom(&s->flash, OBJECT(dev), "stm32g474.flash",
                                STM32G474_FLASH_SIZE, errp)) {
        return;
    }
    memory_region_init_alias(&s->flash_alias, OBJECT(dev),
                             "stm32g474.flash-boot-alias", &s->flash, 0,
                             STM32G474_FLASH_SIZE);

    if (!memory_region_init_ram(&s->sram1, OBJECT(dev), "stm32g474.sram1",
                                STM32G474_SRAM1_SIZE, errp)) {
        return;
    }
    if (!memory_region_init_ram(&s->sram2, OBJECT(dev), "stm32g474.sram2",
                                STM32G474_SRAM2_SIZE, errp)) {
        return;
    }
    if (!memory_region_init_ram(&s->ccm_sram, OBJECT(dev),
                                "stm32g474.ccm-sram",
                                STM32G474_CCM_SRAM_SIZE, errp)) {
        return;
    }
    memory_region_init_alias(&s->ccm_sram_alias, OBJECT(dev),
                             "stm32g474.ccm-sram-alias", &s->ccm_sram, 0,
                             STM32G474_CCM_SRAM_SIZE);

    memory_region_add_subregion(system_memory, STM32G474_FLASH_BASE,
                                &s->flash);
    memory_region_add_subregion(system_memory, 0, &s->flash_alias);
    memory_region_add_subregion(system_memory, STM32G474_SRAM1_BASE,
                                &s->sram1);
    memory_region_add_subregion(system_memory, STM32G474_SRAM2_BASE,
                                &s->sram2);
    memory_region_add_subregion(system_memory, STM32G474_CCM_SRAM_BASE,
                                &s->ccm_sram);
    memory_region_add_subregion(system_memory, STM32G474_CCM_SRAM_ALIAS,
                                &s->ccm_sram_alias);

    qdev_prop_set_uint32(armv7m, "num-irq", STM32G474_NUM_IRQS);
    qdev_prop_set_uint32(armv7m, "num-prio-bits",
                         STM32G474_NUM_PRIO_BITS);
    qdev_prop_set_string(armv7m, "cpu-type",
                         ARM_CPU_TYPE_NAME("cortex-m4"));
    qdev_prop_set_bit(armv7m, "enable-bitband", true);
    qdev_connect_clock_in(armv7m, "cpuclk", s->hsi16);
    qdev_connect_clock_in(armv7m, "refclk", s->cortex_refclk);
    if (!object_property_set_link(OBJECT(&s->armv7m), "memory",
                                  OBJECT(system_memory), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }
}

static void stm32g474_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = stm32g474_realize;
    dc->user_creatable = false;
}

static const TypeInfo stm32g474_types[] = {
    {
        .name = TYPE_STM32G474,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(STM32G474State),
        .instance_init = stm32g474_init,
        .class_init = stm32g474_class_init,
    },
};

DEFINE_TYPES(stm32g474_types)
