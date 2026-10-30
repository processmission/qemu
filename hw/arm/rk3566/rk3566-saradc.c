/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

#define TYPE_RK3566_SARADC "rk3566-saradc"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566SARADCState, RK3566_SARADC)
struct RK3566SARADCState {
    SysBusDevice parent_obj;
    MemoryRegion io;
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t data, control, delay;
    uint32_t inputs_uv[8];
    uint32_t reference_uv;
};
static void saradc_complete(void *opaque)
{
    RK3566SARADCState *s = opaque;
    s->data = MIN((uint64_t)s->inputs_uv[s->control & 7] * 1023 /
                  s->reference_uv, 1023);
    s->control |= 0x40;
    qemu_set_irq(s->irq, (s->control & 0x20) != 0);
}
static uint64_t saradc_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3566SARADCState *s = opaque;
    switch (offset) {
    case 0: return s->data;
    case 4: return timer_pending(s->timer);
    case 8: return s->control;
    case 12: return s->delay;
    default: return 0;
    }
}
static void saradc_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    RK3566SARADCState *s = opaque;
    if (offset == 8) {
        s->control = value & 0x2f;
        qemu_set_irq(s->irq, 0);
        timer_del(s->timer);
        if (value & 8) {
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      (s->delay + 12) * 1000);
        }
    } else if (offset == 12) {
        s->delay = value & 0x3f;
    }
}
static const MemoryRegionOps saradc_ops = {
    .read = saradc_read, .write = saradc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};
static void saradc_init(Object *obj)
{
    RK3566SARADCState *s = RK3566_SARADC(obj);
    memory_region_init_io(&s->io, obj, &saradc_ops, s, TYPE_RK3566_SARADC,
                          0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->io);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, saradc_complete, s);
}

static void saradc_realize(DeviceState *dev, Error **errp)
{
    RK3566SARADCState *s = RK3566_SARADC(dev);

    if (!s->reference_uv) {
        error_setg(errp, "SARADC reference voltage must be greater than zero");
    }
}

static void saradc_reset(DeviceState *dev)
{
    RK3566SARADCState *s = RK3566_SARADC(dev);

    timer_del(s->timer);
    s->data = s->control = s->delay = 0;
    qemu_set_irq(s->irq, 0);
}

static void saradc_finalize(Object *obj)
{
    timer_free(RK3566_SARADC(obj)->timer);
}

static int saradc_post_load(void *opaque, int version_id)
{
    RK3566SARADCState *s = opaque;

    qemu_set_irq(s->irq, (s->control & 0x60) == 0x60);
    return 0;
}

static const VMStateDescription vmstate_saradc = {
    .name = TYPE_RK3566_SARADC, .version_id = 1, .minimum_version_id = 1,
    .post_load = saradc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(data, RK3566SARADCState),
        VMSTATE_UINT32(control, RK3566SARADCState),
        VMSTATE_UINT32(delay, RK3566SARADCState),
        VMSTATE_TIMER_PTR(timer, RK3566SARADCState),
        VMSTATE_END_OF_LIST()
    },
};
static const Property saradc_properties[] = {
    DEFINE_PROP_UINT32("reference-uv", RK3566SARADCState, reference_uv,
                       1800000),
    DEFINE_PROP_UINT32("vin0-uv", RK3566SARADCState, inputs_uv[0], 1800000),
    DEFINE_PROP_UINT32("vin1-uv", RK3566SARADCState, inputs_uv[1], 900000),
    DEFINE_PROP_UINT32("vin2-uv", RK3566SARADCState, inputs_uv[2], 0),
    DEFINE_PROP_UINT32("vin3-uv", RK3566SARADCState, inputs_uv[3], 0),
    DEFINE_PROP_UINT32("vin4-uv", RK3566SARADCState, inputs_uv[4], 0),
    DEFINE_PROP_UINT32("vin5-uv", RK3566SARADCState, inputs_uv[5], 0),
    DEFINE_PROP_UINT32("vin6-uv", RK3566SARADCState, inputs_uv[6], 0),
    DEFINE_PROP_UINT32("vin7-uv", RK3566SARADCState, inputs_uv[7], 0),
};
static void saradc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = saradc_realize;
    dc->vmsd = &vmstate_saradc;
    device_class_set_legacy_reset(dc, saradc_reset);
    device_class_set_props(dc, saradc_properties);
}
static const TypeInfo saradc_info = {
    .name = TYPE_RK3566_SARADC, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3566SARADCState),
    .instance_init = saradc_init, .class_init = saradc_class_init,
    .instance_finalize = saradc_finalize,
};
static void saradc_register(void) { type_register_static(&saradc_info); }
type_init(saradc_register)
