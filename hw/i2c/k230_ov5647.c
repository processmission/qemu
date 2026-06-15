/*
 * K230 OV5647 camera sensor stub
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/k230_ov5647.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

OBJECT_DECLARE_SIMPLE_TYPE(K230OV5647State, K230_OV5647)

#define K230_OV5647_REGS_SIZE     0x10000
#define K230_OV5647_CHIP_ID_H     0x300a
#define K230_OV5647_CHIP_ID_L     0x300b
#define K230_OV5647_LONG_EXP_H    0x3501
#define K230_OV5647_LONG_EXP_L    0x3502
#define K230_OV5647_LONG_AGAIN_H  0x350a
#define K230_OV5647_LONG_AGAIN_L  0x350b

struct K230OV5647State {
    I2CSlave parent_obj;

    uint8_t regs[K230_OV5647_REGS_SIZE];
    uint16_t reg;
    uint8_t addr_len;
};

static void k230_ov5647_reset(DeviceState *dev)
{
    K230OV5647State *s = K230_OV5647(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[K230_OV5647_CHIP_ID_H] = 0x56;
    s->regs[K230_OV5647_CHIP_ID_L] = 0x47;
    s->regs[K230_OV5647_LONG_AGAIN_H] = 0x00;
    s->regs[K230_OV5647_LONG_AGAIN_L] = 0x10;
    s->regs[K230_OV5647_LONG_EXP_H] = 0x01;
    s->regs[K230_OV5647_LONG_EXP_L] = 0x00;
    s->reg = 0;
    s->addr_len = 0;
}

static int k230_ov5647_event(I2CSlave *i2c, enum i2c_event event)
{
    K230OV5647State *s = K230_OV5647(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->addr_len = 0;
        break;
    case I2C_START_RECV:
    case I2C_FINISH:
    case I2C_NACK:
        break;
    default:
        return -1;
    }

    return 0;
}

static uint8_t k230_ov5647_recv(I2CSlave *i2c)
{
    K230OV5647State *s = K230_OV5647(i2c);
    uint8_t value = s->regs[s->reg];

    s->reg++;
    return value;
}

static int k230_ov5647_send(I2CSlave *i2c, uint8_t data)
{
    K230OV5647State *s = K230_OV5647(i2c);

    if (s->addr_len == 0) {
        s->reg = data << 8;
        s->addr_len++;
        return 0;
    }

    if (s->addr_len == 1) {
        s->reg |= data;
        s->addr_len++;
        return 0;
    }

    s->regs[s->reg] = data;
    s->reg++;
    return 0;
}

static const VMStateDescription vmstate_k230_ov5647 = {
    .name = TYPE_K230_OV5647,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, K230OV5647State),
        VMSTATE_UINT8_ARRAY(regs, K230OV5647State, K230_OV5647_REGS_SIZE),
        VMSTATE_UINT16(reg, K230OV5647State),
        VMSTATE_UINT8(addr_len, K230OV5647State),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_ov5647_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    sc->event = k230_ov5647_event;
    sc->recv = k230_ov5647_recv;
    sc->send = k230_ov5647_send;
    device_class_set_legacy_reset(dc, k230_ov5647_reset);
    dc->vmsd = &vmstate_k230_ov5647;
    dc->desc = "K230 OV5647 camera sensor stub";
}

static const TypeInfo k230_ov5647_info = {
    .name = TYPE_K230_OV5647,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(K230OV5647State),
    .class_init = k230_ov5647_class_init,
};

static void k230_ov5647_register_types(void)
{
    type_register_static(&k230_ov5647_info);
}

type_init(k230_ov5647_register_types)
