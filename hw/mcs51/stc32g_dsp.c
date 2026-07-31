/*
 * STC32G DSP32 command accelerator
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/host-utils.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/mcs51/stc32g_dsp.h"
#include "migration/vmstate.h"
#include "target/mcs51/cpu.h"

REG8(DPUST, 0)
    FIELD(DPUST, SC, 0, 6)
    FIELD(DPUST, DBZ, 6, 1)
    FIELD(DPUST, Z, 7, 1)
REG8(DPUOP, 0)
FIELD(DPUCFG, CDRS, 0, 3)

enum Stc32gDSPCommand {
    DSP_FILL_DPUCFG = 0x80,
    DSP_BIN2BCD = 0x81,
    DSP_BCD2BIN = 0x82,
    DSP_NRM_BX = 0x83,
    DSP_NRM_EBX = 0x84,
    DSP_NRM_EABX = 0x85,
    DSP_SWAP_EAB = 0x86,
    DSP_SWAP_ECD = 0x87,
    DSP_SWAP_EAC = 0x88,
    DSP_SWAP_EBD = 0x89,
    DSP_ADDC_EABX = 0x90,
    DSP_ADDC_ABX = 0x91,
    DSP_ADD_EABX = 0x92,
    DSP_ADD_ABX = 0x93,
    DSP_SUBB_EABX = 0x94,
    DSP_SUBB_ABX = 0x95,
    DSP_SUB_EABX = 0x96,
    DSP_SUB_ABX = 0x97,
    DSP_CMP_EABX = 0x98,
    DSP_CMP_ABX = 0x99,
    DSP_MULU_EABX = 0x9a,
    DSP_MULS_EABX = 0x9b,
    DSP_MULU_ABX = 0x9c,
    DSP_MULS_ABX = 0x9d,
    DSP_MULX_KEIL16 = 0x9e,
    DSP_DIVU_EABX = 0x9f,
    DSP_DIVS_EABX = 0xa0,
    DSP_DIVU_ABX = 0xa1,
    DSP_DIVU_KEIL16 = 0xa2,
    DSP_DIVS_ABX = 0xa3,
    DSP_DIVS_KEIL16 = 0xa4,
    DSP_DIVU_EABXD = 0xa5,
    DSP_DIVU_EAXB = 0xa6,
    DSP_SET0_EAX = 0xb0,
    DSP_SET1_EAX = 0xb1,
    DSP_SET0_EBX = 0xb2,
    DSP_SET1_EBX = 0xb3,
    DSP_SET0_ECX = 0xb4,
    DSP_SET1_ECX = 0xb5,
    DSP_SET0_EDX = 0xb6,
    DSP_SET1_EDX = 0xb7,
    DSP_NEGS_EAX = 0xb8,
    DSP_NEGS_EBX = 0xb9,
    DSP_NEGS_AX = 0xba,
    DSP_NEGS_BX = 0xbb,
    DSP_INC1_EAX = 0xc0,
    DSP_INC1_EBX = 0xc1,
    DSP_INC4_EAX = 0xc2,
    DSP_INC4_EBX = 0xc3,
    DSP_INC1_AX = 0xc4,
    DSP_INC1_BX = 0xc5,
    DSP_INC2_AX = 0xc6,
    DSP_INC2_BX = 0xc7,
    DSP_DEC1_EAX = 0xc8,
    DSP_DEC1_EBX = 0xc9,
    DSP_DEC4_EAX = 0xca,
    DSP_DEC4_EBX = 0xcb,
    DSP_DEC1_AX = 0xcc,
    DSP_DEC1_BX = 0xcd,
    DSP_DEC2_AX = 0xce,
    DSP_DEC2_BX = 0xcf,
    DSP_BAND_EABX = 0xd0,
    DSP_BAND_ABX = 0xd1,
    DSP_BOR_EABX = 0xd2,
    DSP_BOR_ABX = 0xd3,
    DSP_BXOR_EABX = 0xd4,
    DSP_BXOR_ABX = 0xd5,
    DSP_BCPL_EAX = 0xd6,
    DSP_BCPL_EBX = 0xd7,
    DSP_SHL_EAX = 0xe0,
    DSP_SHL_EBX = 0xe1,
    DSP_SHL_AX = 0xe2,
    DSP_SHL_BX = 0xe3,
    DSP_SHRU_EAX = 0xe4,
    DSP_SHRU_EBX = 0xe5,
    DSP_SHRU_AX = 0xe6,
    DSP_SHRU_BX = 0xe7,
    DSP_SHRS_EAX = 0xe8,
    DSP_SHRS_EBX = 0xe9,
    DSP_SHRS_AX = 0xea,
    DSP_SHRS_BX = 0xeb,
    DSP_ROR_EAX = 0xec,
    DSP_ROR_EBX = 0xed,
    DSP_ROR_AX = 0xee,
    DSP_ROR_BX = 0xef,
    DSP_MMD32_EABX = 0xf0,
    DSP_MMD16_ABX = 0xf1,
    DSP_LTC32_EAX = 0xf2,
    DSP_LTC16_AX = 0xf3,
    DSP_MA32_EDX = 0xf4,
    DSP_MA64_ECDX = 0xf5,
};

struct Stc32gDSPState {
    SysBusDevice parent_obj;

    MCS251CPU *cpu;
    RegisterInfoArray *status_reg_array;
    RegisterInfoArray *command_reg_array;
    RegisterInfo status_reg_info[1];
    RegisterInfo command_reg_info[1];
    uint8_t status_regs[1];
    uint8_t command_regs[1];
    uint8_t dpucfg;
};

static uint32_t stc32g_dsp_get_data32(Stc32gDSPState *s, unsigned base)
{
    CPUMCS251State *env = &s->cpu->env;
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < 4; i++) {
        value = deposit32(value, (3 - i) * 8, 8,
                          mcs251_cpu_direct_read(env, base + i));
    }
    return value;
}

static void stc32g_dsp_set_data32(Stc32gDSPState *s, unsigned base,
                                  uint32_t value)
{
    CPUMCS251State *env = &s->cpu->env;
    unsigned i;

    for (i = 0; i < 4; i++) {
        mcs251_cpu_direct_write(env, base + i,
                               extract32(value, (3 - i) * 8, 8));
    }
}

static uint32_t stc32g_dsp_get_eax(Stc32gDSPState *s)
{
    return mcs251_cpu_get_reg(&s->cpu->env, 4, 4);
}

static void stc32g_dsp_set_eax(Stc32gDSPState *s, uint32_t value)
{
    mcs251_cpu_set_reg(&s->cpu->env, 4, 4, value);
}

static uint32_t stc32g_dsp_get_ebx(Stc32gDSPState *s)
{
    return mcs251_cpu_get_reg(&s->cpu->env, 0, 4);
}

static void stc32g_dsp_set_ebx(Stc32gDSPState *s, uint32_t value)
{
    mcs251_cpu_set_reg(&s->cpu->env, 0, 4, value);
}

static uint16_t stc32g_dsp_get_ax(Stc32gDSPState *s)
{
    return stc32g_dsp_get_eax(s);
}

static void stc32g_dsp_set_ax(Stc32gDSPState *s, uint16_t value)
{
    stc32g_dsp_set_eax(s, deposit32(stc32g_dsp_get_eax(s),
                                    0, 16, value));
}

static uint16_t stc32g_dsp_get_ax2(Stc32gDSPState *s)
{
    return extract32(stc32g_dsp_get_eax(s), 16, 16);
}

static void stc32g_dsp_set_ax2(Stc32gDSPState *s, uint16_t value)
{
    stc32g_dsp_set_eax(s, deposit32(stc32g_dsp_get_eax(s),
                                    16, 16, value));
}

static void stc32g_dsp_set_bx(Stc32gDSPState *s, uint16_t value)
{
    stc32g_dsp_set_ebx(s, deposit32(stc32g_dsp_get_ebx(s),
                                    0, 16, value));
}

static bool stc32g_dsp_get_cd(Stc32gDSPState *s, bool ecx,
                              uint32_t *value)
{
    unsigned cdrs = FIELD_EX8(s->dpucfg, DPUCFG, CDRS);
    unsigned base;

    if (cdrs < 4) {
        base = cdrs * 8 + (ecx ? 4 : 0);
        *value = stc32g_dsp_get_data32(s, base);
        return true;
    }
    if (cdrs == 5 || cdrs == 6) {
        base = cdrs == 5 ? 16 : 24;
        *value = mcs251_cpu_get_reg(&s->cpu->env,
                                    base + (ecx ? 4 : 0), 4);
        return true;
    }
    return false;
}

static bool stc32g_dsp_set_cd(Stc32gDSPState *s, bool ecx,
                              uint32_t value)
{
    unsigned cdrs = FIELD_EX8(s->dpucfg, DPUCFG, CDRS);
    unsigned base;

    if (cdrs < 4) {
        base = cdrs * 8 + (ecx ? 4 : 0);
        stc32g_dsp_set_data32(s, base, value);
        return true;
    }
    if (cdrs == 5 || cdrs == 6) {
        base = cdrs == 5 ? 16 : 24;
        mcs251_cpu_set_reg(&s->cpu->env, base + (ecx ? 4 : 0),
                           4, value);
        return true;
    }
    return false;
}

static void stc32g_dsp_set_z(Stc32gDSPState *s, uint64_t value)
{
    s->status_regs[R_DPUST] =
        FIELD_DP8(s->status_regs[R_DPUST], DPUST, Z, value == 0);
}

static void stc32g_dsp_set_dbz(Stc32gDSPState *s, bool value)
{
    s->status_regs[R_DPUST] =
        FIELD_DP8(s->status_regs[R_DPUST], DPUST, DBZ, value);
}

static void stc32g_dsp_set_sc(Stc32gDSPState *s, unsigned value)
{
    s->status_regs[R_DPUST] =
        FIELD_DP8(s->status_regs[R_DPUST], DPUST, SC, value);
}

static uint32_t stc32g_dsp_add(Stc32gDSPState *s, uint32_t lhs,
                               uint32_t rhs, unsigned carry,
                               unsigned width, bool update_z)
{
    CPUMCS251State *env = &s->cpu->env;
    uint64_t mask = MAKE_64BIT_MASK(0, width);
    uint64_t wide = (lhs & mask) + (rhs & mask) + carry;
    uint32_t result = wide & mask;
    uint32_t sign = BIT(width - 1);

    env->flag_c = wide > mask;
    env->flag_ov = (~(lhs ^ rhs) & (lhs ^ result) & sign) != 0;
    if (update_z) {
        stc32g_dsp_set_z(s, result);
    }
    return result;
}

static uint32_t stc32g_dsp_sub(Stc32gDSPState *s, uint32_t lhs,
                               uint32_t rhs, unsigned borrow,
                               unsigned width, bool update_z)
{
    CPUMCS251State *env = &s->cpu->env;
    uint64_t mask = MAKE_64BIT_MASK(0, width);
    uint64_t effective_rhs = (rhs & mask) + borrow;
    uint32_t result = (lhs - effective_rhs) & mask;
    uint32_t sign = BIT(width - 1);
    uint32_t masked_rhs = rhs & mask;

    env->flag_c = (lhs & mask) < effective_rhs;
    env->flag_ov =
        ((lhs ^ masked_rhs) & (lhs ^ result) & sign) != 0;
    if (update_z) {
        stc32g_dsp_set_z(s, result);
    }
    return result;
}

static uint32_t stc32g_dsp_accumulate(Stc32gDSPState *s, uint32_t value,
                                      int delta, unsigned width)
{
    CPUMCS251State *env = &s->cpu->env;
    uint64_t mask = MAKE_64BIT_MASK(0, width);
    uint32_t result;

    if (delta >= 0) {
        uint64_t wide = (value & mask) + delta;

        result = wide & mask;
        env->flag_c = wide > mask;
    } else {
        uint64_t magnitude = -delta;

        result = (value - magnitude) & mask;
        env->flag_c = (value & mask) < magnitude;
    }
    stc32g_dsp_set_z(s, result);
    return result;
}

static bool stc32g_dsp_divisor_ok(Stc32gDSPState *s, uint64_t divisor)
{
    stc32g_dsp_set_dbz(s, divisor == 0);
    return divisor != 0;
}

static uint32_t stc32g_dsp_shift_left(Stc32gDSPState *s, uint32_t value,
                                      unsigned count, unsigned width)
{
    uint64_t mask = MAKE_64BIT_MASK(0, width);

    if (count) {
        s->cpu->env.flag_c = (value >> (width - count)) & 1;
    }
    return (value << count) & mask;
}

static uint32_t stc32g_dsp_shift_right(Stc32gDSPState *s, uint32_t value,
                                       unsigned count, unsigned width,
                                       bool arithmetic)
{
    if (count) {
        s->cpu->env.flag_c = (value >> (count - 1)) & 1;
    }
    if (!arithmetic) {
        return value >> count;
    }
    if (width == 16) {
        return (uint16_t)((int16_t)value >> count);
    }
    return (uint32_t)((int32_t)value >> count);
}

static void stc32g_dsp_execute(Stc32gDSPState *s, uint8_t command)
{
    CPUMCS251State *env = &s->cpu->env;
    uint32_t eax = stc32g_dsp_get_eax(s);
    uint32_t ebx = stc32g_dsp_get_ebx(s);
    uint32_t ecx;
    uint32_t edx;
    uint32_t result;
    uint64_t wide;
    unsigned count;

    switch (command) {
    case DSP_FILL_DPUCFG:
        s->dpucfg = mcs251_cpu_get_reg8(env, MCS251_REG_ACC);
        break;
    case DSP_BIN2BCD:
        wide = 0;
        for (count = 0; count < 10; count++) {
            wide |= (uint64_t)(ebx % 10) << (count * 4);
            ebx /= 10;
        }
        stc32g_dsp_set_eax(s, wide >> 32);
        stc32g_dsp_set_ebx(s, wide);
        break;
    case DSP_BCD2BIN:
        wide = ((uint64_t)(eax & 0x7f) << 32) | ebx;
        result = 0;
        for (count = 0; count < 10; count++) {
            result = result * 10 + extract64(wide, (9 - count) * 4, 4);
        }
        stc32g_dsp_set_eax(s, result);
        break;
    case DSP_NRM_BX:
        count = clrsb32((int32_t)(int16_t)ebx) - 16;
        stc32g_dsp_set_bx(s, (uint16_t)ebx << count);
        stc32g_dsp_set_sc(s, count);
        break;
    case DSP_NRM_EBX:
        count = clrsb32(ebx);
        stc32g_dsp_set_ebx(s, ebx << count);
        stc32g_dsp_set_sc(s, count);
        break;
    case DSP_NRM_EABX:
        wide = ((uint64_t)eax << 32) | ebx;
        count = clrsb64(wide);
        wide <<= count;
        stc32g_dsp_set_eax(s, wide >> 32);
        stc32g_dsp_set_ebx(s, wide);
        stc32g_dsp_set_sc(s, count);
        break;
    case DSP_SWAP_EAB:
        stc32g_dsp_set_eax(s, ebx);
        stc32g_dsp_set_ebx(s, eax);
        break;
    case DSP_SWAP_ECD:
        if (stc32g_dsp_get_cd(s, true, &ecx) &&
            stc32g_dsp_get_cd(s, false, &edx)) {
            stc32g_dsp_set_cd(s, true, edx);
            stc32g_dsp_set_cd(s, false, ecx);
        }
        break;
    case DSP_SWAP_EAC:
        if (stc32g_dsp_get_cd(s, true, &ecx)) {
            stc32g_dsp_set_eax(s, ecx);
            stc32g_dsp_set_cd(s, true, eax);
        }
        break;
    case DSP_SWAP_EBD:
        if (stc32g_dsp_get_cd(s, false, &edx)) {
            stc32g_dsp_set_ebx(s, edx);
            stc32g_dsp_set_cd(s, false, ebx);
        }
        break;
    case DSP_ADDC_EABX:
        stc32g_dsp_set_eax(
            s, stc32g_dsp_add(s, eax, ebx, env->flag_c, 32, true));
        break;
    case DSP_ADDC_ABX:
        stc32g_dsp_set_ax(
            s, stc32g_dsp_add(s, eax, ebx, env->flag_c, 16, true));
        break;
    case DSP_ADD_EABX:
        stc32g_dsp_set_eax(
            s, stc32g_dsp_add(s, eax, ebx, 0, 32, true));
        break;
    case DSP_ADD_ABX:
        stc32g_dsp_set_ax(
            s, stc32g_dsp_add(s, eax, ebx, 0, 16, true));
        break;
    case DSP_SUBB_EABX:
        stc32g_dsp_set_eax(
            s, stc32g_dsp_sub(s, eax, ebx, env->flag_c, 32, true));
        break;
    case DSP_SUBB_ABX:
        stc32g_dsp_set_ax(
            s, stc32g_dsp_sub(s, eax, ebx, env->flag_c, 16, true));
        break;
    case DSP_SUB_EABX:
        stc32g_dsp_set_eax(
            s, stc32g_dsp_sub(s, eax, ebx, 0, 32, true));
        break;
    case DSP_SUB_ABX:
        stc32g_dsp_set_ax(
            s, stc32g_dsp_sub(s, eax, ebx, 0, 16, true));
        break;
    case DSP_CMP_EABX:
        stc32g_dsp_sub(s, eax, ebx, 0, 32, true);
        break;
    case DSP_CMP_ABX:
        stc32g_dsp_sub(s, eax, ebx, 0, 16, true);
        break;
    case DSP_MULU_EABX:
        if (stc32g_dsp_get_cd(s, false, &edx)) {
            wide = (uint64_t)eax * ebx;
            stc32g_dsp_set_eax(s, wide);
            stc32g_dsp_set_cd(s, false, wide >> 32);
        }
        break;
    case DSP_MULS_EABX:
        if (stc32g_dsp_get_cd(s, false, &edx)) {
            wide = (int64_t)(int32_t)eax * (int32_t)ebx;
            stc32g_dsp_set_eax(s, wide);
            stc32g_dsp_set_cd(s, false, wide >> 32);
        }
        break;
    case DSP_MULU_ABX:
        stc32g_dsp_set_eax(s, (uint32_t)(uint16_t)eax *
                              (uint16_t)ebx);
        break;
    case DSP_MULS_ABX:
        stc32g_dsp_set_eax(s, (int32_t)(int16_t)eax *
                              (int16_t)ebx);
        break;
    case DSP_MULX_KEIL16:
        stc32g_dsp_set_ax(s, stc32g_dsp_get_ax2(s) *
                             stc32g_dsp_get_ax(s));
        break;
    case DSP_DIVU_EABX:
        if (stc32g_dsp_divisor_ok(s, ebx)) {
            stc32g_dsp_set_eax(s, eax / ebx);
            stc32g_dsp_set_ebx(s, eax % ebx);
        }
        break;
    case DSP_DIVS_EABX:
        if (stc32g_dsp_divisor_ok(s, ebx)) {
            int32_t dividend = eax;
            int32_t divisor = ebx;

            if (dividend == INT32_MIN && divisor == -1) {
                stc32g_dsp_set_eax(s, INT32_MIN);
                stc32g_dsp_set_ebx(s, 0);
            } else {
                stc32g_dsp_set_eax(s, dividend / divisor);
                stc32g_dsp_set_ebx(s, dividend % divisor);
            }
        }
        break;
    case DSP_DIVU_ABX:
        if (stc32g_dsp_divisor_ok(s, (uint16_t)ebx)) {
            uint16_t dividend = eax;
            uint16_t divisor = ebx;

            stc32g_dsp_set_ax(s, dividend / divisor);
            stc32g_dsp_set_bx(s, dividend % divisor);
        }
        break;
    case DSP_DIVU_KEIL16:
        if (stc32g_dsp_divisor_ok(s, stc32g_dsp_get_ax2(s))) {
            uint16_t dividend = stc32g_dsp_get_ax(s);
            uint16_t divisor = stc32g_dsp_get_ax2(s);

            stc32g_dsp_set_ax(s, dividend / divisor);
            stc32g_dsp_set_ax2(s, dividend % divisor);
        }
        break;
    case DSP_DIVS_ABX:
        if (stc32g_dsp_divisor_ok(s, (uint16_t)ebx)) {
            int32_t dividend = (int16_t)eax;
            int32_t divisor = (int16_t)ebx;

            stc32g_dsp_set_ax(s, dividend / divisor);
            stc32g_dsp_set_bx(s, dividend % divisor);
        }
        break;
    case DSP_DIVS_KEIL16:
        if (stc32g_dsp_divisor_ok(s, stc32g_dsp_get_ax2(s))) {
            int32_t dividend = (int16_t)stc32g_dsp_get_ax(s);
            int32_t divisor = (int16_t)stc32g_dsp_get_ax2(s);

            stc32g_dsp_set_ax(s, dividend / divisor);
            stc32g_dsp_set_ax2(s, dividend % divisor);
        }
        break;
    case DSP_DIVU_EABXD:
        if (stc32g_dsp_get_cd(s, false, &edx) &&
            stc32g_dsp_divisor_ok(s, edx)) {
            wide = ((uint64_t)eax << 32) | ebx;
            stc32g_dsp_set_eax(s, (wide / edx) >> 32);
            stc32g_dsp_set_ebx(s, wide / edx);
            stc32g_dsp_set_cd(s, false, wide % edx);
        }
        break;
    case DSP_DIVU_EAXB:
        if (stc32g_dsp_divisor_ok(s, (uint16_t)ebx)) {
            uint16_t divisor = ebx;

            stc32g_dsp_set_eax(s, eax / divisor);
            stc32g_dsp_set_bx(s, eax % divisor);
        }
        break;
    case DSP_SET0_EAX:
    case DSP_SET1_EAX:
        stc32g_dsp_set_eax(s, command & 1 ? UINT32_MAX : 0);
        break;
    case DSP_SET0_EBX:
    case DSP_SET1_EBX:
        stc32g_dsp_set_ebx(s, command & 1 ? UINT32_MAX : 0);
        break;
    case DSP_SET0_ECX:
    case DSP_SET1_ECX:
        stc32g_dsp_set_cd(s, true, command & 1 ? UINT32_MAX : 0);
        break;
    case DSP_SET0_EDX:
    case DSP_SET1_EDX:
        stc32g_dsp_set_cd(s, false, command & 1 ? UINT32_MAX : 0);
        break;
    case DSP_NEGS_EAX:
        stc32g_dsp_set_eax(s, -eax);
        break;
    case DSP_NEGS_EBX:
        stc32g_dsp_set_ebx(s, -ebx);
        break;
    case DSP_NEGS_AX:
        stc32g_dsp_set_ax(s, -(uint16_t)eax);
        break;
    case DSP_NEGS_BX:
        stc32g_dsp_set_bx(s, -(uint16_t)ebx);
        break;
    case DSP_INC1_EAX:
    case DSP_INC4_EAX:
    case DSP_DEC1_EAX:
    case DSP_DEC4_EAX: {
        static const int delta[] = { 1, 4, -1, -4 };
        unsigned index = ((command & 8) >> 2) | ((command & 2) >> 1);

        stc32g_dsp_set_eax(
            s, stc32g_dsp_accumulate(s, eax, delta[index], 32));
        break;
    }
    case DSP_INC1_EBX:
    case DSP_INC4_EBX:
    case DSP_DEC1_EBX:
    case DSP_DEC4_EBX: {
        static const int delta[] = { 1, 4, -1, -4 };
        unsigned index = ((command & 8) >> 2) | ((command & 2) >> 1);

        stc32g_dsp_set_ebx(
            s, stc32g_dsp_accumulate(s, ebx, delta[index], 32));
        break;
    }
    case DSP_INC1_AX:
    case DSP_INC2_AX:
    case DSP_DEC1_AX:
    case DSP_DEC2_AX: {
        static const int delta[] = { 1, 2, -1, -2 };
        unsigned index = ((command & 8) >> 2) | ((command & 2) >> 1);

        stc32g_dsp_set_ax(
            s, stc32g_dsp_accumulate(s, eax, delta[index], 16));
        break;
    }
    case DSP_INC1_BX:
    case DSP_INC2_BX:
    case DSP_DEC1_BX:
    case DSP_DEC2_BX: {
        static const int delta[] = { 1, 2, -1, -2 };
        unsigned index = ((command & 8) >> 2) | ((command & 2) >> 1);

        stc32g_dsp_set_bx(
            s, stc32g_dsp_accumulate(s, ebx, delta[index], 16));
        break;
    }
    case DSP_BAND_EABX:
    case DSP_BOR_EABX:
    case DSP_BXOR_EABX:
        result = command == DSP_BAND_EABX ? ebx & eax :
                 command == DSP_BOR_EABX ? ebx | eax : ebx ^ eax;
        stc32g_dsp_set_ebx(s, result);
        stc32g_dsp_set_z(s, result);
        break;
    case DSP_BAND_ABX:
    case DSP_BOR_ABX:
    case DSP_BXOR_ABX:
        result = command == DSP_BAND_ABX ?
                 (uint16_t)ebx & (uint16_t)eax :
                 command == DSP_BOR_ABX ?
                 (uint16_t)ebx | (uint16_t)eax :
                 (uint16_t)ebx ^ (uint16_t)eax;
        stc32g_dsp_set_bx(s, result);
        stc32g_dsp_set_z(s, result);
        break;
    case DSP_BCPL_EAX:
        result = ~eax;
        stc32g_dsp_set_eax(s, result);
        stc32g_dsp_set_z(s, result);
        break;
    case DSP_BCPL_EBX:
        result = ~ebx;
        stc32g_dsp_set_ebx(s, result);
        stc32g_dsp_set_z(s, result);
        break;
    case DSP_SHL_EAX:
    case DSP_SHRU_EAX:
    case DSP_SHRS_EAX:
    case DSP_ROR_EAX:
        count = mcs251_cpu_get_reg8(env, MCS251_REG_ACC) & 31;
        if (command == DSP_SHL_EAX) {
            result = stc32g_dsp_shift_left(s, eax, count, 32);
        } else if (command == DSP_SHRU_EAX) {
            result = stc32g_dsp_shift_right(s, eax, count, 32, false);
        } else if (command == DSP_SHRS_EAX) {
            result = stc32g_dsp_shift_right(s, eax, count, 32, true);
        } else {
            result = ror32(eax, count);
        }
        stc32g_dsp_set_eax(s, result);
        break;
    case DSP_SHL_EBX:
    case DSP_SHRU_EBX:
    case DSP_SHRS_EBX:
    case DSP_ROR_EBX:
        count = mcs251_cpu_get_reg8(env, MCS251_REG_ACC) & 31;
        if (command == DSP_SHL_EBX) {
            result = stc32g_dsp_shift_left(s, ebx, count, 32);
        } else if (command == DSP_SHRU_EBX) {
            result = stc32g_dsp_shift_right(s, ebx, count, 32, false);
        } else if (command == DSP_SHRS_EBX) {
            result = stc32g_dsp_shift_right(s, ebx, count, 32, true);
        } else {
            result = ror32(ebx, count);
        }
        stc32g_dsp_set_ebx(s, result);
        break;
    case DSP_SHL_AX:
    case DSP_SHRU_AX:
    case DSP_SHRS_AX:
    case DSP_ROR_AX:
        count = mcs251_cpu_get_reg8(env, MCS251_REG_ACC) & 15;
        if (command == DSP_SHL_AX) {
            result = stc32g_dsp_shift_left(s, eax, count, 16);
        } else if (command == DSP_SHRU_AX) {
            result = stc32g_dsp_shift_right(s, (uint16_t)eax,
                                            count, 16, false);
        } else if (command == DSP_SHRS_AX) {
            result = stc32g_dsp_shift_right(s, (uint16_t)eax,
                                            count, 16, true);
        } else {
            result = ror16(eax, count);
        }
        stc32g_dsp_set_ax(s, result);
        break;
    case DSP_SHL_BX:
    case DSP_SHRU_BX:
    case DSP_SHRS_BX:
    case DSP_ROR_BX:
        count = mcs251_cpu_get_reg8(env, MCS251_REG_ACC) & 15;
        if (command == DSP_SHL_BX) {
            result = stc32g_dsp_shift_left(s, ebx, count, 16);
        } else if (command == DSP_SHRU_BX) {
            result = stc32g_dsp_shift_right(s, (uint16_t)ebx,
                                            count, 16, false);
        } else if (command == DSP_SHRS_BX) {
            result = stc32g_dsp_shift_right(s, (uint16_t)ebx,
                                            count, 16, true);
        } else {
            result = ror16(ebx, count);
        }
        stc32g_dsp_set_bx(s, result);
        break;
    case DSP_MMD32_EABX:
        if (stc32g_dsp_get_cd(s, false, &edx) &&
            stc32g_dsp_get_cd(s, true, &ecx) &&
            stc32g_dsp_divisor_ok(s, edx)) {
            int64_t quotient =
                ((int64_t)(int32_t)eax * (int32_t)ebx) / (int32_t)edx;

            stc32g_dsp_set_eax(s, quotient);
            stc32g_dsp_set_cd(s, true, (uint64_t)quotient >> 32);
        }
        break;
    case DSP_MMD16_ABX:
        if (stc32g_dsp_get_cd(s, false, &edx) &&
            stc32g_dsp_divisor_ok(s, (uint16_t)edx)) {
            int32_t quotient =
                ((int32_t)(int16_t)eax * (int16_t)ebx) / (int16_t)edx;

            stc32g_dsp_set_eax(s, quotient);
        }
        break;
    case DSP_LTC32_EAX:
        if (stc32g_dsp_get_cd(s, false, &edx) &&
            stc32g_dsp_get_cd(s, true, &ecx)) {
            int64_t difference = (int64_t)(int32_t)eax - (int32_t)ebx;

            stc32g_dsp_sub(s, eax, ebx, 0, 32, false);
            if (stc32g_dsp_divisor_ok(s, edx)) {
                int64_t quotient =
                    difference * (int32_t)ecx / (int32_t)edx;

                stc32g_dsp_set_eax(s, quotient);
                stc32g_dsp_set_cd(s, true,
                                  (uint64_t)quotient >> 32);
            }
        }
        break;
    case DSP_LTC16_AX:
        if (stc32g_dsp_get_cd(s, false, &edx) &&
            stc32g_dsp_get_cd(s, true, &ecx)) {
            int32_t difference = (int16_t)eax - (int16_t)ebx;

            stc32g_dsp_sub(s, eax, ebx, 0, 16, false);
            if (stc32g_dsp_divisor_ok(s, (uint16_t)edx)) {
                int64_t quotient =
                    (int64_t)difference * (int16_t)ecx / (int16_t)edx;

                stc32g_dsp_set_eax(s, quotient);
            }
        }
        break;
    case DSP_MA32_EDX:
        if (stc32g_dsp_get_cd(s, false, &edx)) {
            result = (int32_t)(int16_t)eax * (int16_t)ebx;
            stc32g_dsp_set_cd(
                s, false, stc32g_dsp_add(s, edx, result, 0, 32, false));
        }
        break;
    case DSP_MA64_ECDX:
        if (stc32g_dsp_get_cd(s, false, &edx) &&
            stc32g_dsp_get_cd(s, true, &ecx)) {
            uint64_t accumulator = ((uint64_t)ecx << 32) | edx;
            uint64_t product = (int64_t)(int32_t)eax * (int32_t)ebx;
            uint64_t sum = accumulator + product;
            uint64_t sign = BIT_ULL(63);

            env->flag_c = sum < accumulator;
            env->flag_ov =
                (~(accumulator ^ product) &
                 (accumulator ^ sum) & sign) != 0;
            stc32g_dsp_set_cd(s, true, sum >> 32);
            stc32g_dsp_set_cd(s, false, sum);
        }
        break;
    default:
        break;
    }
}

static void stc32g_dsp_command_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc32gDSPState *s = STC32G_DSP(reg->opaque);

    if (!device_is_in_reset(DEVICE(s))) {
        stc32g_dsp_execute(s, value);
    }
}

static const RegisterAccessInfo stc32g_dsp_status_regs_info[] = {
    { .name = "DPUST", .addr = A_DPUST, .ro = UINT8_MAX },
};

static const RegisterAccessInfo stc32g_dsp_command_regs_info[] = {
    { .name = "DPUOP", .addr = A_DPUOP,
      .post_write = stc32g_dsp_command_post_write },
};

static const MemoryRegionOps stc32g_dsp_regs_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc32g_dsp_reset(DeviceState *dev)
{
    Stc32gDSPState *s = STC32G_DSP(dev);

    register_reset(&s->status_reg_info[R_DPUST]);
    register_reset(&s->command_reg_info[R_DPUOP]);
    s->dpucfg = 5;
}

static const VMStateDescription stc32g_dsp_vmstate = {
    .name = "stc32g.dsp32",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(status_regs, Stc32gDSPState, 1),
        VMSTATE_UINT8_ARRAY(command_regs, Stc32gDSPState, 1),
        VMSTATE_UINT8(dpucfg, Stc32gDSPState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc32g_dsp_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc32gDSPState, cpu, TYPE_MCS251_CPU,
                     MCS251CPU *),
};

static void stc32g_dsp_realize(DeviceState *dev, Error **errp)
{
    Stc32gDSPState *s = STC32G_DSP(dev);

    if (!s->cpu) {
        error_setg(errp, "stc32g-dsp32 requires a CPU link");
    }
}

static void stc32g_dsp_init(Object *obj)
{
    Stc32gDSPState *s = STC32G_DSP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->status_reg_array = register_init_block8(
        DEVICE(obj), stc32g_dsp_status_regs_info,
        ARRAY_SIZE(stc32g_dsp_status_regs_info), s->status_reg_info,
        s->status_regs, &stc32g_dsp_regs_ops, false, 1);
    s->command_reg_array = register_init_block8(
        DEVICE(obj), stc32g_dsp_command_regs_info,
        ARRAY_SIZE(stc32g_dsp_command_regs_info), s->command_reg_info,
        s->command_regs, &stc32g_dsp_regs_ops, false, 1);
    sysbus_init_mmio(sbd, &s->status_reg_array->mem);
    sysbus_init_mmio(sbd, &s->command_reg_array->mem);
}

static void stc32g_dsp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc32g_dsp_realize;
    device_class_set_legacy_reset(dc, stc32g_dsp_reset);
    device_class_set_props(dc, stc32g_dsp_properties);
    dc->vmsd = &stc32g_dsp_vmstate;
}

static const TypeInfo stc32g_dsp_type = {
    .name = TYPE_STC32G_DSP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc32gDSPState),
    .instance_init = stc32g_dsp_init,
    .class_init = stc32g_dsp_class_init,
};

static void stc32g_dsp_register_types(void)
{
    type_register_static(&stc32g_dsp_type);
}

type_init(stc32g_dsp_register_types)
