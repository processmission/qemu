/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"

#define TYPE_RK3566_TSADC "rk3566-tsadc"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566TSADCState, RK3566_TSADC)
struct RK3566TSADCState {
    SysBusDevice parent_obj;
    MemoryRegion io;
    qemu_irq irq;
    uint32_t regs[64];
    uint32_t temperature_mc;
};

static uint32_t tsadc_code(RK3566TSADCState *s)
{
    /* RK3568 sensor calibration table, 0..125 C in steps of 5 C. */
    const unsigned code[] = {
        1856, 1892, 1924, 1956, 1992, 2024, 2060, 2092, 2128,
        2160, 2196, 2228, 2264, 2300, 2332, 2368, 2400, 2436,
        2468, 2500, 2536, 2572, 2604, 2636, 2672, 2704,
    };
    unsigned temp = MIN(s->temperature_mc, 125000);
    unsigned index = MIN(temp / 5000, ARRAY_SIZE(code) - 2);
    return code[index] +
           (code[index + 1] - code[index]) * (temp - index * 5000) / 5000;
}

static void tsadc_update(RK3566TSADCState *s)
{
    if (s->regs[1] & 1) {
        for (unsigned channel = 0; channel < 2; channel++) {
            s->regs[8 + channel] = tsadc_code(s);
            if ((s->regs[2] & (1 << channel)) &&
                s->regs[8 + channel] >= s->regs[0x30 / 4 + channel]) {
                s->regs[3] |= 1 << channel;
            }
        }
    }
    qemu_set_irq(s->irq, !!(s->regs[3] & s->regs[2] & 3));
}
static uint64_t tsadc_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3566TSADCState *s = opaque;
    tsadc_update(s);
    return s->regs[offset / 4];
}
static void tsadc_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    RK3566TSADCState *s = opaque;
    if (offset == 0xc) {
        s->regs[3] &= ~value;
    } else if (offset != 0x20 && offset != 0x24) {
        s->regs[offset / 4] = value;
    }
    tsadc_update(s);
}
static const MemoryRegionOps tsadc_ops = {
    .read = tsadc_read, .write = tsadc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};
static void tsadc_init(Object *obj)
{
    RK3566TSADCState *s = RK3566_TSADC(obj);
    memory_region_init_io(&s->io, obj, &tsadc_ops, s, TYPE_RK3566_TSADC, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->io);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}
static const Property tsadc_properties[] = {
    DEFINE_PROP_UINT32("temperature-mc", RK3566TSADCState, temperature_mc,
                       25000),
};
static void tsadc_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_props(DEVICE_CLASS(oc), tsadc_properties);
}
static const TypeInfo tsadc_info = {
    .name = TYPE_RK3566_TSADC, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3566TSADCState),
    .instance_init = tsadc_init, .class_init = tsadc_class_init,
};
static void tsadc_register(void) { type_register_static(&tsadc_info); }
type_init(tsadc_register)
