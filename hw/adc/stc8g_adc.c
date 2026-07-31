/*
 * STC8G1K08A 10-bit ADC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/adc/stc8g_adc.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/mcs51/clock.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "target/mcs51/cpu.h"
#include "trace.h"

REG8(ADC_CONTR, 0)
    FIELD(ADC_CONTR, POWER, 7, 1)
    FIELD(ADC_CONTR, START, 6, 1)
    FIELD(ADC_CONTR, FLAG, 5, 1)
    FIELD(ADC_CONTR, EPWMT, 4, 1)
    FIELD(ADC_CONTR, CHS, 0, 4)
REG8(ADC_RES, 0)
REG8(ADC_RESL, 0)
REG8(ADCCFG, 0)
    FIELD(ADCCFG, RESFMT, 5, 1)
    FIELD(ADCCFG, SPEED, 0, 4)
REG8(ADCTIM, 0)
    FIELD(ADCTIM, CSSETUP, 7, 1)
    FIELD(ADCTIM, CSHOLD, 5, 2)
    FIELD(ADCTIM, SMPDUTY, 0, 5)

#define STC8G_ADC_REGS 5
#define STC8G_ADC_CHANNELS 6
#define STC8G_ADC_POWER_STABILIZE_NS 1000000

enum Stc8gADCRegister {
    STC8G_ADC_CONTR,
    STC8G_ADC_RES,
    STC8G_ADC_RESL,
    STC8G_ADC_CFG,
    STC8G_ADC_TIM,
};

struct Stc8gADCState {
    SysBusDevice parent_obj;

    MCS51CPU *cpu;
    RegisterInfoArray *reg_array[STC8G_ADC_REGS];
    RegisterInfo regs_info[STC8G_ADC_REGS];
    uint8_t regs[STC8G_ADC_REGS];
    QEMUTimer *timer;
    Clock *sysclk;
    qemu_irq irq;
    uint16_t channel_value[STC8G_ADC_CHANNELS];
    uint16_t sample;
    uint32_t clock_frequency;
    uint16_t vdd_millivolts;
    uint64_t conversion_cycles;
    uint64_t clock_remainder;
    int64_t power_on_ns;
    int64_t last_ns;
    bool powered;
    bool converting;
};

static void stc8g_adc_update_irq(Stc8gADCState *s)
{
    qemu_set_irq(s->irq, s->powered &&
                 FIELD_EX8(s->regs[STC8G_ADC_CONTR], ADC_CONTR, FLAG));
}

static uint16_t stc8g_adc_channel_value(Stc8gADCState *s, unsigned channel)
{
    if (channel < STC8G_ADC_CHANNELS) {
        return s->channel_value[channel];
    }
    if (channel == 15) {
        return MIN(1023, 1190 * 1024 / s->vdd_millivolts);
    }
    return 0;
}

static uint64_t stc8g_adc_conversion_cycles(Stc8gADCState *s)
{
    uint64_t phases = FIELD_EX8(s->regs[STC8G_ADC_TIM], ADCTIM, CSSETUP) +
                      FIELD_EX8(s->regs[STC8G_ADC_TIM], ADCTIM, CSHOLD) +
                      FIELD_EX8(s->regs[STC8G_ADC_TIM], ADCTIM, SMPDUTY) +
                      13;

    return 2 * (FIELD_EX8(s->regs[STC8G_ADC_CFG], ADCCFG, SPEED) + 1) *
           phases;
}

static void stc8g_adc_start(Stc8gADCState *s);
static void stc8g_adc_sync(Stc8gADCState *s);
static void stc8g_adc_schedule(Stc8gADCState *s);

static void stc8g_adc_clock_update(void *opaque, ClockEvent event)
{
    Stc8gADCState *s = opaque;

    if (event == ClockPreUpdate) {
        stc8g_adc_sync(s);
        return;
    }
    s->clock_frequency = clock_get_hz(s->sysclk);
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (clock_is_enabled(s->sysclk) && s->powered &&
        FIELD_EX8(s->regs[STC8G_ADC_CONTR], ADC_CONTR, START) &&
        !s->converting) {
        stc8g_adc_start(s);
    } else {
        stc8g_adc_schedule(s);
    }
}

static void stc8g_adc_complete(void *opaque)
{
    Stc8gADCState *s = opaque;
    uint16_t result = s->sample;

    s->converting = false;
    s->regs[STC8G_ADC_CONTR] = FIELD_DP8(
        s->regs[STC8G_ADC_CONTR], ADC_CONTR, START, 0);
    s->regs[STC8G_ADC_CONTR] = FIELD_DP8(
        s->regs[STC8G_ADC_CONTR], ADC_CONTR, FLAG, 1);
    if (FIELD_EX8(s->regs[STC8G_ADC_CFG], ADCCFG, RESFMT)) {
        s->regs[STC8G_ADC_RES] = result >> 8;
        s->regs[STC8G_ADC_RESL] = result;
    } else {
        s->regs[STC8G_ADC_RES] = result >> 2;
        s->regs[STC8G_ADC_RESL] = result << 6;
    }
    trace_stc8g_adc_complete(result);
    stc8g_adc_update_irq(s);
}

static void stc8g_adc_sync(Stc8gADCState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->converting && clock_is_enabled(s->sysclk)) {
        uint64_t cycles = mcs51_clock_elapsed_cycles(
            s->sysclk, now - s->last_ns, &s->clock_remainder);
        if (cycles >= s->conversion_cycles) {
            s->conversion_cycles = 0;
        } else {
            s->conversion_cycles -= cycles;
        }
    }
    s->last_ns = now;
    if (s->converting && !s->conversion_cycles &&
        clock_is_enabled(s->sysclk) &&
        now >= s->power_on_ns + STC8G_ADC_POWER_STABILIZE_NS) {
        timer_del(s->timer);
        stc8g_adc_complete(s);
    }
}

static void stc8g_adc_schedule(Stc8gADCState *s)
{
    int64_t deadline;

    timer_del(s->timer);
    if (!s->converting || !clock_is_enabled(s->sysclk)) {
        return;
    }
    deadline = s->power_on_ns + STC8G_ADC_POWER_STABILIZE_NS;
    if (s->conversion_cycles) {
        uint64_t delta = mcs51_clock_cycles_to_ns(s->sysclk,
                                                   s->conversion_cycles,
                                                   s->clock_remainder);

        deadline = MAX(deadline, s->last_ns + MAX(1ull, delta));
    }
    timer_mod_ns(s->timer, deadline);
}

static void stc8g_adc_expire(void *opaque)
{
    Stc8gADCState *s = opaque;

    stc8g_adc_sync(s);
    stc8g_adc_schedule(s);
}

static void stc8g_adc_start(Stc8gADCState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned channel = FIELD_EX8(s->regs[STC8G_ADC_CONTR], ADC_CONTR, CHS);

    if (!clock_is_enabled(s->sysclk) ||
        !FIELD_EX8(s->regs[STC8G_ADC_CONTR], ADC_CONTR, POWER) ||
        s->converting) {
        return;
    }
    s->sample = stc8g_adc_channel_value(s, channel);
    s->conversion_cycles = stc8g_adc_conversion_cycles(s);
    s->clock_remainder = 0;
    s->last_ns = now;
    s->converting = true;
    stc8g_adc_schedule(s);
    trace_stc8g_adc_start(channel, s->sample,
                          MAX(s->power_on_ns + STC8G_ADC_POWER_STABILIZE_NS,
                              now + mcs51_clock_cycles_to_ns(
                                  s->sysclk, s->conversion_cycles, 0)) - now);
}

static uint64_t stc8g_adc_contr_pre_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gADCState *s = STC8G_ADC(reg->opaque);

    if (s->converting && FIELD_EX8(value, ADC_CONTR, POWER)) {
        value = FIELD_DP8(value, ADC_CONTR, START, 1);
    }
    return value;
}

static void stc8g_adc_contr_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gADCState *s = STC8G_ADC(reg->opaque);
    bool powered = FIELD_EX8(value, ADC_CONTR, POWER);

    if (!powered) {
        timer_del(s->timer);
        s->converting = false;
        s->conversion_cycles = 0;
        s->clock_remainder = 0;
        s->regs[STC8G_ADC_CONTR] = FIELD_DP8(
            s->regs[STC8G_ADC_CONTR], ADC_CONTR, START, 0);
    } else if (!s->powered) {
        s->power_on_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    s->powered = powered;
    if (powered && FIELD_EX8(value, ADC_CONTR, START)) {
        stc8g_adc_start(s);
    }
    stc8g_adc_update_irq(s);
}

static const RegisterAccessInfo stc8g_adc_regs_info[] = {
    { .name = "ADC_CONTR", .addr = 0,
      .pre_write = stc8g_adc_contr_pre_write,
      .post_write = stc8g_adc_contr_post_write },
    { .name = "ADC_RES", .addr = 0, .ro = 0xff },
    { .name = "ADC_RESL", .addr = 0, .ro = 0xff },
    { .name = "ADCCFG", .addr = 0, .rsvd = 0xd0 },
    { .name = "ADCTIM", .addr = 0, .reset = 0x2a },
};

static const MemoryRegionOps stc8g_adc_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc8g_adc_set_input(void *opaque, int channel, int level)
{
    Stc8gADCState *s = opaque;

    s->channel_value[channel] = level & 0x3ff;
}

static void stc8g_adc_reset(DeviceState *dev)
{
    Stc8gADCState *s = STC8G_ADC(dev);
    unsigned index;

    timer_del(s->timer);
    s->converting = false;
    s->sample = 0;
    s->conversion_cycles = 0;
    s->clock_remainder = 0;
    s->power_on_ns = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->powered = false;
    for (index = 0; index < ARRAY_SIZE(stc8g_adc_regs_info); index++) {
        register_reset(&s->regs_info[index]);
    }
    stc8g_adc_update_irq(s);
}

static int stc8g_adc_post_load(void *opaque, int version_id)
{
    Stc8gADCState *s = opaque;

    stc8g_adc_update_irq(s);
    return 0;
}

static const VMStateDescription stc8g_adc_vmstate = {
    .name = "stc8g.adc",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stc8g_adc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Stc8gADCState, STC8G_ADC_REGS),
        VMSTATE_UINT16_ARRAY(channel_value, Stc8gADCState,
                             STC8G_ADC_CHANNELS),
        VMSTATE_UINT16(sample, Stc8gADCState),
        VMSTATE_UINT64(conversion_cycles, Stc8gADCState),
        VMSTATE_UINT64(clock_remainder, Stc8gADCState),
        VMSTATE_INT64(power_on_ns, Stc8gADCState),
        VMSTATE_INT64(last_ns, Stc8gADCState),
        VMSTATE_BOOL(powered, Stc8gADCState),
        VMSTATE_BOOL(converting, Stc8gADCState),
        VMSTATE_TIMER_PTR(timer, Stc8gADCState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc8g_adc_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc8gADCState, cpu, TYPE_MCS51_CPU,
                     MCS51CPU *),
    DEFINE_PROP_UINT32("clock-frequency", Stc8gADCState,
                       clock_frequency, 24000000),
    DEFINE_PROP_UINT16("vdd-millivolts", Stc8gADCState,
                       vdd_millivolts, 3300),
};

static void stc8g_adc_realize(DeviceState *dev, Error **errp)
{
    Stc8gADCState *s = STC8G_ADC(dev);

    if (!s->cpu) {
        error_setg(errp, "stc8g-adc requires a CPU link");
    } else if (!s->clock_frequency) {
        error_setg(errp, "stc8g-adc clock-frequency must be nonzero");
    } else if (!s->vdd_millivolts) {
        error_setg(errp, "stc8g-adc vdd-millivolts must be nonzero");
    }
}

static void stc8g_adc_init(Object *obj)
{
    Stc8gADCState *s = STC8G_ADC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned index;

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc8g_adc_regs_info) != STC8G_ADC_REGS);
    for (index = 0; index < ARRAY_SIZE(stc8g_adc_regs_info); index++) {
        s->reg_array[index] = register_init_block8(
            DEVICE(obj), &stc8g_adc_regs_info[index], 1,
            &s->regs_info[index], &s->regs[index], &stc8g_adc_ops,
            false, 1);
        sysbus_init_mmio(sbd, &s->reg_array[index]->mem);
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stc8g_adc_expire, s);
    s->sysclk = qdev_init_clock_in(DEVICE(obj), "sysclk",
                                   stc8g_adc_clock_update, s,
                                   ClockPreUpdate | ClockUpdate);
    qdev_init_gpio_in_named(DEVICE(obj), stc8g_adc_set_input, "adc-in",
                            STC8G_ADC_CHANNELS);
    sysbus_init_irq(sbd, &s->irq);
}

static void stc8g_adc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc8g_adc_realize;
    device_class_set_legacy_reset(dc, stc8g_adc_reset);
    device_class_set_props(dc, stc8g_adc_properties);
    dc->vmsd = &stc8g_adc_vmstate;
}

static const TypeInfo stc8g_adc_type = {
    .name = TYPE_STC8G_ADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc8gADCState),
    .instance_init = stc8g_adc_init,
    .class_init = stc8g_adc_class_init,
};

static void stc8g_adc_register_types(void)
{
    type_register_static(&stc8g_adc_type);
}

type_init(stc8g_adc_register_types)
