/*
 * STC32G144K246 SoC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/mcs51/stc32g.h"
#include "hw/mcs51/stc32g_dsp.h"
#include "hw/mcs51/stc32g_tfpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/address-spaces.h"
#include "system/system.h"

#define STC32G_SFR_PHYS_ADDR(addr) \
    (MCS251_SFR_PHYS_BASE + (addr) - MCS251_SFR_BASE)

#define STC32G_SFR_SCON 0x98u
#define STC32G_SFR_DPUST 0x86u
#define STC32G_SFR_DPUOP 0xd8u
#define STC32G_SFR_DMAIR 0xedu

#define STC32G_TIMER_XFR_BASE 0x7efea0u
#define STC32G_TFPU_XFR_BASE 0x7efe93u

#define STC32G_GPIO_P3_PIN(pin) (3 * 8 + (pin))

static void stc32g_soc_realize(DeviceState *dev, Error **errp)
{
    Stc32gSoCState *s = STC32G_SOC(dev);
    MemoryRegion *sysmem = get_system_memory();
    unsigned i;

    if (!memory_region_init_ram(&s->edata, OBJECT(s), "stc32g.edata",
                                STC32G_EDATA_SIZE, errp)) {
        return;
    }
    if (!memory_region_init_ram(&s->xdata, OBJECT(s), "stc32g.xdata",
                                STC32G_XDATA_SIZE, errp)) {
        return;
    }
    if (!memory_region_init_ram(&s->exec_ram, OBJECT(s),
                                "stc32g.exec-ram",
                                STC32G_EXEC_RAM_SIZE, errp)) {
        return;
    }
    memory_region_init_alias(&s->exec_data_alias, OBJECT(s),
                             "stc32g.exec-data-alias", &s->exec_ram, 0,
                             STC32G_EXEC_RAM_SIZE);
    memory_region_init_alias(&s->exec_code_alias, OBJECT(s),
                             "stc32g.exec-code-alias", &s->exec_ram, 0,
                             STC32G_EXEC_RAM_SIZE);
    memory_region_set_readonly(&s->exec_code_alias, true);
    if (!memory_region_init_rom(&s->flash, OBJECT(s), "stc32g.flash",
                                STC32G_FLASH_SIZE, errp)) {
        return;
    }
    memset(memory_region_get_ram_ptr(&s->flash), 0xff,
           STC32G_FLASH_SIZE);

    if (!qdev_realize(DEVICE(&s->cpu), NULL, errp)) {
        return;
    }
    object_property_set_link(OBJECT(s->dsp), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(s->tfpu), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(s->timer), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(s->uart), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(s->gpio), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    qdev_prop_set_chr(s->uart, "chardev", serial_hd(0));
    if (!sysbus_realize(SYS_BUS_DEVICE(s->dsp), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->tfpu), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->timer), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->uart), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->gpio), errp)) {
        return;
    }

    memory_region_add_subregion(sysmem, STC32G_EDATA_BASE, &s->edata);
    memory_region_add_subregion(sysmem, STC32G_XDATA_BASE, &s->xdata);
    memory_region_add_subregion(sysmem, STC32G_EXEC_DATA_BASE,
                                &s->exec_data_alias);
    memory_region_add_subregion(sysmem, STC32G_EXEC_CODE_BASE,
                                &s->exec_code_alias);
    memory_region_add_subregion(sysmem, STC32G_FLASH_BASE, &s->flash);
    memory_region_add_subregion(sysmem, MCS251_SFR_PHYS_BASE, &s->cpu.sfr);
    memory_region_add_subregion(sysmem, MCS251_DISABLED_PHYS_BASE,
                                &s->cpu.disabled);

    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->timer), 0,
                            STC32G_SFR_PHYS_ADDR(MCS251_SFR_TCON), 1);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->timer), 1,
                    STC32G_TIMER_XFR_BASE);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->uart), 0,
                            STC32G_SFR_PHYS_ADDR(STC32G_SFR_SCON), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->gpio), 0,
                            STC32G_SFR_PHYS_ADDR(MCS251_SFR_P0), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->dsp), 0,
                            STC32G_SFR_PHYS_ADDR(STC32G_SFR_DPUST), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->dsp), 1,
                            STC32G_SFR_PHYS_ADDR(STC32G_SFR_DPUOP), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->tfpu), 0,
                            STC32G_SFR_PHYS_ADDR(STC32G_SFR_DMAIR), 1);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->tfpu), 1,
                    STC32G_TFPU_XFR_BASE);

    for (i = 0; i < 4; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(s->timer), i,
                           qdev_get_gpio_in(DEVICE(&s->cpu), i));
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(s->uart), 0,
                       qdev_get_gpio_in(DEVICE(&s->cpu),
                                        MCS251_IRQ_UART1));
    for (i = 0; i < 2; i++) {
        qdev_connect_gpio_out_named(s->gpio, "int-line", i,
            qdev_get_gpio_in_named(s->timer, "gate", i));
        qdev_connect_gpio_out_named(s->gpio, "gpio-out",
            STC32G_GPIO_P3_PIN(4 + i),
            qdev_get_gpio_in_named(s->timer, "counter", i));
    }
}

static void stc32g_soc_init(Object *obj)
{
    Stc32gSoCState *s = STC32G_SOC(obj);

    object_initialize_child(obj, "cpu", &s->cpu, TYPE_MCS251_CPU);
    s->timer = qdev_new(TYPE_STC32G_TIMER);
    object_property_add_child(obj, "timer", OBJECT(s->timer));
    s->uart = qdev_new(TYPE_STC32G_UART);
    object_property_add_child(obj, "uart1", OBJECT(s->uart));
    s->gpio = qdev_new(TYPE_STC32G_GPIO);
    object_property_add_child(obj, "gpio", OBJECT(s->gpio));
    s->dsp = qdev_new(TYPE_STC32G_DSP);
    object_property_add_child(obj, "dsp32", OBJECT(s->dsp));
    s->tfpu = qdev_new(TYPE_STC32G_TFPU);
    object_property_add_child(obj, "tfpu", OBJECT(s->tfpu));
}

static void stc32g_soc_reset(DeviceState *dev)
{
    Stc32gSoCState *s = STC32G_SOC(dev);

    cpu_reset(CPU(&s->cpu));
}

static void stc32g_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc32g_soc_realize;
    device_class_set_legacy_reset(dc, stc32g_soc_reset);
}

static const TypeInfo stc32g_soc_type = {
    .name = TYPE_STC32G_SOC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc32gSoCState),
    .instance_init = stc32g_soc_init,
    .class_init = stc32g_soc_class_init,
};

static void stc32g_soc_register_types(void)
{
    type_register_static(&stc32g_soc_type);
}

type_init(stc32g_soc_register_types)
