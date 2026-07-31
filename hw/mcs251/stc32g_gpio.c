/*
 * STC32G GPIO P0-P7 and INT0/INT1 pin path
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/mcs251/stc32g.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

static const uint8_t stc32g_gpio_data_address[STC32G_GPIO_PORTS] = {
    0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xc8, 0xe8, 0xf8,
};

static const uint8_t stc32g_gpio_mode1_address[STC32G_GPIO_PORTS] = {
    0x93, 0x91, 0x95, 0xb1, 0xb3, 0xc9, 0xcb, 0xe1,
};

static const uint8_t stc32g_gpio_mode0_address[STC32G_GPIO_PORTS] = {
    0x94, 0x92, 0x96, 0xb2, 0xb4, 0xca, 0xcc, 0xe2,
};

static bool stc32g_gpio_pin_level(Stc32gGPIOState *s, unsigned port,
                                  unsigned pin)
{
    bool latch = extract8(s->latch[port], pin, 1);
    bool input = extract8(s->input[port], pin, 1);
    bool mode1 = extract8(s->mode1[port], pin, 1);
    bool mode0 = extract8(s->mode0[port], pin, 1);

    if (mode1 && !mode0) {
        return input;
    }
    if (!mode1 && mode0) {
        return latch;
    }
    return latch ? input : false;
}

static uint8_t stc32g_gpio_sample(Stc32gGPIOState *s, unsigned port)
{
    uint8_t value = 0;
    unsigned pin;

    for (pin = 0; pin < 8; pin++) {
        value = deposit32(value, pin, 1,
                          stc32g_gpio_pin_level(s, port, pin));
    }
    return value;
}

static void stc32g_gpio_update_port(Stc32gGPIOState *s, unsigned port)
{
    unsigned pin;

    for (pin = 0; pin < 8; pin++) {
        qemu_set_irq(s->output[port * 8 + pin],
                     stc32g_gpio_pin_level(s, port, pin));
    }
}

static int stc32g_gpio_find_port(const uint8_t *addresses, uint8_t address)
{
    unsigned port;

    for (port = 0; port < STC32G_GPIO_PORTS; port++) {
        if (addresses[port] == address) {
            return port;
        }
    }
    return -1;
}

static uint64_t stc32g_gpio_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    Stc32gGPIOReg *reg = opaque;
    Stc32gGPIOState *s = reg->parent;
    int port;

    port = stc32g_gpio_find_port(stc32g_gpio_data_address, reg->address);
    if (port >= 0) {
        if (s->cpu->env.direct_rmw) {
            return s->latch[port];
        }
        return stc32g_gpio_sample(s, port);
    }

    port = stc32g_gpio_find_port(stc32g_gpio_mode1_address, reg->address);
    if (port >= 0) {
        return s->mode1[port];
    }

    port = stc32g_gpio_find_port(stc32g_gpio_mode0_address, reg->address);
    return port >= 0 ? s->mode0[port] : 0;
}

static void stc32g_gpio_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    Stc32gGPIOReg *reg = opaque;
    Stc32gGPIOState *s = reg->parent;
    uint8_t byte = value;
    int port;

    port = stc32g_gpio_find_port(stc32g_gpio_data_address, reg->address);
    if (port >= 0) {
        s->latch[port] = byte;
        stc32g_gpio_update_port(s, port);
        return;
    }

    port = stc32g_gpio_find_port(stc32g_gpio_mode1_address, reg->address);
    if (port >= 0) {
        s->mode1[port] = byte;
        stc32g_gpio_update_port(s, port);
        return;
    }

    port = stc32g_gpio_find_port(stc32g_gpio_mode0_address, reg->address);
    if (port >= 0) {
        s->mode0[port] = byte;
        stc32g_gpio_update_port(s, port);
    }
}

static const MemoryRegionOps stc32g_gpio_ops = {
    .read = stc32g_gpio_read,
    .write = stc32g_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc32g_gpio_set_input(void *opaque, int n, int level)
{
    Stc32gGPIOState *s = opaque;
    unsigned port = n / 8;
    unsigned pin = n % 8;

    s->input[port] = deposit32(s->input[port], pin, 1, level);
    stc32g_gpio_update_port(s, port);

    if (port == 3 && (pin == 2 || pin == 3)) {
        qemu_set_irq(s->int_line[pin - 2], level);
    }
}

static void stc32g_gpio_update_all(Stc32gGPIOState *s)
{
    unsigned port;

    for (port = 0; port < STC32G_GPIO_PORTS; port++) {
        stc32g_gpio_update_port(s, port);
    }
    qemu_set_irq(s->int_line[0], extract8(s->input[3], 2, 1));
    qemu_set_irq(s->int_line[1], extract8(s->input[3], 3, 1));
}

static void stc32g_gpio_reset(DeviceState *dev)
{
    Stc32gGPIOState *s = STC32G_GPIO(dev);

    memset(s->latch, 0xff, sizeof(s->latch));
    memset(s->input, 0xff, sizeof(s->input));
    memset(s->mode1, 0xff, sizeof(s->mode1));
    memset(s->mode0, 0, sizeof(s->mode0));
    s->mode1[3] = 0xfc;
    stc32g_gpio_update_all(s);
}

static int stc32g_gpio_post_load(void *opaque, int version_id)
{
    Stc32gGPIOState *s = opaque;

    stc32g_gpio_update_all(s);
    return 0;
}

static const VMStateDescription stc32g_gpio_vmstate = {
    .name = "stc32g.gpio",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stc32g_gpio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(latch, Stc32gGPIOState, STC32G_GPIO_PORTS),
        VMSTATE_UINT8_ARRAY(input, Stc32gGPIOState, STC32G_GPIO_PORTS),
        VMSTATE_UINT8_ARRAY(mode1, Stc32gGPIOState, STC32G_GPIO_PORTS),
        VMSTATE_UINT8_ARRAY(mode0, Stc32gGPIOState, STC32G_GPIO_PORTS),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc32g_gpio_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc32gGPIOState, cpu, TYPE_MCS251_CPU,
                     MCS251CPU *),
};

static void stc32g_gpio_realize(DeviceState *dev, Error **errp)
{
    Stc32gGPIOState *s = STC32G_GPIO(dev);

    if (!s->cpu) {
        error_setg(errp, "stc32g-gpio requires a CPU link");
    }
}

static void stc32g_gpio_init(Object *obj)
{
    Stc32gGPIOState *s = STC32G_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned port;
    unsigned index = 0;

    for (port = 0; port < STC32G_GPIO_PORTS; port++) {
        s->reg[index].parent = s;
        s->reg[index].address = stc32g_gpio_data_address[port];
        memory_region_init_io(&s->reg[index].mr, obj, &stc32g_gpio_ops,
                              &s->reg[index], "stc32g.gpio-data", 1);
        sysbus_init_mmio(sbd, &s->reg[index++].mr);

        s->reg[index].parent = s;
        s->reg[index].address = stc32g_gpio_mode1_address[port];
        memory_region_init_io(&s->reg[index].mr, obj, &stc32g_gpio_ops,
                              &s->reg[index], "stc32g.gpio-mode1", 1);
        sysbus_init_mmio(sbd, &s->reg[index++].mr);

        s->reg[index].parent = s;
        s->reg[index].address = stc32g_gpio_mode0_address[port];
        memory_region_init_io(&s->reg[index].mr, obj, &stc32g_gpio_ops,
                              &s->reg[index], "stc32g.gpio-mode0", 1);
        sysbus_init_mmio(sbd, &s->reg[index++].mr);
    }

    qdev_init_gpio_in_named(DEVICE(obj), stc32g_gpio_set_input,
                            "gpio-in", STC32G_GPIO_PINS);
    qdev_init_gpio_out_named(DEVICE(obj), s->output, "gpio-out",
                             STC32G_GPIO_PINS);
    qdev_init_gpio_out_named(DEVICE(obj), s->int_line, "int-line", 2);
}

static void stc32g_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc32g_gpio_realize;
    device_class_set_legacy_reset(dc, stc32g_gpio_reset);
    device_class_set_props(dc, stc32g_gpio_properties);
    dc->vmsd = &stc32g_gpio_vmstate;
}

static const TypeInfo stc32g_gpio_type = {
    .name = TYPE_STC32G_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc32gGPIOState),
    .instance_init = stc32g_gpio_init,
    .class_init = stc32g_gpio_class_init,
};

static void stc32g_gpio_register_types(void)
{
    type_register_static(&stc32g_gpio_type);
}

type_init(stc32g_gpio_register_types)
