/*
 * K230 ADC register block
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/misc/k230_adc.h"

#define K230_ADC_CFG       0x04
#define K230_ADC_DATA_BASE 0x14
#define K230_ADC_CHANNELS  6

static uint64_t k230_adc_read_bytes(uint8_t *regs, hwaddr addr,
                                    unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_adc_write_bytes(uint8_t *regs, hwaddr addr, uint64_t val,
                                 unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static uint32_t k230_adc_sample(unsigned int channel)
{
    return 0x800 + channel * 0x40;
}

static void k230_adc_update_samples(K230AdcState *s)
{
    for (int i = 0; i < K230_ADC_CHANNELS; i++) {
        stl_le_p(s->regs + K230_ADC_DATA_BASE + i * 4, k230_adc_sample(i));
    }
}

static uint64_t k230_adc_read(void *opaque, hwaddr addr, unsigned int size)
{
    return k230_adc_read_bytes(K230_ADC(opaque)->regs, addr, size);
}

static void k230_adc_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned int size)
{
    K230AdcState *s = K230_ADC(opaque);

    k230_adc_write_bytes(s->regs, addr, val, size);

    if (addr == K230_ADC_CFG && size == 4 && (val & 0x10)) {
        unsigned int channel = val & 0x7;

        if (channel < K230_ADC_CHANNELS) {
            stl_le_p(s->regs + K230_ADC_DATA_BASE + channel * 4,
                     k230_adc_sample(channel));
        }
    }
}

static const MemoryRegionOps k230_adc_ops = {
    .read = k230_adc_read,
    .write = k230_adc_write,
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

static void k230_adc_reset(DeviceState *dev)
{
    K230AdcState *s = K230_ADC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    k230_adc_update_samples(s);
}

static const VMStateDescription vmstate_k230_adc = {
    .name = TYPE_K230_ADC,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230AdcState, K230_ADC_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_adc_realize(DeviceState *dev, Error **errp)
{
    K230AdcState *s = K230_ADC(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_adc_ops, s,
                          TYPE_K230_ADC, K230_ADC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_adc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_adc_realize;
    device_class_set_legacy_reset(dc, k230_adc_reset);
    dc->vmsd = &vmstate_k230_adc;
    dc->desc = "K230 ADC register block";
}

static const TypeInfo k230_adc_type_info = {
    .name = TYPE_K230_ADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230AdcState),
    .class_init = k230_adc_class_init,
};

static void k230_register_adc_types(void)
{
    type_register_static(&k230_adc_type_info);
}

type_init(k230_register_adc_types)
