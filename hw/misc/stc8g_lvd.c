/*
 * STC8G1K08A low-voltage detector
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/misc/stc8g_lvd.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "target/mcs51/cpu.h"
#include "trace.h"

REG8(RSTCFG, 0)
    FIELD(RSTCFG, ENLVR, 6, 1)
    FIELD(RSTCFG, P54RST, 4, 1)
    FIELD(RSTCFG, LVDS, 0, 2)

#define STC8G_PCON_LVDF BIT(5)

struct Stc8gLvdState {
    SysBusDevice parent_obj;

    MCS51CPU *cpu;
    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[1];
    uint8_t regs[1];
    qemu_irq irq;
    uint16_t vdd_millivolts;
    bool below_threshold;
    bool reset_pending;
};

static uint16_t stc8g_lvd_threshold(Stc8gLvdState *s)
{
    static const uint16_t thresholds[] = { 2000, 2400, 2700, 3000 };

    return thresholds[FIELD_EX8(s->regs[STC8G_LVD_MMIO_RSTCFG], RSTCFG,
                                LVDS)];
}

static void stc8g_lvd_update_irq(Stc8gLvdState *s)
{
    qemu_set_irq(s->irq, (s->cpu->env.pcon & STC8G_PCON_LVDF) &&
                 !FIELD_EX8(s->regs[STC8G_LVD_MMIO_RSTCFG], RSTCFG,
                            ENLVR));
}

static void stc8g_lvd_update(Stc8gLvdState *s)
{
    uint16_t threshold = stc8g_lvd_threshold(s);
    bool below_threshold = s->vdd_millivolts < threshold;

    if (below_threshold && !s->below_threshold) {
        s->cpu->env.pcon |= STC8G_PCON_LVDF;
        trace_stc8g_lvd_event(s->vdd_millivolts, threshold,
                              FIELD_EX8(s->regs[STC8G_LVD_MMIO_RSTCFG],
                                        RSTCFG, ENLVR));
        if (FIELD_EX8(s->regs[STC8G_LVD_MMIO_RSTCFG], RSTCFG, ENLVR)) {
            s->reset_pending = true;
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
    }
    s->below_threshold = below_threshold;
    if (!s->reset_pending) {
        stc8g_lvd_update_irq(s);
    }
}

static void stc8g_lvd_set_vdd(void *opaque, int n, int level)
{
    Stc8gLvdState *s = opaque;

    s->vdd_millivolts = level;
    stc8g_lvd_update(s);
}

static void stc8g_lvd_rstcfg_post_write(RegisterInfo *reg, uint64_t value)
{
    stc8g_lvd_update(STC8G_LVD(reg->opaque));
}

static const RegisterAccessInfo stc8g_lvd_regs_info[] = {
    { .name = "RSTCFG", .addr = 0, .rsvd = 0xac,
      .post_write = stc8g_lvd_rstcfg_post_write },
};

static const MemoryRegionOps stc8g_lvd_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc8g_lvd_cpu_sfr_write(void *opaque, uint8_t addr,
                                     uint8_t value)
{
    Stc8gLvdState *s = opaque;

    if (addr == MCS251_SFR_PCON) {
        stc8g_lvd_update_irq(s);
    }
}

static void stc8g_lvd_reset(DeviceState *dev)
{
    Stc8gLvdState *s = STC8G_LVD(dev);

    s->below_threshold = false;
    s->reset_pending = false;
    register_reset(&s->regs_info[STC8G_LVD_MMIO_RSTCFG]);
    stc8g_lvd_update(s);
}

static int stc8g_lvd_post_load(void *opaque, int version_id)
{
    stc8g_lvd_update_irq(STC8G_LVD(opaque));
    return 0;
}

static const VMStateDescription stc8g_lvd_vmstate = {
    .name = "stc8g.lvd",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stc8g_lvd_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Stc8gLvdState, 1),
        VMSTATE_UINT16(vdd_millivolts, Stc8gLvdState),
        VMSTATE_BOOL(below_threshold, Stc8gLvdState),
        VMSTATE_BOOL(reset_pending, Stc8gLvdState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc8g_lvd_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc8gLvdState, cpu, TYPE_MCS51_CPU,
                     MCS51CPU *),
    DEFINE_PROP_UINT16("vdd-millivolts", Stc8gLvdState,
                       vdd_millivolts, 3300),
};

static void stc8g_lvd_realize(DeviceState *dev, Error **errp)
{
    Stc8gLvdState *s = STC8G_LVD(dev);

    if (!s->cpu) {
        error_setg(errp, "stc8g-lvd requires a CPU link");
        return;
    }
    mcs251_cpu_add_sfr_write_notifier(s->cpu, stc8g_lvd_cpu_sfr_write, s);
}

static void stc8g_lvd_init(Object *obj)
{
    Stc8gLvdState *s = STC8G_LVD(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->reg_array = register_init_block8(DEVICE(obj), stc8g_lvd_regs_info, 1,
                                         s->regs_info, s->regs,
                                         &stc8g_lvd_ops, false, 1);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), stc8g_lvd_set_vdd,
                            "vdd-millivolts", 1);
}

static void stc8g_lvd_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc8g_lvd_realize;
    device_class_set_legacy_reset(dc, stc8g_lvd_reset);
    device_class_set_props(dc, stc8g_lvd_properties);
    dc->vmsd = &stc8g_lvd_vmstate;
    dc->desc = "STC8G low-voltage detector";
}

static const TypeInfo stc8g_lvd_type = {
    .name = TYPE_STC8G_LVD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc8gLvdState),
    .instance_init = stc8g_lvd_init,
    .class_init = stc8g_lvd_class_init,
};

static void stc8g_lvd_register_types(void)
{
    type_register_static(&stc8g_lvd_type);
}

type_init(stc8g_lvd_register_types)
