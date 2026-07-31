/*
 * STC32G Timer 0 and Timer 1
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "exec/cputlb.h"
#include "hw/core/irq.h"
#include "hw/mcs251/stc32g.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#define TCON_IE0 0x02
#define TCON_IE1 0x08
#define TCON_TR0 0x10
#define TCON_TF0 0x20
#define TCON_TR1 0x40
#define TCON_TF1 0x80

static unsigned stc32g_timer_mode(Stc32gTimerState *s, unsigned n)
{
    return extract8(s->tmod, n * 4, 2);
}

static bool stc32g_timer_run_bit(Stc32gTimerState *s, unsigned n)
{
    CPUMCS251State *env = &s->cpu->env;
    uint8_t mask = n ? TCON_TR1 : TCON_TR0;

    return env->tcon & mask;
}

static bool stc32g_timer_gate_open(Stc32gTimerState *s, unsigned n)
{
    uint8_t gate_mask = 0x08 << (n * 4);

    return !(s->tmod & gate_mask) || s->gate[n];
}

static bool stc32g_timer_counter_mode(Stc32gTimerState *s, unsigned n)
{
    return s->tmod & (0x04 << (n * 4));
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

static uint32_t stc32g_timer_rate(Stc32gTimerState *s, unsigned n)
{
    CPUMCS251State *env = &s->cpu->env;
    uint8_t x12_mask = n ? 0x40 : 0x80;
    uint32_t divider = env->auxr & x12_mask ? 1 : 12;

    divider *= s->prescaler[n] + 1;
    return MAX(1u, s->clock_frequency / divider);
}

static uint32_t stc32g_timer_value(Stc32gTimerState *s, unsigned n)
{
    if (stc32g_timer_mode(s, n) == 2) {
        return s->tl[n];
    }
    return s->th[n] << 8 | s->tl[n];
}

static uint32_t stc32g_timer_reload(Stc32gTimerState *s, unsigned n)
{
    if (stc32g_timer_mode(s, n) == 2) {
        return s->th[n];
    }
    return s->reload_th[n] << 8 | s->reload_tl[n];
}

static void stc32g_timer_set_value(Stc32gTimerState *s, unsigned n,
                                   uint32_t value)
{
    s->tl[n] = value;
    if (stc32g_timer_mode(s, n) != 2) {
        s->th[n] = value >> 8;
    }
}

static void stc32g_timer_update_irq(Stc32gTimerState *s, unsigned source)
{
    static const uint8_t masks[4] = {
        TCON_IE0, TCON_TF0, TCON_IE1, TCON_TF1,
    };

    qemu_set_irq(s->irq[source], s->cpu->env.tcon & masks[source]);
}

static void stc32g_timer_update_irqs(Stc32gTimerState *s)
{
    unsigned source;

    for (source = 0; source < 4; source++) {
        stc32g_timer_update_irq(s, source);
    }
}

static void stc32g_timer_overflow(Stc32gTimerState *s, unsigned n)
{
    s->cpu->env.tcon |= n ? TCON_TF1 : TCON_TF0;
    stc32g_timer_update_irq(s, n ? MCS251_IRQ_TIMER1 :
                                  MCS251_IRQ_TIMER0);
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

    if (!ticks) {
        return;
    }

    distance = limit - value;
    if (ticks < distance) {
        stc32g_timer_set_value(s, n, value + ticks);
        return;
    }

    ticks -= distance;
    stc32g_timer_overflow(s, n);
    if (!reload_mode) {
        stc32g_timer_set_value(s, n, ticks % limit);
        return;
    }

    value = reload;
    distance = limit - value;
    if (ticks >= distance) {
        ticks %= distance;
        stc32g_timer_overflow(s, n);
    }
    stc32g_timer_set_value(s, n, value + ticks);
}

static void stc32g_timer_sync(Stc32gTimerState *s, unsigned n)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (stc32g_timer_active(s, n, false)) {
        uint64_t elapsed = now - s->last_ns[n];
        uint64_t ticks = muldiv64(elapsed, stc32g_timer_rate(s, n),
                                  NANOSECONDS_PER_SECOND);

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
    uint64_t delta;
    uint32_t rate;

    timer_del(s->timer[n]);
    if (!stc32g_timer_active(s, n, false)) {
        return;
    }

    ticks = stc32g_timer_ticks_to_overflow(s, n);
    rate = stc32g_timer_rate(s, n);
    delta = (ticks * NANOSECONDS_PER_SECOND + rate - 1) / rate;
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

static void stc32g_timer_expire(void *opaque)
{
    Stc32gTimerChannel *channel = opaque;
    Stc32gTimerState *s = channel->parent;

    stc32g_timer_sync(s, channel->index);
    stc32g_timer_schedule(s, channel->index);
}

static uint64_t stc32g_timer_sfr_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    Stc32gTimerState *s = opaque;
    CPUMCS251State *env = &s->cpu->env;

    switch (offset) {
    case 0:
        return env->tcon;
    case 1:
        return s->tmod;
    case 2:
    case 3:
        stc32g_timer_sync(s, offset - 2);
        stc32g_timer_schedule(s, offset - 2);
        return s->tl[offset - 2];
    case 4:
    case 5:
        stc32g_timer_sync(s, offset - 4);
        stc32g_timer_schedule(s, offset - 4);
        return s->th[offset - 4];
    case 6:
        return env->auxr;
    case 7:
        return env->intclko;
    default:
        return 0;
    }
}

static void stc32g_timer_counter_write(Stc32gTimerState *s, unsigned n,
                                       bool high, uint8_t value)
{
    unsigned mode;

    stc32g_timer_sync(s, n);
    mode = stc32g_timer_mode(s, n);
    if (mode == 2) {
        if (high) {
            s->th[n] = value;
        } else {
            s->tl[n] = value;
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
            s->th[n] = value;
            s->reload_th[n] = value;
        } else {
            s->tl[n] = value;
            s->reload_tl[n] = value;
        }
    }
    s->last_ns[n] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    stc32g_timer_schedule(s, n);
}

static void stc32g_timer_sfr_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    Stc32gTimerState *s = opaque;
    CPUMCS251State *env = &s->cpu->env;
    uint8_t byte = value;

    switch (offset) {
    case 0:
        stc32g_timer_resync(s);
        env->tcon = byte;
        stc32g_timer_update_irqs(s);
        stc32g_timer_resync(s);
        break;
    case 1:
        stc32g_timer_resync(s);
        s->tmod = byte;
        env->timer0_mode3 = (byte & 3) == 3;
        if (!env->timer0_mode3) {
            env->timer0_mode3_armed = false;
        } else if (env->ie & 0x02) {
            env->timer0_mode3_armed = true;
        }
        stc32g_timer_resync(s);
        break;
    case 2:
    case 3:
        stc32g_timer_counter_write(s, offset - 2, false, byte);
        break;
    case 4:
    case 5:
        stc32g_timer_counter_write(s, offset - 4, true, byte);
        break;
    case 6: {
        bool flush = (env->auxr ^ byte) & 0x02;

        stc32g_timer_resync(s);
        env->auxr = byte;
        if (flush) {
            tlb_flush(CPU(s->cpu));
        }
        stc32g_timer_resync(s);
        break;
    }
    case 7:
        env->intclko = byte;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps stc32g_timer_sfr_ops = {
    .read = stc32g_timer_sfr_read,
    .write = stc32g_timer_sfr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static uint64_t stc32g_timer_xfr_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    Stc32gTimerState *s = opaque;

    return offset < 2 ? s->prescaler[offset] : 0;
}

static void stc32g_timer_xfr_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    Stc32gTimerState *s = opaque;

    if (offset < 2) {
        stc32g_timer_sync(s, offset);
        s->prescaler[offset] = value;
        s->counter_prescale_count[offset] = 0;
        s->last_ns[offset] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        stc32g_timer_schedule(s, offset);
    }
}

static const MemoryRegionOps stc32g_timer_xfr_ops = {
    .read = stc32g_timer_xfr_read,
    .write = stc32g_timer_xfr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc32g_timer_set_gate(void *opaque, int n, int level)
{
    Stc32gTimerState *s = opaque;
    CPUMCS251State *env = &s->cpu->env;
    uint8_t it_mask = n ? 0x04 : 0x01;
    uint8_t flag_mask = n ? TCON_IE1 : TCON_IE0;
    bool changed = s->gate[n] != !!level;
    bool falling = s->gate[n] && !level;

    /*
     * P3.2/P3.3 are both the timer gate inputs and the dedicated external
     * interrupt pins.  Keep TCON flag generation and its IRQ output in this
     * device so each CPU interrupt input has only one qirq driver.
     */
    stc32g_timer_sync(s, n);
    s->gate[n] = level;
    if (env->tcon & it_mask ? falling : changed) {
        env->tcon |= flag_mask;
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
        if (s->counter_prescale_count[n] == s->prescaler[n]) {
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

    s->tmod = 0;
    memset(s->tl, 0, sizeof(s->tl));
    memset(s->th, 0, sizeof(s->th));
    memset(s->reload_tl, 0, sizeof(s->reload_tl));
    memset(s->reload_th, 0, sizeof(s->reload_th));
    memset(s->prescaler, 0, sizeof(s->prescaler));
    memset(s->counter_prescale_count, 0,
           sizeof(s->counter_prescale_count));
    memset(s->counter_input, 0, sizeof(s->counter_input));
    for (n = 0; n < 2; n++) {
        s->last_ns[n] = now;
        s->gate[n] = true;
        timer_del(s->timer[n]);
    }
    s->cpu->env.tcon = 0;
    s->cpu->env.timer0_mode3 = false;
    s->cpu->env.timer0_mode3_armed = false;
    stc32g_timer_update_irqs(s);
}

static int stc32g_timer_post_load(void *opaque, int version_id)
{
    Stc32gTimerState *s = opaque;

    stc32g_timer_update_irqs(s);
    return 0;
}

static const VMStateDescription stc32g_timer_vmstate = {
    .name = "stc32g.timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stc32g_timer_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(tmod, Stc32gTimerState),
        VMSTATE_UINT8_ARRAY(tl, Stc32gTimerState, 2),
        VMSTATE_UINT8_ARRAY(th, Stc32gTimerState, 2),
        VMSTATE_UINT8_ARRAY(reload_tl, Stc32gTimerState, 2),
        VMSTATE_UINT8_ARRAY(reload_th, Stc32gTimerState, 2),
        VMSTATE_UINT8_ARRAY(prescaler, Stc32gTimerState, 2),
        VMSTATE_UINT8_ARRAY(counter_prescale_count, Stc32gTimerState, 2),
        VMSTATE_INT64_ARRAY(last_ns, Stc32gTimerState, 2),
        VMSTATE_BOOL_ARRAY(gate, Stc32gTimerState, 2),
        VMSTATE_BOOL_ARRAY(counter_input, Stc32gTimerState, 2),
        VMSTATE_TIMER_PTR_ARRAY(timer, Stc32gTimerState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc32g_timer_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc32gTimerState, cpu, TYPE_MCS251_CPU,
                     MCS251CPU *),
    DEFINE_PROP_UINT32("clock-frequency", Stc32gTimerState,
                       clock_frequency, 24000000),
};

static void stc32g_timer_realize(DeviceState *dev, Error **errp)
{
    Stc32gTimerState *s = STC32G_TIMER(dev);

    if (!s->cpu) {
        error_setg(errp, "stc32g-timer requires a CPU link");
    }
}

static void stc32g_timer_init(Object *obj)
{
    Stc32gTimerState *s = STC32G_TIMER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned n;

    memory_region_init_io(&s->sfr, obj, &stc32g_timer_sfr_ops, s,
                          "stc32g.timer-sfr", 8);
    memory_region_init_io(&s->xfr, obj, &stc32g_timer_xfr_ops, s,
                          "stc32g.timer-xfr", 2);
    sysbus_init_mmio(sbd, &s->sfr);
    sysbus_init_mmio(sbd, &s->xfr);
    for (n = 0; n < 4; n++) {
        sysbus_init_irq(sbd, &s->irq[n]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), stc32g_timer_set_gate,
                            "gate", 2);
    qdev_init_gpio_in_named(DEVICE(obj), stc32g_timer_set_counter,
                            "counter", 2);
    for (n = 0; n < 2; n++) {
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

static const TypeInfo stc32g_timer_type = {
    .name = TYPE_STC32G_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc32gTimerState),
    .instance_init = stc32g_timer_init,
    .instance_finalize = stc32g_timer_finalize,
    .class_init = stc32g_timer_class_init,
};

static void stc32g_timer_register_types(void)
{
    type_register_static(&stc32g_timer_type);
}

type_init(stc32g_timer_register_types)
