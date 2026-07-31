/*
 * STC8G1K08A SoC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/char/stc8g_uart.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/gpio/stc8g_gpio.h"
#include "hw/mcs51/stc8g.h"
#include "hw/timer/stc32g_timer.h"
#include "system/address-spaces.h"
#include "system/system.h"

#define STC8G_SFR_PHYS_ADDR(addr) \
    (MCS51_SFR_PHYS_BASE + (addr) - MCS251_SFR_BASE)
#define STC8G_XFR_PHYS_ADDR(addr) \
    (MCS51_XFR_PHYS_BASE + (addr) - MCS51_XFR_VIRT_BASE)

#define STC8G_SFR_TCON 0x88u
#define STC8G_SFR_SCON 0x98u
#define STC8G_SFR_P3 0xb0u
#define STC8G_XFR_P3PU 0xfe13u

static void stc8g_soc_realize(DeviceState *dev, Error **errp)
{
    Stc8gSoCState *s = STC8G_SOC(dev);
    MemoryRegion *sysmem = get_system_memory();
    unsigned i;

    if (!memory_region_init_rom(&s->flash, OBJECT(s), "stc8g.flash",
                                STC8G_FLASH_SIZE, errp)) {
        return;
    }
    if (!memory_region_init_ram(&s->idata, OBJECT(s), "stc8g.idata",
                                STC8G_IDATA_SIZE, errp)) {
        return;
    }
    if (!memory_region_init_ram(&s->xdata, OBJECT(s), "stc8g.xdata",
                                STC8G_XDATA_SIZE, errp)) {
        return;
    }

    if (!qdev_realize(DEVICE(&s->cpu), NULL, errp)) {
        return;
    }
    object_property_set_link(OBJECT(s->gpio), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(s->timer), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(s->uart), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    qdev_prop_set_chr(s->uart, "chardev", serial_hd(0));
    if (!sysbus_realize(SYS_BUS_DEVICE(s->gpio), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->timer), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->uart), errp)) {
        return;
    }

    memory_region_add_subregion(sysmem, STC8G_FLASH_BASE, &s->flash);
    memory_region_add_subregion(sysmem, STC8G_IDATA_BASE, &s->idata);
    memory_region_add_subregion(sysmem, STC8G_XDATA_BASE, &s->xdata);
    memory_region_add_subregion(sysmem, MCS51_SFR_PHYS_BASE, &s->cpu.sfr);
    memory_region_add_subregion(sysmem, MCS51_DISABLED_PHYS_BASE,
                                &s->cpu.disabled);

    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->timer), 0,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_TCON), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->uart), 0,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_SCON), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->gpio), 0,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_P3), 1);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gpio), 1,
                    STC8G_XFR_PHYS_ADDR(STC8G_XFR_P3PU));

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
        qdev_connect_gpio_out_named(s->gpio, "counter-line", i,
            qdev_get_gpio_in_named(s->timer, "counter", i));
    }
}

static void stc8g_soc_init(Object *obj)
{
    Stc8gSoCState *s = STC8G_SOC(obj);

    object_initialize_child(obj, "cpu", &s->cpu, TYPE_MCS51_CPU);
    s->gpio = qdev_new(TYPE_STC8G_GPIO);
    object_property_add_child(obj, "gpio", OBJECT(s->gpio));
    s->timer = qdev_new(TYPE_STC8G_TIMER);
    object_property_add_child(obj, "timer", OBJECT(s->timer));
    s->uart = qdev_new(TYPE_STC8G_UART);
    object_property_add_child(obj, "uart1", OBJECT(s->uart));
}

static void stc8g_soc_reset(DeviceState *dev)
{
    Stc8gSoCState *s = STC8G_SOC(dev);

    cpu_reset(CPU(&s->cpu));
}

static void stc8g_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc8g_soc_realize;
    device_class_set_legacy_reset(dc, stc8g_soc_reset);
}

static const TypeInfo stc8g_soc_type = {
    .name = TYPE_STC8G_SOC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc8gSoCState),
    .instance_init = stc8g_soc_init,
    .class_init = stc8g_soc_class_init,
};

static void stc8g_soc_register_types(void)
{
    type_register_static(&stc8g_soc_type);
}

type_init(stc8g_soc_register_types)
