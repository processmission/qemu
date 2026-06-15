/*
 * Rockchip secure timer stub
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The RK firmware only needs a writable secure timer control block and
 * a non-stuck counter while it initializes cntfrq_el0 and moves on.
 */

#include "qemu/osdep.h"
#include "hw/timer/rockchip_stimer.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define ROCKCHIP_STIMER_COUNTER_L 0x00
#define ROCKCHIP_STIMER_COUNTER_H 0x08
#define ROCKCHIP_STIMER_HZ        24000000ULL

static uint64_t rockchip_stimer_counter(void)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                    ROCKCHIP_STIMER_HZ, NANOSECONDS_PER_SECOND);
}

static uint64_t rockchip_stimer_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    RockchipSTimerState *s = opaque;

    if (offset + size > ROCKCHIP_STIMER_SIZE || (offset & 3) || size != 4) {
        return 0;
    }

    switch (offset) {
    case ROCKCHIP_STIMER_COUNTER_L:
        return (uint32_t)rockchip_stimer_counter();
    case ROCKCHIP_STIMER_COUNTER_H:
        return (uint32_t)(rockchip_stimer_counter() >> 32);
    default:
        return s->regs[offset >> 2];
    }
}

static void rockchip_stimer_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    RockchipSTimerState *s = opaque;

    if (offset + size > ROCKCHIP_STIMER_SIZE || (offset & 3) || size != 4) {
        return;
    }

    s->regs[offset >> 2] = value;
}

static const MemoryRegionOps rockchip_stimer_ops = {
    .read = rockchip_stimer_read,
    .write = rockchip_stimer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rockchip_stimer_reset(DeviceState *dev)
{
    RockchipSTimerState *s = ROCKCHIP_STIMER(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void rockchip_stimer_realize(DeviceState *dev, Error **errp)
{
    RockchipSTimerState *s = ROCKCHIP_STIMER(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &rockchip_stimer_ops, s,
                          "rockchip-stimer", ROCKCHIP_STIMER_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static void rockchip_stimer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rockchip_stimer_realize;
    device_class_set_legacy_reset(dc, rockchip_stimer_reset);
    dc->user_creatable = false;
}

static const TypeInfo rockchip_stimer_info = {
    .name = TYPE_ROCKCHIP_STIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipSTimerState),
    .class_init = rockchip_stimer_class_init,
};

static void rockchip_stimer_register_types(void)
{
    type_register_static(&rockchip_stimer_info);
}

type_init(rockchip_stimer_register_types)
