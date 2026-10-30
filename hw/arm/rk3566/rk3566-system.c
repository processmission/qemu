/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/guest-random.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"

#define TYPE_RK3566_SYSTEM "rk3566-system"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566SystemState, RK3566_SYSTEM)
struct RK3566SystemState {
    SysBusDevice parent_obj;
    MemoryRegion io;
    uint32_t regs[0x2000 / 4];
    bool rng;
    bool pvtm;
    uint32_t oscillator_hz;
    int64_t deadline;
};
static uint64_t system_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3566SystemState *s = opaque;
    if (s->pvtm && (s->regs[1] & 3) == 3 &&
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->deadline) {
        s->regs[0x80 / 4] = 1;
        s->regs[0x84 / 4] = (uint64_t)s->regs[2] * s->oscillator_hz / 24000000;
    }
    return s->regs[offset / 4];
}
static void system_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    RK3566SystemState *s = opaque;
    uint32_t *reg = &s->regs[offset / 4];
    uint32_t mask = value >> 16;
    if ((s->rng && offset != 0x400) || (s->pvtm && offset == 8)) {
        *reg = value;
    } else {
        *reg = (*reg & ~mask) | (value & mask);
    }
    if (s->pvtm) {
        if (offset == 4 && (*reg & 3) == 3) {
            s->regs[0x80 / 4] = 0;
            s->deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                (uint64_t)s->regs[2] * NANOSECONDS_PER_SECOND / 24000000;
        }
    } else if (s->rng) {
        if (offset == 0x400 && (*reg & 3) == 3) {
            if (!qemu_guest_getrandom(&s->regs[0x410 / 4], 32, &error_fatal)) {
                *reg &= ~1;
            }
        }
    } else {
        if (offset == 0x50) {
            s->regs[0x60 / 4] = *reg;
            s->regs[0x68 / 4] = *reg;
        }
        if (offset == 0xa0) {
            s->regs[0x98 / 4] = *reg;
        }
    }
}
static const MemoryRegionOps system_ops = {
    .read = system_read, .write = system_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};
static void system_init(Object *obj)
{
    RK3566SystemState *s = RK3566_SYSTEM(obj);
    memory_region_init_io(&s->io, obj, &system_ops, s, TYPE_RK3566_SYSTEM,
                          0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->io);
}
static const Property system_properties[] = {
    DEFINE_PROP_BOOL("rng", RK3566SystemState, rng, false),
    DEFINE_PROP_BOOL("pvtm", RK3566SystemState, pvtm, false),
    DEFINE_PROP_UINT32("oscillator-hz", RK3566SystemState, oscillator_hz,
                       84000000),
};
static void system_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_props(DEVICE_CLASS(oc), system_properties);
}
static const TypeInfo system_info = {
    .name = TYPE_RK3566_SYSTEM, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3566SystemState),
    .instance_init = system_init, .class_init = system_class_init,
};
static void system_register(void) { type_register_static(&system_info); }
type_init(system_register)
