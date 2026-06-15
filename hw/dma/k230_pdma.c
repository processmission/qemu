/*
 * K230 PDMA controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/dma/k230_pdma.h"

static uint64_t k230_pdma_read_bytes(uint8_t *regs, hwaddr addr,
                                     unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_pdma_write_bytes(uint8_t *regs, hwaddr addr, uint64_t val,
                                  unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static uint64_t k230_pdma_read(void *opaque, hwaddr addr, unsigned int size)
{
    return k230_pdma_read_bytes(K230_PDMA(opaque)->regs, addr, size);
}

static void k230_pdma_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned int size)
{
    k230_pdma_write_bytes(K230_PDMA(opaque)->regs, addr, val, size);
}

static const MemoryRegionOps k230_pdma_ops = {
    .read = k230_pdma_read,
    .write = k230_pdma_write,
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

static void k230_pdma_reset(DeviceState *dev)
{
    K230PdmaState *s = K230_PDMA(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static const VMStateDescription vmstate_k230_pdma = {
    .name = TYPE_K230_PDMA,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230PdmaState, K230_PDMA_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_pdma_realize(DeviceState *dev, Error **errp)
{
    K230PdmaState *s = K230_PDMA(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_pdma_ops, s,
                          TYPE_K230_PDMA, K230_PDMA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_pdma_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_pdma_realize;
    device_class_set_legacy_reset(dc, k230_pdma_reset);
    dc->vmsd = &vmstate_k230_pdma;
    dc->desc = "K230 PDMA controller";
}

static const TypeInfo k230_pdma_type_info = {
    .name = TYPE_K230_PDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230PdmaState),
    .class_init = k230_pdma_class_init,
};

static void k230_register_pdma_types(void)
{
    type_register_static(&k230_pdma_type_info);
}

type_init(k230_register_pdma_types)
