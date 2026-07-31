/*
 * STC32G GPIO P0-P7 and INT0/INT1 pin path
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/gpio/stc32g_gpio.h"
#include "migration/vmstate.h"
#include "target/mcs51/cpu.h"

REG8(P0, 0x00)
REG8(P0M1, 0x13)
REG8(P0M0, 0x14)
REG8(P1, 0x10)
REG8(P1M1, 0x11)
REG8(P1M0, 0x12)
REG8(P2, 0x20)
REG8(P2M1, 0x15)
REG8(P2M0, 0x16)
REG8(P3, 0x30)
REG8(P3M1, 0x31)
REG8(P3M0, 0x32)
REG8(P4, 0x40)
REG8(P4M1, 0x33)
REG8(P4M0, 0x34)
REG8(P5, 0x48)
REG8(P5M1, 0x49)
REG8(P5M0, 0x4a)
REG8(P6, 0x68)
REG8(P6M1, 0x4b)
REG8(P6M0, 0x4c)
REG8(P7, 0x78)
REG8(P7M1, 0x61)
REG8(P7M0, 0x62)

#define STC32G_GPIO_SFR_SIZE 0x80
#define STC32G_GPIO_INT_PORT 3
#define STC32G_GPIO_INT_FIRST_PIN 2
#define STC32G_GPIO_INT_LINES 2

enum Stc32gGPIORegisterKind {
    STC32G_GPIO_DATA,
    STC32G_GPIO_MODE1,
    STC32G_GPIO_MODE0,
    STC32G_GPIO_REGS_PER_PORT,
};

#define STC32G_GPIO_REGS \
    (STC32G_GPIO_PORTS * STC32G_GPIO_REGS_PER_PORT)

struct Stc32gGPIOState {
    SysBusDevice parent_obj;

    MCS251CPU *cpu;
    MemoryRegion sfr;
    RegisterInfoArray *reg_array[STC32G_GPIO_REGS];
    RegisterInfo regs_info[STC32G_GPIO_REGS];
    qemu_irq output[STC32G_GPIO_PINS];
    qemu_irq int_line[STC32G_GPIO_INT_LINES];
    uint8_t regs[STC32G_GPIO_REGS];
    uint8_t input[STC32G_GPIO_PORTS];
};

static unsigned stc32g_gpio_reg_index(unsigned port,
                                      enum Stc32gGPIORegisterKind kind)
{
    return port * STC32G_GPIO_REGS_PER_PORT + kind;
}

static bool stc32g_gpio_pin_level(Stc32gGPIOState *s, unsigned port,
                                  unsigned pin)
{
    bool latch = extract8(
        s->regs[stc32g_gpio_reg_index(port, STC32G_GPIO_DATA)], pin, 1);
    bool input = extract8(s->input[port], pin, 1);
    bool mode1 = extract8(
        s->regs[stc32g_gpio_reg_index(port, STC32G_GPIO_MODE1)], pin, 1);
    bool mode0 = extract8(
        s->regs[stc32g_gpio_reg_index(port, STC32G_GPIO_MODE0)], pin, 1);

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
        bool level = stc32g_gpio_pin_level(s, port, pin);

        qemu_set_irq(s->output[port * 8 + pin], level);
        if (port == STC32G_GPIO_INT_PORT &&
            pin >= STC32G_GPIO_INT_FIRST_PIN &&
            pin < STC32G_GPIO_INT_FIRST_PIN + STC32G_GPIO_INT_LINES) {
            qemu_set_irq(s->int_line[pin - STC32G_GPIO_INT_FIRST_PIN],
                         level);
        }
    }
}

static uint64_t stc32g_gpio_post_read(RegisterInfo *reg, uint64_t value)
{
    Stc32gGPIOState *s = STC32G_GPIO(reg->opaque);
    unsigned index = reg - s->regs_info;
    unsigned port = index / STC32G_GPIO_REGS_PER_PORT;
    unsigned kind = index % STC32G_GPIO_REGS_PER_PORT;

    if (kind == STC32G_GPIO_DATA) {
        return s->cpu->env.direct_rmw ? value :
               stc32g_gpio_sample(s, port);
    }
    return value;
}

static void stc32g_gpio_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc32gGPIOState *s = STC32G_GPIO(reg->opaque);
    unsigned index = reg - s->regs_info;

    stc32g_gpio_update_port(s, index / STC32G_GPIO_REGS_PER_PORT);
}

#define STC32G_GPIO_ACCESS_INFO(NAME, RESET)                              \
    { .name = #NAME, .addr = 0, .reset = (RESET),                         \
      .post_read = stc32g_gpio_post_read,                                 \
      .post_write = stc32g_gpio_post_write }

static const RegisterAccessInfo stc32g_gpio_regs_info[] = {
    STC32G_GPIO_ACCESS_INFO(P0, 0xff),
    STC32G_GPIO_ACCESS_INFO(P0M1, 0xff),
    STC32G_GPIO_ACCESS_INFO(P0M0, 0x00),
    STC32G_GPIO_ACCESS_INFO(P1, 0xff),
    STC32G_GPIO_ACCESS_INFO(P1M1, 0xff),
    STC32G_GPIO_ACCESS_INFO(P1M0, 0x00),
    STC32G_GPIO_ACCESS_INFO(P2, 0xff),
    STC32G_GPIO_ACCESS_INFO(P2M1, 0xff),
    STC32G_GPIO_ACCESS_INFO(P2M0, 0x00),
    STC32G_GPIO_ACCESS_INFO(P3, 0xff),
    STC32G_GPIO_ACCESS_INFO(P3M1, 0xfc),
    STC32G_GPIO_ACCESS_INFO(P3M0, 0x00),
    STC32G_GPIO_ACCESS_INFO(P4, 0xff),
    STC32G_GPIO_ACCESS_INFO(P4M1, 0xff),
    STC32G_GPIO_ACCESS_INFO(P4M0, 0x00),
    STC32G_GPIO_ACCESS_INFO(P5, 0xff),
    STC32G_GPIO_ACCESS_INFO(P5M1, 0xff),
    STC32G_GPIO_ACCESS_INFO(P5M0, 0x00),
    STC32G_GPIO_ACCESS_INFO(P6, 0xff),
    STC32G_GPIO_ACCESS_INFO(P6M1, 0xff),
    STC32G_GPIO_ACCESS_INFO(P6M0, 0x00),
    STC32G_GPIO_ACCESS_INFO(P7, 0xff),
    STC32G_GPIO_ACCESS_INFO(P7M1, 0xff),
    STC32G_GPIO_ACCESS_INFO(P7M0, 0x00),
};

#undef STC32G_GPIO_ACCESS_INFO

static const hwaddr stc32g_gpio_reg_offsets[] = {
    A_P0, A_P0M1, A_P0M0,
    A_P1, A_P1M1, A_P1M0,
    A_P2, A_P2M1, A_P2M0,
    A_P3, A_P3M1, A_P3M0,
    A_P4, A_P4M1, A_P4M0,
    A_P5, A_P5M1, A_P5M0,
    A_P6, A_P6M1, A_P6M0,
    A_P7, A_P7M1, A_P7M0,
};

static const MemoryRegionOps stc32g_gpio_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
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
}

static void stc32g_gpio_update_all(Stc32gGPIOState *s)
{
    unsigned port;

    for (port = 0; port < STC32G_GPIO_PORTS; port++) {
        stc32g_gpio_update_port(s, port);
    }
}

static void stc32g_gpio_reset(DeviceState *dev)
{
    Stc32gGPIOState *s = STC32G_GPIO(dev);
    unsigned index;

    for (index = 0; index < ARRAY_SIZE(stc32g_gpio_regs_info); index++) {
        register_reset(&s->regs_info[index]);
    }
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
        VMSTATE_UINT8_ARRAY(regs, Stc32gGPIOState, STC32G_GPIO_REGS),
        VMSTATE_UINT8_ARRAY(input, Stc32gGPIOState, STC32G_GPIO_PORTS),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc32g_gpio_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc32gGPIOState, cpu, TYPE_MCS51_CPU,
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
    unsigned index;

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc32g_gpio_regs_info) !=
                      STC32G_GPIO_REGS);
    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc32g_gpio_reg_offsets) !=
                      STC32G_GPIO_REGS);
    memset(s->input, 0xff, sizeof(s->input));
    memory_region_init(&s->sfr, obj, "stc32g.gpio-sfr",
                       STC32G_GPIO_SFR_SIZE);
    for (index = 0; index < ARRAY_SIZE(stc32g_gpio_regs_info); index++) {
        s->reg_array[index] = register_init_block8(
            DEVICE(obj), &stc32g_gpio_regs_info[index], 1,
            &s->regs_info[index], &s->regs[index],
            &stc32g_gpio_ops, false, 1);
        memory_region_add_subregion(
            &s->sfr, stc32g_gpio_reg_offsets[index],
            &s->reg_array[index]->mem);
    }
    sysbus_init_mmio(sbd, &s->sfr);

    qdev_init_gpio_in_named(DEVICE(obj), stc32g_gpio_set_input,
                            "gpio-in", STC32G_GPIO_PINS);
    qdev_init_gpio_out_named(DEVICE(obj), s->output, "gpio-out",
                             STC32G_GPIO_PINS);
    qdev_init_gpio_out_named(DEVICE(obj), s->int_line, "int-line",
                             STC32G_GPIO_INT_LINES);
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
