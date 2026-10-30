/*
 * Sony PlayStation 2 Emotion Engine timers
 *
 * Four 16-bit up-counters clocked from the bus clock (optionally divided
 * by 16 or 256) or from HBLANK.  Each can raise an interrupt on a compare
 * match or an overflow, and can return to zero on a compare match.  The
 * counters advance lazily from QEMU_CLOCK_VIRTUAL; a QEMUTimer is only
 * armed when an interrupt is enabled.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/timer/ps2_timer.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

static uint64_t ps2_timer_freq(PS2TimerChannel *t)
{
    switch (t->mode & PS2_TIMER_MODE_CLKS) {
    case 0:
        return PS2_BUS_CLOCK_HZ;
    case 1:
        return PS2_BUS_CLOCK_HZ / 16;
    case 2:
        return PS2_BUS_CLOCK_HZ / 256;
    default:
        return PS2_HBLANK_HZ;
    }
}

static uint32_t ps2_timer_period(PS2TimerChannel *t)
{
    return t->comp ? t->comp : 0x10000;
}

static void ps2_timer_advance(PS2TimerChannel *t, uint64_t delta)
{
    uint64_t c = t->count + delta;
    bool target = false;
    bool overflow = false;
    bool fire = false;

    if (t->mode & PS2_TIMER_MODE_ZRET) {
        uint32_t period = ps2_timer_period(t);

        if (t->count >= period) {
            /* Started above the target: it has to wrap around first. */
            if (c > 0xffff) {
                overflow = true;
                c -= 0x10000;
            } else {
                period = 0;
            }
        }
        if (period && c >= period) {
            target = true;
            c %= period;
        }
    } else {
        if (t->count < t->comp && c >= t->comp) {
            target = true;
        }
        if (c > 0xffff) {
            overflow = true;
            c &= 0xffff;
            if (c >= t->comp) {
                target = true;
            }
        }
    }
    t->count = c;

    if (target && (t->mode & PS2_TIMER_MODE_CMPE) &&
        !(t->mode & PS2_TIMER_MODE_EQUF)) {
        t->mode |= PS2_TIMER_MODE_EQUF;
        fire = true;
    }
    if (overflow && (t->mode & PS2_TIMER_MODE_OVFE) &&
        !(t->mode & PS2_TIMER_MODE_OVFF)) {
        t->mode |= PS2_TIMER_MODE_OVFF;
        fire = true;
    }
    if (fire) {
        qemu_irq_pulse(t->irq);
    }
}

static void ps2_timer_sync(PS2TimerChannel *t, int64_t now)
{
    uint64_t ticks;

    if (!(t->mode & PS2_TIMER_MODE_CUE)) {
        t->base_ns = now;
        t->ticks_seen = 0;
        return;
    }

    ticks = muldiv64(now - t->base_ns, ps2_timer_freq(t),
                     NANOSECONDS_PER_SECOND);
    if (ticks > t->ticks_seen) {
        uint64_t delta = ticks - t->ticks_seen;

        t->ticks_seen = ticks;
        ps2_timer_advance(t, delta);
    }
}

static void ps2_timer_rebase(PS2TimerChannel *t, int64_t now)
{
    t->base_ns = now;
    t->ticks_seen = 0;
}

static void ps2_timer_schedule(PS2TimerChannel *t)
{
    uint64_t n;

    if (!(t->mode & PS2_TIMER_MODE_CUE) ||
        !(t->mode & (PS2_TIMER_MODE_CMPE | PS2_TIMER_MODE_OVFE))) {
        timer_del(t->timer);
        return;
    }

    n = 0x10000 - t->count;
    if (t->mode & PS2_TIMER_MODE_ZRET) {
        uint32_t period = ps2_timer_period(t);

        if (t->count < period) {
            n = period - t->count;
        }
    } else if (t->count < t->comp) {
        n = t->comp - t->count;
    }

    timer_mod(t->timer, t->base_ns + 1 +
              muldiv64(t->ticks_seen + n, NANOSECONDS_PER_SECOND,
                       ps2_timer_freq(t)));
}

static void ps2_timer_expire(void *opaque)
{
    PS2TimerChannel *t = opaque;

    ps2_timer_sync(t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    ps2_timer_schedule(t);
}

static uint64_t ps2_timer_read(void *opaque, hwaddr addr, unsigned int size)
{
    PS2TimerState *s = opaque;
    PS2TimerChannel *t = &s->ch[addr / PS2_TIMER_STRIDE];

    switch (addr % PS2_TIMER_STRIDE) {
    case PS2_TIMER_COUNT:
        ps2_timer_sync(t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        return t->count;
    case PS2_TIMER_MODE:
        return t->mode;
    case PS2_TIMER_COMP:
        return t->comp;
    case PS2_TIMER_HOLD:
        return t->hold;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
}

static void ps2_timer_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned int size)
{
    PS2TimerState *s = opaque;
    PS2TimerChannel *t = &s->ch[addr / PS2_TIMER_STRIDE];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    ps2_timer_sync(t, now);

    switch (addr % PS2_TIMER_STRIDE) {
    case PS2_TIMER_COUNT:
        t->count = value & 0xffff;
        ps2_timer_rebase(t, now);
        break;
    case PS2_TIMER_MODE:
        /* EQUF and OVFF are write-1-to-clear. */
        t->mode &= ~(value & PS2_TIMER_MODE_FLAGS);
        t->mode = (t->mode & PS2_TIMER_MODE_FLAGS) | (value & 0x3ff);
        ps2_timer_rebase(t, now);
        break;
    case PS2_TIMER_COMP:
        t->comp = value & 0xffff;
        break;
    case PS2_TIMER_HOLD:
        t->hold = value & 0xffff;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }
    ps2_timer_schedule(t);
}

static const MemoryRegionOps ps2_timer_ops = {
    .read = ps2_timer_read,
    .write = ps2_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void ps2_timer_reset_hold(Object *obj, ResetType type)
{
    PS2TimerState *s = PS2_TIMER(obj);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (int i = 0; i < PS2_TIMER_NUM; i++) {
        PS2TimerChannel *t = &s->ch[i];

        timer_del(t->timer);
        t->count = 0;
        t->mode = 0;
        t->comp = 0;
        t->hold = 0;
        ps2_timer_rebase(t, now);
    }
}

static void ps2_timer_init(Object *obj)
{
    PS2TimerState *s = PS2_TIMER(obj);

    memory_region_init_io(&s->iomem, obj, &ps2_timer_ops, s, TYPE_PS2_TIMER,
                          PS2_TIMER_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);

    for (int i = 0; i < PS2_TIMER_NUM; i++) {
        PS2TimerChannel *t = &s->ch[i];

        t->parent = s;
        t->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ps2_timer_expire, t);
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &t->irq);
    }
}

static void ps2_timer_finalize(Object *obj)
{
    PS2TimerState *s = PS2_TIMER(obj);

    for (int i = 0; i < PS2_TIMER_NUM; i++) {
        timer_free(s->ch[i].timer);
    }
}

static const VMStateDescription vmstate_ps2_timer_channel = {
    .name = TYPE_PS2_TIMER "/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, PS2TimerChannel),
        VMSTATE_UINT32(count, PS2TimerChannel),
        VMSTATE_UINT32(mode, PS2TimerChannel),
        VMSTATE_UINT32(comp, PS2TimerChannel),
        VMSTATE_UINT32(hold, PS2TimerChannel),
        VMSTATE_INT64(base_ns, PS2TimerChannel),
        VMSTATE_UINT64(ticks_seen, PS2TimerChannel),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_ps2_timer = {
    .name = TYPE_PS2_TIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(ch, PS2TimerState, PS2_TIMER_NUM, 1,
                             vmstate_ps2_timer_channel, PS2TimerChannel),
        VMSTATE_END_OF_LIST()
    },
};

static void ps2_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = ps2_timer_reset_hold;
    dc->vmsd = &vmstate_ps2_timer;
}

static const TypeInfo ps2_timer_types[] = {
    {
        .name = TYPE_PS2_TIMER,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PS2TimerState),
        .instance_init = ps2_timer_init,
        .instance_finalize = ps2_timer_finalize,
        .class_init = ps2_timer_class_init,
    },
};

DEFINE_TYPES(ps2_timer_types);
