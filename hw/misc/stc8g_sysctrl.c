/*
 * STC8G1K08A system clock controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/misc/stc8g_sysctrl.h"
#include "migration/vmstate.h"
#include "target/mcs51/cpu.h"
#include "trace.h"

REG8(CLKSEL, 0)
    FIELD(CLKSEL, MCKSEL, 0, 2)
REG8(HIRCCR, 0)
    FIELD(HIRCCR, ENHIRC, 7, 1)
    FIELD(HIRCCR, HIRCST, 0, 1)
REG8(XOSCCR, 0)
    FIELD(XOSCCR, ENXOSC, 7, 1)
    FIELD(XOSCCR, XITYPE, 6, 1)
    FIELD(XOSCCR, XOSCST, 0, 1)
REG8(IRC32KCR, 0)
    FIELD(IRC32KCR, ENIRC32K, 7, 1)
    FIELD(IRC32KCR, IRC32KST, 0, 1)
REG8(MCLKOCR, 0)
    FIELD(MCLKOCR, MCLKO_S, 7, 1)
    FIELD(MCLKOCR, MCLKODIV, 0, 7)

enum Stc8gSysctrlRegister {
    STC8G_SYSCTRL_CLKSEL,
    STC8G_SYSCTRL_CLKDIV,
    STC8G_SYSCTRL_HIRCCR,
    STC8G_SYSCTRL_XOSCCR,
    STC8G_SYSCTRL_IRC32KCR,
    STC8G_SYSCTRL_MCLKOCR,
    STC8G_SYSCTRL_IRCDB,
    STC8G_SYSCTRL_IRCBAND,
    STC8G_SYSCTRL_LIRTRIM,
    STC8G_SYSCTRL_IRTRIM,
};

struct Stc8gSysctrlState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array[STC8G_SYSCTRL_MMIO_REGS];
    RegisterInfo regs_info[STC8G_SYSCTRL_MMIO_REGS];
    uint8_t regs[STC8G_SYSCTRL_MMIO_REGS];
    MCS51CPU *cpu;
    Clock *sysclk;
    Clock *mclko;
    uint32_t hirc_frequency;
    uint32_t hirc_high_frequency;
    uint32_t xosc_frequency;
    uint32_t irc32k_frequency;
};

static uint64_t stc8g_sysctrl_hirc_frequency(Stc8gSysctrlState *s)
{
    int64_t trim = s->regs[STC8G_SYSCTRL_IRTRIM] - 0x80;
    uint64_t frequency = s->regs[STC8G_SYSCTRL_IRCBAND] & 1 ?
                         s->hirc_high_frequency : s->hirc_frequency;
    unsigned fine_trim = s->regs[STC8G_SYSCTRL_LIRTRIM] & 3;

    frequency = frequency * (100000 + trim * 240) / 100000;
    switch (fine_trim) {
    case 1:
        return frequency * 1001 / 1000;
    case 2:
        return frequency * 10004 / 10000;
    case 3:
        return frequency * 999 / 1000;
    default:
        return frequency;
    }
}

static uint64_t stc8g_sysctrl_source_frequency(Stc8gSysctrlState *s)
{
    if (FIELD_EX8(s->cpu->env.pcon, PCON, PD)) {
        return 0;
    }
    switch (FIELD_EX8(s->regs[STC8G_SYSCTRL_CLKSEL], CLKSEL, MCKSEL)) {
    case 0:
        return FIELD_EX8(s->regs[STC8G_SYSCTRL_HIRCCR], HIRCCR, ENHIRC) ?
            stc8g_sysctrl_hirc_frequency(s) : 0;
    case 1:
    case 2:
        return FIELD_EX8(s->regs[STC8G_SYSCTRL_XOSCCR], XOSCCR, ENXOSC) ?
            s->xosc_frequency : 0;
    case 3:
        return FIELD_EX8(s->regs[STC8G_SYSCTRL_IRC32KCR], IRC32KCR,
                         ENIRC32K) ? s->irc32k_frequency : 0;
    default:
        g_assert_not_reached();
    }
}

static uint64_t stc8g_sysctrl_clock_period(uint64_t source,
                                            unsigned divider)
{
    uint64_t low;
    uint64_t high;

    if (!source) {
        return 0;
    }
    mulu64(&low, &high, CLOCK_PERIOD_1SEC, divider);
    divu128(&low, &high, source);
    return low;
}

static void stc8g_sysctrl_update_clocks(Stc8gSysctrlState *s)
{
    uint64_t source = stc8g_sysctrl_source_frequency(s);
    unsigned divider = MAX(1, s->regs[STC8G_SYSCTRL_CLKDIV]);
    unsigned mclko_divider = FIELD_EX8(
        s->regs[STC8G_SYSCTRL_MCLKOCR], MCLKOCR, MCLKODIV);
    uint64_t sysclk = stc8g_sysctrl_clock_period(source, divider);
    uint64_t mclko = mclko_divider ?
        stc8g_sysctrl_clock_period(source, divider * mclko_divider) : 0;

    if (clock_set(s->sysclk, sysclk)) {
        clock_propagate(s->sysclk);
    }
    if (clock_set(s->mclko, mclko)) {
        clock_propagate(s->mclko);
    }
    trace_stc8g_sysctrl_clock(clock_get_hz(s->sysclk),
                              clock_get_hz(s->mclko),
                              FIELD_EX8(s->regs[STC8G_SYSCTRL_CLKSEL],
                                        CLKSEL, MCKSEL), divider);
}

static void stc8g_sysctrl_update_status(Stc8gSysctrlState *s)
{
    s->regs[STC8G_SYSCTRL_HIRCCR] = FIELD_DP8(
        s->regs[STC8G_SYSCTRL_HIRCCR], HIRCCR, HIRCST,
        FIELD_EX8(s->regs[STC8G_SYSCTRL_HIRCCR], HIRCCR, ENHIRC));
    s->regs[STC8G_SYSCTRL_XOSCCR] = FIELD_DP8(
        s->regs[STC8G_SYSCTRL_XOSCCR], XOSCCR, XOSCST,
        FIELD_EX8(s->regs[STC8G_SYSCTRL_XOSCCR], XOSCCR, ENXOSC));
    s->regs[STC8G_SYSCTRL_IRC32KCR] = FIELD_DP8(
        s->regs[STC8G_SYSCTRL_IRC32KCR], IRC32KCR, IRC32KST,
        FIELD_EX8(s->regs[STC8G_SYSCTRL_IRC32KCR], IRC32KCR, ENIRC32K));
}

static void stc8g_sysctrl_clock_post_write(RegisterInfo *reg,
                                            uint64_t value)
{
    Stc8gSysctrlState *s = STC8G_SYSCTRL(reg->opaque);

    stc8g_sysctrl_update_status(s);
    stc8g_sysctrl_update_clocks(s);
}

static void stc8g_sysctrl_sfr_write(void *opaque, uint8_t addr,
                                    uint8_t value)
{
    Stc8gSysctrlState *s = opaque;

    if (addr == MCS251_SFR_PCON) {
        stc8g_sysctrl_update_clocks(s);
    }
}

static const RegisterAccessInfo stc8g_sysctrl_regs_info[] = {
    { .name = "CLKSEL", .addr = 0, .rsvd = 0xfc,
      .post_write = stc8g_sysctrl_clock_post_write },
    { .name = "CLKDIV", .addr = 0,
      .post_write = stc8g_sysctrl_clock_post_write },
    { .name = "HIRCCR", .addr = 0, .ro = 0x01, .rsvd = 0x7e,
      .reset = 0x81, .post_write = stc8g_sysctrl_clock_post_write },
    { .name = "XOSCCR", .addr = 0, .ro = 0x01, .rsvd = 0x3e,
      .post_write = stc8g_sysctrl_clock_post_write },
    { .name = "IRC32KCR", .addr = 0, .ro = 0x01, .rsvd = 0x7e,
      .post_write = stc8g_sysctrl_clock_post_write },
    { .name = "MCLKOCR", .addr = 0,
      .post_write = stc8g_sysctrl_clock_post_write },
    { .name = "IRCDB", .addr = 0, .reset = 0x80 },
    { .name = "IRCBAND", .addr = 0, .rsvd = 0xfe,
      .post_write = stc8g_sysctrl_clock_post_write },
    { .name = "LIRTRIM", .addr = 0, .rsvd = 0xfc,
      .post_write = stc8g_sysctrl_clock_post_write },
    { .name = "IRTRIM", .addr = 0, .reset = 0x80,
      .post_write = stc8g_sysctrl_clock_post_write },
};

static const MemoryRegionOps stc8g_sysctrl_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc8g_sysctrl_reset(DeviceState *dev)
{
    Stc8gSysctrlState *s = STC8G_SYSCTRL(dev);
    unsigned index;

    for (index = 0; index < ARRAY_SIZE(stc8g_sysctrl_regs_info); index++) {
        register_reset(&s->regs_info[index]);
    }
    stc8g_sysctrl_update_status(s);
    stc8g_sysctrl_update_clocks(s);
}

static int stc8g_sysctrl_post_load(void *opaque, int version_id)
{
    Stc8gSysctrlState *s = opaque;

    stc8g_sysctrl_update_status(s);
    stc8g_sysctrl_update_clocks(s);
    return 0;
}

static const VMStateDescription stc8g_sysctrl_vmstate = {
    .name = "stc8g.sysctrl",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stc8g_sysctrl_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Stc8gSysctrlState,
                            STC8G_SYSCTRL_MMIO_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc8g_sysctrl_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc8gSysctrlState, cpu, TYPE_MCS51_CPU,
                     MCS51CPU *),
    DEFINE_PROP_UINT32("hirc-frequency", Stc8gSysctrlState,
                       hirc_frequency, 24000000),
    DEFINE_PROP_UINT32("hirc-high-frequency", Stc8gSysctrlState,
                       hirc_high_frequency, 33000000),
    DEFINE_PROP_UINT32("xosc-frequency", Stc8gSysctrlState,
                       xosc_frequency, 24000000),
    DEFINE_PROP_UINT32("irc32k-frequency", Stc8gSysctrlState,
                       irc32k_frequency, 32768),
};

static void stc8g_sysctrl_realize(DeviceState *dev, Error **errp)
{
    Stc8gSysctrlState *s = STC8G_SYSCTRL(dev);

    if (!s->cpu) {
        error_setg(errp, "stc8g-sysctrl requires a CPU link");
        return;
    }
    if (!s->hirc_frequency || !s->hirc_high_frequency ||
        !s->xosc_frequency || !s->irc32k_frequency) {
        error_setg(errp,
                   "stc8g-sysctrl oscillator frequencies must be nonzero");
        return;
    }
    mcs251_cpu_add_sfr_write_notifier(s->cpu, stc8g_sysctrl_sfr_write, s);
    stc8g_sysctrl_update_clocks(s);
}

static void stc8g_sysctrl_init(Object *obj)
{
    Stc8gSysctrlState *s = STC8G_SYSCTRL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned index;

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc8g_sysctrl_regs_info) !=
                      STC8G_SYSCTRL_MMIO_REGS);
    for (index = 0; index < ARRAY_SIZE(stc8g_sysctrl_regs_info); index++) {
        s->reg_array[index] = register_init_block8(
            DEVICE(obj), &stc8g_sysctrl_regs_info[index], 1,
            &s->regs_info[index], &s->regs[index], &stc8g_sysctrl_ops,
            false, 1);
        sysbus_init_mmio(sbd, &s->reg_array[index]->mem);
    }
    s->sysclk = qdev_init_clock_out(DEVICE(obj), "sysclk");
    s->mclko = qdev_init_clock_out(DEVICE(obj), "mclko");
}

static void stc8g_sysctrl_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc8g_sysctrl_realize;
    device_class_set_legacy_reset(dc, stc8g_sysctrl_reset);
    device_class_set_props(dc, stc8g_sysctrl_properties);
    dc->vmsd = &stc8g_sysctrl_vmstate;
}

static const TypeInfo stc8g_sysctrl_type = {
    .name = TYPE_STC8G_SYSCTRL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc8gSysctrlState),
    .instance_init = stc8g_sysctrl_init,
    .class_init = stc8g_sysctrl_class_init,
};

static void stc8g_sysctrl_register_types(void)
{
    type_register_static(&stc8g_sysctrl_type);
}

type_init(stc8g_sysctrl_register_types)
