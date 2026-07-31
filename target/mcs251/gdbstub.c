/*
 * STC32G MCS-251 GDB register access
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "gdbstub/helpers.h"
#include "cpu.h"

static unsigned mcs251_gdb_reg_position(int n)
{
    return n < 32 ? n : 56 + n - 32;
}

int mcs251_cpu_gdb_read_register(CPUState *cs, GByteArray *buf, int n)
{
    CPUMCS251State *env = cpu_env(cs);

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
}

int mcs251_cpu_gdb_write_register(CPUState *cs, uint8_t *buf, int n)
{
    CPUMCS251State *env = cpu_env(cs);

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
        env->pc = ldl_be_p(buf) & MCS251_ADDR_MASK;
        return 4;
    default:
        return 0;
    }
}
