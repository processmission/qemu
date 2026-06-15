/*
 * K230 IOMUX register block
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/misc/k230_iomux.h"

static uint64_t k230_iomux_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230IomuxState *s = K230_IOMUX(opaque);
    uint64_t val = 0;

    if (addr > K230_IOMUX_SIZE || size > K230_IOMUX_SIZE - addr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      TYPE_K230_IOMUX, addr);
        return 0;
    }

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)s->regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_iomux_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned int size)
{
    K230IomuxState *s = K230_IOMUX(opaque);

    if (addr > K230_IOMUX_SIZE || size > K230_IOMUX_SIZE - addr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      TYPE_K230_IOMUX, addr);
        return;
    }

    for (int i = 0; i < size; i++) {
        s->regs[addr + i] = val >> (i * 8);
    }
}

static const MemoryRegionOps k230_iomux_ops = {
    .read = k230_iomux_read,
    .write = k230_iomux_write,
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

static void k230_iomux_reset(DeviceState *dev)
{
    K230IomuxState *s = K230_IOMUX(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static const VMStateDescription vmstate_k230_iomux = {
    .name = TYPE_K230_IOMUX,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230IomuxState, K230_IOMUX_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_iomux_realize(DeviceState *dev, Error **errp)
{
    K230IomuxState *s = K230_IOMUX(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_iomux_ops, s,
                          TYPE_K230_IOMUX, K230_IOMUX_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_iomux_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_iomux_realize;
    device_class_set_legacy_reset(dc, k230_iomux_reset);
    dc->vmsd = &vmstate_k230_iomux;
    dc->desc = "K230 IOMUX register block";
}

static const TypeInfo k230_iomux_type_info = {
    .name = TYPE_K230_IOMUX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230IomuxState),
    .class_init = k230_iomux_class_init,
};

static void k230_iomux_register_types(void)
{
    type_register_static(&k230_iomux_type_info);
}

type_init(k230_iomux_register_types)
