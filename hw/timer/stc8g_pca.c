/*
 * STC8G1K08A PCA/CCP/PWM controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/mcs51/clock.h"
#include "hw/timer/stc8g_pca.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "target/mcs51/cpu.h"
#include "trace.h"

REG8(CCON, 0)
    FIELD(CCON, CF, 7, 1)
    FIELD(CCON, CR, 6, 1)
    FIELD(CCON, CCF0, 0, 1)
REG8(CMOD, 0)
    FIELD(CMOD, CIDL, 7, 1)
    FIELD(CMOD, CPS, 1, 3)
    FIELD(CMOD, ECF, 0, 1)
REG8(CCAPM, 0)
    FIELD(CCAPM, ECOM, 6, 1)
    FIELD(CCAPM, CAPP, 5, 1)
    FIELD(CCAPM, CAPN, 4, 1)
    FIELD(CCAPM, MAT, 3, 1)
    FIELD(CCAPM, TOG, 2, 1)
    FIELD(CCAPM, PWM, 1, 1)
    FIELD(CCAPM, ECCF, 0, 1)
REG8(PCA_PWM, 0)
    FIELD(PCA_PWM, EBS, 6, 2)
    FIELD(PCA_PWM, XCCAPH, 4, 2)
    FIELD(PCA_PWM, XCCAPL, 2, 2)
    FIELD(PCA_PWM, EPCH, 1, 1)
    FIELD(PCA_PWM, EPCL, 0, 1)

#define STC8G_PCA_CHANNELS 3
#define STC8G_PCA_CCON_FLAGS 0x87

enum Stc8gPCARegister {
    STC8G_PCA_CCON,
    STC8G_PCA_CMOD,
    STC8G_PCA_CCAPM0,
    STC8G_PCA_CCAPM1,
    STC8G_PCA_CCAPM2,
    STC8G_PCA_CL,
    STC8G_PCA_CCAP0L,
    STC8G_PCA_CCAP1L,
    STC8G_PCA_CCAP2L,
    STC8G_PCA_PWM0,
    STC8G_PCA_PWM1,
    STC8G_PCA_PWM2,
    STC8G_PCA_CH,
    STC8G_PCA_CCAP0H,
    STC8G_PCA_CCAP1H,
    STC8G_PCA_CCAP2H,
};

struct Stc8gPCAState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array[STC8G_PCA_MMIO_REGS];
    RegisterInfo regs_info[STC8G_PCA_MMIO_REGS];
    uint8_t regs[STC8G_PCA_MMIO_REGS];
    QEMUTimer *timer;
    Clock *sysclk;
    MCS51CPU *cpu;
    qemu_irq irq;
    qemu_irq ccp_out[STC8G_PCA_CHANNELS];
    uint32_t clock_frequency;
    uint64_t clock_remainder;
    uint32_t clock_prescale_count;
    int64_t last_ns;
    bool ccp_input[STC8G_PCA_CHANNELS];
    bool ccp_output[STC8G_PCA_CHANNELS];
    bool eci_input;
    bool idle_paused;
};

static uint16_t stc8g_pca_count(Stc8gPCAState *s)
{
    return s->regs[STC8G_PCA_CH] << 8 | s->regs[STC8G_PCA_CL];
}

static void stc8g_pca_set_count(Stc8gPCAState *s, uint16_t value)
{
    s->regs[STC8G_PCA_CL] = value;
    s->regs[STC8G_PCA_CH] = value >> 8;
}

static uint16_t stc8g_pca_compare(Stc8gPCAState *s, unsigned channel)
{
    return s->regs[STC8G_PCA_CCAP0H + channel] << 8 |
           s->regs[STC8G_PCA_CCAP0L + channel];
}

static bool stc8g_pca_running(Stc8gPCAState *s)
{
    return FIELD_EX8(s->regs[STC8G_PCA_CCON], CCON, CR);
}

static bool stc8g_pca_should_pause_for_idle(Stc8gPCAState *s)
{
    return FIELD_EX8(s->cpu->env.pcon, PCON, IDL) &&
        FIELD_EX8(s->regs[STC8G_PCA_CMOD], CMOD, CIDL);
}

static unsigned stc8g_pca_clock_source(Stc8gPCAState *s)
{
    return FIELD_EX8(s->regs[STC8G_PCA_CMOD], CMOD, CPS);
}

static bool stc8g_pca_internal_clock(Stc8gPCAState *s)
{
    unsigned source = stc8g_pca_clock_source(s);

    return source != 2 && source != 3;
}

static unsigned stc8g_pca_clock_divider(Stc8gPCAState *s)
{
    static const unsigned divisors[] = { 12, 2, 0, 0, 1, 4, 6, 8 };

    return divisors[stc8g_pca_clock_source(s)];
}

static uint8_t stc8g_pca_ccapm(Stc8gPCAState *s, unsigned channel)
{
    return s->regs[STC8G_PCA_CCAPM0 + channel];
}

static bool stc8g_pca_pwm_enabled(Stc8gPCAState *s, unsigned channel)
{
    uint8_t mode = stc8g_pca_ccapm(s, channel);

    return FIELD_EX8(mode, CCAPM, ECOM) && FIELD_EX8(mode, CCAPM, PWM);
}

static unsigned stc8g_pca_pwm_width(Stc8gPCAState *s, unsigned channel)
{
    static const unsigned widths[] = { 8, 7, 6, 10 };
    uint8_t pwm = s->regs[STC8G_PCA_PWM0 + channel];

    return widths[FIELD_EX8(pwm, PCA_PWM, EBS)];
}

static unsigned stc8g_pca_pwm_compare(Stc8gPCAState *s, unsigned channel)
{
    uint8_t pwm = s->regs[STC8G_PCA_PWM0 + channel];
    unsigned width = stc8g_pca_pwm_width(s, channel);
    unsigned compare = s->regs[STC8G_PCA_CCAP0L + channel];

    if (width == 10) {
        compare = (compare & 0xff) |
                  (FIELD_EX8(pwm, PCA_PWM, XCCAPL) << 8);
    } else {
        compare &= MAKE_64BIT_MASK(0, width);
    }
    return compare | (FIELD_EX8(pwm, PCA_PWM, EPCL) << width);
}

static void stc8g_pca_update_irq(Stc8gPCAState *s)
{
    uint8_t ccon = s->regs[STC8G_PCA_CCON];
    bool pending = FIELD_EX8(ccon, CCON, CF) &&
                   FIELD_EX8(s->regs[STC8G_PCA_CMOD], CMOD, ECF);
    unsigned channel;

    for (channel = 0; channel < STC8G_PCA_CHANNELS; channel++) {
        if (extract8(ccon, channel, 1) &&
            FIELD_EX8(stc8g_pca_ccapm(s, channel), CCAPM, ECCF)) {
            pending = true;
        }
    }
    qemu_set_irq(s->irq, pending);
}

static void stc8g_pca_set_flag(Stc8gPCAState *s, unsigned channel)
{
    s->regs[STC8G_PCA_CCON] = deposit32(
        s->regs[STC8G_PCA_CCON], channel, 1, 1);
    trace_stc8g_pca_channel_event(channel, stc8g_pca_count(s));
}

static void stc8g_pca_reload_pwm(Stc8gPCAState *s, unsigned channel)
{
    unsigned reg = STC8G_PCA_PWM0 + channel;
    uint8_t pwm = s->regs[reg];

    s->regs[STC8G_PCA_CCAP0L + channel] =
        s->regs[STC8G_PCA_CCAP0H + channel];
    s->regs[reg] = FIELD_DP8(pwm, PCA_PWM, XCCAPL,
                              FIELD_EX8(pwm, PCA_PWM, XCCAPH));
    s->regs[reg] = FIELD_DP8(s->regs[reg], PCA_PWM, EPCL,
                              FIELD_EX8(pwm, PCA_PWM, EPCH));
}

static void stc8g_pca_update_pwm_output(Stc8gPCAState *s,
                                         unsigned channel,
                                         bool report_edge)
{
    unsigned width = stc8g_pca_pwm_width(s, channel);
    unsigned count = stc8g_pca_count(s) & MAKE_64BIT_MASK(0, width);
    bool output = count >= stc8g_pca_pwm_compare(s, channel);
    uint8_t mode = stc8g_pca_ccapm(s, channel);
    bool old = s->ccp_output[channel];

    if (old == output) {
        return;
    }
    s->ccp_output[channel] = output;
    qemu_set_irq(s->ccp_out[channel], output);
    trace_stc8g_pca_output(channel, output, stc8g_pca_count(s));
    if (report_edge &&
        ((output && FIELD_EX8(mode, CCAPM, CAPP)) ||
         (!output && FIELD_EX8(mode, CCAPM, CAPN)))) {
        stc8g_pca_set_flag(s, channel);
    }
}

static void stc8g_pca_update_outputs(Stc8gPCAState *s, bool report_edge)
{
    unsigned channel;

    for (channel = 0; channel < STC8G_PCA_CHANNELS; channel++) {
        if (stc8g_pca_pwm_enabled(s, channel)) {
            stc8g_pca_update_pwm_output(s, channel, report_edge);
        }
    }
}

static uint32_t stc8g_pca_next_event(Stc8gPCAState *s)
{
    uint16_t count = stc8g_pca_count(s);
    uint32_t distance = 0x10000 - count;
    unsigned channel;

    for (channel = 0; channel < STC8G_PCA_CHANNELS; channel++) {
        uint8_t mode = stc8g_pca_ccapm(s, channel);
        uint32_t candidate;

        if (stc8g_pca_pwm_enabled(s, channel)) {
            unsigned width = stc8g_pca_pwm_width(s, channel);
            unsigned period = 1u << width;
            unsigned low_count = count & (period - 1);
            unsigned compare = stc8g_pca_pwm_compare(s, channel);

            candidate = period - low_count;
            distance = MIN(distance, candidate);
            if (compare < period) {
                candidate = compare > low_count ? compare - low_count :
                    period - low_count + compare;
                distance = MIN(distance, candidate);
            }
        } else if (FIELD_EX8(mode, CCAPM, ECOM) &&
                   FIELD_EX8(mode, CCAPM, MAT)) {
            candidate = (stc8g_pca_compare(s, channel) - count) & 0xffff;
            distance = MIN(distance, candidate ? candidate : 0x10000);
        }
    }
    return distance;
}

static void stc8g_pca_process_event(Stc8gPCAState *s)
{
    uint16_t count = stc8g_pca_count(s);
    unsigned channel;

    if (!count) {
        s->regs[STC8G_PCA_CCON] = FIELD_DP8(
            s->regs[STC8G_PCA_CCON], CCON, CF, 1);
        trace_stc8g_pca_overflow();
    }
    for (channel = 0; channel < STC8G_PCA_CHANNELS; channel++) {
        uint8_t mode = stc8g_pca_ccapm(s, channel);

        if (stc8g_pca_pwm_enabled(s, channel)) {
            unsigned width = stc8g_pca_pwm_width(s, channel);

            if (!(count & ((1u << width) - 1))) {
                stc8g_pca_reload_pwm(s, channel);
            }
        } else if (FIELD_EX8(mode, CCAPM, ECOM) &&
                   FIELD_EX8(mode, CCAPM, MAT) &&
                   count == stc8g_pca_compare(s, channel)) {
            stc8g_pca_set_flag(s, channel);
            if (FIELD_EX8(mode, CCAPM, TOG)) {
                s->ccp_output[channel] = !s->ccp_output[channel];
                qemu_set_irq(s->ccp_out[channel], s->ccp_output[channel]);
                trace_stc8g_pca_output(channel, s->ccp_output[channel],
                                       count);
            }
        }
    }
    stc8g_pca_update_outputs(s, true);
    stc8g_pca_update_irq(s);
}

static void stc8g_pca_advance(Stc8gPCAState *s, uint64_t ticks)
{
    while (ticks) {
        uint32_t distance = MIN(ticks, stc8g_pca_next_event(s));

        stc8g_pca_set_count(s, stc8g_pca_count(s) + distance);
        ticks -= distance;
        stc8g_pca_process_event(s);
    }
}

static void stc8g_pca_sync(Stc8gPCAState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!s->idle_paused && clock_is_enabled(s->sysclk) &&
        stc8g_pca_running(s) && stc8g_pca_internal_clock(s)) {
        uint64_t cycles = mcs51_clock_elapsed_cycles(
            s->sysclk, now - s->last_ns, &s->clock_remainder);
        uint64_t prescaled;
        uint64_t ticks;

        prescaled = cycles + s->clock_prescale_count;
        ticks = prescaled / stc8g_pca_clock_divider(s);
        s->clock_prescale_count =
            prescaled % stc8g_pca_clock_divider(s);
        if (ticks) {
            stc8g_pca_advance(s, ticks);
        }
    }
    s->last_ns = now;
}

static void stc8g_pca_schedule(Stc8gPCAState *s)
{
    uint64_t cycles;
    uint64_t delta;
    unsigned divider;

    timer_del(s->timer);
    if (s->idle_paused || !clock_is_enabled(s->sysclk) ||
        !stc8g_pca_running(s) || !stc8g_pca_internal_clock(s)) {
        return;
    }
    divider = stc8g_pca_clock_divider(s);
    cycles = (uint64_t)stc8g_pca_next_event(s) * divider -
             s->clock_prescale_count;
    delta = mcs51_clock_cycles_to_ns(s->sysclk, cycles,
                                     s->clock_remainder);
    timer_mod_ns(s->timer, s->last_ns + MAX(1ull, delta));
}

static void stc8g_pca_expire(void *opaque)
{
    Stc8gPCAState *s = opaque;

    stc8g_pca_sync(s);
    stc8g_pca_schedule(s);
}

static void stc8g_pca_resync(Stc8gPCAState *s)
{
    stc8g_pca_sync(s);
    stc8g_pca_schedule(s);
}

static void stc8g_pca_clock_update(void *opaque, ClockEvent event)
{
    Stc8gPCAState *s = opaque;

    if (event == ClockPreUpdate) {
        stc8g_pca_resync(s);
        return;
    }
    s->clock_frequency = clock_get_hz(s->sysclk);
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    stc8g_pca_schedule(s);
}

static void stc8g_pca_cpu_sfr_write(void *opaque, uint8_t addr,
                                     uint8_t value)
{
    Stc8gPCAState *s = opaque;

    if (addr == MCS251_SFR_PCON) {
        stc8g_pca_sync(s);
        s->idle_paused = stc8g_pca_should_pause_for_idle(s);
        stc8g_pca_schedule(s);
    }
}

static uint64_t stc8g_pca_counter_post_read(RegisterInfo *reg,
                                             uint64_t value)
{
    Stc8gPCAState *s = STC8G_PCA(reg->opaque);

    stc8g_pca_resync(s);
    return s->regs[reg - s->regs_info];
}

static uint64_t stc8g_pca_counter_pre_write(RegisterInfo *reg,
                                             uint64_t value)
{
    stc8g_pca_resync(STC8G_PCA(reg->opaque));
    return value;
}

static uint64_t stc8g_pca_ccon_pre_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gPCAState *s = STC8G_PCA(reg->opaque);
    uint8_t old = s->regs[STC8G_PCA_CCON];

    stc8g_pca_resync(s);
    return (value & ~STC8G_PCA_CCON_FLAGS) |
           (old & value & STC8G_PCA_CCON_FLAGS);
}

static void stc8g_pca_ccon_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gPCAState *s = STC8G_PCA(reg->opaque);

    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    stc8g_pca_update_outputs(s, false);
    stc8g_pca_update_irq(s);
    stc8g_pca_schedule(s);
}

static uint64_t stc8g_pca_config_pre_write(RegisterInfo *reg,
                                            uint64_t value)
{
    stc8g_pca_resync(STC8G_PCA(reg->opaque));
    return value;
}

static uint64_t stc8g_pca_cmod_pre_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gPCAState *s = STC8G_PCA(reg->opaque);
    uint8_t old = s->regs[STC8G_PCA_CMOD];

    stc8g_pca_resync(s);
    if (FIELD_EX8(old, CMOD, CPS) != FIELD_EX8(value, CMOD, CPS)) {
        s->clock_prescale_count = 0;
    }
    return value;
}

static void stc8g_pca_config_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gPCAState *s = STC8G_PCA(reg->opaque);

    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->idle_paused = stc8g_pca_should_pause_for_idle(s);
    stc8g_pca_update_outputs(s, false);
    stc8g_pca_update_irq(s);
    stc8g_pca_schedule(s);
}

static const RegisterAccessInfo stc8g_pca_regs_info[] = {
    { .name = "CCON", .addr = 0, .rsvd = 0x30,
      .pre_write = stc8g_pca_ccon_pre_write,
      .post_read = stc8g_pca_counter_post_read,
      .post_write = stc8g_pca_ccon_post_write },
    { .name = "CMOD", .addr = 0, .rsvd = 0x70,
      .pre_write = stc8g_pca_cmod_pre_write,
      .post_write = stc8g_pca_config_post_write },
    { .name = "CCAPM0", .addr = 0, .rsvd = 0x80,
      .pre_write = stc8g_pca_config_pre_write,
      .post_write = stc8g_pca_config_post_write },
    { .name = "CCAPM1", .addr = 0, .rsvd = 0x80,
      .pre_write = stc8g_pca_config_pre_write,
      .post_write = stc8g_pca_config_post_write },
    { .name = "CCAPM2", .addr = 0, .rsvd = 0x80,
      .pre_write = stc8g_pca_config_pre_write,
      .post_write = stc8g_pca_config_post_write },
    { .name = "CL", .addr = 0,
      .pre_write = stc8g_pca_counter_pre_write,
      .post_read = stc8g_pca_counter_post_read,
      .post_write = stc8g_pca_config_post_write },
    { .name = "CCAP0L", .addr = 0,
      .pre_write = stc8g_pca_config_pre_write,
      .post_write = stc8g_pca_config_post_write },
    { .name = "CCAP1L", .addr = 0,
      .pre_write = stc8g_pca_config_pre_write,
      .post_write = stc8g_pca_config_post_write },
    { .name = "CCAP2L", .addr = 0,
      .pre_write = stc8g_pca_config_pre_write,
      .post_write = stc8g_pca_config_post_write },
    { .name = "PCA_PWM0", .addr = 0,
      .pre_write = stc8g_pca_config_pre_write,
      .post_write = stc8g_pca_config_post_write },
    { .name = "PCA_PWM1", .addr = 0,
      .pre_write = stc8g_pca_config_pre_write,
      .post_write = stc8g_pca_config_post_write },
    { .name = "PCA_PWM2", .addr = 0,
      .pre_write = stc8g_pca_config_pre_write,
      .post_write = stc8g_pca_config_post_write },
    { .name = "CH", .addr = 0,
      .pre_write = stc8g_pca_counter_pre_write,
      .post_read = stc8g_pca_counter_post_read,
      .post_write = stc8g_pca_config_post_write },
    { .name = "CCAP0H", .addr = 0,
      .pre_write = stc8g_pca_config_pre_write,
      .post_write = stc8g_pca_config_post_write },
    { .name = "CCAP1H", .addr = 0,
      .pre_write = stc8g_pca_config_pre_write,
      .post_write = stc8g_pca_config_post_write },
    { .name = "CCAP2H", .addr = 0,
      .pre_write = stc8g_pca_config_pre_write,
      .post_write = stc8g_pca_config_post_write },
};

static const MemoryRegionOps stc8g_pca_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc8g_pca_set_ccp_input(void *opaque, int channel, int level)
{
    Stc8gPCAState *s = opaque;
    uint8_t mode = stc8g_pca_ccapm(s, channel);
    bool old = s->ccp_input[channel];
    bool rising = !old && level;
    bool falling = old && !level;

    s->ccp_input[channel] = !!level;
    if (!stc8g_pca_running(s) ||
        (!(rising && FIELD_EX8(mode, CCAPM, CAPP)) &&
         !(falling && FIELD_EX8(mode, CCAPM, CAPN)))) {
        return;
    }
    stc8g_pca_resync(s);
    s->regs[STC8G_PCA_CCAP0L + channel] = stc8g_pca_count(s);
    s->regs[STC8G_PCA_CCAP0H + channel] = stc8g_pca_count(s) >> 8;
    stc8g_pca_set_flag(s, channel);
    stc8g_pca_update_irq(s);
}

static void stc8g_pca_set_eci(void *opaque, int n, int level)
{
    Stc8gPCAState *s = opaque;
    bool falling = s->eci_input && !level;

    s->eci_input = !!level;
    if (falling && !s->idle_paused && stc8g_pca_running(s) &&
        stc8g_pca_clock_source(s) == 3) {
        stc8g_pca_advance(s, 1);
        stc8g_pca_schedule(s);
    }
}

static void stc8g_pca_timer0_overflow(void *opaque, int n, int level)
{
    Stc8gPCAState *s = opaque;

    if (level && !s->idle_paused && stc8g_pca_running(s) &&
        stc8g_pca_clock_source(s) == 2) {
        stc8g_pca_advance(s, level);
        stc8g_pca_schedule(s);
    }
}

static void stc8g_pca_reset(DeviceState *dev)
{
    Stc8gPCAState *s = STC8G_PCA(dev);
    unsigned index;
    unsigned channel;

    timer_del(s->timer);
    s->clock_remainder = 0;
    s->clock_prescale_count = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    memset(s->ccp_output, 0, sizeof(s->ccp_output));
    for (index = 0; index < ARRAY_SIZE(stc8g_pca_regs_info); index++) {
        register_reset(&s->regs_info[index]);
    }
    s->idle_paused = stc8g_pca_should_pause_for_idle(s);
    qemu_set_irq(s->irq, 0);
    for (channel = 0; channel < STC8G_PCA_CHANNELS; channel++) {
        qemu_set_irq(s->ccp_out[channel], 0);
    }
}

static int stc8g_pca_post_load(void *opaque, int version_id)
{
    Stc8gPCAState *s = opaque;
    unsigned channel;

    if (version_id < 2) {
        s->idle_paused = stc8g_pca_should_pause_for_idle(s);
    }
    stc8g_pca_update_irq(s);
    for (channel = 0; channel < STC8G_PCA_CHANNELS; channel++) {
        qemu_set_irq(s->ccp_out[channel], s->ccp_output[channel]);
    }
    return 0;
}

static const VMStateDescription stc8g_pca_vmstate = {
    .name = "stc8g.pca",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = stc8g_pca_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Stc8gPCAState, STC8G_PCA_MMIO_REGS),
        VMSTATE_UINT64(clock_remainder, Stc8gPCAState),
        VMSTATE_UINT32(clock_prescale_count, Stc8gPCAState),
        VMSTATE_INT64(last_ns, Stc8gPCAState),
        VMSTATE_BOOL_ARRAY(ccp_input, Stc8gPCAState, STC8G_PCA_CHANNELS),
        VMSTATE_BOOL_ARRAY(ccp_output, Stc8gPCAState, STC8G_PCA_CHANNELS),
        VMSTATE_BOOL(eci_input, Stc8gPCAState),
        VMSTATE_BOOL_V(idle_paused, Stc8gPCAState, 2),
        VMSTATE_TIMER_PTR(timer, Stc8gPCAState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc8g_pca_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc8gPCAState, cpu, TYPE_MCS51_CPU,
                     MCS51CPU *),
    DEFINE_PROP_UINT32("clock-frequency", Stc8gPCAState,
                       clock_frequency, 24000000),
};

static void stc8g_pca_realize(DeviceState *dev, Error **errp)
{
    Stc8gPCAState *s = STC8G_PCA(dev);

    if (!s->cpu) {
        error_setg(errp, "stc8g-pca requires a CPU link");
    } else if (!s->clock_frequency) {
        error_setg(errp, "stc8g-pca clock-frequency must be nonzero");
    } else {
        mcs251_cpu_add_sfr_write_notifier(s->cpu,
                                          stc8g_pca_cpu_sfr_write, s);
    }
}

static void stc8g_pca_init(Object *obj)
{
    Stc8gPCAState *s = STC8G_PCA(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned index;

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc8g_pca_regs_info) !=
                      STC8G_PCA_MMIO_REGS);
    for (index = 0; index < ARRAY_SIZE(stc8g_pca_regs_info); index++) {
        s->reg_array[index] = register_init_block8(
            DEVICE(obj), &stc8g_pca_regs_info[index], 1,
            &s->regs_info[index], &s->regs[index], &stc8g_pca_ops,
            false, 1);
        sysbus_init_mmio(sbd, &s->reg_array[index]->mem);
    }
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), stc8g_pca_set_ccp_input,
                            "ccp-in", STC8G_PCA_CHANNELS);
    qdev_init_gpio_in_named(DEVICE(obj), stc8g_pca_set_eci, "eci", 1);
    qdev_init_gpio_in_named(DEVICE(obj), stc8g_pca_timer0_overflow,
                            "timer0-overflow", 1);
    s->sysclk = qdev_init_clock_in(DEVICE(obj), "sysclk",
                                   stc8g_pca_clock_update, s,
                                   ClockPreUpdate | ClockUpdate);
    qdev_init_gpio_out_named(DEVICE(obj), s->ccp_out, "ccp-out",
                             STC8G_PCA_CHANNELS);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stc8g_pca_expire, s);
}

static void stc8g_pca_finalize(Object *obj)
{
    Stc8gPCAState *s = STC8G_PCA(obj);

    timer_free(s->timer);
}

static void stc8g_pca_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc8g_pca_realize;
    device_class_set_legacy_reset(dc, stc8g_pca_reset);
    device_class_set_props(dc, stc8g_pca_properties);
    dc->vmsd = &stc8g_pca_vmstate;
}

static const TypeInfo stc8g_pca_type = {
    .name = TYPE_STC8G_PCA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc8gPCAState),
    .instance_init = stc8g_pca_init,
    .instance_finalize = stc8g_pca_finalize,
    .class_init = stc8g_pca_class_init,
};

static void stc8g_pca_register_types(void)
{
    type_register_static(&stc8g_pca_type);
}

type_init(stc8g_pca_register_types)
