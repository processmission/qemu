/*
 * STC32G144K246 SoC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/mcs251/stc32g.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/address-spaces.h"
#include "system/system.h"

static void stc32g_soc_realize(DeviceState *dev, Error **errp)
{
    Stc32gSoCState *s = STC32G_SOC(dev);
    MemoryRegion *sysmem = get_system_memory();
    unsigned i;

    qdev_realize(DEVICE(&s->cpu), NULL, errp);
    if (*errp) {
        return;
    }
    object_property_set_link(OBJECT(&s->timer), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(&s->uart), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(&s->gpio), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    qdev_prop_set_chr(DEVICE(&s->uart), "chardev", serial_hd(0));
    sysbus_realize(SYS_BUS_DEVICE(&s->timer), errp);
    if (*errp) {
        return;
    }
    sysbus_realize(SYS_BUS_DEVICE(&s->uart), errp);
    if (*errp) {
        return;
    }
    sysbus_realize(SYS_BUS_DEVICE(&s->gpio), errp);
    if (*errp) {
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

    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&s->timer), 0,
                            MCS251_SFR_PHYS_BASE + 0x08, 1);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->timer), 1, 0x7efea0);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&s->uart), 0,
                            MCS251_SFR_PHYS_BASE + 0x18, 1);
    for (i = 0; i < STC32G_GPIO_REGS; i++) {
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&s->gpio), i,
                                MCS251_SFR_PHYS_BASE +
                                s->gpio.reg[i].address - 0x80, 1);
    }

    for (i = 0; i < 4; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->timer), i,
                           qdev_get_gpio_in(DEVICE(&s->cpu), i));
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->uart), 0,
                       qdev_get_gpio_in(DEVICE(&s->cpu),
                                        MCS251_IRQ_UART1));
    for (i = 0; i < 2; i++) {
        qdev_connect_gpio_out_named(DEVICE(&s->gpio), "int-line", i,
            qdev_get_gpio_in_named(DEVICE(&s->timer), "gate", i));
        qdev_connect_gpio_out_named(DEVICE(&s->gpio), "gpio-out",
            3 * 8 + 4 + i,
            qdev_get_gpio_in_named(DEVICE(&s->timer), "counter", i));
    }
}

static void stc32g_soc_init(Object *obj)
{
    Stc32gSoCState *s = STC32G_SOC(obj);

    object_initialize_child(obj, "cpu", &s->cpu, TYPE_STC32G_CPU);
    object_initialize_child(obj, "timer", &s->timer, TYPE_STC32G_TIMER);
    object_initialize_child(obj, "uart1", &s->uart, TYPE_STC32G_UART);
    object_initialize_child(obj, "gpio", &s->gpio, TYPE_STC32G_GPIO);
    memory_region_init_ram(&s->edata, obj, "stc32g.edata",
                           STC32G_EDATA_SIZE, &error_abort);
    memory_region_init_ram(&s->xdata, obj, "stc32g.xdata",
                           STC32G_XDATA_SIZE, &error_abort);
    memory_region_init_ram(&s->exec_ram, obj, "stc32g.exec-ram",
                           STC32G_EXEC_RAM_SIZE, &error_abort);
    memory_region_init_alias(&s->exec_data_alias, obj,
                             "stc32g.exec-data-alias", &s->exec_ram, 0,
                             STC32G_EXEC_RAM_SIZE);
    memory_region_init_alias(&s->exec_code_alias, obj,
                             "stc32g.exec-code-alias", &s->exec_ram, 0,
                             STC32G_EXEC_RAM_SIZE);
    memory_region_set_readonly(&s->exec_code_alias, true);
    memory_region_init_rom(&s->flash, obj, "stc32g.flash",
                           STC32G_FLASH_SIZE, &error_abort);
}

static void stc32g_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc32g_soc_realize;
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
