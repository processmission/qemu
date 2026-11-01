/*
 * GC2093 camera sensor
 *
 * I2C register interface and streaming control for the GC2093 sensor.
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

struct GC2093State {
    I2CSlave parent_obj;
    uint8_t regs[0x10000];
    uint16_t address;
    uint8_t address_bytes;
    qemu_irq streaming;
    bool powered, resetn;
};

static void gc2093_reset(DeviceState *device)
{
    GC2093State *s = GC2093(device);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0x3f0] = 0x20;
    s->regs[0x3f1] = 0x93;
    s->regs[0x3e] = 0x11;
    s->address = s->address_bytes = 0;
    qemu_set_irq(s->streaming, 0);
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

static int gc2093_send(I2CSlave *slave, uint8_t value)
{
    GC2093State *s = GC2093(slave);
    if (s->address_bytes < 2) {
        s->address = (s->address << 8) | value;
        s->address_bytes++;
    } else {
        if (s->address == 0x3fe && value == 0xf0) {
            gc2093_reset(DEVICE(s));
            return 0;
        }
        if (s->address != 0x3f0 && s->address != 0x3f1) {
            s->regs[s->address] = value;
        }
        if (s->address == 0x3e) {
            qemu_set_irq(s->streaming,
                         s->powered && s->resetn && value == 0x91);
        }
        s->address++;
    }
    return 0;
}

static uint8_t gc2093_recv(I2CSlave *slave)
{
    GC2093State *s = GC2093(slave);
    return s->regs[s->address++];
}

static void gc2093_power(void *opaque, int line, int level)
{
    GC2093State *s = GC2093(opaque);
    s->powered = level;
    if (!level) {
        gc2093_reset(DEVICE(s));
    }
}

static void gc2093_resetn(void *opaque, int line, int level)
{
    GC2093State *s = GC2093(opaque);
    s->resetn = level;
    if (!level) {
        gc2093_reset(DEVICE(s));
    }
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
