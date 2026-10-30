/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"

#define TYPE_RK3566_PWM "rk3566-pwm"
#define PWM_NUM_CHANNELS 4
#define PWM_REG_SIZE 0x100

OBJECT_DECLARE_SIMPLE_TYPE(RK3566PWMState, RK3566_PWM)

struct RK3566PWMState {
    SysBusDevice parent_obj;
    uint32_t regs[PWM_REG_SIZE / 4];
    RegisterInfo regs_info[PWM_REG_SIZE / 4];
    RegisterInfoArray *reg_array;
    uint32_t frequency;
    int64_t epoch[PWM_NUM_CHANNELS];
};

static uint64_t pwm_ctrl_post_read(RegisterInfo *reg, uint64_t val)
{
    RK3566PWMState *s = RK3566_PWM(reg->opaque);
    unsigned channel = reg->access->addr / 16;
    unsigned period;
    uint64_t ticks;

    if (!(s->regs[channel * 4 + 3] & 1)) {
        return val;
    }
    period = s->regs[channel * 4 + 1];
    ticks = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->epoch[channel],
                     s->frequency, NANOSECONDS_PER_SECOND);
    return period ? ticks % period : 0;
}

static uint64_t pwm_enable_pre_write(RegisterInfo *reg, uint64_t val)
{
    RK3566PWMState *s = RK3566_PWM(reg->opaque);
    unsigned channel = reg->access->addr / 16;

    if (!(s->regs[channel * 4 + 3] & 1) && (val & 1)) {
        s->epoch[channel] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    return val;
}

static const RegisterAccessInfo pwm_regs_info[] = {
    { .name = "CH0_CTRL", .addr = 0x00, .post_read = pwm_ctrl_post_read },
    { .name = "CH0_PERIOD", .addr = 0x04 },
    { .name = "CH0_DUTY", .addr = 0x08 },
    { .name = "CH0_ENABLE", .addr = 0x0c, .pre_write = pwm_enable_pre_write },
    { .name = "CH1_CTRL", .addr = 0x10, .post_read = pwm_ctrl_post_read },
    { .name = "CH1_PERIOD", .addr = 0x14 },
    { .name = "CH1_DUTY", .addr = 0x18 },
    { .name = "CH1_ENABLE", .addr = 0x1c, .pre_write = pwm_enable_pre_write },
    { .name = "CH2_CTRL", .addr = 0x20, .post_read = pwm_ctrl_post_read },
    { .name = "CH2_PERIOD", .addr = 0x24 },
    { .name = "CH2_DUTY", .addr = 0x28 },
    { .name = "CH2_ENABLE", .addr = 0x2c, .pre_write = pwm_enable_pre_write },
    { .name = "CH3_CTRL", .addr = 0x30, .post_read = pwm_ctrl_post_read },
    { .name = "CH3_PERIOD", .addr = 0x34 },
    { .name = "CH3_DUTY", .addr = 0x38 },
    { .name = "CH3_ENABLE", .addr = 0x3c, .pre_write = pwm_enable_pre_write },
    { .name = "RSVD_40", .addr = 0x40 },
    { .name = "RSVD_44", .addr = 0x44 },
    { .name = "RSVD_48", .addr = 0x48 },
    { .name = "RSVD_4c", .addr = 0x4c },
    { .name = "RSVD_50", .addr = 0x50 },
    { .name = "RSVD_54", .addr = 0x54 },
    { .name = "RSVD_58", .addr = 0x58 },
    { .name = "RSVD_5c", .addr = 0x5c },
    { .name = "RSVD_60", .addr = 0x60 },
    { .name = "RSVD_64", .addr = 0x64 },
    { .name = "RSVD_68", .addr = 0x68 },
    { .name = "RSVD_6c", .addr = 0x6c },
    { .name = "RSVD_70", .addr = 0x70 },
    { .name = "RSVD_74", .addr = 0x74 },
    { .name = "RSVD_78", .addr = 0x78 },
    { .name = "RSVD_7c", .addr = 0x7c },
    { .name = "RSVD_80", .addr = 0x80 },
    { .name = "RSVD_84", .addr = 0x84 },
    { .name = "RSVD_88", .addr = 0x88 },
    { .name = "RSVD_8c", .addr = 0x8c },
    { .name = "RSVD_90", .addr = 0x90 },
    { .name = "RSVD_94", .addr = 0x94 },
    { .name = "RSVD_98", .addr = 0x98 },
    { .name = "RSVD_9c", .addr = 0x9c },
    { .name = "RSVD_a0", .addr = 0xa0 },
    { .name = "RSVD_a4", .addr = 0xa4 },
    { .name = "RSVD_a8", .addr = 0xa8 },
    { .name = "RSVD_ac", .addr = 0xac },
    { .name = "RSVD_b0", .addr = 0xb0 },
    { .name = "RSVD_b4", .addr = 0xb4 },
    { .name = "RSVD_b8", .addr = 0xb8 },
    { .name = "RSVD_bc", .addr = 0xbc },
    { .name = "RSVD_c0", .addr = 0xc0 },
    { .name = "RSVD_c4", .addr = 0xc4 },
    { .name = "RSVD_c8", .addr = 0xc8 },
    { .name = "RSVD_cc", .addr = 0xcc },
    { .name = "RSVD_d0", .addr = 0xd0 },
    { .name = "RSVD_d4", .addr = 0xd4 },
    { .name = "RSVD_d8", .addr = 0xd8 },
    { .name = "RSVD_dc", .addr = 0xdc },
    { .name = "RSVD_e0", .addr = 0xe0 },
    { .name = "RSVD_e4", .addr = 0xe4 },
    { .name = "RSVD_e8", .addr = 0xe8 },
    { .name = "RSVD_ec", .addr = 0xec },
    { .name = "RSVD_f0", .addr = 0xf0 },
    { .name = "RSVD_f4", .addr = 0xf4 },
    { .name = "RSVD_f8", .addr = 0xf8 },
    { .name = "RSVD_fc", .addr = 0xfc },
};

static const MemoryRegionOps pwm_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void pwm_init(Object *obj)
{
    RK3566PWMState *s = RK3566_PWM(obj);

    s->reg_array = register_init_block32(DEVICE(obj), pwm_regs_info,
                                         ARRAY_SIZE(pwm_regs_info),
                                         s->regs_info, s->regs,
                                         &pwm_ops, false, PWM_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->reg_array->mem);
}

static const Property pwm_properties[] = {
    DEFINE_PROP_UINT32("clock-hz", RK3566PWMState, frequency, 24000000),
};

static void pwm_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_props(DEVICE_CLASS(oc), pwm_properties);
}

static const TypeInfo pwm_info = {
    .name = TYPE_RK3566_PWM, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3566PWMState),
    .instance_init = pwm_init, .class_init = pwm_class_init,
};

static void pwm_register(void) { type_register_static(&pwm_info); }
type_init(pwm_register)
