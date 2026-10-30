/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "system/rtc.h"
#include "migration/vmstate.h"

#define TYPE_MICRODUCK_I2C "microduck-register-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(MicroDuckI2CState, MICRODUCK_I2C)
enum { RK809, SYR827, PCA9535 };
struct MicroDuckI2CState {
    I2CSlave parent_obj;
    uint8_t regs[256];
    uint8_t pointer;
    bool address_byte;
    unsigned kind;
    qemu_irq nirq;
    int64_t rtc_offset;
};

static void update_irq(MicroDuckI2CState *s)
{
    unsigned pending = 0;
    if (s->kind == RK809) {
        for (unsigned reg = 0xf8; reg <= 0xfc; reg += 2) {
            pending |= s->regs[reg] & ~s->regs[reg + 1];
        }
    }
    qemu_set_irq(s->nirq, !pending);
}

static uint8_t to_bcd(unsigned value)
{
    return ((value / 10) << 4) | value % 10;
}

static void rtc_sample(MicroDuckI2CState *s)
{
    struct tm tm;
    qemu_get_timedate(&tm, s->rtc_offset);
    s->regs[0] = to_bcd(tm.tm_sec);
    s->regs[1] = to_bcd(tm.tm_min);
    s->regs[2] = to_bcd(tm.tm_hour);
    s->regs[3] = to_bcd(tm.tm_mday);
    s->regs[4] = to_bcd(tm.tm_mon + 1);
    s->regs[5] = to_bcd(tm.tm_year % 100);
    s->regs[6] = tm.tm_wday;
}

static unsigned from_bcd(uint8_t value)
{
    return (value >> 4) * 10 + (value & 0xf);
}

static void rtc_set_time(MicroDuckI2CState *s)
{
    struct tm tm = {
        .tm_sec = from_bcd(s->regs[0] & 0x7f),
        .tm_min = from_bcd(s->regs[1] & 0x7f),
        .tm_hour = from_bcd(s->regs[2] & 0x3f),
        .tm_mday = from_bcd(s->regs[3] & 0x3f),
        .tm_mon = from_bcd(s->regs[4] & 0x1f) - 1,
        .tm_year = from_bcd(s->regs[5]) + 100,
        .tm_isdst = -1,
    };

    s->rtc_offset = qemu_timedate_diff(&tm);
}

static int i2c_event_handler(I2CSlave *dev, enum i2c_event event)
{
    MicroDuckI2CState *s = MICRODUCK_I2C(dev);
    if (event == I2C_START_SEND) {
        s->address_byte = true;
    }
    if (event == I2C_START_RECV && s->kind == RK809 && !(s->regs[0xd] & 1)) {
        rtc_sample(s);
    }
    return 0;
}

static uint8_t i2c_read_byte(I2CSlave *dev)
{
    MicroDuckI2CState *s = MICRODUCK_I2C(dev);
    unsigned reg = s->pointer++;
    if (s->kind == PCA9535 && reg < 2) {
        return (s->regs[2 + reg] | s->regs[6 + reg]) ^ s->regs[4 + reg];
    }
    return s->regs[reg];
}

static int i2c_write_byte(I2CSlave *dev, uint8_t value)
{
    MicroDuckI2CState *s = MICRODUCK_I2C(dev);
    if (s->address_byte) {
        s->pointer = value;
        s->address_byte = false;
        return 0;
    }
    unsigned reg = s->pointer++;
    if (s->kind == RK809) {
        if (reg == 0xd) {
            if (!(s->regs[reg] & 1) && (value & 1)) {
                rtc_sample(s);
            } else if ((s->regs[reg] & 1) && !(value & 1)) {
                rtc_set_time(s);
            }
        }
        if (reg == 0xed || reg == 0xee) {
            return 0;
        }
        if (reg >= 0xb1 && reg <= 0xb4) {
            unsigned mask = value >> 4;
            s->regs[reg] = (s->regs[reg] & ~mask) | (value & mask);
            return 0;
        }
        if (reg == 0xe || reg == 0xf8 || reg == 0xfa || reg == 0xfc) {
            s->regs[reg] &= ~value;
            update_irq(s);
            return 0;
        }
    }
    if (s->kind == SYR827 && (reg == 3 || reg == 4)) {
        return 0;
    }
    if (s->kind != PCA9535 || (reg >= 2 && reg <= 7)) {
        s->regs[reg] = value;
    }
    update_irq(s);
    return 0;
}

static void i2c_reset(DeviceState *dev)
{
    MicroDuckI2CState *s = MICRODUCK_I2C(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->pointer = 0;
    s->address_byte = true;
    if (s->kind == RK809) {
        s->regs[0xed] = 0x80;
        s->regs[0xee] = 0x90;
        for (unsigned i = 0xb1; i <= 0xb4; i++) {
            s->regs[i] = 0x0f;
        }
        s->regs[0xbb] = s->regs[0xbe] = s->regs[0xc1] = 50;
        s->regs[0xc4] = 30;
        rtc_sample(s);
    } else if (s->kind == SYR827) {
        s->regs[0] = s->regs[1] = 0x80 | 23;
        s->regs[3] = 8;
    } else {
        s->regs[0] = s->regs[1] = s->regs[2] = s->regs[3] = 0xff;
        s->regs[6] = s->regs[7] = 0xff;
    }
    update_irq(s);
}

static void i2c_init(Object *obj)
{
    MicroDuckI2CState *s = MICRODUCK_I2C(obj);
    qdev_init_gpio_out_named(DEVICE(s), &s->nirq, "nirq", 1);
}

static const Property i2c_properties[] = {
    DEFINE_PROP_UINT32("kind", MicroDuckI2CState, kind, RK809),
};
static const VMStateDescription vmstate_i2c = {
    .name = TYPE_MICRODUCK_I2C, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, MicroDuckI2CState, 256),
        VMSTATE_UINT8(pointer, MicroDuckI2CState),
        VMSTATE_BOOL(address_byte, MicroDuckI2CState),
        VMSTATE_INT64(rtc_offset, MicroDuckI2CState),
        VMSTATE_END_OF_LIST()
    },
};
static void i2c_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);
    ic->event = i2c_event_handler;
    ic->recv = i2c_read_byte;
    ic->send = i2c_write_byte;
    dc->vmsd = &vmstate_i2c;
    device_class_set_props(dc, i2c_properties);
    device_class_set_legacy_reset(dc, i2c_reset);
}
static const TypeInfo i2c_info = {
    .name = TYPE_MICRODUCK_I2C, .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(MicroDuckI2CState), .class_init = i2c_class_init,
    .instance_init = i2c_init,
};
static void i2c_register(void) { type_register_static(&i2c_info); }
type_init(i2c_register)
