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
#define STM32G474_HSI48_FREQ_HZ 48000000
#define STM32G474_LSI_FREQ_HZ 32000

static void stm32g474_init(Object *obj)
{
    STM32G474State *s = STM32G474(obj);

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);
    object_initialize_child(obj, "rcc", &s->rcc, TYPE_STM32G474_RCC);
    object_initialize_child(obj, "pwr", &s->pwr, TYPE_STM32G474_PWR);
    object_initialize_child(obj, "flash", &s->flash, TYPE_STM32G474_FLASH);

    /* Fixed-frequency clocks do not need migration state. */
    s->hsi16 = clock_new(obj, "hsi16");
    clock_set_hz(s->hsi16, STM32G474_HSI16_FREQ_HZ);
    s->hsi48 = clock_new(obj, "hsi48");
    clock_set_hz(s->hsi48, STM32G474_HSI48_FREQ_HZ);
    s->lsi = clock_new(obj, "lsi");
    clock_set_hz(s->lsi, STM32G474_LSI_FREQ_HZ);
    qdev_alias_clock(DEVICE(&s->rcc), "hse-in", DEVICE(obj), "hse");
}

static void stm32g474_realize(DeviceState *dev, Error **errp)
{
    STM32G474State *s = STM32G474(dev);
    MemoryRegion *system_memory = get_system_memory();
    DeviceState *armv7m = DEVICE(&s->armv7m);
    DeviceState *rcc = DEVICE(&s->rcc);
    DeviceState *pwr = DEVICE(&s->pwr);
    DeviceState *flash = DEVICE(&s->flash);

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

    qdev_connect_clock_in(rcc, "hsi16-in", s->hsi16);
    qdev_connect_clock_in(rcc, "hsi48-in", s->hsi48);
    qdev_connect_clock_in(rcc, "lsi-in", s->lsi);
    qdev_connect_clock_in(armv7m, "cpuclk",
                          qdev_get_clock_out(rcc, "hclk"));
    qdev_connect_clock_in(armv7m, "refclk",
                          qdev_get_clock_out(rcc, "cortex-refclk"));
    qdev_connect_clock_in(pwr, "clk", qdev_get_clock_out(rcc, "pwr"));
    qdev_connect_clock_in(flash, "clk", qdev_get_clock_out(rcc, "flash"));
    qdev_connect_gpio_out_named(
        rcc, "peripheral-reset", STM32G474_RCC_RESET_PWR,
        qdev_get_gpio_in_named(pwr, "reset", 0));
    qdev_connect_gpio_out_named(
        rcc, "peripheral-reset", STM32G474_RCC_RESET_FLASH,
        qdev_get_gpio_in_named(flash, "reset", 0));
    if (!sysbus_realize(SYS_BUS_DEVICE(rcc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(rcc), 0, STM32G474_RCC_BASE);
    if (!sysbus_realize(SYS_BUS_DEVICE(pwr), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(pwr), 0, STM32G474_PWR_BASE);
    if (!sysbus_realize(SYS_BUS_DEVICE(flash), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(flash), 0, STM32G474_FLASH_IF_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(flash), 1, STM32G474_FLASH_BASE);
    memory_region_init_alias(
        &s->flash_alias, OBJECT(dev), "stm32g474.flash-boot-alias",
        sysbus_mmio_get_region(SYS_BUS_DEVICE(flash), 1), 0,
        STM32G474_FLASH_SIZE);
    memory_region_add_subregion(system_memory, 0, &s->flash_alias);
    sysbus_mmio_map(SYS_BUS_DEVICE(flash), 2, STM32G474_FLASH_SIZE_BASE);

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
    if (!object_property_set_link(OBJECT(&s->armv7m), "memory",
                                  OBJECT(system_memory), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(rcc), 0,
                       qdev_get_gpio_in(armv7m, STM32G474_RCC_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(flash), 0,
                       qdev_get_gpio_in(armv7m, STM32G474_FLASH_IRQ));
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
