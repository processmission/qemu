/*
 * MCS-51 family GDB register access
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "gdbstub/helpers.h"
#include "internals.h"

#ifdef TARGET_MCS251
static unsigned mcs251_gdb_reg_position(int n)
{
    return n < 32 ? n : 56 + n - 32;
}
#endif

int mcs251_cpu_gdb_read_register(CPUState *cs, GByteArray *buf, int n)
{
    CPUMCS251State *env = cpu_env(cs);

#ifndef TARGET_MCS251
    if (n < 8) {
        return gdb_get_reg8(buf, mcs251_cpu_get_reg8(env, n));
    }
    switch (n) {
    case 8:
        return gdb_get_reg8(buf,
                            mcs251_cpu_get_reg8(env, MCS251_REG_ACC));
    case 9:
        return gdb_get_reg8(buf,
                            mcs251_cpu_get_reg8(env, MCS251_REG_B));
    case 10:
        return gdb_get_reg8(buf,
                            mcs251_cpu_get_reg8(env, MCS251_REG_SP));
    case 11:
        return gdb_get_reg8(buf,
                            mcs251_cpu_get_reg8(env, MCS251_REG_DPL));
    case 12:
        return gdb_get_reg8(buf,
                            mcs251_cpu_get_reg8(env, MCS251_REG_DPH));
    case 13:
        return gdb_get_reg8(buf, mcs251_cpu_get_psw(env));
    case 14:
        return gdb_get_reg16(buf, env->pc);
    default:
        return 0;
    }
#else
    if (n < 40) {
        return gdb_get_reg8(buf,
                            mcs251_cpu_get_reg8(env,
                                                mcs251_gdb_reg_position(n)));
    }
    switch (n) {
    case 40:
        return gdb_get_reg8(buf, mcs251_cpu_get_psw(env));
    case 41:
        return gdb_get_reg8(buf, mcs251_cpu_get_psw1(env));
    case 42:
        return gdb_get_reg32(buf, env->pc);
    default:
        return 0;
    }
#endif
}

int mcs251_cpu_gdb_write_register(CPUState *cs, uint8_t *buf, int n)
{
    CPUMCS251State *env = cpu_env(cs);

#ifndef TARGET_MCS251
    if (n < 8) {
        mcs251_cpu_set_reg8(env, n, *buf);
        return 1;
    }
    switch (n) {
    case 8:
        mcs251_cpu_set_reg8(env, MCS251_REG_ACC, *buf);
        return 1;
    case 9:
        mcs251_cpu_set_reg8(env, MCS251_REG_B, *buf);
        return 1;
    case 10:
        mcs251_cpu_set_reg8(env, MCS251_REG_SP, *buf);
        return 1;
    case 11:
        mcs251_cpu_set_reg8(env, MCS251_REG_DPL, *buf);
        return 1;
    case 12:
        mcs251_cpu_set_reg8(env, MCS251_REG_DPH, *buf);
        return 1;
    case 13:
        mcs251_cpu_set_psw(env, *buf);
        return 1;
    case 14:
        env->pc = lduw_be_p(buf);
        return 2;
    default:
        return 0;
    }
#else
    if (n < 40) {
        mcs251_cpu_set_reg8(env, mcs251_gdb_reg_position(n), *buf);
        return 1;
    }
    switch (n) {
    case 40:
        mcs251_cpu_set_psw(env, *buf);
        return 1;
    case 41:
        mcs251_cpu_set_psw1(env, *buf);
        return 1;
    case 42:
        env->pc = ldl_be_p(buf) & MCS_TARGET_ADDR_MASK;
        return 4;
    default:
        return 0;
    }
#endif
}
