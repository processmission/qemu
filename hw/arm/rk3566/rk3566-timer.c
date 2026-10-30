/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "migration/vmstate.h"

#define TYPE_RK3566_TIMER "rk3566-timer"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566TimerState, RK3566_TIMER)

struct RK3566TimerState {
    SysBusDevice parent_obj;
    MemoryRegion io;
    qemu_irq irq;
    ptimer_state *timer;
    uint64_t load;
    uint32_t control;
    uint32_t status;
};

static void timer_irq(RK3566TimerState *s)
{
    qemu_set_irq(s->irq, s->status && (s->control & 4));
}

static void timer_tick(void *opaque)
{
    RK3566TimerState *s = opaque;
    s->status = 1;
    timer_irq(s);
}

static uint64_t timer_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3566TimerState *s = opaque;
    switch (offset) {
    case 0: return s->load;
    case 4: return s->load >> 32;
    case 8: return ptimer_get_count(s->timer);
    case 12: return ptimer_get_count(s->timer) >> 32;
    case 16: return s->control;
    case 24: return s->status;
    default: return 0;
    }
}

static void timer_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    RK3566TimerState *s = opaque;
    ptimer_transaction_begin(s->timer);
    switch (offset) {
    case 0:
        s->load = deposit64(s->load, 0, 32, value);
        break;
    case 4:
        s->load = deposit64(s->load, 32, 32, value);
        break;
    case 16:
        s->control = value & 7;
        ptimer_stop(s->timer);
        if (s->control & 1) {
            ptimer_set_limit(s->timer, s->load, 1);
            ptimer_run(s->timer, (s->control & 2) != 0);
        }
        break;
    case 24:
        s->status &= ~value;
        break;
    }
    ptimer_transaction_commit(s->timer);
    timer_irq(s);
}

static const MemoryRegionOps timer_ops = {
    .read = timer_read, .write = timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void timer_reset(DeviceState *dev)
{
    RK3566TimerState *s = RK3566_TIMER(dev);
    ptimer_transaction_begin(s->timer);
    ptimer_stop(s->timer);
    ptimer_set_freq(s->timer, 24000000);
    ptimer_transaction_commit(s->timer);
    s->load = s->control = s->status = 0;
    timer_irq(s);
}

static void rk_timer_init(Object *obj)
{
    RK3566TimerState *s = RK3566_TIMER(obj);
    memory_region_init_io(&s->io, obj, &timer_ops, s, TYPE_RK3566_TIMER,
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->io);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    s->timer = ptimer_init(timer_tick, s,
                           PTIMER_POLICY_WRAP_AFTER_ONE_PERIOD |
                           PTIMER_POLICY_CONTINUOUS_TRIGGER |
                           PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                           PTIMER_POLICY_NO_IMMEDIATE_RELOAD |
                           PTIMER_POLICY_NO_COUNTER_ROUND_DOWN);
}

static const VMStateDescription vmstate_timer = {
    .name = TYPE_RK3566_TIMER, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(load, RK3566TimerState),
        VMSTATE_UINT32(control, RK3566TimerState),
        VMSTATE_UINT32(status, RK3566TimerState),
        VMSTATE_PTIMER(timer, RK3566TimerState),
        VMSTATE_END_OF_LIST()
    },
};

static void timer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->vmsd = &vmstate_timer;
    device_class_set_legacy_reset(dc, timer_reset);
}

static const TypeInfo timer_info = {
    .name = TYPE_RK3566_TIMER, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3566TimerState),
    .instance_init = rk_timer_init, .class_init = timer_class_init,
};
static void timer_register(void) { type_register_static(&timer_info); }
type_init(timer_register)
