/*
 * GC2093 camera sensor
 *
 * I2C register interface, reset and streaming control for the GC2093 sensor.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"

#define TYPE_GC2093 "microduck-gc2093"
OBJECT_DECLARE_SIMPLE_TYPE(GC2093State, GC2093)

#define GC2093_REG_CHIP_ID_H    0x03f0
#define GC2093_REG_CHIP_ID_L    0x03f1
#define GC2093_REG_CTRL_MODE    0x003e
#define GC2093_REG_MIRROR_FLIP  0x0017
#define GC2093_REG_EXP_LONG_H   0x0003
#define GC2093_REG_EXP_LONG_L   0x0004
#define GC2093_REG_VTS_H        0x0041
#define GC2093_REG_VTS_L        0x0042
#define GC2093_REG_SOFT_RESET   0x03fe

#define GC2093_CHIP_ID_H        0x20
#define GC2093_CHIP_ID_L        0x93
#define GC2093_MODE_STREAMING   0x91
#define GC2093_MODE_STANDBY     0x11
#define GC2093_SOFT_RESET       0xf0
#define GC2093_HI_BYTE_MASK     0x3f

struct GC2093State {
    I2CSlave parent_obj;
    uint8_t regs[0x10000];
    uint16_t address;
    uint8_t address_bytes;
    qemu_irq streaming;
    bool powered, resetn;
};

static bool gc2093_is_streaming(GC2093State *s)
{
    return s->powered && s->resetn &&
           s->regs[GC2093_REG_CTRL_MODE] == GC2093_MODE_STREAMING;
}

static void gc2093_update_streaming(GC2093State *s)
{
    qemu_set_irq(s->streaming, gc2093_is_streaming(s));
}

static void gc2093_reset_registers(GC2093State *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[GC2093_REG_CHIP_ID_H] = GC2093_CHIP_ID_H;
    s->regs[GC2093_REG_CHIP_ID_L] = GC2093_CHIP_ID_L;
    s->regs[GC2093_REG_CTRL_MODE] = GC2093_MODE_STANDBY;
    s->address = 0;
    s->address_bytes = 0;
}

static void gc2093_reset(DeviceState *device)
{
    GC2093State *s = GC2093(device);

    gc2093_reset_registers(s);
    gc2093_update_streaming(s);
}

static int gc2093_event(I2CSlave *slave, enum i2c_event event)
{
    GC2093State *s = GC2093(slave);

    if ((event == I2C_START_SEND || event == I2C_START_RECV) &&
        (!s->powered || !s->resetn)) {
        return 1;
    }
    if (event == I2C_START_SEND) {
        s->address_bytes = 0;
    }
    return 0;
}

static void gc2093_write_reg(GC2093State *s, uint16_t reg, uint8_t value)
{
    switch (reg) {
    case GC2093_REG_CHIP_ID_H:
    case GC2093_REG_CHIP_ID_L:
        /* Chip identification stays read-only. */
        return;
    case GC2093_REG_CTRL_MODE:
        s->regs[reg] = value;
        gc2093_update_streaming(s);
        return;
    case GC2093_REG_EXP_LONG_H:
    case GC2093_REG_VTS_H:
        s->regs[reg] = value & GC2093_HI_BYTE_MASK;
        return;
    case GC2093_REG_MIRROR_FLIP:
        /* Bit 0 mirrors the image, bit 1 flips it. */
        s->regs[reg] = value;
        return;
    default:
        s->regs[reg] = value;
        return;
    }
}

static int gc2093_send(I2CSlave *slave, uint8_t value)
{
    GC2093State *s = GC2093(slave);

    if (s->address_bytes < 2) {
        s->address = (s->address << 8) | value;
        s->address_bytes++;
        return 0;
    }

    if (s->address == GC2093_REG_SOFT_RESET && value == GC2093_SOFT_RESET) {
        gc2093_reset_registers(s);
        gc2093_update_streaming(s);
        return 0;
    }

    gc2093_write_reg(s, s->address, value);
    s->address++;
    return 0;
}

static uint8_t gc2093_recv(I2CSlave *slave)
{
    GC2093State *s = GC2093(slave);
    uint8_t value = s->regs[s->address];

    s->address++;
    return value;
}

static void gc2093_power(void *opaque, int line, int level)
{
    GC2093State *s = GC2093(opaque);

    s->powered = level;
    if (!level) {
        gc2093_reset_registers(s);
    }
    gc2093_update_streaming(s);
}

static void gc2093_resetn(void *opaque, int line, int level)
{
    GC2093State *s = GC2093(opaque);

    s->resetn = level;
    if (!level) {
        gc2093_reset_registers(s);
    }
    gc2093_update_streaming(s);
}

static void gc2093_init(Object *object)
{
    qdev_init_gpio_out_named(DEVICE(object), &GC2093(object)->streaming,
                             "streaming", 1);
    qdev_init_gpio_in_named(DEVICE(object), gc2093_power, "power", 1);
    qdev_init_gpio_in_named(DEVICE(object), gc2093_resetn, "resetn", 1);
}

static const VMStateDescription gc2093_vmstate = {
    .name = TYPE_GC2093, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, GC2093State),
        VMSTATE_UINT8_ARRAY(regs, GC2093State, 0x10000),
        VMSTATE_UINT16(address, GC2093State),
        VMSTATE_UINT8(address_bytes, GC2093State),
        VMSTATE_BOOL(powered, GC2093State),
        VMSTATE_BOOL(resetn, GC2093State),
        VMSTATE_END_OF_LIST()
    },
};

static void gc2093_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *device = DEVICE_CLASS(class);
    I2CSlaveClass *slave = I2C_SLAVE_CLASS(class);

    device_class_set_legacy_reset(device, gc2093_reset);
    device->vmsd = &gc2093_vmstate;
    slave->event = gc2093_event;
    slave->send = gc2093_send;
    slave->recv = gc2093_recv;
}

static const TypeInfo gc2093_types[] = {
    { .name = TYPE_GC2093, .parent = TYPE_I2C_SLAVE,
      .instance_size = sizeof(GC2093State), .instance_init = gc2093_init,
      .class_init = gc2093_class_init },
};

DEFINE_TYPES(gc2093_types)
