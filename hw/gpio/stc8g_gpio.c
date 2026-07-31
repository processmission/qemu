/*
 * STC8G1K08A GPIO
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
#include "hw/gpio/stc8g_gpio.h"
#include "migration/vmstate.h"
#include "target/mcs51/cpu.h"

REG8(P3, 0x00)
REG8(P3M1, 0x01)
REG8(P3M0, 0x02)
REG8(P5, 0x18)
REG8(P5M1, 0x19)
REG8(P5M0, 0x1a)

REG8(P3PU, 0x00)
REG8(P5PU, 0x02)
REG8(P3NCS, 0x08)
REG8(P5NCS, 0x0a)
REG8(P3SR, 0x10)
REG8(P5SR, 0x12)
REG8(P3DR, 0x18)
REG8(P5DR, 0x1a)
REG8(P3IE, 0x20)
REG8(P5IE, 0x22)

#define STC8G_GPIO_PORTS 2
#define STC8G_GPIO_SFR_SIZE (A_P5M0 + 1)
#define STC8G_GPIO_XFR_SIZE (A_P5IE + 1)

enum Stc8gGPIOPort {
    STC8G_GPIO_PORT3,
    STC8G_GPIO_PORT5,
};

enum Stc8gGPIORegisterKind {
    STC8G_GPIO_DATA,
    STC8G_GPIO_MODE1,
    STC8G_GPIO_MODE0,
    STC8G_GPIO_PULLUP,
    STC8G_GPIO_NCS,
    STC8G_GPIO_SLEW_RATE,
    STC8G_GPIO_DRIVE,
    STC8G_GPIO_INPUT_ENABLE,
    STC8G_GPIO_REGS_PER_PORT,
};

#define STC8G_GPIO_REGS \
    (STC8G_GPIO_PORTS * STC8G_GPIO_REGS_PER_PORT)

typedef struct Stc8gGPIORegisterDef {
    hwaddr offset;
    bool xfr;
} Stc8gGPIORegisterDef;

struct Stc8gGPIOState {
    SysBusDevice parent_obj;

    MCS51CPU *cpu;
    MemoryRegion sfr;
    MemoryRegion xfr;
    RegisterInfoArray *reg_array[STC8G_GPIO_REGS];
    RegisterInfo regs_info[STC8G_GPIO_REGS];
    qemu_irq output[STC8G_GPIO_PINS];
    qemu_irq int_line[2];
    qemu_irq counter_line[2];
    uint8_t regs[STC8G_GPIO_REGS];
    uint8_t external_level[STC8G_GPIO_PORTS];
    uint8_t external_driven[STC8G_GPIO_PORTS];
    bool resetting;
};

static const uint8_t stc8g_gpio_valid_mask[STC8G_GPIO_PORTS] = {
    [STC8G_GPIO_PORT3] = 0x0f,
    [STC8G_GPIO_PORT5] = 0x30,
};

static unsigned stc8g_gpio_reg_index(unsigned port,
                                      enum Stc8gGPIORegisterKind kind)
{
    return port * STC8G_GPIO_REGS_PER_PORT + kind;
}

static uint8_t stc8g_gpio_reg_value(Stc8gGPIOState *s, unsigned port,
                                    enum Stc8gGPIORegisterKind kind)
{
    return s->regs[stc8g_gpio_reg_index(port, kind)];
}

static bool stc8g_gpio_floating_level(Stc8gGPIOState *s, unsigned port,
                                      unsigned bit)
{
    if (extract8(s->external_driven[port], bit, 1)) {
        return extract8(s->external_level[port], bit, 1);
    }
    return extract8(stc8g_gpio_reg_value(s, port, STC8G_GPIO_PULLUP),
                    bit, 1);
}

static bool stc8g_gpio_pin_level(Stc8gGPIOState *s, unsigned port,
                                 unsigned bit)
{
    bool latch = extract8(stc8g_gpio_reg_value(s, port, STC8G_GPIO_DATA),
                          bit, 1);
    bool mode1 = extract8(stc8g_gpio_reg_value(s, port,
                                               STC8G_GPIO_MODE1), bit, 1);
    bool mode0 = extract8(stc8g_gpio_reg_value(s, port,
                                               STC8G_GPIO_MODE0), bit, 1);

    if (!mode1 && mode0) {
        return latch;
    }
    if (mode1 && !mode0) {
        return stc8g_gpio_floating_level(s, port, bit);
    }
    if (mode1 && mode0) {
        return latch ? stc8g_gpio_floating_level(s, port, bit) : false;
    }

    if (!latch) {
        return false;
    }
    if (extract8(s->external_driven[port], bit, 1)) {
        return extract8(s->external_level[port], bit, 1);
    }
    return true;
}

static void stc8g_gpio_update_outputs(Stc8gGPIOState *s)
{
    unsigned pin;

    for (pin = 0; pin < STC8G_GPIO_PINS; pin++) {
        unsigned port = pin < STC8G_GPIO_P54 ?
                        STC8G_GPIO_PORT3 : STC8G_GPIO_PORT5;
        bool level = stc8g_gpio_pin_level(s, port, pin);
        bool input_enabled = extract8(
            stc8g_gpio_reg_value(s, port, STC8G_GPIO_INPUT_ENABLE),
            pin, 1);

        qemu_set_irq(s->output[pin], level);
        if (pin == STC8G_GPIO_P32 || pin == STC8G_GPIO_P33) {
            qemu_set_irq(s->int_line[pin - STC8G_GPIO_P32],
                         input_enabled && level);
        } else if (pin == STC8G_GPIO_P54 || pin == STC8G_GPIO_P55) {
            qemu_set_irq(s->counter_line[pin - STC8G_GPIO_P54],
                         input_enabled && level);
        }
    }
}

static uint8_t stc8g_gpio_sample_port(Stc8gGPIOState *s, unsigned port)
{
    uint8_t value = 0;
    uint8_t mask = stc8g_gpio_valid_mask[port];
    unsigned bit;

    for (bit = 0; bit < 8; bit++) {
        if (extract8(mask, bit, 1) &&
            extract8(stc8g_gpio_reg_value(
                         s, port, STC8G_GPIO_INPUT_ENABLE), bit, 1)) {
            value = deposit32(value, bit, 1,
                              stc8g_gpio_pin_level(s, port, bit));
        }
    }
    return value;
}

static uint64_t stc8g_gpio_post_read(RegisterInfo *reg, uint64_t value)
{
    Stc8gGPIOState *s = STC8G_GPIO(reg->opaque);
    unsigned index = reg - s->regs_info;
    unsigned port = index / STC8G_GPIO_REGS_PER_PORT;
    unsigned kind = index % STC8G_GPIO_REGS_PER_PORT;
    uint8_t mask = stc8g_gpio_valid_mask[port];

    if (kind == STC8G_GPIO_DATA) {
        if (!s->cpu->env.direct_rmw) {
            value = stc8g_gpio_sample_port(s, port);
        }
        return (value & mask) | (uint8_t)~mask;
    }
    return value & mask;
}

static void stc8g_gpio_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gGPIOState *s = STC8G_GPIO(reg->opaque);

    if (!s->resetting) {
        stc8g_gpio_update_outputs(s);
    }
}

#define STC8G_GPIO_ACCESS_INFO(NAME, RESET, MASK)                         \
    { .name = #NAME, .addr = 0, .reset = (RESET),                        \
      .rsvd = (uint8_t)~(MASK),                                          \
      .post_read = stc8g_gpio_post_read,                                 \
      .post_write = stc8g_gpio_post_write }

static const RegisterAccessInfo stc8g_gpio_regs_info[] = {
    STC8G_GPIO_ACCESS_INFO(P3, 0xff, 0x0f),
    STC8G_GPIO_ACCESS_INFO(P3M1, 0x0c, 0x0f),
    STC8G_GPIO_ACCESS_INFO(P3M0, 0x00, 0x0f),
    STC8G_GPIO_ACCESS_INFO(P3PU, 0x00, 0x0f),
    STC8G_GPIO_ACCESS_INFO(P3NCS, 0x00, 0x0f),
    STC8G_GPIO_ACCESS_INFO(P3SR, 0x0f, 0x0f),
    STC8G_GPIO_ACCESS_INFO(P3DR, 0x0f, 0x0f),
    STC8G_GPIO_ACCESS_INFO(P3IE, 0x0f, 0x0f),
    STC8G_GPIO_ACCESS_INFO(P5, 0xff, 0x30),
    STC8G_GPIO_ACCESS_INFO(P5M1, 0x30, 0x30),
    STC8G_GPIO_ACCESS_INFO(P5M0, 0x00, 0x30),
    STC8G_GPIO_ACCESS_INFO(P5PU, 0x00, 0x30),
    STC8G_GPIO_ACCESS_INFO(P5NCS, 0x00, 0x30),
    STC8G_GPIO_ACCESS_INFO(P5SR, 0x30, 0x30),
    STC8G_GPIO_ACCESS_INFO(P5DR, 0x30, 0x30),
    STC8G_GPIO_ACCESS_INFO(P5IE, 0x30, 0x30),
};

#undef STC8G_GPIO_ACCESS_INFO

static const Stc8gGPIORegisterDef stc8g_gpio_reg_defs[] = {
    { A_P3, false }, { A_P3M1, false }, { A_P3M0, false },
    { A_P3PU, true }, { A_P3NCS, true }, { A_P3SR, true },
    { A_P3DR, true }, { A_P3IE, true },
    { A_P5, false }, { A_P5M1, false }, { A_P5M0, false },
    { A_P5PU, true }, { A_P5NCS, true }, { A_P5SR, true },
    { A_P5DR, true }, { A_P5IE, true },
};

static const MemoryRegionOps stc8g_gpio_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc8g_gpio_set_input(void *opaque, int n, int level)
{
    Stc8gGPIOState *s = opaque;
    unsigned port = n < STC8G_GPIO_P54 ?
                    STC8G_GPIO_PORT3 : STC8G_GPIO_PORT5;

    s->external_level[port] =
        deposit32(s->external_level[port], n, 1, !!level);
    s->external_driven[port] =
        deposit32(s->external_driven[port], n, 1, 1);
    stc8g_gpio_update_outputs(s);
}

static void stc8g_gpio_set_float(void *opaque, int n, int level)
{
    Stc8gGPIOState *s = opaque;
    unsigned port = n < STC8G_GPIO_P54 ?
                    STC8G_GPIO_PORT3 : STC8G_GPIO_PORT5;

    s->external_driven[port] =
        deposit32(s->external_driven[port], n, 1, !level);
    stc8g_gpio_update_outputs(s);
}

static void stc8g_gpio_reset(DeviceState *dev)
{
    Stc8gGPIOState *s = STC8G_GPIO(dev);
    unsigned index;

    s->resetting = true;
    for (index = 0; index < ARRAY_SIZE(stc8g_gpio_regs_info); index++) {
        register_reset(&s->regs_info[index]);
    }
    s->resetting = false;
    stc8g_gpio_update_outputs(s);
}

static int stc8g_gpio_post_load(void *opaque, int version_id)
{
    stc8g_gpio_update_outputs(opaque);
    return 0;
}

static const VMStateDescription stc8g_gpio_vmstate = {
    .name = "stc8g.gpio",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stc8g_gpio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Stc8gGPIOState, STC8G_GPIO_REGS),
        VMSTATE_UINT8_ARRAY(external_level, Stc8gGPIOState,
                            STC8G_GPIO_PORTS),
        VMSTATE_UINT8_ARRAY(external_driven, Stc8gGPIOState,
                            STC8G_GPIO_PORTS),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc8g_gpio_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc8gGPIOState, cpu, TYPE_MCS51_CPU,
                     MCS51CPU *),
};

static void stc8g_gpio_realize(DeviceState *dev, Error **errp)
{
    Stc8gGPIOState *s = STC8G_GPIO(dev);

    if (!s->cpu) {
        error_setg(errp, "stc8g-gpio requires a CPU link");
    }
}

static void stc8g_gpio_init(Object *obj)
{
    Stc8gGPIOState *s = STC8G_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned index;

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc8g_gpio_regs_info) !=
                      STC8G_GPIO_REGS);
    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc8g_gpio_reg_defs) !=
                      STC8G_GPIO_REGS);
    s->external_level[STC8G_GPIO_PORT3] = 0x0f;
    s->external_level[STC8G_GPIO_PORT5] = 0x30;
    s->external_driven[STC8G_GPIO_PORT3] = 0x0f;
    s->external_driven[STC8G_GPIO_PORT5] = 0x30;
    memory_region_init(&s->sfr, obj, "stc8g.gpio-sfr",
                       STC8G_GPIO_SFR_SIZE);
    memory_region_init(&s->xfr, obj, "stc8g.gpio-xfr",
                       STC8G_GPIO_XFR_SIZE);
    for (index = 0; index < ARRAY_SIZE(stc8g_gpio_regs_info); index++) {
        const Stc8gGPIORegisterDef *def = &stc8g_gpio_reg_defs[index];

        s->reg_array[index] = register_init_block8(
            DEVICE(obj), &stc8g_gpio_regs_info[index], 1,
            &s->regs_info[index], &s->regs[index],
            &stc8g_gpio_ops, false, 1);
        memory_region_add_subregion(def->xfr ? &s->xfr : &s->sfr,
                                    def->offset,
                                    &s->reg_array[index]->mem);
    }
    sysbus_init_mmio(sbd, &s->sfr);
    sysbus_init_mmio(sbd, &s->xfr);
    qdev_init_gpio_in_named(DEVICE(obj), stc8g_gpio_set_input,
                            "gpio-in", STC8G_GPIO_PINS);
    qdev_init_gpio_in_named(DEVICE(obj), stc8g_gpio_set_float,
                            "gpio-float", STC8G_GPIO_PINS);
    qdev_init_gpio_out_named(DEVICE(obj), s->output, "gpio-out",
                             STC8G_GPIO_PINS);
    qdev_init_gpio_out_named(DEVICE(obj), s->int_line, "int-line", 2);
    qdev_init_gpio_out_named(DEVICE(obj), s->counter_line,
                             "counter-line", 2);
}

static void stc8g_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc8g_gpio_realize;
    device_class_set_legacy_reset(dc, stc8g_gpio_reset);
    device_class_set_props(dc, stc8g_gpio_properties);
    dc->vmsd = &stc8g_gpio_vmstate;
}

static const TypeInfo stc8g_gpio_type = {
    .name = TYPE_STC8G_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc8gGPIOState),
    .instance_init = stc8g_gpio_init,
    .class_init = stc8g_gpio_class_init,
};

static void stc8g_gpio_register_types(void)
{
    type_register_static(&stc8g_gpio_type);
}

type_init(stc8g_gpio_register_types)
