/*
 * K230 DW200 dewarp engine
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/misc/k230_dewarp.h"
#include "migration/vmstate.h"
#include "trace.h"

#define K230_DWE_CTRL            0xc04
#define K230_DWE_CTRL_LOW        0x004
#define K230_DWE_DMA_START0      0x010
#define K230_DWE_DMA_START1      0x014
#define K230_DWE_IRQ_STATUS      0xc70
#define K230_DWE_IRQ_STATUS_LOW  0x070
#define K230_DWE_BUS_CTRL_LOW    0x074
#define K230_DWE_IRQ_CLEAR       0xd00
#define K230_FE_START            0xd04
#define K230_FE_IRQ_STATUS       0xd14
#define K230_FE_IRQ_CLEAR        0xd18
#define K230_VSE_CTRL            0x004
#define K230_VSE_DMA_TRIGGER     0x9e8
#define K230_VSE_IRQ_STATUS      0xa50
#define K230_VSE_IRQ_CLEAR       0xa58

#define K230_DWE_START           BIT(1)
#define K230_FE_START_CMD        BIT(16)
#define K230_VSE_START_DMA       BIT(14)

static bool k230_dewarp_access_hits(hwaddr addr, unsigned int size,
                                    hwaddr offset)
{
    return addr <= offset && offset < addr + size;
}

static uint32_t k230_dewarp_read32(K230DewarpState *s, hwaddr addr)
{
    uint32_t val = 0;

    if (addr > K230_DEWARP_SIZE - sizeof(val)) {
        return 0;
    }

    for (int i = 0; i < 4; i++) {
        val |= (uint32_t)s->regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_dewarp_write32(K230DewarpState *s, hwaddr addr, uint32_t val)
{
    if (addr > K230_DEWARP_SIZE - sizeof(val)) {
        return;
    }

    for (int i = 0; i < 4; i++) {
        s->regs[addr + i] = val >> (i * 8);
    }
}

static void k230_dewarp_set_irq(qemu_irq irq, bool *level, bool value)
{
    *level = value;
    qemu_set_irq(irq, value);
}

static void k230_dewarp_raise_dwe(K230DewarpState *s)
{
    k230_dewarp_write32(s, K230_DWE_IRQ_STATUS, 1);
    k230_dewarp_write32(s, K230_DWE_IRQ_STATUS_LOW, 1);
    trace_k230_dewarp_irq("dwe", true);
    k230_dewarp_set_irq(s->dwe_irq, &s->dwe_irq_level, true);
}

static void k230_dewarp_raise_fe(K230DewarpState *s)
{
    k230_dewarp_write32(s, K230_FE_IRQ_STATUS, 1);
    trace_k230_dewarp_irq("fe", true);
    k230_dewarp_set_irq(s->fe_irq, &s->fe_irq_level, true);
}

static void k230_dewarp_raise_vse(K230DewarpState *s)
{
    k230_dewarp_write32(s, K230_VSE_IRQ_STATUS, 0x7);
    trace_k230_dewarp_irq("vse", true);
    k230_dewarp_set_irq(s->vse_irq, &s->vse_irq_level, true);
}

static void k230_dewarp_clear_dwe(K230DewarpState *s)
{
    k230_dewarp_write32(s, K230_DWE_IRQ_STATUS, 0);
    k230_dewarp_write32(s, K230_DWE_IRQ_STATUS_LOW, 0);
    trace_k230_dewarp_irq("dwe", false);
    k230_dewarp_set_irq(s->dwe_irq, &s->dwe_irq_level, false);
}

static void k230_dewarp_clear_fe(K230DewarpState *s)
{
    k230_dewarp_write32(s, K230_FE_IRQ_STATUS, 0);
    trace_k230_dewarp_irq("fe", false);
    k230_dewarp_set_irq(s->fe_irq, &s->fe_irq_level, false);
}

static void k230_dewarp_clear_vse(K230DewarpState *s)
{
    k230_dewarp_write32(s, K230_VSE_IRQ_STATUS, 0);
    trace_k230_dewarp_irq("vse", false);
    k230_dewarp_set_irq(s->vse_irq, &s->vse_irq_level, false);
}

static uint64_t k230_dewarp_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230DewarpState *s = K230_DEWARP(opaque);
    uint64_t val = 0;

    if (addr >= K230_DEWARP_SIZE || size > K230_DEWARP_SIZE - addr) {
        return 0;
    }

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)s->regs[addr + i] << (i * 8);
    }

    trace_k230_dewarp_read(addr, val, size);

    return val;
}

static void k230_dewarp_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned int size)
{
    K230DewarpState *s = K230_DEWARP(opaque);
    bool dwe_start;

    if (addr >= K230_DEWARP_SIZE || size > K230_DEWARP_SIZE - addr) {
        return;
    }

    for (int i = 0; i < size; i++) {
        s->regs[addr + i] = val >> (i * 8);
    }

    trace_k230_dewarp_write(addr, val, size);

    if (k230_dewarp_access_hits(addr, size, K230_DWE_IRQ_CLEAR) && val) {
        k230_dewarp_clear_dwe(s);
    }
    if (k230_dewarp_access_hits(addr, size, K230_DWE_IRQ_STATUS_LOW) && val) {
        k230_dewarp_clear_dwe(s);
    }
    if (k230_dewarp_access_hits(addr, size, K230_FE_IRQ_CLEAR) && val) {
        k230_dewarp_clear_fe(s);
    }
    if (k230_dewarp_access_hits(addr, size, K230_VSE_IRQ_CLEAR) && val) {
        k230_dewarp_clear_vse(s);
    }

    if (k230_dewarp_access_hits(addr, size, K230_FE_START) &&
        (k230_dewarp_read32(s, K230_FE_START) & K230_FE_START_CMD)) {
        k230_dewarp_raise_fe(s);
    }

    dwe_start = k230_dewarp_access_hits(addr, size, K230_DWE_CTRL) &&
                (k230_dewarp_read32(s, K230_DWE_CTRL) & K230_DWE_START);
    if (dwe_start ||
        (k230_dewarp_access_hits(addr, size, K230_DWE_CTRL_LOW) &&
         (k230_dewarp_read32(s, K230_DWE_CTRL_LOW) & K230_DWE_START))) {
        k230_dewarp_raise_dwe(s);
        if (dwe_start) {
            k230_dewarp_raise_vse(s);
        }
    }

    if ((k230_dewarp_access_hits(addr, size, K230_VSE_CTRL) &&
         (k230_dewarp_read32(s, K230_VSE_CTRL) & K230_VSE_START_DMA)) ||
        (k230_dewarp_access_hits(addr, size, K230_VSE_DMA_TRIGGER) && val)) {
        k230_dewarp_raise_vse(s);
    }

    if (k230_dewarp_access_hits(addr, size, K230_DWE_BUS_CTRL_LOW) &&
        (val & BIT(31))) {
        k230_dewarp_write32(s, K230_DWE_BUS_CTRL_LOW,
                            k230_dewarp_read32(s, K230_DWE_BUS_CTRL_LOW) |
                            BIT(31));
    }
}

static const MemoryRegionOps k230_dewarp_ops = {
    .read = k230_dewarp_read,
    .write = k230_dewarp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

static void k230_dewarp_reset(DeviceState *dev)
{
    K230DewarpState *s = K230_DEWARP(dev);

    memset(s->regs, 0, sizeof(s->regs));
    k230_dewarp_set_irq(s->dwe_irq, &s->dwe_irq_level, false);
    k230_dewarp_set_irq(s->fe_irq, &s->fe_irq_level, false);
    k230_dewarp_set_irq(s->vse_irq, &s->vse_irq_level, false);
}

static const VMStateDescription vmstate_k230_dewarp = {
    .name = TYPE_K230_DEWARP,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(dwe_irq_level, K230DewarpState),
        VMSTATE_BOOL(fe_irq_level, K230DewarpState),
        VMSTATE_BOOL(vse_irq_level, K230DewarpState),
        VMSTATE_UINT8_ARRAY(regs, K230DewarpState, K230_DEWARP_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_dewarp_realize(DeviceState *dev, Error **errp)
{
    K230DewarpState *s = K230_DEWARP(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_dewarp_ops, s,
                          TYPE_K230_DEWARP, K230_DEWARP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->dwe_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->fe_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->vse_irq);
}

static void k230_dewarp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_dewarp_realize;
    device_class_set_legacy_reset(dc, k230_dewarp_reset);
    dc->vmsd = &vmstate_k230_dewarp;
    dc->desc = "K230 DW200 dewarp engine";
}

static const TypeInfo k230_dewarp_type_info = {
    .name = TYPE_K230_DEWARP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230DewarpState),
    .class_init = k230_dewarp_class_init,
};

static void k230_dewarp_register_types(void)
{
    type_register_static(&k230_dewarp_type_info);
}

type_init(k230_dewarp_register_types)
