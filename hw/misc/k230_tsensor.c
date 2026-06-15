/*
 * K230 temperature sensor
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/misc/k230_tsensor.h"

#define K230_TSENSOR_CONFIG 0x00
#define K230_TSENSOR_DATA   0x04
#define K230_TSENSOR_TEMP   42000

static uint64_t k230_tsensor_read_bytes(uint8_t *regs, hwaddr addr,
                                        unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_tsensor_write_bytes(uint8_t *regs, hwaddr addr,
                                     uint64_t val, unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static uint64_t k230_tsensor_read(void *opaque, hwaddr addr,
                                  unsigned int size)
{
    K230TSensorState *s = K230_TSENSOR(opaque);

    if (addr == K230_TSENSOR_DATA && size == 4) {
        return K230_TSENSOR_TEMP;
    }

    return k230_tsensor_read_bytes(s->regs, addr, size);
}

static void k230_tsensor_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned int size)
{
    K230TSensorState *s = K230_TSENSOR(opaque);

    k230_tsensor_write_bytes(s->regs, addr, val, size);
    if (addr == K230_TSENSOR_CONFIG && size == 4) {
        stl_le_p(s->regs + K230_TSENSOR_DATA, K230_TSENSOR_TEMP);
    }
}

static const MemoryRegionOps k230_tsensor_ops = {
    .read = k230_tsensor_read,
    .write = k230_tsensor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void k230_tsensor_reset(DeviceState *dev)
{
    K230TSensorState *s = K230_TSENSOR(dev);

    memset(s->regs, 0, sizeof(s->regs));
    stl_le_p(s->regs + K230_TSENSOR_DATA, K230_TSENSOR_TEMP);
}

static const VMStateDescription vmstate_k230_tsensor = {
    .name = TYPE_K230_TSENSOR,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230TSensorState, K230_TSENSOR_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_tsensor_realize(DeviceState *dev, Error **errp)
{
    K230TSensorState *s = K230_TSENSOR(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_tsensor_ops, s,
                          TYPE_K230_TSENSOR, K230_TSENSOR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_tsensor_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_tsensor_realize;
    device_class_set_legacy_reset(dc, k230_tsensor_reset);
    dc->vmsd = &vmstate_k230_tsensor;
    dc->desc = "K230 temperature sensor";
}

static const TypeInfo k230_tsensor_type_info = {
    .name = TYPE_K230_TSENSOR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230TSensorState),
    .class_init = k230_tsensor_class_init,
};

static void k230_register_tsensor_types(void)
{
    type_register_static(&k230_tsensor_type_info);
}

type_init(k230_register_tsensor_types)
