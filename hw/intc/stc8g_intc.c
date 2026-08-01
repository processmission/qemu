/*
 * STC8G1K08A interrupt controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "hw/intc/stc8g_intc.h"
#include "migration/vmstate.h"
#include "target/mcs51/cpu.h"

#define STC8G_INTC_REGS 4

enum Stc8gIntcRegister {
    STC8G_INTC_REG_IE2,
    STC8G_INTC_REG_IP2,
    STC8G_INTC_REG_IP2H,
    STC8G_INTC_REG_AUXINTIF,
};

struct Stc8gIntcState {
    SysBusDevice parent_obj;

    MCS51CPU *cpu;
    RegisterInfoArray *reg_array[STC8G_INTC_REGS];
    RegisterInfo regs_info[STC8G_INTC_REGS];
    uint8_t regs[STC8G_INTC_REGS];
    bool source_level[STC8G_INTC_NUM_SOURCES];
    qemu_irq irq[STC8G_INTC_NUM_SOURCES];
};

static const unsigned stc8g_intc_irq[] = {
    [STC8G_INTC_ADC] = MCS251_IRQ_ADC,
    [STC8G_INTC_LVD] = MCS251_IRQ_LVD,
    [STC8G_INTC_PCA] = MCS251_IRQ_PCA,
    [STC8G_INTC_SPI] = MCS251_IRQ_PCA + 1,
    [STC8G_INTC_INT2] = MCS251_IRQ_PCA + 2,
    [STC8G_INTC_INT3] = MCS251_IRQ_PCA + 3,
    [STC8G_INTC_INT4] = MCS251_IRQ_PCA + 4,
    [STC8G_INTC_I2C] = MCS251_IRQ_PCA + 5,
};

static const uint32_t stc8g_intc_vector[] = {
    [STC8G_INTC_ADC] = 0x002b,
    [STC8G_INTC_LVD] = 0x0033,
    [STC8G_INTC_PCA] = 0x003b,
    [STC8G_INTC_SPI] = 0x004b,
    [STC8G_INTC_INT2] = 0x0053,
    [STC8G_INTC_INT3] = 0x005b,
    [STC8G_INTC_INT4] = 0x0083,
    [STC8G_INTC_I2C] = 0x00c3,
};

static bool stc8g_intc_source_enabled(Stc8gIntcState *s, unsigned source)
{
    CPUMCS251State *env = &s->cpu->env;

    switch (source) {
    case STC8G_INTC_ADC:
        return FIELD_EX8(env->ie, IE, EADC);
    case STC8G_INTC_LVD:
        return FIELD_EX8(env->ie, IE, ELVD);
    case STC8G_INTC_PCA:
    case STC8G_INTC_I2C:
        return true;
    case STC8G_INTC_SPI:
        return extract8(s->regs[STC8G_INTC_REG_IE2], 1, 1);
    case STC8G_INTC_INT2:
        return extract8(env->intclko, 4, 1);
    case STC8G_INTC_INT3:
        return extract8(env->intclko, 5, 1);
    case STC8G_INTC_INT4:
        return extract8(env->intclko, 6, 1);
    default:
        g_assert_not_reached();
    }
}

static unsigned stc8g_intc_source_priority(Stc8gIntcState *s,
                                            unsigned source)
{
    CPUMCS251State *env = &s->cpu->env;
    unsigned bit;

    switch (source) {
    case STC8G_INTC_ADC:
        bit = 5;
        return extract8(env->iph, bit, 1) * 2 +
               extract8(env->ip, bit, 1);
    case STC8G_INTC_LVD:
        bit = 6;
        return extract8(env->iph, bit, 1) * 2 +
               extract8(env->ip, bit, 1);
    case STC8G_INTC_PCA:
        bit = 7;
        return extract8(env->iph, bit, 1) * 2 +
               extract8(env->ip, bit, 1);
    case STC8G_INTC_INT2:
    case STC8G_INTC_INT3:
        return 0;
    case STC8G_INTC_SPI:
        bit = 1;
        break;
    case STC8G_INTC_INT4:
        bit = 4;
        break;
    case STC8G_INTC_I2C:
        bit = 6;
        break;
    default:
        g_assert_not_reached();
    }
    return extract8(s->regs[STC8G_INTC_REG_IP2H], bit, 1) * 2 +
           extract8(s->regs[STC8G_INTC_REG_IP2], bit, 1);
}

static bool stc8g_intc_source_latched(unsigned source)
{
    return source >= STC8G_INTC_INT2 && source <= STC8G_INTC_INT4;
}

static bool stc8g_intc_source_pending(Stc8gIntcState *s, unsigned source)
{
    if (stc8g_intc_source_latched(source)) {
        return extract8(s->regs[STC8G_INTC_REG_AUXINTIF], source, 1);
    }
    return s->source_level[source];
}

static void stc8g_intc_update_irq(Stc8gIntcState *s, unsigned source)
{
    qemu_set_irq(s->irq[source], stc8g_intc_source_pending(s, source));
}

static void stc8g_intc_update_configuration(Stc8gIntcState *s)
{
    unsigned source;

    for (source = 0; source < STC8G_INTC_NUM_SOURCES; source++) {
        mcs251_cpu_configure_irq(s->cpu, stc8g_intc_irq[source],
                                 stc8g_intc_vector[source],
                                 stc8g_intc_source_priority(s, source),
                                 stc8g_intc_source_enabled(s, source),
                                 false);
    }
}

static void stc8g_intc_sfr_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gIntcState *s = STC8G_INTC(reg->opaque);

    stc8g_intc_update_configuration(s);
}

static void stc8g_intc_auxintif_post_write(RegisterInfo *reg,
                                            uint64_t value)
{
    Stc8gIntcState *s = STC8G_INTC(reg->opaque);
    unsigned source;

    for (source = STC8G_INTC_INT2; source <= STC8G_INTC_INT4; source++) {
        stc8g_intc_update_irq(s, source);
    }
}

static const RegisterAccessInfo stc8g_intc_regs_info[] = {
    { .name = "IE2", .addr = 0, .rsvd = 0xfd,
      .post_write = stc8g_intc_sfr_post_write },
    { .name = "IP2", .addr = 0, .rsvd = 0xad,
      .post_write = stc8g_intc_sfr_post_write },
    { .name = "IP2H", .addr = 0, .rsvd = 0xad,
      .post_write = stc8g_intc_sfr_post_write },
    { .name = "AUXINTIF", .addr = 0, .w1c = 0x70, .rsvd = 0x8f,
      .post_write = stc8g_intc_auxintif_post_write },
};

static const MemoryRegionOps stc8g_intc_sfr_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc8g_intc_set_input(void *opaque, int source, int level)
{
    Stc8gIntcState *s = opaque;
    bool old_level = s->source_level[source];

    s->source_level[source] = !!level;
    if (level && !old_level && stc8g_intc_source_latched(source)) {
        unsigned bit = source;

        s->regs[STC8G_INTC_REG_AUXINTIF] = deposit32(
            s->regs[STC8G_INTC_REG_AUXINTIF], bit, 1, 1);
    }
    stc8g_intc_update_irq(s, source);
}

static void stc8g_intc_cpu_sfr_write(void *opaque, uint8_t addr,
                                      uint8_t value)
{
    Stc8gIntcState *s = opaque;

    if (addr == MCS251_SFR_IE || addr == MCS251_SFR_IP ||
        addr == MCS251_SFR_IPH || addr == MCS251_SFR_INTCLKO) {
        stc8g_intc_update_configuration(s);
    }
}

static void stc8g_intc_reset(DeviceState *dev)
{
    Stc8gIntcState *s = STC8G_INTC(dev);
    unsigned index;
    unsigned source;

    for (index = 0; index < ARRAY_SIZE(stc8g_intc_regs_info); index++) {
        register_reset(&s->regs_info[index]);
    }
    stc8g_intc_update_configuration(s);
    for (source = 0; source < STC8G_INTC_NUM_SOURCES; source++) {
        stc8g_intc_update_irq(s, source);
    }
}

static int stc8g_intc_post_load(void *opaque, int version_id)
{
    Stc8gIntcState *s = opaque;
    unsigned source;

    stc8g_intc_update_configuration(s);
    for (source = 0; source < STC8G_INTC_NUM_SOURCES; source++) {
        stc8g_intc_update_irq(s, source);
    }
    return 0;
}

static const VMStateDescription stc8g_intc_vmstate = {
    .name = "stc8g.intc",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stc8g_intc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Stc8gIntcState, STC8G_INTC_REGS),
        VMSTATE_BOOL_ARRAY(source_level, Stc8gIntcState,
                           STC8G_INTC_NUM_SOURCES),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc8g_intc_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc8gIntcState, cpu, TYPE_MCS51_CPU,
                     MCS51CPU *),
};

static void stc8g_intc_realize(DeviceState *dev, Error **errp)
{
    Stc8gIntcState *s = STC8G_INTC(dev);

    if (!s->cpu) {
        error_setg(errp, "stc8g-intc requires a CPU link");
        return;
    }
    mcs251_cpu_add_sfr_write_notifier(s->cpu, stc8g_intc_cpu_sfr_write, s);
    stc8g_intc_update_configuration(s);
}

static void stc8g_intc_init(Object *obj)
{
    Stc8gIntcState *s = STC8G_INTC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned index;

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc8g_intc_regs_info) !=
                      STC8G_INTC_REGS);
    for (index = 0; index < ARRAY_SIZE(stc8g_intc_regs_info); index++) {
        s->reg_array[index] = register_init_block8(
            DEVICE(obj), &stc8g_intc_regs_info[index], 1,
            &s->regs_info[index], &s->regs[index], &stc8g_intc_sfr_ops,
            false, 1);
        sysbus_init_mmio(sbd, &s->reg_array[index]->mem);
    }
    qdev_init_gpio_in_named(DEVICE(obj), stc8g_intc_set_input, "irq-in",
                            STC8G_INTC_NUM_SOURCES);
    for (index = 0; index < STC8G_INTC_NUM_SOURCES; index++) {
        sysbus_init_irq(sbd, &s->irq[index]);
    }
}

static void stc8g_intc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc8g_intc_realize;
    device_class_set_legacy_reset(dc, stc8g_intc_reset);
    device_class_set_props(dc, stc8g_intc_properties);
    dc->vmsd = &stc8g_intc_vmstate;
}

static const TypeInfo stc8g_intc_type = {
    .name = TYPE_STC8G_INTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc8gIntcState),
    .instance_init = stc8g_intc_init,
    .class_init = stc8g_intc_class_init,
};

static void stc8g_intc_register_types(void)
{
    type_register_static(&stc8g_intc_type);
}

type_init(stc8g_intc_register_types)
