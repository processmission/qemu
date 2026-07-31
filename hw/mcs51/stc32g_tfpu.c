/*
 * STC32G trigonometric and floating-point unit
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qapi/error.h"
#include "fpu/softfloat.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/mcs51/stc32g_tfpu.h"
#include "migration/vmstate.h"
#include "target/mcs51/cpu.h"

#define STC32G_SFR_DMAIR 0xed

REG8(DMAIR, 0)
REG8(TFPU_CLKDIV, 0)

FIELD(TFPU_STATUS, INVALID, 0, 1)
FIELD(TFPU_STATUS, DIVBYZERO, 1, 1)
FIELD(TFPU_STATUS, OVERFLOW, 2, 1)
FIELD(TFPU_STATUS, UNDERFLOW, 3, 1)
FIELD(TFPU_STATUS, INEXACT, 4, 1)
FIELD(TFPU_CONTROL, ROUNDING, 0, 2)

enum Stc32gTFPUCommand {
    TFPU_ADD = 0x1c,
    TFPU_SUB = 0x1d,
    TFPU_MUL = 0x1e,
    TFPU_DIV = 0x1f,
    TFPU_SQRT = 0x20,
    TFPU_COMPARE = 0x21,
    TFPU_CLASSIFY = 0x22,
    TFPU_FLOAT_TO_INT8 = 0x23,
    TFPU_FLOAT_TO_INT16 = 0x24,
    TFPU_FLOAT_TO_INT32 = 0x25,
    TFPU_INT8_TO_FLOAT = 0x27,
    TFPU_INT16_TO_FLOAT = 0x28,
    TFPU_INT32_TO_FLOAT = 0x29,
    TFPU_SIN = 0x2d,
    TFPU_COS = 0x2e,
    TFPU_TAN = 0x2f,
    TFPU_ATAN = 0x30,
    TFPU_INITIALIZE = 0x31,
    TFPU_CLEAR_EXCEPTIONS = 0x32,
    TFPU_READ_STATUS = 0x33,
    TFPU_WRITE_STATUS = 0x34,
    TFPU_READ_CONTROL = 0x35,
    TFPU_WRITE_CONTROL = 0x36,
    TFPU_SELECT_SYSTEM_CLOCK = 0x3e,
    TFPU_SELECT_PLL_CLOCK = 0x3f,
};

enum Stc32gTFPUClass {
    TFPU_CLASS_POSITIVE_NAN = 0x0,
    TFPU_CLASS_NEGATIVE_NAN = 0x3,
    TFPU_CLASS_POSITIVE_NORMAL = 0x4,
    TFPU_CLASS_POSITIVE_INFINITY = 0x5,
    TFPU_CLASS_NEGATIVE_NORMAL = 0x6,
    TFPU_CLASS_NEGATIVE_INFINITY = 0x7,
    TFPU_CLASS_POSITIVE_ZERO = 0x8,
    TFPU_CLASS_NEGATIVE_ZERO = 0xa,
    TFPU_CLASS_POSITIVE_DENORMAL = 0xc,
    TFPU_CLASS_NEGATIVE_DENORMAL = 0xe,
};

struct Stc32gTFPUState {
    SysBusDevice parent_obj;

    MCS251CPU *cpu;
    RegisterInfoArray *command_reg_array;
    RegisterInfoArray *clock_reg_array;
    RegisterInfo command_reg_info[1];
    RegisterInfo clock_reg_info[1];
    uint8_t command_regs[1];
    uint8_t clock_regs[1];
    uint8_t status;
    uint8_t control;
    bool pll_clock;
};

static float_status stc32g_tfpu_float_status(Stc32gTFPUState *s)
{
    static const FloatRoundMode rounding_modes[] = {
        float_round_nearest_even,
        float_round_to_zero,
        float_round_down,
        float_round_up,
    };
    float_status status = {};
    unsigned rounding = FIELD_EX8(s->control, TFPU_CONTROL, ROUNDING);

    set_float_rounding_mode(rounding_modes[rounding], &status);
    set_float_default_nan_pattern(0b01000000, &status);
    set_default_nan_mode(true, &status);
    set_snan_rule(float_snan_bit_is_zero, &status);
    return status;
}

static void stc32g_tfpu_record_exceptions(Stc32gTFPUState *s,
                                          float_status *status)
{
    FloatExceptionFlags flags = get_float_exception_flags(status);

    if (flags & (float_flag_invalid | float_flag_invalid_isi |
                 float_flag_invalid_imz | float_flag_invalid_idi |
                 float_flag_invalid_zdz | float_flag_invalid_sqrt |
                 float_flag_invalid_cvti | float_flag_invalid_snan)) {
        s->status |= R_TFPU_STATUS_INVALID_MASK;
    }
    if (flags & float_flag_divbyzero) {
        s->status |= R_TFPU_STATUS_DIVBYZERO_MASK;
    }
    if (flags & float_flag_overflow) {
        s->status |= R_TFPU_STATUS_OVERFLOW_MASK;
    }
    if (flags & float_flag_underflow) {
        s->status |= R_TFPU_STATUS_UNDERFLOW_MASK;
    }
    if (flags & float_flag_inexact) {
        s->status |= R_TFPU_STATUS_INEXACT_MASK;
    }
}

static float32 stc32g_tfpu_get_ar(Stc32gTFPUState *s)
{
    return make_float32(mcs251_cpu_get_reg(&s->cpu->env, 4, 4));
}

static float32 stc32g_tfpu_get_br(Stc32gTFPUState *s)
{
    return make_float32(mcs251_cpu_get_reg(&s->cpu->env, 0, 4));
}

static void stc32g_tfpu_set_ar(Stc32gTFPUState *s, float32 value)
{
    mcs251_cpu_set_reg(&s->cpu->env, 4, 4, float32_val(value));
}

static void stc32g_tfpu_set_r7(Stc32gTFPUState *s, uint8_t value)
{
    mcs251_cpu_set_reg8(&s->cpu->env, 7, value);
}

static float stc32g_tfpu_float32_to_host(float32 value)
{
    uint32_t bits = float32_val(value);
    float result;

    QEMU_BUILD_BUG_ON(sizeof(result) != sizeof(bits));
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static float32 stc32g_tfpu_float32_from_host(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return make_float32(bits);
}

static uint8_t stc32g_tfpu_classify(float32 value)
{
    bool negative = float32_is_neg(value);

    if (float32_is_any_nan(value)) {
        return negative ? TFPU_CLASS_NEGATIVE_NAN :
                          TFPU_CLASS_POSITIVE_NAN;
    }
    if (float32_is_infinity(value)) {
        return negative ? TFPU_CLASS_NEGATIVE_INFINITY :
                          TFPU_CLASS_POSITIVE_INFINITY;
    }
    if (float32_is_zero(value)) {
        return negative ? TFPU_CLASS_NEGATIVE_ZERO :
                          TFPU_CLASS_POSITIVE_ZERO;
    }
    if (float32_is_denormal(value)) {
        return negative ? TFPU_CLASS_NEGATIVE_DENORMAL :
                          TFPU_CLASS_POSITIVE_DENORMAL;
    }
    return negative ? TFPU_CLASS_NEGATIVE_NORMAL :
                      TFPU_CLASS_POSITIVE_NORMAL;
}

static int8_t stc32g_tfpu_float_to_int8(float32 value,
                                         float_status *status)
{
    int32_t result = float32_to_int32(value, status);

    if (result < INT8_MIN || result > INT8_MAX) {
        FloatExceptionFlags flags = get_float_exception_flags(status);

        flags &= ~float_flag_inexact;
        flags |= float_flag_invalid | float_flag_invalid_cvti;
        set_float_exception_flags(flags, status);
        result = result < 0 ? INT8_MIN : INT8_MAX;
    }
    return result;
}

static float32 stc32g_tfpu_trigonometric(Stc32gTFPUState *s,
                                         uint8_t command, float32 value)
{
    float input = stc32g_tfpu_float32_to_host(value);
    float result;

    switch (command) {
    case TFPU_SIN:
        result = sinf(input);
        break;
    case TFPU_COS:
        result = cosf(input);
        break;
    case TFPU_TAN:
        result = tanf(input);
        break;
    case TFPU_ATAN:
        result = atanf(input);
        break;
    default:
        g_assert_not_reached();
    }

    if (isinf(input) && command != TFPU_ATAN) {
        s->status |= R_TFPU_STATUS_INVALID_MASK;
    } else if (isfinite(input) && isinf(result)) {
        s->status |= R_TFPU_STATUS_OVERFLOW_MASK;
    }
    return stc32g_tfpu_float32_from_host(result);
}

static void stc32g_tfpu_execute(Stc32gTFPUState *s, uint8_t command)
{
    CPUMCS251State *env = &s->cpu->env;
    float_status status = stc32g_tfpu_float_status(s);
    float32 ar = stc32g_tfpu_get_ar(s);
    float32 br = stc32g_tfpu_get_br(s);
    float32 result;

    switch (command) {
    case TFPU_ADD:
        result = float32_add(ar, br, &status);
        stc32g_tfpu_set_ar(s, result);
        stc32g_tfpu_record_exceptions(s, &status);
        break;
    case TFPU_SUB:
        result = float32_sub(ar, br, &status);
        stc32g_tfpu_set_ar(s, result);
        stc32g_tfpu_record_exceptions(s, &status);
        break;
    case TFPU_MUL:
        result = float32_mul(ar, br, &status);
        stc32g_tfpu_set_ar(s, result);
        stc32g_tfpu_record_exceptions(s, &status);
        break;
    case TFPU_DIV:
        result = float32_div(ar, br, &status);
        stc32g_tfpu_set_ar(s, result);
        stc32g_tfpu_record_exceptions(s, &status);
        break;
    case TFPU_SQRT:
        result = float32_sqrt(ar, &status);
        stc32g_tfpu_set_ar(s, result);
        stc32g_tfpu_record_exceptions(s, &status);
        break;
    case TFPU_COMPARE:
        switch (float32_compare(ar, br, &status)) {
        case float_relation_greater:
            stc32g_tfpu_set_r7(s, 0x00);
            break;
        case float_relation_equal:
            stc32g_tfpu_set_r7(s, 0x08);
            break;
        case float_relation_less:
            stc32g_tfpu_set_r7(s, 0x01);
            break;
        case float_relation_unordered:
            break;
        default:
            g_assert_not_reached();
        }
        stc32g_tfpu_record_exceptions(s, &status);
        break;
    case TFPU_CLASSIFY:
        stc32g_tfpu_set_r7(s, stc32g_tfpu_classify(ar));
        break;
    case TFPU_FLOAT_TO_INT8:
        stc32g_tfpu_set_r7(s, stc32g_tfpu_float_to_int8(ar, &status));
        stc32g_tfpu_record_exceptions(s, &status);
        break;
    case TFPU_FLOAT_TO_INT16:
        mcs251_cpu_set_reg(env, 6, 2, float32_to_int16(ar, &status));
        stc32g_tfpu_record_exceptions(s, &status);
        break;
    case TFPU_FLOAT_TO_INT32:
        mcs251_cpu_set_reg(env, 4, 4, float32_to_int32(ar, &status));
        stc32g_tfpu_record_exceptions(s, &status);
        break;
    case TFPU_INT8_TO_FLOAT:
        result = int32_to_float32((int8_t)mcs251_cpu_get_reg8(env, 7),
                                  &status);
        stc32g_tfpu_set_ar(s, result);
        stc32g_tfpu_record_exceptions(s, &status);
        break;
    case TFPU_INT16_TO_FLOAT:
        result = int16_to_float32(
            (int16_t)mcs251_cpu_get_reg(env, 6, 2), &status);
        stc32g_tfpu_set_ar(s, result);
        stc32g_tfpu_record_exceptions(s, &status);
        break;
    case TFPU_INT32_TO_FLOAT:
        result = int32_to_float32(
            (int32_t)mcs251_cpu_get_reg(env, 4, 4), &status);
        stc32g_tfpu_set_ar(s, result);
        stc32g_tfpu_record_exceptions(s, &status);
        break;
    case TFPU_SIN:
    case TFPU_COS:
    case TFPU_TAN:
    case TFPU_ATAN:
        stc32g_tfpu_set_ar(
            s, stc32g_tfpu_trigonometric(s, command, ar));
        break;
    case TFPU_INITIALIZE:
        s->status = R_TFPU_STATUS_INVALID_MASK;
        s->control = 0;
        break;
    case TFPU_CLEAR_EXCEPTIONS:
        s->status = 0;
        break;
    case TFPU_READ_STATUS:
        stc32g_tfpu_set_r7(s, s->status);
        break;
    case TFPU_WRITE_STATUS:
        s->status = mcs251_cpu_get_reg8(env, 7);
        stc32g_tfpu_set_r7(s, s->status);
        break;
    case TFPU_READ_CONTROL:
        stc32g_tfpu_set_r7(s, s->control);
        break;
    case TFPU_WRITE_CONTROL:
        s->control = mcs251_cpu_get_reg8(env, 7);
        stc32g_tfpu_set_r7(s, s->control);
        break;
    case TFPU_SELECT_SYSTEM_CLOCK:
        s->pll_clock = false;
        break;
    case TFPU_SELECT_PLL_CLOCK:
        s->pll_clock = true;
        break;
    default:
        break;
    }
}

static const RegisterAccessInfo stc32g_tfpu_command_regs_info[] = {
    { .name = "DMAIR", .addr = A_DMAIR },
};

static const RegisterAccessInfo stc32g_tfpu_clock_regs_info[] = {
    { .name = "TFPU_CLKDIV", .addr = A_TFPU_CLKDIV },
};

static const MemoryRegionOps stc32g_tfpu_regs_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc32g_tfpu_immediate_write(void *opaque, uint8_t addr,
                                        uint8_t value)
{
    Stc32gTFPUState *s = opaque;

    if (addr == STC32G_SFR_DMAIR) {
        stc32g_tfpu_execute(s, value);
    }
}

static void stc32g_tfpu_reset(DeviceState *dev)
{
    Stc32gTFPUState *s = STC32G_TFPU(dev);

    register_reset(&s->command_reg_info[R_DMAIR]);
    register_reset(&s->clock_reg_info[R_TFPU_CLKDIV]);
    s->status = 0;
    s->control = 0;
    s->pll_clock = false;
}

static const VMStateDescription stc32g_tfpu_vmstate = {
    .name = "stc32g.tfpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(command_regs, Stc32gTFPUState, 1),
        VMSTATE_UINT8_ARRAY(clock_regs, Stc32gTFPUState, 1),
        VMSTATE_UINT8(status, Stc32gTFPUState),
        VMSTATE_UINT8(control, Stc32gTFPUState),
        VMSTATE_BOOL(pll_clock, Stc32gTFPUState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc32g_tfpu_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc32gTFPUState, cpu, TYPE_MCS251_CPU,
                     MCS251CPU *),
};

static void stc32g_tfpu_realize(DeviceState *dev, Error **errp)
{
    Stc32gTFPUState *s = STC32G_TFPU(dev);

    if (!s->cpu) {
        error_setg(errp, "stc32g-tfpu requires a CPU link");
        return;
    }
    mcs251_cpu_set_sfr_immediate_write(s->cpu,
                                       stc32g_tfpu_immediate_write, s);
}

static void stc32g_tfpu_init(Object *obj)
{
    Stc32gTFPUState *s = STC32G_TFPU(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->command_reg_array = register_init_block8(
        DEVICE(obj), stc32g_tfpu_command_regs_info,
        ARRAY_SIZE(stc32g_tfpu_command_regs_info), s->command_reg_info,
        s->command_regs, &stc32g_tfpu_regs_ops, false, 1);
    s->clock_reg_array = register_init_block8(
        DEVICE(obj), stc32g_tfpu_clock_regs_info,
        ARRAY_SIZE(stc32g_tfpu_clock_regs_info), s->clock_reg_info,
        s->clock_regs, &stc32g_tfpu_regs_ops, false, 1);
    sysbus_init_mmio(sbd, &s->command_reg_array->mem);
    sysbus_init_mmio(sbd, &s->clock_reg_array->mem);
}

static void stc32g_tfpu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc32g_tfpu_realize;
    device_class_set_legacy_reset(dc, stc32g_tfpu_reset);
    device_class_set_props(dc, stc32g_tfpu_properties);
    dc->vmsd = &stc32g_tfpu_vmstate;
}

static const TypeInfo stc32g_tfpu_type = {
    .name = TYPE_STC32G_TFPU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc32gTFPUState),
    .instance_init = stc32g_tfpu_init,
    .class_init = stc32g_tfpu_class_init,
};

static void stc32g_tfpu_register_types(void)
{
    type_register_static(&stc32g_tfpu_type);
}

type_init(stc32g_tfpu_register_types)
