/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"

#define TYPE_RK3566_PWM "rk3566-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566PWMState, RK3566_PWM)
struct RK3566PWMState {
    SysBusDevice parent_obj;
    MemoryRegion io;
    uint32_t regs[64];
    uint32_t frequency;
    int64_t epoch[4];
};
static uint64_t pwm_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3566PWMState *s = opaque;
    unsigned channel = offset / 16;
    if (channel < 4 && offset % 16 == 0 && (s->regs[channel * 4 + 3] & 1)) {
        unsigned period = s->regs[channel * 4 + 1];
        uint64_t ticks = muldiv64(
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->epoch[channel],
            s->frequency, NANOSECONDS_PER_SECOND);
        return period ? ticks % period : 0;
    }
    return s->regs[offset / 4];
}
static void pwm_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    RK3566PWMState *s = opaque;
    unsigned channel = offset / 16;
    if (channel < 4 && offset % 16 == 12 && !(s->regs[offset / 4] & 1) &&
        (value & 1)) {
        s->epoch[channel] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    s->regs[offset / 4] = value;
}
static const MemoryRegionOps pwm_ops = {
    .read = pwm_read, .write = pwm_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};
static void pwm_init(Object *obj)
{
    RK3566PWMState *s = RK3566_PWM(obj);
    memory_region_init_io(&s->io, obj, &pwm_ops, s, TYPE_RK3566_PWM, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->io);
}
static const Property pwm_properties[] = {
    DEFINE_PROP_UINT32("clock-hz", RK3566PWMState, frequency, 24000000),
};
static void pwm_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_props(DEVICE_CLASS(oc), pwm_properties);
}
static const TypeInfo pwm_info = {
    .name = TYPE_RK3566_PWM, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3566PWMState),
    .instance_init = pwm_init, .class_init = pwm_class_init,
};
static void pwm_register(void) { type_register_static(&pwm_info); }
type_init(pwm_register)
