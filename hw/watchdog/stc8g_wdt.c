/*
 * STC8G1K08A watchdog timer
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
#include "hw/mcs51/clock.h"
#include "hw/watchdog/stc8g_wdt.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/watchdog.h"
#include "target/mcs51/cpu.h"
#include "trace.h"

REG8(WDT_CONTR, 0)
    FIELD(WDT_CONTR, WDT_FLAG, 7, 1)
    FIELD(WDT_CONTR, EN_WDT, 5, 1)
    FIELD(WDT_CONTR, CLR_WDT, 4, 1)
    FIELD(WDT_CONTR, IDL_WDT, 3, 1)
    FIELD(WDT_CONTR, WDT_PS, 0, 3)

#define STC8G_WDT_CYCLES_BASE (12ULL * 32768)

struct Stc8gWdtState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[STC8G_WDT_MMIO_REGS];
    uint8_t regs[STC8G_WDT_MMIO_REGS];
    uint8_t old_wdt_contr;
    MCS51CPU *cpu;
    Clock *sysclk;
    QEMUTimer *timer;
    uint64_t cycles_left;
    uint32_t clock_frequency;
    uint64_t clock_remainder;
    int64_t last_ns;
    bool clear_requested;
    bool idle_paused;
};

static bool stc8g_wdt_enabled(Stc8gWdtState *s)
{
    return FIELD_EX8(s->regs[STC8G_WDT_MMIO_CONTR], WDT_CONTR, EN_WDT);
}

static bool stc8g_wdt_should_pause_for_idle(Stc8gWdtState *s)
{
    return FIELD_EX8(s->cpu->env.pcon, PCON, IDL) &&
        !FIELD_EX8(s->regs[STC8G_WDT_MMIO_CONTR], WDT_CONTR, IDL_WDT);
}

static uint64_t stc8g_wdt_cycles(Stc8gWdtState *s)
{
    unsigned prescale = FIELD_EX8(s->regs[STC8G_WDT_MMIO_CONTR],
                                  WDT_CONTR, WDT_PS);

    return STC8G_WDT_CYCLES_BASE << (prescale + 1);
}

static void stc8g_wdt_schedule(Stc8gWdtState *s)
{
    uint64_t timeout;

    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!stc8g_wdt_enabled(s) || s->idle_paused ||
        !clock_is_enabled(s->sysclk) || !s->cycles_left) {
        timer_del(s->timer);
        return;
    }

    timeout = mcs51_clock_cycles_to_ns(s->sysclk, s->cycles_left,
                                       s->clock_remainder);
    timer_mod_ns(s->timer, s->last_ns + timeout);
    trace_stc8g_wdt_reload(
        FIELD_EX8(s->regs[STC8G_WDT_MMIO_CONTR], WDT_CONTR, WDT_PS),
        s->cycles_left, timeout);
}

static void stc8g_wdt_reload(Stc8gWdtState *s)
{
    s->cycles_left = stc8g_wdt_cycles(s);
    s->clock_remainder = 0;
    stc8g_wdt_schedule(s);
}

static void stc8g_wdt_expire(Stc8gWdtState *s)
{
    s->cycles_left = 0;
    timer_del(s->timer);
    s->regs[STC8G_WDT_MMIO_CONTR] = FIELD_DP8(
        s->regs[STC8G_WDT_MMIO_CONTR], WDT_CONTR, WDT_FLAG, 1);
    trace_stc8g_wdt_expire(s->clock_frequency);
    watchdog_perform_action();
}

static void stc8g_wdt_sync(Stc8gWdtState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed;

    if (!stc8g_wdt_enabled(s) || s->idle_paused ||
        !clock_is_enabled(s->sysclk) || !s->cycles_left) {
        s->last_ns = now;
        return;
    }

    elapsed = mcs51_clock_elapsed_cycles(s->sysclk, now - s->last_ns,
                                         &s->clock_remainder);
    if (elapsed >= s->cycles_left) {
        stc8g_wdt_expire(s);
        return;
    }
    s->cycles_left -= elapsed;
    s->last_ns = now;
}

static void stc8g_wdt_timer_expire(void *opaque)
{
    stc8g_wdt_expire(STC8G_WDT(opaque));
}

static void stc8g_wdt_clock_update(void *opaque, ClockEvent event)
{
    Stc8gWdtState *s = opaque;

    if (event == ClockPreUpdate) {
        stc8g_wdt_sync(s);
    } else {
        s->clock_frequency = clock_get_hz(s->sysclk);
        stc8g_wdt_schedule(s);
    }
}

static void stc8g_wdt_cpu_sfr_write(void *opaque, uint8_t addr,
                                     uint8_t value)
{
    Stc8gWdtState *s = opaque;

    if (addr == MCS251_SFR_PCON) {
        stc8g_wdt_sync(s);
        s->idle_paused = stc8g_wdt_should_pause_for_idle(s);
        stc8g_wdt_schedule(s);
    }
}

static uint64_t stc8g_wdt_contr_pre_write(RegisterInfo *reg,
                                           uint64_t value)
{
    Stc8gWdtState *s = STC8G_WDT(reg->opaque);
    uint8_t old = s->regs[STC8G_WDT_MMIO_CONTR];
    uint8_t next = value;

    stc8g_wdt_sync(s);
    s->old_wdt_contr = old;
    s->clear_requested = FIELD_EX8(next, WDT_CONTR, CLR_WDT);
    next = FIELD_DP8(next, WDT_CONTR, CLR_WDT, 0);
    if (FIELD_EX8(old, WDT_CONTR, EN_WDT)) {
        next = FIELD_DP8(next, WDT_CONTR, EN_WDT, 1);
    }
    next = FIELD_DP8(next, WDT_CONTR, WDT_FLAG,
                     FIELD_EX8(old, WDT_CONTR, WDT_FLAG) &&
                     FIELD_EX8(value, WDT_CONTR, WDT_FLAG));
    return next;
}

static void stc8g_wdt_contr_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gWdtState *s = STC8G_WDT(reg->opaque);
    uint8_t old = s->old_wdt_contr;
    uint8_t next = s->regs[STC8G_WDT_MMIO_CONTR];

    s->idle_paused = stc8g_wdt_should_pause_for_idle(s);
    if ((!FIELD_EX8(old, WDT_CONTR, EN_WDT) &&
         FIELD_EX8(next, WDT_CONTR, EN_WDT)) ||
        (FIELD_EX8(old, WDT_CONTR, WDT_PS) !=
         FIELD_EX8(next, WDT_CONTR, WDT_PS)) || s->clear_requested) {
        stc8g_wdt_reload(s);
    } else {
        stc8g_wdt_schedule(s);
    }
    s->clear_requested = false;
}

static const RegisterAccessInfo stc8g_wdt_regs_info[] = {
    { .name = "WDT_CONTR", .addr = 0, .rsvd = 0x40,
      .pre_write = stc8g_wdt_contr_pre_write,
      .post_write = stc8g_wdt_contr_post_write },
};

static const MemoryRegionOps stc8g_wdt_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc8g_wdt_reset(DeviceState *dev)
{
    Stc8gWdtState *s = STC8G_WDT(dev);

    /* EN_WDT can only be cleared by a power-on reset, not a warm reset. */
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->idle_paused = stc8g_wdt_should_pause_for_idle(s);
    if (stc8g_wdt_enabled(s)) {
        stc8g_wdt_reload(s);
    } else {
        timer_del(s->timer);
    }
}

static int stc8g_wdt_pre_save(void *opaque)
{
    stc8g_wdt_sync(STC8G_WDT(opaque));
    return 0;
}

static int stc8g_wdt_post_load(void *opaque, int version_id)
{
    Stc8gWdtState *s = opaque;

    s->clock_frequency = clock_get_hz(s->sysclk);
    if (version_id < 2) {
        s->idle_paused = stc8g_wdt_should_pause_for_idle(s);
    }
    stc8g_wdt_schedule(s);
    return 0;
}

static const VMStateDescription stc8g_wdt_vmstate = {
    .name = "stc8g.wdt",
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_save = stc8g_wdt_pre_save,
    .post_load = stc8g_wdt_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Stc8gWdtState, STC8G_WDT_MMIO_REGS),
        VMSTATE_UINT64(cycles_left, Stc8gWdtState),
        VMSTATE_UINT64(clock_remainder, Stc8gWdtState),
        VMSTATE_INT64(last_ns, Stc8gWdtState),
        VMSTATE_BOOL_V(idle_paused, Stc8gWdtState, 2),
        VMSTATE_TIMER_PTR(timer, Stc8gWdtState),
        VMSTATE_END_OF_LIST()
    },
};

static void stc8g_wdt_init(Object *obj)
{
    Stc8gWdtState *s = STC8G_WDT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->reg_array = register_init_block8(DEVICE(obj), stc8g_wdt_regs_info,
                                         STC8G_WDT_MMIO_REGS, s->regs_info,
                                         s->regs, &stc8g_wdt_ops, false, 1);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
    s->sysclk = qdev_init_clock_in(DEVICE(obj), "sysclk",
                                   stc8g_wdt_clock_update, s,
                                   ClockPreUpdate | ClockUpdate);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stc8g_wdt_timer_expire, s);
}

static void stc8g_wdt_finalize(Object *obj)
{
    Stc8gWdtState *s = STC8G_WDT(obj);

    timer_free(s->timer);
}

static void stc8g_wdt_realize(DeviceState *dev, Error **errp)
{
    Stc8gWdtState *s = STC8G_WDT(dev);

    if (!s->cpu) {
        error_setg(errp, "stc8g-wdt requires a CPU link");
        return;
    }
    mcs251_cpu_add_sfr_write_notifier(s->cpu, stc8g_wdt_cpu_sfr_write, s);
    s->clock_frequency = clock_get_hz(s->sysclk);
}

static const Property stc8g_wdt_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc8gWdtState, cpu, TYPE_MCS51_CPU,
                     MCS51CPU *),
};

static void stc8g_wdt_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc8g_wdt_realize;
    device_class_set_legacy_reset(dc, stc8g_wdt_reset);
    device_class_set_props(dc, stc8g_wdt_properties);
    dc->vmsd = &stc8g_wdt_vmstate;
    set_bit(DEVICE_CATEGORY_WATCHDOG, dc->categories);
    dc->desc = "STC8G watchdog timer";
}

static const TypeInfo stc8g_wdt_type = {
    .name = TYPE_STC8G_WDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc8gWdtState),
    .instance_init = stc8g_wdt_init,
    .instance_finalize = stc8g_wdt_finalize,
    .class_init = stc8g_wdt_class_init,
};

static void stc8g_wdt_register_types(void)
{
    type_register_static(&stc8g_wdt_type);
}

type_init(stc8g_wdt_register_types)
