/* SPDX-License-Identifier: GPL-2.0-or-later */
/* LSM6DSV16X I2C registers and FIFO, fed by MuJoCo sensor records. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/fifo8.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/qdev-properties.h"
#include "chardev/char-fe.h"

#define TYPE_LSM6DSV16X "microduck-lsm6dsv16x"
OBJECT_DECLARE_SIMPLE_TYPE(LSM6DSV16XState, LSM6DSV16X)
struct LSM6DSV16XState {
    I2CSlave parent_obj;
    CharFrontend sensor;
    Fifo8 fifo;
    uint8_t regs[256];
    uint8_t embedded[256];
    uint8_t pointer;
    bool address_byte;
    uint8_t sample[21];
    unsigned sample_length;
};

static int sensor_can_read(void *opaque) { return 4096; }
static void sensor_receive(void *opaque, const uint8_t *bytes, int size)
{
    LSM6DSV16XState *s = opaque;
    for (int i = 0; i < size; i++) {
        s->sample[s->sample_length++] = bytes[i];
        if (s->sample_length == sizeof(s->sample)) {
            if ((s->regs[0x0a] & 7) == 6 && (s->embedded[0x44] & 2)) {
                while (fifo8_num_free(&s->fifo) < sizeof(s->sample)) {
                    fifo8_drop(&s->fifo, 7);
                }
                fifo8_push_all(&s->fifo, s->sample, sizeof(s->sample));
            }
            s->sample_length = 0;
        }
    }
}

static int imu_event(I2CSlave *dev, enum i2c_event event)
{
    LSM6DSV16XState *s = LSM6DSV16X(dev);
    if (event == I2C_START_SEND) {
        s->address_byte = true;
    }
    return 0;
}
static uint8_t imu_read(I2CSlave *dev)
{
    LSM6DSV16XState *s = LSM6DSV16X(dev);
    unsigned reg = s->pointer;
    s->pointer++;
    if (reg == 1) {
        return s->regs[1];
    }
    if (s->regs[1] & 0x80) {
        return s->embedded[reg];
    }
    if (reg == 0xf) {
        return 0x70;
    }
    if (reg == 0x1b) {
        return fifo8_num_used(&s->fifo) / 7;
    }
    if (reg == 0x1c) {
        return (fifo8_num_used(&s->fifo) / 7) >> 8;
    }
    if (reg >= 0x78 && reg <= 0x7e) {
        if (reg == 0x7e) {
            s->pointer = 0x78;
        }
        return fifo8_is_empty(&s->fifo) ? 0 : fifo8_pop(&s->fifo);
    }
    return s->regs[reg];
}
static int imu_write(I2CSlave *dev, uint8_t value)
{
    LSM6DSV16XState *s = LSM6DSV16X(dev);
    if (s->address_byte) {
        s->pointer = value;
        s->address_byte = false;
        return 0;
    }
    unsigned reg = s->pointer++;
    if (reg == 1) {
        s->regs[1] = value;
    } else if (s->regs[1] & 0x80) {
        s->embedded[reg] = value;
    } else if (reg != 0xf) {
        if (reg == 0x12 && (value & 1)) {
            memset(s->regs, 0, sizeof(s->regs));
            memset(s->embedded, 0, sizeof(s->embedded));
            fifo8_reset(&s->fifo);
        }
        s->regs[reg] = reg == 0x12 ? value & ~0x81 : value;
        if (reg == 0x0a && !(value & 7)) {
            fifo8_reset(&s->fifo);
        }
    }
    return 0;
}
static void imu_realize(DeviceState *dev, Error **errp)
{
    LSM6DSV16XState *s = LSM6DSV16X(dev);
    fifo8_create(&s->fifo, 512 * 7);
    qemu_chr_fe_set_handlers(&s->sensor, sensor_can_read, sensor_receive,
                             NULL, NULL, s, NULL, true);
}
static void imu_finalize(Object *obj)
{
    LSM6DSV16XState *s = LSM6DSV16X(obj);
    fifo8_destroy(&s->fifo);
}
static const Property imu_properties[] = {
    DEFINE_PROP_CHR("chardev", LSM6DSV16XState, sensor),
};
static void imu_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);
    ic->event = imu_event;
    ic->recv = imu_read;
    ic->send = imu_write;
    dc->realize = imu_realize;
    device_class_set_props(dc, imu_properties);
}
static const TypeInfo imu_info = {
    .name = TYPE_LSM6DSV16X, .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(LSM6DSV16XState),
    .class_init = imu_class_init, .instance_finalize = imu_finalize,
};
static void imu_register(void) { type_register_static(&imu_info); }
type_init(imu_register)
