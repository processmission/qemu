/*
 * Rockchip RAM-backed syscon register bank
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Rockchip GRF/IOC/firewall style syscons are mostly side-effect
 * configuration registers. The common write convention is a 32-bit
 * HIWORD update: bits 31:16 are the write mask, bits 15:0 carry the
 * data. If no high-half mask is present, store the full value.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/rockchip_syscon.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"

static uint64_t rockchip_syscon_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    RockchipSysconState *s = opaque;
    uint32_t val;

    if (offset + size > s->size || (offset & 3) || size != 4) {
        return 0;
    }

    val = s->regs[offset >> 2];
    return val;
}

static void rockchip_syscon_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    RockchipSysconState *s = opaque;
    uint32_t old, mask, val = value;

    if (offset + size > s->size || (offset & 3) || size != 4) {
        return;
    }

    old = s->regs[offset >> 2];
    mask = val >> 16;
    if (mask) {
        s->regs[offset >> 2] = (old & ~mask) | (val & mask);
    } else {
        s->regs[offset >> 2] = val;
    }
}

static const MemoryRegionOps rockchip_syscon_ops = {
    .read = rockchip_syscon_read,
    .write = rockchip_syscon_write,
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

void rockchip_syscon_set_u32(RockchipSysconState *s, hwaddr offset,
                             uint32_t value)
{
    if (offset + sizeof(uint32_t) > s->size || (offset & 3)) {
        return;
    }

    s->regs[offset >> 2] = value;
}

uint32_t rockchip_syscon_get_u32(RockchipSysconState *s, hwaddr offset)
{
    if (offset + sizeof(uint32_t) > s->size || (offset & 3)) {
        return 0;
    }

    return s->regs[offset >> 2];
}

static void rockchip_syscon_reset(DeviceState *dev)
{
    RockchipSysconState *s = ROCKCHIP_SYSCON(dev);

    memset(s->regs, 0, s->size);
}

static void rockchip_syscon_realize(DeviceState *dev, Error **errp)
{
    RockchipSysconState *s = ROCKCHIP_SYSCON(dev);

    if (!s->size || (s->size & 3)) {
        error_setg(errp, "rockchip-syscon size must be a non-zero "
                   "multiple of 4");
        return;
    }

    s->regs = g_new0(uint32_t, s->size / sizeof(uint32_t));
    memory_region_init_io(&s->iomem, OBJECT(s), &rockchip_syscon_ops, s,
                          "rockchip-syscon", s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static void rockchip_syscon_finalize(Object *obj)
{
    RockchipSysconState *s = ROCKCHIP_SYSCON(obj);

    g_free(s->regs);
}

static const Property rockchip_syscon_properties[] = {
    DEFINE_PROP_UINT32("size", RockchipSysconState, size, 0x1000),
};

static void rockchip_syscon_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rockchip_syscon_realize;
    device_class_set_legacy_reset(dc, rockchip_syscon_reset);
    dc->user_creatable = false;
    device_class_set_props(dc, rockchip_syscon_properties);
}

static const TypeInfo rockchip_syscon_info = {
    .name = TYPE_ROCKCHIP_SYSCON,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipSysconState),
    .instance_finalize = rockchip_syscon_finalize,
    .class_init = rockchip_syscon_class_init,
};

static void rockchip_syscon_register_types(void)
{
    type_register_static(&rockchip_syscon_info);
}

type_init(rockchip_syscon_register_types)
