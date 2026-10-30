/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/register.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

#define TYPE_RK3566_SARADC "rk3566-saradc"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566SARADCState, RK3566_SARADC)

REG32(DATA, 0x00)
REG32(STATUS, 0x04)
REG32(CONTROL, 0x08)
REG32(DELAY, 0x0c)

#define RK3566_SARADC_NUM_REGS (A_DELAY / 4 + 1)

struct RK3566SARADCState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[RK3566_SARADC_NUM_REGS];
    uint32_t regs[RK3566_SARADC_NUM_REGS];

    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t inputs_uv[8];
    uint32_t reference_uv;
};

static void saradc_complete(void *opaque)
{
    RK3566SARADCState *s = opaque;

    s->regs[R_DATA] =
        MIN((uint64_t)s->inputs_uv[s->regs[R_CONTROL] & 7] * 1023 /
            s->reference_uv, 1023);
    s->regs[R_CONTROL] |= 0x40;
    qemu_set_irq(s->irq, (s->regs[R_CONTROL] & 0x20) != 0);
}

static uint64_t saradc_status_post_read(RegisterInfo *reg, uint64_t val)
{
    RK3566SARADCState *s = RK3566_SARADC(reg->opaque);

    return timer_pending(s->timer);
}

static uint64_t saradc_control_pre_write(RegisterInfo *reg, uint64_t val)
{
    return val & 0x2f;
}

static void saradc_control_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566SARADCState *s = RK3566_SARADC(reg->opaque);

    qemu_set_irq(s->irq, 0);
    timer_del(s->timer);
    if (val & 8) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (s->regs[R_DELAY] + 12) * 1000);
    }
}

static uint64_t saradc_delay_pre_write(RegisterInfo *reg, uint64_t val)
{
    return val & 0x3f;
}

static const RegisterAccessInfo saradc_regs_info[] = {
    {
        .name = "DATA",
        .addr = A_DATA,
        .ro = UINT32_MAX,
    }, {
        .name = "STATUS",
        .addr = A_STATUS,
        .ro = UINT32_MAX,
        .post_read = saradc_status_post_read,
    }, {
        .name = "CONTROL",
        .addr = A_CONTROL,
        .pre_write = saradc_control_pre_write,
        .post_write = saradc_control_post_write,
    }, {
        .name = "DELAY",
        .addr = A_DELAY,
        .pre_write = saradc_delay_pre_write,
    },
};

static const MemoryRegionOps saradc_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void saradc_init(Object *obj)
{
    RK3566SARADCState *s = RK3566_SARADC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->reg_array = register_init_block32(DEVICE(obj), saradc_regs_info,
                                         ARRAY_SIZE(saradc_regs_info),
                                         s->regs_info, s->regs,
                                         &saradc_ops, false, 0x100);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
    sysbus_init_irq(sbd, &s->irq);
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
    for (unsigned int i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        register_reset(&s->regs_info[i]);
    }
    qemu_set_irq(s->irq, 0);
}

static void saradc_finalize(Object *obj)
{
    timer_free(RK3566_SARADC(obj)->timer);
}

static int saradc_post_load(void *opaque, int version_id)
{
    RK3566SARADCState *s = opaque;

    qemu_set_irq(s->irq, (s->regs[R_CONTROL] & 0x60) == 0x60);
    return 0;
}

static const VMStateDescription vmstate_saradc = {
    .name = TYPE_RK3566_SARADC, .version_id = 1, .minimum_version_id = 1,
    .post_load = saradc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RK3566SARADCState, RK3566_SARADC_NUM_REGS),
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
