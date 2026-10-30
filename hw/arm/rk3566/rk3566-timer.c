/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "hw/core/register.h"
#include "migration/vmstate.h"

#define TYPE_RK3566_TIMER "rk3566-timer"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566TimerState, RK3566_TIMER)

REG32(LOAD0, 0x00)
REG32(LOAD1, 0x04)
REG32(CUR0, 0x08)
REG32(CUR1, 0x0c)
REG32(CONTROL, 0x10)
REG32(STATUS, 0x18)

#define RK3566_TIMER_NUM_REGS (A_STATUS / 4 + 1)

struct RK3566TimerState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[RK3566_TIMER_NUM_REGS];
    uint32_t regs[RK3566_TIMER_NUM_REGS];

    qemu_irq irq;
    ptimer_state *timer;
};

static uint64_t timer_load(RK3566TimerState *s)
{
    return ((uint64_t)s->regs[R_LOAD1] << 32) | s->regs[R_LOAD0];
}

static void timer_irq(RK3566TimerState *s)
{
    qemu_set_irq(s->irq, s->regs[R_STATUS] && (s->regs[R_CONTROL] & 4));
}

static void timer_tick(void *opaque)
{
    RK3566TimerState *s = opaque;

    s->regs[R_STATUS] = 1;
    timer_irq(s);
}

static uint64_t timer_cur_post_read(RegisterInfo *reg, uint64_t val)
{
    RK3566TimerState *s = RK3566_TIMER(reg->opaque);
    uint64_t count = ptimer_get_count(s->timer);

    return reg->access->addr == A_CUR0 ? count : count >> 32;
}

static uint64_t timer_control_pre_write(RegisterInfo *reg, uint64_t val)
{
    return val & 7;
}

static void timer_control_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566TimerState *s = RK3566_TIMER(reg->opaque);

    ptimer_transaction_begin(s->timer);
    ptimer_stop(s->timer);
    if (val & 1) {
        ptimer_set_limit(s->timer, timer_load(s), 1);
        ptimer_run(s->timer, (val & 2) != 0);
    }
    ptimer_transaction_commit(s->timer);
    timer_irq(s);
}

static void timer_status_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566TimerState *s = RK3566_TIMER(reg->opaque);

    timer_irq(s);
}

static const RegisterAccessInfo timer_regs_info[] = {
    {
        .name = "LOAD0",
        .addr = A_LOAD0,
    }, {
        .name = "LOAD1",
        .addr = A_LOAD1,
    }, {
        .name = "CUR0",
        .addr = A_CUR0,
        .ro = UINT32_MAX,
        .post_read = timer_cur_post_read,
    }, {
        .name = "CUR1",
        .addr = A_CUR1,
        .ro = UINT32_MAX,
        .post_read = timer_cur_post_read,
    }, {
        .name = "CONTROL",
        .addr = A_CONTROL,
        .pre_write = timer_control_pre_write,
        .post_write = timer_control_post_write,
    }, {
        .name = "STATUS",
        .addr = A_STATUS,
        .w1c = UINT32_MAX,
        .post_write = timer_status_post_write,
    },
};

static const MemoryRegionOps timer_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
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

    for (unsigned int i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        register_reset(&s->regs_info[i]);
    }
    timer_irq(s);
}

static void rk_timer_init(Object *obj)
{
    RK3566TimerState *s = RK3566_TIMER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->reg_array = register_init_block32(DEVICE(obj), timer_regs_info,
                                         ARRAY_SIZE(timer_regs_info),
                                         s->regs_info, s->regs,
                                         &timer_ops, false, 0x1000);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
    sysbus_init_irq(sbd, &s->irq);
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
        VMSTATE_UINT32_ARRAY(regs, RK3566TimerState, RK3566_TIMER_NUM_REGS),
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
