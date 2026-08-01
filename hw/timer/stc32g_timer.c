/*
 * STC32G Timer 0 and Timer 1
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "exec/cputlb.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/mcs51/clock.h"
#include "hw/timer/stc32g_timer.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "target/mcs51/cpu.h"

REG8(TCON, 0)
REG8(TMOD, 1)
    FIELD(TMOD, T0_M, 0, 2)
    FIELD(TMOD, T0_CT, 2, 1)
    FIELD(TMOD, T0_GATE, 3, 1)
    FIELD(TMOD, T1_M, 4, 2)
    FIELD(TMOD, T1_CT, 6, 1)
    FIELD(TMOD, T1_GATE, 7, 1)
REG8(TL0, 2)
REG8(TL1, 3)
REG8(TH0, 4)
REG8(TH1, 5)
REG8(AUXR, 6)
REG8(INTCLKO, 7)

REG8(TM0PS, 0)
REG8(TM1PS, 1)

#define STC32G_TIMER_SFR_REGS (R_INTCLKO + 1)
#define STC32G_TIMER_XFR_REGS (R_TM1PS + 1)

typedef struct Stc32gTimerChannel {
    Stc32gTimerState *parent;
    unsigned index;
} Stc32gTimerChannel;

struct Stc32gTimerState {
    SysBusDevice parent_obj;

    MCS251CPU *cpu;
    RegisterInfoArray *sfr_reg_array;
    RegisterInfoArray *xfr_reg_array;
    RegisterInfo sfr_regs_info[STC32G_TIMER_SFR_REGS];
    RegisterInfo xfr_regs_info[STC32G_TIMER_XFR_REGS];
    uint8_t sfr_regs[STC32G_TIMER_SFR_REGS];
    uint8_t xfr_regs[STC32G_TIMER_XFR_REGS];
    QEMUTimer *timer[2];
    Clock *sysclk;
    Stc32gTimerChannel channel[2];
    qemu_irq irq[4];
    qemu_irq pca_clock;
    uint32_t clock_frequency;
    uint8_t reload_tl[2];
    uint8_t reload_th[2];
    uint8_t counter_prescale_count[2];
    /* Whole source-clock cycles accumulated toward the next timer tick. */
    uint32_t clock_prescale_count[2];
    /* Fractional source-clock cycles, in Clock period units. */
    uint64_t clock_remainder[2];
    int64_t last_ns[2];
    bool gate[2];
    bool counter_input[2];
    bool resetting;
};

static unsigned stc32g_timer_mode(Stc32gTimerState *s, unsigned n)
{
    return n ? FIELD_EX8(s->sfr_regs[R_TMOD], TMOD, T1_M) :
               FIELD_EX8(s->sfr_regs[R_TMOD], TMOD, T0_M);
}

static bool stc32g_timer_run_bit(Stc32gTimerState *s, unsigned n)
{
    CPUMCS251State *env = &s->cpu->env;
    return n ? FIELD_EX8(env->tcon, TCON, TR1) :
               FIELD_EX8(env->tcon, TCON, TR0);
}

static bool stc32g_timer_gate_open(Stc32gTimerState *s, unsigned n)
{
    bool gate_enabled =
        n ? FIELD_EX8(s->sfr_regs[R_TMOD], TMOD, T1_GATE) :
            FIELD_EX8(s->sfr_regs[R_TMOD], TMOD, T0_GATE);

    return !gate_enabled || s->gate[n];
}

static bool stc32g_timer_counter_mode(Stc32gTimerState *s, unsigned n)
{
    return n ? FIELD_EX8(s->sfr_regs[R_TMOD], TMOD, T1_CT) :
               FIELD_EX8(s->sfr_regs[R_TMOD], TMOD, T0_CT);
}

static bool stc32g_timer_active(Stc32gTimerState *s, unsigned n,
                                bool counter_mode)
{
    if (!stc32g_timer_run_bit(s, n) ||
        !stc32g_timer_gate_open(s, n) ||
        (n == 1 && stc32g_timer_mode(s, n) == 3)) {
        return false;
    }
    return stc32g_timer_counter_mode(s, n) == counter_mode;
}

static uint32_t stc32g_timer_divider(Stc32gTimerState *s, unsigned n)
{
    CPUMCS251State *env = &s->cpu->env;
    bool x12 = n ? FIELD_EX8(env->auxr, AUXR, T1X12) :
                   FIELD_EX8(env->auxr, AUXR, T0X12);
    uint32_t divider = x12 ? 1 : 12;

    return divider * (s->xfr_regs[R_TM0PS + n] + 1);
}

static uint32_t stc32g_timer_value(Stc32gTimerState *s, unsigned n)
{
    if (stc32g_timer_mode(s, n) == 2) {
        return s->sfr_regs[R_TL0 + n];
    }
    return s->sfr_regs[R_TH0 + n] << 8 |
           s->sfr_regs[R_TL0 + n];
}

static uint32_t stc32g_timer_reload(Stc32gTimerState *s, unsigned n)
{
    if (stc32g_timer_mode(s, n) == 2) {
        return s->sfr_regs[R_TH0 + n];
    }
    return s->reload_th[n] << 8 | s->reload_tl[n];
}

static void stc32g_timer_set_value(Stc32gTimerState *s, unsigned n,
                                   uint32_t value)
{
    s->sfr_regs[R_TL0 + n] = value;
    if (stc32g_timer_mode(s, n) != 2) {
        s->sfr_regs[R_TH0 + n] = value >> 8;
    }
}

static void stc32g_timer_update_irq(Stc32gTimerState *s, unsigned source)
{
    CPUMCS251State *env = &s->cpu->env;
    bool level;

    switch (source) {
    case MCS251_IRQ_INT0:
        level = FIELD_EX8(env->tcon, TCON, IT0) ?
            FIELD_EX8(env->tcon, TCON, IE0) : !s->gate[0];
        break;
    case MCS251_IRQ_TIMER0:
        level = FIELD_EX8(env->tcon, TCON, TF0);
        break;
    case MCS251_IRQ_INT1:
        level = FIELD_EX8(env->tcon, TCON, IT1) ?
            FIELD_EX8(env->tcon, TCON, IE1) : !s->gate[1];
        break;
    case MCS251_IRQ_TIMER1:
        level = FIELD_EX8(env->tcon, TCON, TF1);
        break;
    default:
        g_assert_not_reached();
    }
    qemu_set_irq(s->irq[source], level);
}

static void stc32g_timer_update_irqs(Stc32gTimerState *s)
{
    unsigned source;

    for (source = 0; source < 4; source++) {
        stc32g_timer_update_irq(s, source);
    }
}

static void stc32g_timer_overflows(Stc32gTimerState *s, unsigned n,
                                   uint64_t count)
{
    uint64_t batch;

    g_assert(count);
    if (n) {
        s->cpu->env.tcon =
            FIELD_DP8(s->cpu->env.tcon, TCON, TF1, 1);
    } else {
        s->cpu->env.tcon =
            FIELD_DP8(s->cpu->env.tcon, TCON, TF0, 1);
    }
    stc32g_timer_update_irq(s, n ? MCS251_IRQ_TIMER1 :
                                  MCS251_IRQ_TIMER0);
    if (!n) {
        /*
         * The PCA clock input receives the number of Timer 0 overflow
         * edges, so a long virtual-clock step does not require one qirq
         * callback per edge.
         */
        while (count) {
            batch = MIN(count, (uint64_t)INT_MAX);
            qemu_set_irq(s->pca_clock, batch);
            count -= batch;
        }
    }
}

static void stc32g_timer_advance(Stc32gTimerState *s, unsigned n,
                                 uint64_t ticks)
{
    unsigned mode = stc32g_timer_mode(s, n);
    uint32_t value = stc32g_timer_value(s, n);
    uint32_t limit = mode == 2 ? 0x100 : 0x10000;
    bool reload_mode = mode == 0 || (n == 0 && mode == 3) || mode == 2;
    uint32_t reload = stc32g_timer_reload(s, n);
    uint64_t distance;
    uint64_t overflows;

    if (!ticks) {
        return;
    }

    distance = limit - value;
    if (ticks < distance) {
        stc32g_timer_set_value(s, n, value + ticks);
        return;
    }

    ticks -= distance;
    if (!reload_mode) {
        overflows = 1 + ticks / limit;
        ticks %= limit;
        stc32g_timer_overflows(s, n, overflows);
        stc32g_timer_set_value(s, n, ticks % limit);
        return;
    }

    value = reload;
    distance = limit - value;
    overflows = 1 + ticks / distance;
    ticks %= distance;
    stc32g_timer_overflows(s, n, overflows);
    stc32g_timer_set_value(s, n, value + ticks);
}

static void stc32g_timer_sync(Stc32gTimerState *s, unsigned n)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (clock_is_enabled(s->sysclk) && stc32g_timer_active(s, n, false)) {
        uint32_t divider = stc32g_timer_divider(s, n);
        uint64_t cycles = mcs51_clock_elapsed_cycles(
            s->sysclk, now - s->last_ns[n], &s->clock_remainder[n]);
        uint64_t prescaled;
        uint64_t ticks;

        prescaled = cycles + s->clock_prescale_count[n];
        ticks = prescaled / divider;
        s->clock_prescale_count[n] = prescaled % divider;
        stc32g_timer_advance(s, n, ticks);
    }
    s->last_ns[n] = now;
}

static uint32_t stc32g_timer_ticks_to_overflow(Stc32gTimerState *s,
                                               unsigned n)
{
    unsigned mode = stc32g_timer_mode(s, n);
    uint32_t limit = mode == 2 ? 0x100 : 0x10000;

    return limit - stc32g_timer_value(s, n);
}

static void stc32g_timer_schedule(Stc32gTimerState *s, unsigned n)
{
    uint64_t ticks;
    uint64_t cycles;
    uint64_t delta;
    uint32_t divider;

    timer_del(s->timer[n]);
    if (!clock_is_enabled(s->sysclk) ||
        !stc32g_timer_active(s, n, false)) {
        return;
    }

    ticks = stc32g_timer_ticks_to_overflow(s, n);
    divider = stc32g_timer_divider(s, n);
    cycles = ticks * divider - s->clock_prescale_count[n];
    delta = mcs51_clock_cycles_to_ns(s->sysclk, cycles,
                                     s->clock_remainder[n]);
    timer_mod_ns(s->timer[n], s->last_ns[n] + MAX(1ull, delta));
}

static void stc32g_timer_resync(Stc32gTimerState *s)
{
    unsigned n;

    for (n = 0; n < 2; n++) {
        stc32g_timer_sync(s, n);
        stc32g_timer_schedule(s, n);
    }
}

static void stc32g_timer_clock_update(void *opaque, ClockEvent event)
{
    Stc32gTimerState *s = opaque;
    unsigned n;

    if (event == ClockPreUpdate) {
        if (!s->resetting) {
            stc32g_timer_resync(s);
        }
        return;
    }
    s->clock_frequency = clock_get_hz(s->sysclk);
    if (!s->resetting) {
        for (n = 0; n < 2; n++) {
            s->last_ns[n] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            stc32g_timer_schedule(s, n);
        }
    }
}

static void stc32g_timer_expire(void *opaque)
{
    Stc32gTimerChannel *channel = opaque;
    Stc32gTimerState *s = channel->parent;

    stc32g_timer_sync(s, channel->index);
    stc32g_timer_schedule(s, channel->index);
}

static uint64_t stc32g_timer_sfr_post_read(RegisterInfo *reg,
                                           uint64_t value)
{
    Stc32gTimerState *s = STC32G_TIMER(reg->opaque);
    CPUMCS251State *env = &s->cpu->env;
    unsigned index = reg - s->sfr_regs_info;
    unsigned n;

    switch (index) {
    case R_TCON:
        return env->tcon;
    case R_TL0:
    case R_TL1:
        n = index - R_TL0;
        stc32g_timer_sync(s, n);
        stc32g_timer_schedule(s, n);
        return s->sfr_regs[index];
    case R_TH0:
    case R_TH1:
        n = index - R_TH0;
        stc32g_timer_sync(s, n);
        stc32g_timer_schedule(s, n);
        return s->sfr_regs[index];
    case R_AUXR:
        return env->auxr;
    case R_INTCLKO:
        return env->intclko;
    default:
        return value;
    }
}

static void stc32g_timer_counter_write(Stc32gTimerState *s, unsigned n,
                                       bool high, uint8_t value)
{
    unsigned mode;

    if (s->resetting) {
        if (high) {
            s->reload_th[n] = value;
        } else {
            s->reload_tl[n] = value;
        }
        return;
    }

    stc32g_timer_sync(s, n);
    mode = stc32g_timer_mode(s, n);
    if (mode == 2) {
        if (high) {
            s->sfr_regs[R_TH0 + n] = value;
        } else {
            s->sfr_regs[R_TL0 + n] = value;
        }
    } else if ((mode == 0 || (n == 0 && mode == 3)) &&
               stc32g_timer_run_bit(s, n)) {
        if (high) {
            s->reload_th[n] = value;
        } else {
            s->reload_tl[n] = value;
        }
    } else {
        if (high) {
            s->sfr_regs[R_TH0 + n] = value;
            s->reload_th[n] = value;
        } else {
            s->sfr_regs[R_TL0 + n] = value;
            s->reload_tl[n] = value;
        }
    }
    s->last_ns[n] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    stc32g_timer_schedule(s, n);
}

static uint64_t stc32g_timer_counter_pre_write(RegisterInfo *reg,
                                               uint64_t value)
{
    Stc32gTimerState *s = STC32G_TIMER(reg->opaque);
    unsigned index = reg - s->sfr_regs_info;
    bool high = index >= R_TH0;
    unsigned n = index - (high ? R_TH0 : R_TL0);

    stc32g_timer_counter_write(s, n, high, value);
    return s->resetting ? value : s->sfr_regs[index];
}

static void stc32g_timer_tcon_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc32gTimerState *s = STC32G_TIMER(reg->opaque);
    CPUMCS251State *env = &s->cpu->env;

    if (!s->resetting) {
        stc32g_timer_resync(s);
    }
    env->tcon = value;
    if (!s->resetting) {
        mcs251_cpu_sync_irq_configuration(s->cpu);
    }
    stc32g_timer_update_irqs(s);
    if (!s->resetting) {
        stc32g_timer_resync(s);
    }
}

static uint64_t stc32g_timer_tmod_pre_write(RegisterInfo *reg,
                                             uint64_t value)
{
    Stc32gTimerState *s = STC32G_TIMER(reg->opaque);

    if (!s->resetting) {
        stc32g_timer_resync(s);
    }
    return value;
}

static void stc32g_timer_tmod_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc32gTimerState *s = STC32G_TIMER(reg->opaque);
    CPUMCS251State *env = &s->cpu->env;

    env->timer0_mode3 = FIELD_EX8(value, TMOD, T0_M) == 3;
    if (!env->timer0_mode3) {
        env->timer0_mode3_armed = false;
    } else if (FIELD_EX8(env->ie, IE, ET0)) {
        env->timer0_mode3_armed = true;
    }
    if (!s->resetting) {
        stc32g_timer_resync(s);
    }
}

static void stc32g_timer_auxr_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc32gTimerState *s = STC32G_TIMER(reg->opaque);
    CPUMCS251State *env = &s->cpu->env;
    uint8_t old_auxr = env->auxr;
    bool flush = FIELD_EX8(env->auxr, AUXR, RAMEXE) !=
                 FIELD_EX8(value, AUXR, RAMEXE);
    unsigned n;

    if (!s->resetting) {
        stc32g_timer_resync(s);
    }
    env->auxr = value;
    if (!s->resetting) {
        for (n = 0; n < 2; n++) {
            unsigned field = n ? R_AUXR_T1X12_SHIFT :
                                 R_AUXR_T0X12_SHIFT;

            if (extract8(old_auxr, field, 1) !=
                extract8(value, field, 1)) {
                s->clock_prescale_count[n] = 0;
            }
        }
        if (flush) {
            tlb_flush(CPU(s->cpu));
        }
        stc32g_timer_resync(s);
    }
}

static void stc32g_timer_intclko_post_write(RegisterInfo *reg,
                                            uint64_t value)
{
    Stc32gTimerState *s = STC32G_TIMER(reg->opaque);

    s->cpu->env.intclko = value;
    mcs251_cpu_notify_sfr_write(s->cpu, MCS251_SFR_INTCLKO, value);
}

static uint64_t stc32g_timer_prescaler_pre_write(RegisterInfo *reg,
                                                  uint64_t value)
{
    Stc32gTimerState *s = STC32G_TIMER(reg->opaque);
    unsigned n = reg - s->xfr_regs_info;

    if (!s->resetting) {
        stc32g_timer_sync(s, n);
    }
    return value;
}

static void stc32g_timer_prescaler_post_write(RegisterInfo *reg,
                                              uint64_t value)
{
    Stc32gTimerState *s = STC32G_TIMER(reg->opaque);
    unsigned n = reg - s->xfr_regs_info;

    s->counter_prescale_count[n] = 0;
    s->clock_prescale_count[n] = 0;
    s->last_ns[n] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!s->resetting) {
        stc32g_timer_schedule(s, n);
    }
}

static const RegisterAccessInfo stc32g_timer_sfr_regs_info[] = {
    {   .name = "TCON", .addr = A_TCON,
        .post_read = stc32g_timer_sfr_post_read,
        .post_write = stc32g_timer_tcon_post_write,
    },{ .name = "TMOD", .addr = A_TMOD,
        .pre_write = stc32g_timer_tmod_pre_write,
        .post_write = stc32g_timer_tmod_post_write,
    },{ .name = "TL0", .addr = A_TL0,
        .pre_write = stc32g_timer_counter_pre_write,
        .post_read = stc32g_timer_sfr_post_read,
    },{ .name = "TL1", .addr = A_TL1,
        .pre_write = stc32g_timer_counter_pre_write,
        .post_read = stc32g_timer_sfr_post_read,
    },{ .name = "TH0", .addr = A_TH0,
        .pre_write = stc32g_timer_counter_pre_write,
        .post_read = stc32g_timer_sfr_post_read,
    },{ .name = "TH1", .addr = A_TH1,
        .pre_write = stc32g_timer_counter_pre_write,
        .post_read = stc32g_timer_sfr_post_read,
    },{ .name = "AUXR", .addr = A_AUXR, .reset = 0x01,
        .post_read = stc32g_timer_sfr_post_read,
        .post_write = stc32g_timer_auxr_post_write,
    },{ .name = "INTCLKO", .addr = A_INTCLKO,
        .post_read = stc32g_timer_sfr_post_read,
        .post_write = stc32g_timer_intclko_post_write,
    },
};

static const RegisterAccessInfo stc32g_timer_xfr_regs_info[] = {
    {   .name = "TM0PS", .addr = A_TM0PS,
        .pre_write = stc32g_timer_prescaler_pre_write,
        .post_write = stc32g_timer_prescaler_post_write,
    },{ .name = "TM1PS", .addr = A_TM1PS,
        .pre_write = stc32g_timer_prescaler_pre_write,
        .post_write = stc32g_timer_prescaler_post_write,
    },
};

static const MemoryRegionOps stc32g_timer_regs_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc32g_timer_set_gate(void *opaque, int n, int level)
{
    Stc32gTimerState *s = opaque;
    CPUMCS251State *env = &s->cpu->env;
    bool falling = s->gate[n] && !level;
    bool edge_triggered;

    /*
     * P3.2/P3.3 are both the timer gate inputs and the dedicated external
     * interrupt pins.  Keep TCON flag generation and its IRQ output in this
     * device so each CPU interrupt input has only one qirq driver.
     */
    stc32g_timer_sync(s, n);
    s->gate[n] = level;
    edge_triggered = n ? FIELD_EX8(env->tcon, TCON, IT1) :
                         FIELD_EX8(env->tcon, TCON, IT0);
    if (edge_triggered && falling) {
        if (n) {
            env->tcon = FIELD_DP8(env->tcon, TCON, IE1, 1);
        } else {
            env->tcon = FIELD_DP8(env->tcon, TCON, IE0, 1);
        }
        stc32g_timer_update_irq(s, n ? MCS251_IRQ_INT1 :
                                      MCS251_IRQ_INT0);
    } else if (!edge_triggered) {
        if (n) {
            env->tcon = FIELD_DP8(env->tcon, TCON, IE1, !level);
        } else {
            env->tcon = FIELD_DP8(env->tcon, TCON, IE0, !level);
        }
        stc32g_timer_update_irq(s, n ? MCS251_IRQ_INT1 :
                                      MCS251_IRQ_INT0);
    }
    s->last_ns[n] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    stc32g_timer_schedule(s, n);
}

static void stc32g_timer_set_counter(void *opaque, int n, int level)
{
    Stc32gTimerState *s = opaque;
    bool falling = !level && s->counter_input[n];

    s->counter_input[n] = level;
    if (falling && stc32g_timer_active(s, n, true)) {
        if (s->counter_prescale_count[n] ==
            s->xfr_regs[R_TM0PS + n]) {
            s->counter_prescale_count[n] = 0;
            stc32g_timer_advance(s, n, 1);
        } else {
            s->counter_prescale_count[n]++;
        }
    }
}

static void stc32g_timer_reset(DeviceState *dev)
{
    Stc32gTimerState *s = STC32G_TIMER(dev);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned n;
    unsigned reg;

    s->resetting = true;
    memset(s->reload_tl, 0, sizeof(s->reload_tl));
    memset(s->reload_th, 0, sizeof(s->reload_th));
    memset(s->counter_prescale_count, 0,
           sizeof(s->counter_prescale_count));
    memset(s->clock_prescale_count, 0,
           sizeof(s->clock_prescale_count));
    memset(s->clock_remainder, 0, sizeof(s->clock_remainder));
    for (n = 0; n < 2; n++) {
        s->last_ns[n] = now;
        timer_del(s->timer[n]);
    }
    for (reg = 0; reg < ARRAY_SIZE(stc32g_timer_sfr_regs_info); reg++) {
        register_reset(&s->sfr_regs_info[reg]);
    }
    for (reg = 0; reg < ARRAY_SIZE(stc32g_timer_xfr_regs_info); reg++) {
        register_reset(&s->xfr_regs_info[reg]);
    }
    s->resetting = false;
    stc32g_timer_update_irqs(s);
}

static int stc32g_timer_post_load(void *opaque, int version_id)
{
    Stc32gTimerState *s = opaque;

    s->sfr_regs[R_TCON] = s->cpu->env.tcon;
    s->sfr_regs[R_AUXR] = s->cpu->env.auxr;
    s->sfr_regs[R_INTCLKO] = s->cpu->env.intclko;
    stc32g_timer_update_irqs(s);
    return 0;
}

static const VMStateDescription stc32g_timer_vmstate = {
    .name = "stc32g.timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stc32g_timer_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(sfr_regs, Stc32gTimerState,
                            STC32G_TIMER_SFR_REGS),
        VMSTATE_UINT8_ARRAY(xfr_regs, Stc32gTimerState,
                            STC32G_TIMER_XFR_REGS),
        VMSTATE_UINT8_ARRAY(reload_tl, Stc32gTimerState, 2),
        VMSTATE_UINT8_ARRAY(reload_th, Stc32gTimerState, 2),
        VMSTATE_UINT8_ARRAY(counter_prescale_count, Stc32gTimerState, 2),
        VMSTATE_UINT32_ARRAY(clock_prescale_count, Stc32gTimerState, 2),
        VMSTATE_UINT64_ARRAY(clock_remainder, Stc32gTimerState, 2),
        VMSTATE_INT64_ARRAY(last_ns, Stc32gTimerState, 2),
        VMSTATE_BOOL_ARRAY(gate, Stc32gTimerState, 2),
        VMSTATE_BOOL_ARRAY(counter_input, Stc32gTimerState, 2),
        VMSTATE_TIMER_PTR_ARRAY(timer, Stc32gTimerState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc32g_timer_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc32gTimerState, cpu, TYPE_MCS51_CPU,
                     MCS251CPU *),
    DEFINE_PROP_UINT32("clock-frequency", Stc32gTimerState,
                       clock_frequency, 24000000),
};

static void stc32g_timer_realize(DeviceState *dev, Error **errp)
{
    Stc32gTimerState *s = STC32G_TIMER(dev);

    if (!s->cpu) {
        error_setg(errp, "stc32g-timer requires a CPU link");
    } else if (!s->clock_frequency) {
        error_setg(errp, "stc32g-timer clock-frequency must be nonzero");
    } else if (!clock_has_source(s->sysclk)) {
        clock_set_hz(s->sysclk, s->clock_frequency);
    }
}

static void stc32g_timer_init(Object *obj)
{
    Stc32gTimerState *s = STC32G_TIMER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned n;

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc32g_timer_sfr_regs_info) !=
                      STC32G_TIMER_SFR_REGS);
    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc32g_timer_xfr_regs_info) !=
                      STC32G_TIMER_XFR_REGS);
    s->sfr_reg_array = register_init_block8(
        DEVICE(obj), stc32g_timer_sfr_regs_info,
        ARRAY_SIZE(stc32g_timer_sfr_regs_info), s->sfr_regs_info,
        s->sfr_regs, &stc32g_timer_regs_ops, false,
        STC32G_TIMER_SFR_REGS);
    s->xfr_reg_array = register_init_block8(
        DEVICE(obj), stc32g_timer_xfr_regs_info,
        ARRAY_SIZE(stc32g_timer_xfr_regs_info), s->xfr_regs_info,
        s->xfr_regs, &stc32g_timer_regs_ops, false,
        STC32G_TIMER_XFR_REGS);
    sysbus_init_mmio(sbd, &s->sfr_reg_array->mem);
    sysbus_init_mmio(sbd, &s->xfr_reg_array->mem);
    for (n = 0; n < 4; n++) {
        sysbus_init_irq(sbd, &s->irq[n]);
    }
    qdev_init_gpio_out_named(DEVICE(obj), &s->pca_clock, "pca-clock", 1);
    qdev_init_gpio_in_named(DEVICE(obj), stc32g_timer_set_gate,
                            "gate", 2);
    qdev_init_gpio_in_named(DEVICE(obj), stc32g_timer_set_counter,
                            "counter", 2);
    s->sysclk = qdev_init_clock_in(DEVICE(obj), "sysclk",
                                   stc32g_timer_clock_update, s,
                                   ClockPreUpdate | ClockUpdate);
    for (n = 0; n < 2; n++) {
        s->gate[n] = true;
        s->counter_input[n] = true;
        s->channel[n].parent = s;
        s->channel[n].index = n;
        s->timer[n] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   stc32g_timer_expire,
                                   &s->channel[n]);
    }
}

static void stc32g_timer_finalize(Object *obj)
{
    Stc32gTimerState *s = STC32G_TIMER(obj);
    unsigned n;

    for (n = 0; n < 2; n++) {
        timer_free(s->timer[n]);
    }
}

static void stc32g_timer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc32g_timer_realize;
    device_class_set_legacy_reset(dc, stc32g_timer_reset);
    device_class_set_props(dc, stc32g_timer_properties);
    dc->vmsd = &stc32g_timer_vmstate;
}

static void stc32g_timer_register_types(void)
{
    static const TypeInfo types[] = {
        {
            .name = TYPE_STC32G_TIMER,
            .parent = TYPE_SYS_BUS_DEVICE,
            .instance_size = sizeof(Stc32gTimerState),
            .instance_init = stc32g_timer_init,
            .instance_finalize = stc32g_timer_finalize,
            .class_init = stc32g_timer_class_init,
        }, {
            .name = TYPE_STC8G_TIMER,
            .parent = TYPE_STC32G_TIMER,
        },
    };

    type_register_static_array(types, ARRAY_SIZE(types));
}

type_init(stc32g_timer_register_types)
