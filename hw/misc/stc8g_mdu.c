/*
 * STC8G1K08A MDU16
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/mcs51/clock.h"
#include "hw/misc/stc8g_mdu.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "trace.h"

REG8(ARCON, 0)
    FIELD(ARCON, MODE, 5, 3)
    FIELD(ARCON, SC, 0, 5)
REG8(OPCON, 0)
    FIELD(OPCON, MDOV, 6, 1)
    FIELD(OPCON, RST, 1, 1)
    FIELD(OPCON, ENOP, 0, 1)

enum Stc8gMDURegister {
    STC8G_MDU_MD3,
    STC8G_MDU_MD2,
    STC8G_MDU_MD1,
    STC8G_MDU_MD0,
    STC8G_MDU_MD5,
    STC8G_MDU_MD4,
    STC8G_MDU_ARCON,
    STC8G_MDU_OPCON,
};

struct Stc8gMDUState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array[STC8G_MDU_MMIO_REGS];
    RegisterInfo regs_info[STC8G_MDU_MMIO_REGS];
    uint8_t regs[STC8G_MDU_MMIO_REGS];
    QEMUTimer *timer;
    Clock *sysclk;
    uint32_t clock_frequency;
    uint32_t operand;
    uint16_t divisor;
    uint8_t mode;
    uint8_t shift;
    uint64_t remaining_cycles;
    uint64_t clock_remainder;
    int64_t last_ns;
    bool active;
};

static uint32_t stc8g_mdu_get_operand(Stc8gMDUState *s)
{
    return ((uint32_t)s->regs[STC8G_MDU_MD3] << 24) |
           ((uint32_t)s->regs[STC8G_MDU_MD2] << 16) |
           ((uint32_t)s->regs[STC8G_MDU_MD1] << 8) |
           s->regs[STC8G_MDU_MD0];
}

static uint16_t stc8g_mdu_get_divisor(Stc8gMDUState *s)
{
    return (s->regs[STC8G_MDU_MD5] << 8) |
           s->regs[STC8G_MDU_MD4];
}

static void stc8g_mdu_set_operand(Stc8gMDUState *s, uint32_t value)
{
    s->regs[STC8G_MDU_MD3] = value >> 24;
    s->regs[STC8G_MDU_MD2] = value >> 16;
    s->regs[STC8G_MDU_MD1] = value >> 8;
    s->regs[STC8G_MDU_MD0] = value;
}

static void stc8g_mdu_set_divisor(Stc8gMDUState *s, uint16_t value)
{
    s->regs[STC8G_MDU_MD5] = value >> 8;
    s->regs[STC8G_MDU_MD4] = value;
}

static void stc8g_mdu_set_overflow(Stc8gMDUState *s, bool overflow)
{
    s->regs[STC8G_MDU_OPCON] = FIELD_DP8(
        s->regs[STC8G_MDU_OPCON], OPCON, MDOV, overflow);
}

static uint64_t stc8g_mdu_operation_cycles(Stc8gMDUState *s)
{
    switch (s->mode) {
    case 1:
    case 2:
        return MIN(18, 3 + s->shift);
    case 3:
        return MIN(20, 3 + s->shift);
    case 4:
        return 10;
    case 5:
        return 9;
    case 6:
        return 17;
    default:
        return 0;
    }
}

static void stc8g_mdu_sync(Stc8gMDUState *s);
static void stc8g_mdu_schedule(Stc8gMDUState *s);

static void stc8g_mdu_clock_update(void *opaque, ClockEvent event)
{
    Stc8gMDUState *s = opaque;

    if (event == ClockPreUpdate) {
        stc8g_mdu_sync(s);
        return;
    }
    s->clock_frequency = clock_get_hz(s->sysclk);
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    stc8g_mdu_schedule(s);
}

static void stc8g_mdu_complete(void *opaque)
{
    Stc8gMDUState *s = opaque;
    uint32_t result;
    uint16_t dividend;
    uint16_t remainder;
    uint8_t count;

    s->active = false;
    s->regs[STC8G_MDU_OPCON] = FIELD_DP8(
        s->regs[STC8G_MDU_OPCON], OPCON, ENOP, 0);
    switch (s->mode) {
    case 1:
        stc8g_mdu_set_operand(s, s->operand >> s->shift);
        break;
    case 2:
        stc8g_mdu_set_operand(s, s->operand << s->shift);
        break;
    case 3:
        count = s->operand ? MIN(31, clz32(s->operand)) : 0;
        stc8g_mdu_set_operand(s, s->operand << count);
        s->regs[STC8G_MDU_ARCON] = FIELD_DP8(
            s->regs[STC8G_MDU_ARCON], ARCON, SC, count);
        break;
    case 4:
        result = (uint16_t)s->operand * s->divisor;
        stc8g_mdu_set_operand(s, result);
        stc8g_mdu_set_overflow(s, result > UINT16_MAX);
        break;
    case 5:
        if (!s->divisor) {
            stc8g_mdu_set_overflow(s, true);
            break;
        }
        dividend = s->operand;
        remainder = dividend % s->divisor;
        s->regs[STC8G_MDU_MD1] = (dividend / s->divisor) >> 8;
        s->regs[STC8G_MDU_MD0] = dividend / s->divisor;
        stc8g_mdu_set_divisor(s, remainder);
        break;
    case 6:
        if (!s->divisor) {
            stc8g_mdu_set_overflow(s, true);
            break;
        }
        result = s->operand / s->divisor;
        remainder = s->operand % s->divisor;
        stc8g_mdu_set_operand(s, result);
        stc8g_mdu_set_divisor(s, remainder);
        break;
    default:
        break;
    }
    trace_stc8g_mdu_complete(s->mode, stc8g_mdu_get_operand(s),
                             stc8g_mdu_get_divisor(s));
}

static void stc8g_mdu_sync(Stc8gMDUState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->active && clock_is_enabled(s->sysclk)) {
        uint64_t cycles = mcs51_clock_elapsed_cycles(
            s->sysclk, now - s->last_ns, &s->clock_remainder);
        if (cycles >= s->remaining_cycles) {
            s->remaining_cycles = 0;
            s->last_ns = now;
            timer_del(s->timer);
            stc8g_mdu_complete(s);
            return;
        }
        s->remaining_cycles -= cycles;
    }
    s->last_ns = now;
}

static void stc8g_mdu_schedule(Stc8gMDUState *s)
{
    uint64_t delta;

    timer_del(s->timer);
    if (!s->active || !clock_is_enabled(s->sysclk)) {
        return;
    }
    delta = mcs51_clock_cycles_to_ns(s->sysclk, s->remaining_cycles,
                                     s->clock_remainder);
    timer_mod_ns(s->timer, s->last_ns + MAX(1ull, delta));
}

static void stc8g_mdu_expire(void *opaque)
{
    Stc8gMDUState *s = opaque;

    stc8g_mdu_sync(s);
    stc8g_mdu_schedule(s);
}

static void stc8g_mdu_start(Stc8gMDUState *s)
{
    s->operand = stc8g_mdu_get_operand(s);
    s->divisor = stc8g_mdu_get_divisor(s);
    s->mode = FIELD_EX8(s->regs[STC8G_MDU_ARCON], ARCON, MODE);
    s->shift = FIELD_EX8(s->regs[STC8G_MDU_ARCON], ARCON, SC);
    if (s->mode == 3) {
        s->shift = s->operand ? MIN(31, clz32(s->operand)) : 0;
    }
    stc8g_mdu_set_overflow(s, false);
    s->remaining_cycles = stc8g_mdu_operation_cycles(s);
    if (!s->remaining_cycles) {
        s->regs[STC8G_MDU_OPCON] = FIELD_DP8(
            s->regs[STC8G_MDU_OPCON], OPCON, ENOP, 0);
        return;
    }
    s->clock_remainder = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->active = true;
    stc8g_mdu_schedule(s);
    trace_stc8g_mdu_start(s->mode, s->operand, s->divisor,
                          mcs51_clock_cycles_to_ns(s->sysclk,
                                                   s->remaining_cycles, 0));
}

static void stc8g_mdu_arcon_post_write(RegisterInfo *reg, uint64_t value)
{
    stc8g_mdu_set_overflow(STC8G_MDU(reg->opaque), false);
}

static void stc8g_mdu_opcon_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gMDUState *s = STC8G_MDU(reg->opaque);

    if (FIELD_EX8(value, OPCON, RST)) {
        timer_del(s->timer);
        s->active = false;
        s->remaining_cycles = 0;
        s->clock_remainder = 0;
        s->regs[STC8G_MDU_ARCON] = 0;
        s->regs[STC8G_MDU_OPCON] = 0;
        trace_stc8g_mdu_reset();
    } else if (FIELD_EX8(value, OPCON, ENOP)) {
        stc8g_mdu_start(s);
    }
}

static const RegisterAccessInfo stc8g_mdu_regs_info[] = {
    { .name = "MD3", .addr = 0 },
    { .name = "MD2", .addr = 0 },
    { .name = "MD1", .addr = 0 },
    { .name = "MD0", .addr = 0 },
    { .name = "MD5", .addr = 0 },
    { .name = "MD4", .addr = 0 },
    { .name = "ARCON", .addr = 0,
      .post_write = stc8g_mdu_arcon_post_write },
    { .name = "OPCON", .addr = 0, .ro = 0x40, .rsvd = 0xbc,
      .post_write = stc8g_mdu_opcon_post_write },
};

static const MemoryRegionOps stc8g_mdu_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc8g_mdu_reset(DeviceState *dev)
{
    Stc8gMDUState *s = STC8G_MDU(dev);
    unsigned index;

    timer_del(s->timer);
    s->active = false;
    s->operand = 0;
    s->divisor = 0;
    s->mode = 0;
    s->shift = 0;
    s->remaining_cycles = 0;
    s->clock_remainder = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (index = 0; index < ARRAY_SIZE(stc8g_mdu_regs_info); index++) {
        register_reset(&s->regs_info[index]);
    }
}

static const VMStateDescription stc8g_mdu_vmstate = {
    .name = "stc8g.mdu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Stc8gMDUState, STC8G_MDU_MMIO_REGS),
        VMSTATE_UINT32(operand, Stc8gMDUState),
        VMSTATE_UINT16(divisor, Stc8gMDUState),
        VMSTATE_UINT8(mode, Stc8gMDUState),
        VMSTATE_UINT8(shift, Stc8gMDUState),
        VMSTATE_UINT64(remaining_cycles, Stc8gMDUState),
        VMSTATE_UINT64(clock_remainder, Stc8gMDUState),
        VMSTATE_INT64(last_ns, Stc8gMDUState),
        VMSTATE_BOOL(active, Stc8gMDUState),
        VMSTATE_TIMER_PTR(timer, Stc8gMDUState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc8g_mdu_properties[] = {
    DEFINE_PROP_UINT32("clock-frequency", Stc8gMDUState,
                       clock_frequency, 24000000),
};

static void stc8g_mdu_realize(DeviceState *dev, Error **errp)
{
    Stc8gMDUState *s = STC8G_MDU(dev);

    if (!s->clock_frequency) {
        error_setg(errp, "stc8g-mdu clock-frequency must be nonzero");
    }
}

static void stc8g_mdu_init(Object *obj)
{
    Stc8gMDUState *s = STC8G_MDU(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned index;

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc8g_mdu_regs_info) !=
                      STC8G_MDU_MMIO_REGS);
    for (index = 0; index < ARRAY_SIZE(stc8g_mdu_regs_info); index++) {
        s->reg_array[index] = register_init_block8(
            DEVICE(obj), &stc8g_mdu_regs_info[index], 1,
            &s->regs_info[index], &s->regs[index], &stc8g_mdu_ops,
            false, 1);
        sysbus_init_mmio(sbd, &s->reg_array[index]->mem);
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stc8g_mdu_expire, s);
    s->sysclk = qdev_init_clock_in(DEVICE(obj), "sysclk",
                                   stc8g_mdu_clock_update, s,
                                   ClockPreUpdate | ClockUpdate);
}

static void stc8g_mdu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc8g_mdu_realize;
    device_class_set_legacy_reset(dc, stc8g_mdu_reset);
    device_class_set_props(dc, stc8g_mdu_properties);
    dc->vmsd = &stc8g_mdu_vmstate;
}

static const TypeInfo stc8g_mdu_type = {
    .name = TYPE_STC8G_MDU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc8gMDUState),
    .instance_init = stc8g_mdu_init,
    .class_init = stc8g_mdu_class_init,
};

static void stc8g_mdu_register_types(void)
{
    type_register_static(&stc8g_mdu_type);
}

type_init(stc8g_mdu_register_types)
