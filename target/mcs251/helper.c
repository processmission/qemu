/*
 * STC32G MCS-251 execution helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "cpu.h"
#include "accel/tcg/cpu-ldst.h"
#include "exec/helper-proto.h"
#include "qemu/plugin.h"

#define DPS_ID1 0x80
#define DPS_ID0 0x40
#define DPS_TSL 0x20
#define DPS_AU1 0x10
#define DPS_AU0 0x08
#define DPS_SEL 0x01

static uint8_t mcs251_code_load8(CPUMCS251State *env, uint32_t addr)
{
    CPUState *cs = env_cpu(env);
    MemOpIdx oi = make_memop_idx(MO_UB, cpu_mmu_index(cs, true));

    return cpu_ldb_code_mmu(env, addr & MCS251_ADDR_MASK, oi, GETPC());
}

static uint8_t mcs251_fetch8(CPUMCS251State *env, uint32_t *pc)
{
    uint8_t value = mcs251_code_load8(env, *pc);

    *pc = (*pc + 1) & MCS251_ADDR_MASK;
    return value;
}

static uint16_t mcs251_fetch16(CPUMCS251State *env, uint32_t *pc)
{
    uint16_t value = mcs251_fetch8(env, pc) << 8;

    return value | mcs251_fetch8(env, pc);
}

static uint32_t mcs251_fetch24(CPUMCS251State *env, uint32_t *pc)
{
    uint32_t value = mcs251_fetch8(env, pc) << 16;

    value |= mcs251_fetch8(env, pc) << 8;
    return value | mcs251_fetch8(env, pc);
}

static uint8_t mcs251_load8(CPUMCS251State *env, uint32_t addr)
{
    return cpu_ldub_data_ra(env, addr & MCS251_ADDR_MASK, GETPC());
}

static void mcs251_store8(CPUMCS251State *env, uint32_t addr, uint8_t value)
{
    cpu_stb_data_ra(env, addr & MCS251_ADDR_MASK, value, GETPC());
}

static uint32_t mcs251_load(CPUMCS251State *env, uint32_t addr,
                            unsigned bytes)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < bytes; i++) {
        value = (value << 8) | mcs251_load8(env, addr + i);
    }
    return value;
}

static void mcs251_store(CPUMCS251State *env, uint32_t addr, unsigned bytes,
                         uint32_t value)
{
    unsigned i;

    for (i = 0; i < bytes; i++) {
        unsigned shift = (bytes - i - 1) * 8;

        mcs251_store8(env, addr + i, value >> shift);
    }
}

static uint16_t mcs251_sp(CPUMCS251State *env)
{
    return mcs251_cpu_get_reg(env, 62, 2);
}

static void mcs251_push(CPUMCS251State *env, uint8_t value)
{
    uint16_t sp = mcs251_sp(env) + 1;

    mcs251_cpu_set_reg(env, 62, 2, sp);
    mcs251_store8(env, sp, value);
}

static uint8_t mcs251_pop(CPUMCS251State *env)
{
    uint16_t sp = mcs251_sp(env);
    uint8_t value = mcs251_load8(env, sp);

    mcs251_cpu_set_reg(env, 62, 2, sp - 1);
    return value;
}

static uint8_t mcs251_indirect_read(CPUMCS251State *env, unsigned ri)
{
    return mcs251_load8(env, mcs251_cpu_get_reg8(env, ri));
}

static void mcs251_indirect_write(CPUMCS251State *env, unsigned ri,
                                  uint8_t value)
{
    mcs251_store8(env, mcs251_cpu_get_reg8(env, ri), value);
}

static void mcs251_set_nz(CPUMCS251State *env, uint32_t value,
                          unsigned bits)
{
    uint32_t mask = bits == 32 ? UINT32_MAX : MAKE_64BIT_MASK(0, bits);

    value &= mask;
    env->flag_n = extract32(value, bits - 1, 1);
    env->flag_z = value == 0;
}

static uint32_t mcs251_add(CPUMCS251State *env, uint32_t lhs, uint32_t rhs,
                           unsigned carry, unsigned bits)
{
    uint64_t mask = bits == 32 ? UINT32_MAX : MAKE_64BIT_MASK(0, bits);
    uint64_t sign = 1ull << (bits - 1);
    uint64_t result = (lhs & mask) + (rhs & mask) + carry;
    uint32_t truncated = result & mask;

    env->flag_c = result > mask;
    if (bits == 8) {
        env->flag_ac = ((lhs & 0xf) + (rhs & 0xf) + carry) > 0xf;
    }
    env->flag_ov = (~(lhs ^ rhs) & (lhs ^ truncated) & sign) != 0;
    mcs251_set_nz(env, truncated, bits);
    return truncated;
}

static uint32_t mcs251_sub(CPUMCS251State *env, uint32_t lhs, uint32_t rhs,
                           unsigned borrow, unsigned bits)
{
    uint64_t mask = bits == 32 ? UINT32_MAX : MAKE_64BIT_MASK(0, bits);
    uint64_t sign = 1ull << (bits - 1);
    uint64_t subtrahend = (rhs & mask) + borrow;
    uint32_t truncated = (lhs - subtrahend) & mask;

    env->flag_c = (lhs & mask) < subtrahend;
    if (bits == 8) {
        env->flag_ac = (lhs & 0xf) < ((rhs & 0xf) + borrow);
    }
    env->flag_ov = ((lhs ^ rhs) & (lhs ^ truncated) & sign) != 0;
    mcs251_set_nz(env, truncated, bits);
    return truncated;
}

static uint8_t mcs251_bit_read(CPUMCS251State *env, uint8_t bit_addr)
{
    uint8_t byte_addr;

    if (bit_addr < 0x80) {
        byte_addr = 0x20 + (bit_addr >> 3);
    } else {
        byte_addr = bit_addr & 0xf8;
    }
    return extract8(mcs251_cpu_direct_read(env, byte_addr),
                    bit_addr & 7, 1);
}

static uint8_t mcs251_bit_rmw_read(CPUMCS251State *env, uint8_t bit_addr)
{
    uint8_t byte_addr;

    if (bit_addr < 0x80) {
        byte_addr = 0x20 + (bit_addr >> 3);
    } else {
        byte_addr = bit_addr & 0xf8;
    }
    return extract8(mcs251_cpu_direct_rmw_read(env, byte_addr),
                    bit_addr & 7, 1);
}

static void mcs251_bit_write(CPUMCS251State *env, uint8_t bit_addr,
                             bool value)
{
    uint8_t byte_addr;
    uint8_t byte;

    if (bit_addr < 0x80) {
        byte_addr = 0x20 + (bit_addr >> 3);
    } else {
        byte_addr = bit_addr & 0xf8;
    }
    byte = mcs251_cpu_direct_rmw_read(env, byte_addr);
    byte = deposit32(byte, bit_addr & 7, 1, value);
    mcs251_cpu_direct_write(env, byte_addr, byte);
}

static uint32_t mcs251_direct_load(CPUMCS251State *env, uint8_t addr,
                                  unsigned bytes)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < bytes; i++) {
        value = (value << 8) |
                mcs251_cpu_direct_read(env, addr + i);
    }
    return value;
}

static void mcs251_direct_store(CPUMCS251State *env, uint8_t addr,
                               unsigned bytes, uint32_t value)
{
    unsigned i;

    for (i = 0; i < bytes; i++) {
        unsigned shift = (bytes - i - 1) * 8;

        mcs251_cpu_direct_write(env, addr + i, value >> shift);
    }
}

static void mcs251_push_value(CPUMCS251State *env, uint32_t value,
                              unsigned bytes)
{
    unsigned i;

    for (i = 0; i < bytes; i++) {
        mcs251_push(env, value >> ((bytes - i - 1) * 8));
    }
}

static uint32_t mcs251_pop_value(CPUMCS251State *env, unsigned bytes)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < bytes; i++) {
        value |= mcs251_pop(env) << (i * 8);
    }
    return value;
}

static bool mcs251_native_dr_position(unsigned code, unsigned *position)
{
    if (code < 8) {
        *position = code * 4;
        return true;
    }
    if (code >= 14) {
        *position = 56 + (code - 14) * 4;
        return true;
    }
    return false;
}

static void mcs251_dptr_finish(CPUMCS251State *env, bool auto_update)
{
    unsigned selected = env->dps & DPS_SEL;
    uint8_t au_mask = selected ? DPS_AU1 : DPS_AU0;
    uint8_t id_mask = selected ? DPS_ID1 : DPS_ID0;

    if (auto_update && (env->dps & au_mask)) {
        uint32_t value = mcs251_cpu_get_reg(env, 56, 4);
        uint32_t address = value & MCS251_ADDR_MASK;

        address = (address + (env->dps & id_mask ? -1 : 1)) &
                  MCS251_ADDR_MASK;
        mcs251_cpu_set_reg(env, 56, 4,
                           (value & ~MCS251_ADDR_MASK) | address);
    }
    if (env->dps & DPS_TSL) {
        env->dps ^= DPS_SEL;
    }
}

static uint32_t mcs251_native_wr_address(CPUMCS251State *env, unsigned code)
{
    return mcs251_cpu_get_reg(env, code * 2, 2);
}

static bool mcs251_native_dr_address(CPUMCS251State *env, unsigned code,
                                     uint32_t *address)
{
    unsigned position;

    if (!mcs251_native_dr_position(code, &position)) {
        return false;
    }
    *address = mcs251_cpu_get_reg(env, position, 4) & MCS251_ADDR_MASK;
    return true;
}

typedef struct MCS251NativeOperand {
    uint32_t value;
    unsigned position;
    unsigned bytes;
    bool valid;
    bool dptr_access;
} MCS251NativeOperand;

static MCS251NativeOperand
mcs251_native_decode_operand(CPUMCS251State *env, uint8_t specifier,
                             uint32_t *pc, uint16_t allowed_modes)
{
    MCS251NativeOperand operand = { 0 };
    unsigned code = specifier >> 4;
    unsigned mode = specifier & 0xf;
    bool mode_allowed = allowed_modes & (1u << mode);
    uint32_t address;
    uint8_t direct;

    switch (mode) {
    case 0x0:
        operand.position = code;
        operand.bytes = 1;
        operand.value = mcs251_fetch8(env, pc);
        operand.valid = true;
        break;
    case 0x4:
        operand.position = code * 2;
        operand.bytes = 2;
        operand.value = mcs251_fetch16(env, pc);
        operand.valid = true;
        break;
    case 0x8:
    case 0xc:
        operand.valid = mcs251_native_dr_position(code,
                                                  &operand.position);
        operand.bytes = 4;
        operand.value = mcs251_fetch16(env, pc);
        if (mode == 0xc) {
            operand.value |= 0xffff0000u;
        }
        break;
    case 0x1:
    case 0x5:
    case 0xd:
        direct = mcs251_fetch8(env, pc);
        operand.bytes = mode == 0x1 ? 1 : mode == 0x5 ? 2 : 4;
        if (operand.bytes == 4) {
            operand.valid = mcs251_native_dr_position(code,
                                                      &operand.position);
        } else {
            operand.position = code * operand.bytes;
            operand.valid = true;
        }
        if (operand.valid && mode_allowed) {
            operand.value = mcs251_direct_load(env, direct, operand.bytes);
        }
        break;
    case 0x3:
    case 0x7:
    case 0xf:
        address = mcs251_fetch16(env, pc);
        operand.bytes = mode == 0x3 ? 1 : mode == 0x7 ? 2 : 4;
        if (operand.bytes == 4) {
            operand.valid = mcs251_native_dr_position(code,
                                                      &operand.position);
        } else {
            operand.position = code * operand.bytes;
            operand.valid = true;
        }
        if (operand.valid && mode_allowed) {
            operand.value = mcs251_load(env, address, operand.bytes);
        }
        break;
    case 0x9:
    case 0xb: {
        uint8_t destination = mcs251_fetch8(env, pc);

        operand.position = destination >> 4;
        operand.bytes = 1;
        operand.valid = !(destination & 0xf);
        if (mode == 0x9) {
            address = mcs251_native_wr_address(env, code);
        } else if (!mcs251_native_dr_address(env, code, &address)) {
            operand.valid = false;
            address = 0;
        } else if (code == 14) {
            operand.dptr_access = true;
        }
        if (operand.valid && mode_allowed) {
            operand.value = mcs251_load8(env, address);
        }
        break;
    }
    default:
        break;
    }

    operand.valid = operand.valid && mode_allowed;
    return operand;
}

static uint8_t mcs251_acc_source(CPUMCS251State *env, uint8_t opcode,
                                 uint32_t *pc)
{
    switch (opcode & 0x0f) {
    case 4:
        return mcs251_fetch8(env, pc);
    case 5:
        return mcs251_cpu_direct_read(env, mcs251_fetch8(env, pc));
    case 6:
    case 7:
        return mcs251_indirect_read(env, opcode & 1);
    default:
        return mcs251_cpu_get_reg8(env, opcode & 7);
    }
}

static void mcs251_relative(uint32_t *pc, int8_t displacement)
{
    *pc = (*pc + displacement) & MCS251_ADDR_MASK;
}

static void mcs251_cjne_flags(CPUMCS251State *env, uint8_t lhs,
                              uint8_t rhs)
{
    env->flag_c = lhs < rhs;
    mcs251_set_nz(env, lhs - rhs, 8);
}

static void mcs251_push_near_return(CPUMCS251State *env, uint32_t pc)
{
    mcs251_push(env, pc);
    mcs251_push(env, pc >> 8);
}

static void mcs251_return_near(CPUMCS251State *env, uint32_t *pc)
{
    uint32_t high = mcs251_pop(env);
    uint32_t low = mcs251_pop(env);

    *pc = (*pc & 0xff0000) | (high << 8) | low;
}

static void mcs251_return_extended(CPUMCS251State *env, uint32_t *pc)
{
    uint32_t low = mcs251_pop(env);
    uint32_t middle = mcs251_pop(env);
    uint32_t high = mcs251_pop(env);

    *pc = high << 16 | middle << 8 | low;
}

static void mcs251_return_interrupt(CPUMCS251State *env, uint32_t *pc)
{
    uint32_t middle = mcs251_pop(env);
    uint32_t low = mcs251_pop(env);
    uint32_t high = mcs251_pop(env);

    mcs251_cpu_set_psw1(env, mcs251_pop(env));
    *pc = high << 16 | middle << 8 | low;
    if (env->irq_depth) {
        env->irq_level = env->irq_level_stack[--env->irq_depth];
    } else {
        env->irq_level = UINT32_MAX;
    }
}

static void mcs251_classic_execute(CPUMCS251State *env, uint8_t opcode,
                                   uint32_t *pc)
{
    uint8_t acc = mcs251_cpu_get_reg8(env, 11);
    uint8_t value;
    uint8_t direct;
    uint8_t rel;
    uint16_t dptr;
    uint32_t address;

    if ((opcode & 0x1f) == 0x01) {
        uint8_t low = mcs251_fetch8(env, pc);
        uint32_t target = (*pc & 0xfff800) |
                          ((opcode & 0xe0) << 3) | low;

        *pc = target;
        return;
    }
    if ((opcode & 0x1f) == 0x11) {
        uint8_t low = mcs251_fetch8(env, pc);
        uint32_t target = (*pc & 0xfff800) |
                          ((opcode & 0xe0) << 3) | low;

        mcs251_push_near_return(env, *pc);
        *pc = target;
        return;
    }

    if ((opcode & 0xf8) == 0x08) {
        value = mcs251_cpu_get_reg8(env, opcode & 7) + 1;
        mcs251_cpu_set_reg8(env, opcode & 7, value);
        return;
    }
    if ((opcode & 0xf8) == 0x18) {
        value = mcs251_cpu_get_reg8(env, opcode & 7) - 1;
        mcs251_cpu_set_reg8(env, opcode & 7, value);
        mcs251_set_nz(env, value, 8);
        return;
    }
    if ((opcode & 0xf8) == 0x28) {
        value = mcs251_add(env, acc, mcs251_cpu_get_reg8(env, opcode & 7),
                           0, 8);
        mcs251_cpu_set_reg8(env, 11, value);
        return;
    }
    if ((opcode & 0xf8) == 0x38) {
        value = mcs251_add(env, acc, mcs251_cpu_get_reg8(env, opcode & 7),
                           env->flag_c, 8);
        mcs251_cpu_set_reg8(env, 11, value);
        return;
    }
    if ((opcode & 0xf8) == 0x48 ||
        (opcode & 0xf8) == 0x58 ||
        (opcode & 0xf8) == 0x68) {
        uint8_t rhs = mcs251_cpu_get_reg8(env, opcode & 7);

        if ((opcode & 0xf0) == 0x40) {
            value = acc | rhs;
        } else if ((opcode & 0xf0) == 0x50) {
            value = acc & rhs;
        } else {
            value = acc ^ rhs;
        }
        mcs251_cpu_set_reg8(env, 11, value);
        mcs251_set_nz(env, value, 8);
        return;
    }
    if ((opcode & 0xf8) == 0x78) {
        mcs251_cpu_set_reg8(env, opcode & 7, mcs251_fetch8(env, pc));
        return;
    }
    if ((opcode & 0xf8) == 0x88) {
        direct = mcs251_fetch8(env, pc);
        mcs251_cpu_direct_write(env, direct,
                                mcs251_cpu_get_reg8(env, opcode & 7));
        return;
    }
    if ((opcode & 0xf8) == 0x98) {
        value = mcs251_sub(env, acc,
                           mcs251_cpu_get_reg8(env, opcode & 7),
                           env->flag_c, 8);
        mcs251_cpu_set_reg8(env, 11, value);
        return;
    }
    if ((opcode & 0xf8) == 0xa8) {
        direct = mcs251_fetch8(env, pc);
        mcs251_cpu_set_reg8(env, opcode & 7,
                            mcs251_cpu_direct_read(env, direct));
        return;
    }
    if ((opcode & 0xf8) == 0xb8) {
        uint8_t lhs = mcs251_cpu_get_reg8(env, opcode & 7);
        uint8_t rhs = mcs251_fetch8(env, pc);

        rel = mcs251_fetch8(env, pc);
        mcs251_cjne_flags(env, lhs, rhs);
        if (lhs != rhs) {
            mcs251_relative(pc, rel);
        }
        return;
    }
    if ((opcode & 0xf8) == 0xc8) {
        value = mcs251_cpu_get_reg8(env, opcode & 7);
        mcs251_cpu_set_reg8(env, opcode & 7, acc);
        mcs251_cpu_set_reg8(env, 11, value);
        return;
    }
    if ((opcode & 0xf8) == 0xd8) {
        unsigned reg = opcode & 7;

        value = mcs251_cpu_get_reg8(env, reg) - 1;
        rel = mcs251_fetch8(env, pc);
        mcs251_cpu_set_reg8(env, reg, value);
        mcs251_set_nz(env, value, 8);
        if (value) {
            mcs251_relative(pc, rel);
        }
        return;
    }
    if ((opcode & 0xf8) == 0xe8) {
        mcs251_cpu_set_reg8(env, 11,
                            mcs251_cpu_get_reg8(env, opcode & 7));
        return;
    }
    if ((opcode & 0xf8) == 0xf8) {
        mcs251_cpu_set_reg8(env, opcode & 7, acc);
        return;
    }

    switch (opcode) {
    case 0x00:
    case 0xa5:
        break;
    case 0x02:
        dptr = mcs251_fetch16(env, pc);
        *pc = (*pc & 0xff0000) | dptr;
        break;
    case 0x03:
        value = ror8(acc, 1);
        mcs251_cpu_set_reg8(env, 11, value);
        mcs251_set_nz(env, value, 8);
        break;
    case 0x04:
        value = acc + 1;
        mcs251_cpu_set_reg8(env, 11, value);
        break;
    case 0x05:
        direct = mcs251_fetch8(env, pc);
        value = mcs251_cpu_direct_rmw_read(env, direct) + 1;
        mcs251_cpu_direct_write(env, direct, value);
        break;
    case 0x06:
    case 0x07:
        value = mcs251_indirect_read(env, opcode & 1) + 1;
        mcs251_indirect_write(env, opcode & 1, value);
        break;
    case 0x10:
        direct = mcs251_fetch8(env, pc);
        rel = mcs251_fetch8(env, pc);
        if (mcs251_bit_rmw_read(env, direct)) {
            mcs251_bit_write(env, direct, false);
            mcs251_relative(pc, rel);
        }
        break;
    case 0x12:
        dptr = mcs251_fetch16(env, pc);
        address = (*pc & 0xff0000) | dptr;
        mcs251_push_near_return(env, *pc);
        *pc = address;
        break;
    case 0x13:
        value = (env->flag_c << 7) | (acc >> 1);
        env->flag_c = acc & 1;
        mcs251_cpu_set_reg8(env, 11, value);
        mcs251_set_nz(env, value, 8);
        break;
    case 0x14:
        value = acc - 1;
        mcs251_cpu_set_reg8(env, 11, value);
        mcs251_set_nz(env, value, 8);
        break;
    case 0x15:
        direct = mcs251_fetch8(env, pc);
        value = mcs251_cpu_direct_rmw_read(env, direct) - 1;
        mcs251_cpu_direct_write(env, direct, value);
        mcs251_set_nz(env, value, 8);
        break;
    case 0x16:
    case 0x17:
        value = mcs251_indirect_read(env, opcode & 1) - 1;
        mcs251_indirect_write(env, opcode & 1, value);
        mcs251_set_nz(env, value, 8);
        break;
    case 0x20:
    case 0x30:
        direct = mcs251_fetch8(env, pc);
        rel = mcs251_fetch8(env, pc);
        if (mcs251_bit_read(env, direct) == (opcode == 0x20)) {
            mcs251_relative(pc, rel);
        }
        break;
    case 0x22:
        mcs251_return_near(env, pc);
        break;
    case 0x23:
        value = rol8(acc, 1);
        mcs251_cpu_set_reg8(env, 11, value);
        mcs251_set_nz(env, value, 8);
        break;
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x27:
        value = mcs251_add(env, acc,
                           mcs251_acc_source(env, opcode, pc), 0, 8);
        mcs251_cpu_set_reg8(env, 11, value);
        break;
    case 0x32:
        mcs251_return_interrupt(env, pc);
        break;
    case 0x33:
        value = (acc << 1) | env->flag_c;
        env->flag_c = acc >> 7;
        mcs251_cpu_set_reg8(env, 11, value);
        mcs251_set_nz(env, value, 8);
        break;
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x37: {
        uint8_t carry = env->flag_c;

        value = mcs251_add(env, acc,
                           mcs251_acc_source(env, opcode, pc), carry, 8);
        mcs251_cpu_set_reg8(env, 11, value);
        break;
    }
    case 0x40:
    case 0x50:
    case 0x60:
    case 0x70:
        rel = mcs251_fetch8(env, pc);
        if ((opcode == 0x40 && env->flag_c) ||
            (opcode == 0x50 && !env->flag_c) ||
            (opcode == 0x60 && acc == 0) ||
            (opcode == 0x70 && acc != 0)) {
            mcs251_relative(pc, rel);
        }
        break;
    case 0x42:
    case 0x52:
    case 0x62:
        direct = mcs251_fetch8(env, pc);
        value = mcs251_cpu_direct_rmw_read(env, direct);
        if (opcode == 0x42) {
            value |= acc;
        } else if (opcode == 0x52) {
            value &= acc;
        } else {
            value ^= acc;
        }
        mcs251_cpu_direct_write(env, direct, value);
        mcs251_set_nz(env, value, 8);
        break;
    case 0x43:
    case 0x53:
    case 0x63: {
        uint8_t immediate;

        direct = mcs251_fetch8(env, pc);
        immediate = mcs251_fetch8(env, pc);
        value = mcs251_cpu_direct_rmw_read(env, direct);
        if (opcode == 0x43) {
            value |= immediate;
        } else if (opcode == 0x53) {
            value &= immediate;
        } else {
            value ^= immediate;
        }
        mcs251_cpu_direct_write(env, direct, value);
        mcs251_set_nz(env, value, 8);
        break;
    }
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x67: {
        uint8_t rhs = mcs251_acc_source(env, opcode, pc);

        if ((opcode & 0xf0) == 0x40) {
            value = acc | rhs;
        } else if ((opcode & 0xf0) == 0x50) {
            value = acc & rhs;
        } else {
            value = acc ^ rhs;
        }
        mcs251_cpu_set_reg8(env, 11, value);
        mcs251_set_nz(env, value, 8);
        break;
    }
    case 0x72:
    case 0x82:
    case 0xa0:
    case 0xb0:
        direct = mcs251_fetch8(env, pc);
        value = mcs251_bit_read(env, direct);
        if (opcode & 0x20) {
            value = !value;
        }
        if ((opcode & 0xf0) == 0x70 || (opcode & 0xf0) == 0xa0) {
            env->flag_c |= value;
        } else {
            env->flag_c &= value;
        }
        break;
    case 0x73:
        *pc = (*pc & 0xff0000) |
              ((mcs251_cpu_get_reg(env, 58, 2) + acc) & 0xffff);
        break;
    case 0x74:
        mcs251_cpu_set_reg8(env, 11, mcs251_fetch8(env, pc));
        break;
    case 0x75:
        direct = mcs251_fetch8(env, pc);
        mcs251_cpu_direct_write(env, direct, mcs251_fetch8(env, pc));
        break;
    case 0x76:
    case 0x77:
        mcs251_indirect_write(env, opcode & 1, mcs251_fetch8(env, pc));
        break;
    case 0x80:
        mcs251_relative(pc, mcs251_fetch8(env, pc));
        break;
    case 0x83:
        address = (*pc & 0xff0000) | ((*pc + acc) & 0xffff);
        mcs251_cpu_set_reg8(env, 11, mcs251_code_load8(env, address));
        break;
    case 0x84: {
        uint8_t divisor = mcs251_cpu_get_reg8(env, 10);

        env->flag_c = 0;
        if (!divisor) {
            env->flag_ov = 1;
        } else {
            env->flag_ov = 0;
            mcs251_cpu_set_reg8(env, 11, acc / divisor);
            mcs251_cpu_set_reg8(env, 10, acc % divisor);
            mcs251_set_nz(env, acc / divisor, 8);
        }
        break;
    }
    case 0x85: {
        uint8_t source = mcs251_fetch8(env, pc);
        uint8_t destination = mcs251_fetch8(env, pc);

        mcs251_cpu_direct_write(env, destination,
                                mcs251_cpu_direct_read(env, source));
        break;
    }
    case 0x86:
    case 0x87:
        direct = mcs251_fetch8(env, pc);
        mcs251_cpu_direct_write(env, direct,
                                mcs251_indirect_read(env, opcode & 1));
        break;
    case 0x90:
        dptr = mcs251_fetch16(env, pc);
        mcs251_cpu_set_reg(env, 58, 2, dptr);
        mcs251_dptr_finish(env, false);
        break;
    case 0x92:
        mcs251_bit_write(env, mcs251_fetch8(env, pc), env->flag_c);
        break;
    case 0x93:
        address = (*pc & 0xff0000) |
                  ((mcs251_cpu_get_reg(env, 58, 2) + acc) & 0xffff);
        mcs251_cpu_set_reg8(env, 11, mcs251_code_load8(env, address));
        mcs251_dptr_finish(env, true);
        break;
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97: {
        uint8_t borrow = env->flag_c;

        value = mcs251_sub(env, acc,
                           mcs251_acc_source(env, opcode, pc), borrow, 8);
        mcs251_cpu_set_reg8(env, 11, value);
        break;
    }
    case 0xa2:
        env->flag_c = mcs251_bit_read(env, mcs251_fetch8(env, pc));
        break;
    case 0xa3:
        dptr = mcs251_cpu_get_reg(env, 58, 2) + 1;
        mcs251_cpu_set_reg(env, 58, 2, dptr);
        mcs251_set_nz(env, dptr, 16);
        mcs251_dptr_finish(env, false);
        break;
    case 0xa4: {
        uint16_t result = acc * mcs251_cpu_get_reg8(env, 10);

        mcs251_cpu_set_reg8(env, 11, result);
        mcs251_cpu_set_reg8(env, 10, result >> 8);
        env->flag_c = 0;
        env->flag_ov = result > 0xff;
        mcs251_set_nz(env, result, 16);
        break;
    }
    case 0xa6:
    case 0xa7:
        direct = mcs251_fetch8(env, pc);
        mcs251_indirect_write(env, opcode & 1,
                              mcs251_cpu_direct_read(env, direct));
        break;
    case 0xb2:
        direct = mcs251_fetch8(env, pc);
        mcs251_bit_write(env, direct, !mcs251_bit_rmw_read(env, direct));
        break;
    case 0xb3:
        env->flag_c ^= 1;
        break;
    case 0xb4: {
        uint8_t rhs = mcs251_fetch8(env, pc);

        rel = mcs251_fetch8(env, pc);
        mcs251_cjne_flags(env, acc, rhs);
        if (acc != rhs) {
            mcs251_relative(pc, rel);
        }
        break;
    }
    case 0xb5: {
        uint8_t rhs;

        direct = mcs251_fetch8(env, pc);
        rhs = mcs251_cpu_direct_read(env, direct);
        rel = mcs251_fetch8(env, pc);
        mcs251_cjne_flags(env, acc, rhs);
        if (acc != rhs) {
            mcs251_relative(pc, rel);
        }
        break;
    }
    case 0xb6:
    case 0xb7: {
        uint8_t lhs = mcs251_indirect_read(env, opcode & 1);
        uint8_t rhs = mcs251_fetch8(env, pc);

        rel = mcs251_fetch8(env, pc);
        mcs251_cjne_flags(env, lhs, rhs);
        if (lhs != rhs) {
            mcs251_relative(pc, rel);
        }
        break;
    }
    case 0xc0:
        mcs251_push(env,
                    mcs251_cpu_direct_read(env, mcs251_fetch8(env, pc)));
        break;
    case 0xc2:
        mcs251_bit_write(env, mcs251_fetch8(env, pc), false);
        break;
    case 0xc3:
        env->flag_c = 0;
        break;
    case 0xc4:
        value = (acc << 4) | (acc >> 4);
        mcs251_cpu_set_reg8(env, 11, value);
        mcs251_set_nz(env, value, 8);
        break;
    case 0xc5:
        direct = mcs251_fetch8(env, pc);
        value = mcs251_cpu_direct_rmw_read(env, direct);
        mcs251_cpu_direct_write(env, direct, acc);
        mcs251_cpu_set_reg8(env, 11, value);
        break;
    case 0xc6:
    case 0xc7:
        value = mcs251_indirect_read(env, opcode & 1);
        mcs251_indirect_write(env, opcode & 1, acc);
        mcs251_cpu_set_reg8(env, 11, value);
        break;
    case 0xd0:
        direct = mcs251_fetch8(env, pc);
        mcs251_cpu_direct_write(env, direct, mcs251_pop(env));
        break;
    case 0xd2:
        mcs251_bit_write(env, mcs251_fetch8(env, pc), true);
        break;
    case 0xd3:
        env->flag_c = 1;
        break;
    case 0xd4: {
        uint16_t adjusted = acc;
        bool carry = env->flag_c;

        if ((acc & 0x0f) > 9 || env->flag_ac) {
            adjusted += 0x06;
        }
        if (adjusted > 0x9f || carry) {
            adjusted += 0x60;
        }
        env->flag_c = carry || adjusted > 0xff;
        mcs251_cpu_set_reg8(env, 11, adjusted);
        mcs251_set_nz(env, adjusted, 8);
        break;
    }
    case 0xd5:
        direct = mcs251_fetch8(env, pc);
        rel = mcs251_fetch8(env, pc);
        value = mcs251_cpu_direct_rmw_read(env, direct) - 1;
        mcs251_cpu_direct_write(env, direct, value);
        mcs251_set_nz(env, value, 8);
        if (value) {
            mcs251_relative(pc, rel);
        }
        break;
    case 0xd6:
    case 0xd7: {
        uint8_t memory = mcs251_indirect_read(env, opcode & 1);

        mcs251_indirect_write(env, opcode & 1,
                              (memory & 0xf0) | (acc & 0x0f));
        mcs251_cpu_set_reg8(env, 11, (acc & 0xf0) | (memory & 0x0f));
        break;
    }
    case 0xe0:
        address = mcs251_cpu_get_reg(env, 56, 4) & MCS251_ADDR_MASK;
        mcs251_cpu_set_reg8(env, 11, mcs251_load8(env, address));
        mcs251_dptr_finish(env, true);
        break;
    case 0xe2:
    case 0xe3:
        address = env->mxax << 16 |
                  mcs251_cpu_direct_read(env, 0xa0) << 8 |
                  mcs251_cpu_get_reg8(env, opcode & 1);
        mcs251_cpu_set_reg8(env, 11, mcs251_load8(env, address));
        break;
    case 0xe4:
        mcs251_cpu_set_reg8(env, 11, 0);
        mcs251_set_nz(env, 0, 8);
        break;
    case 0xe5:
        mcs251_cpu_set_reg8(env, 11,
                            mcs251_cpu_direct_read(env,
                                                  mcs251_fetch8(env, pc)));
        break;
    case 0xe6:
    case 0xe7:
        mcs251_cpu_set_reg8(env, 11,
                            mcs251_indirect_read(env, opcode & 1));
        break;
    case 0xf0:
        address = mcs251_cpu_get_reg(env, 56, 4) & MCS251_ADDR_MASK;
        mcs251_store8(env, address, acc);
        mcs251_dptr_finish(env, true);
        break;
    case 0xf2:
    case 0xf3:
        address = env->mxax << 16 |
                  mcs251_cpu_direct_read(env, 0xa0) << 8 |
                  mcs251_cpu_get_reg8(env, opcode & 1);
        mcs251_store8(env, address, acc);
        break;
    case 0xf4:
        value = ~acc;
        mcs251_cpu_set_reg8(env, 11, value);
        mcs251_set_nz(env, value, 8);
        break;
    case 0xf5:
        mcs251_cpu_direct_write(env, mcs251_fetch8(env, pc), acc);
        break;
    case 0xf6:
    case 0xf7:
        mcs251_indirect_write(env, opcode & 1, acc);
        break;
    default:
        /* All unused encodings are architecturally NOP. */
        break;
    }
}

static bool mcs251_native_register_positions(uint8_t opcode,
                                             uint8_t specifier,
                                             unsigned *destination,
                                             unsigned *source,
                                             unsigned *bytes)
{
    unsigned destination_code = specifier >> 4;
    unsigned source_code = specifier & 0xf;

    switch (opcode & 3) {
    case 0:
        *destination = destination_code;
        *source = source_code;
        *bytes = 1;
        return true;
    case 1:
        *destination = destination_code * 2;
        *source = source_code * 2;
        *bytes = 2;
        return true;
    case 3:
        *bytes = 4;
        return mcs251_native_dr_position(destination_code, destination) &&
               mcs251_native_dr_position(source_code, source);
    default:
        return false;
    }
}

static void mcs251_native_register_execute(CPUMCS251State *env,
                                           uint8_t opcode,
                                           uint8_t specifier)
{
    unsigned destination;
    unsigned source;
    unsigned bytes;
    uint32_t lhs;
    uint32_t rhs;
    uint32_t result;

    if (!mcs251_native_register_positions(opcode, specifier, &destination,
                                          &source, &bytes)) {
        return;
    }

    lhs = mcs251_cpu_get_reg(env, destination, bytes);
    rhs = mcs251_cpu_get_reg(env, source, bytes);
    switch (opcode & 0xfc) {
    case 0x2c:
        result = mcs251_add(env, lhs, rhs, 0, bytes * 8);
        break;
    case 0x4c:
        result = lhs | rhs;
        mcs251_set_nz(env, result, bytes * 8);
        break;
    case 0x5c:
        result = lhs & rhs;
        mcs251_set_nz(env, result, bytes * 8);
        break;
    case 0x6c:
        result = lhs ^ rhs;
        mcs251_set_nz(env, result, bytes * 8);
        break;
    case 0x7c:
        result = rhs;
        break;
    case 0x9c:
        result = mcs251_sub(env, lhs, rhs, 0, bytes * 8);
        break;
    case 0xbc:
        mcs251_sub(env, lhs, rhs, 0, bytes * 8);
        return;
    default:
        return;
    }
    mcs251_cpu_set_reg(env, destination, bytes, result);
    if (bytes == 4 && destination == 56 &&
        ((opcode & 0xfc) == 0x2c ||
         (opcode & 0xfc) == 0x7c ||
         (opcode & 0xfc) == 0x9c)) {
        mcs251_dptr_finish(env, false);
    }
}

static void mcs251_native_muldiv_execute(CPUMCS251State *env,
                                         uint8_t opcode,
                                         uint8_t specifier)
{
    unsigned destination_code = specifier >> 4;
    unsigned source_code = specifier & 0xf;
    unsigned destination;
    unsigned source;
    unsigned base;
    unsigned bytes;
    unsigned result_bytes;
    uint32_t lhs;
    uint32_t rhs;

    if ((opcode & 1) == 0) {
        destination = destination_code;
        source = source_code;
        bytes = 1;
        base = destination & ~1;
    } else {
        destination = destination_code * 2;
        source = source_code * 2;
        bytes = 2;
        base = destination & ~3;
    }
    result_bytes = bytes * 2;
    lhs = mcs251_cpu_get_reg(env, destination, bytes);
    rhs = mcs251_cpu_get_reg(env, source, bytes);
    env->flag_c = 0;

    if ((opcode & 0xfe) == 0xac) {
        uint32_t result = lhs * rhs;
        uint32_t operand_mask = bytes == 1 ? UINT8_MAX : UINT16_MAX;

        mcs251_cpu_set_reg(env, base, result_bytes, result);
        env->flag_ov = result > operand_mask;
        mcs251_set_nz(env, result, result_bytes * 8);
    } else if (!rhs) {
        env->flag_ov = 1;
    } else {
        uint32_t quotient = lhs / rhs;
        uint32_t remainder = lhs % rhs;

        mcs251_cpu_set_reg(env, base, bytes, remainder);
        mcs251_cpu_set_reg(env, base + bytes, bytes, quotient);
        env->flag_ov = 0;
        mcs251_set_nz(env, quotient, bytes * 8);
    }
}

static void mcs251_native_generic_execute(CPUMCS251State *env,
                                          uint8_t opcode, uint32_t *pc)
{
    static const uint16_t arithmetic_modes =
        (1u << 0x0) | (1u << 0x1) | (1u << 0x3) |
        (1u << 0x4) | (1u << 0x5) | (1u << 0x7) |
        (1u << 0x8) | (1u << 0x9) | (1u << 0xb);
    static const uint16_t logical_modes =
        (1u << 0x0) | (1u << 0x1) | (1u << 0x3) |
        (1u << 0x4) | (1u << 0x5) | (1u << 0x7) |
        (1u << 0x9) | (1u << 0xb);
    static const uint16_t move_modes =
        (1u << 0x0) | (1u << 0x1) | (1u << 0x3) |
        (1u << 0x4) | (1u << 0x5) | (1u << 0x7) |
        (1u << 0x8) | (1u << 0x9) | (1u << 0xb) |
        (1u << 0xc) | (1u << 0xd) | (1u << 0xf);
    uint16_t allowed_modes;
    MCS251NativeOperand operand;
    uint32_t lhs;
    uint32_t result;
    uint8_t specifier = mcs251_fetch8(env, pc);

    switch (opcode) {
    case 0x2e:
    case 0x9e:
        allowed_modes = arithmetic_modes;
        break;
    case 0x4e:
    case 0x5e:
    case 0x6e:
        allowed_modes = logical_modes;
        break;
    case 0x7e:
        allowed_modes = move_modes;
        break;
    case 0xbe:
        allowed_modes = arithmetic_modes | (1u << 0xc);
        break;
    default:
        return;
    }

    operand = mcs251_native_decode_operand(env, specifier, pc,
                                           allowed_modes);
    if (!operand.valid) {
        return;
    }
    lhs = mcs251_cpu_get_reg(env, operand.position, operand.bytes);

    switch (opcode) {
    case 0x2e:
        result = mcs251_add(env, lhs, operand.value, 0,
                            operand.bytes * 8);
        break;
    case 0x4e:
        result = lhs | operand.value;
        mcs251_set_nz(env, result, operand.bytes * 8);
        break;
    case 0x5e:
        result = lhs & operand.value;
        mcs251_set_nz(env, result, operand.bytes * 8);
        break;
    case 0x6e:
        result = lhs ^ operand.value;
        mcs251_set_nz(env, result, operand.bytes * 8);
        break;
    case 0x7e:
        result = operand.value;
        break;
    case 0x9e:
        result = mcs251_sub(env, lhs, operand.value, 0,
                            operand.bytes * 8);
        break;
    case 0xbe:
        mcs251_sub(env, lhs, operand.value, 0, operand.bytes * 8);
        if (operand.dptr_access) {
            mcs251_dptr_finish(env, true);
        }
        return;
    default:
        return;
    }
    mcs251_cpu_set_reg(env, operand.position, operand.bytes, result);
    if (operand.dptr_access) {
        mcs251_dptr_finish(env, true);
    } else if (operand.bytes == 4 && operand.position == 56 &&
               (opcode == 0x2e || opcode == 0x7e || opcode == 0x9e)) {
        mcs251_dptr_finish(env, false);
    }
}

static void mcs251_native_incdec_execute(CPUMCS251State *env,
                                         uint8_t opcode, uint32_t *pc)
{
    uint8_t specifier = mcs251_fetch8(env, pc);
    unsigned code = specifier >> 4;
    unsigned mode = specifier & 0xf;
    unsigned position;
    unsigned bytes;
    uint32_t address;
    uint32_t value;

    if (mode == 0x8 || mode == 0xa) {
        uint8_t register_specifier = mcs251_fetch8(env, pc);
        unsigned register_code = register_specifier >> 4;

        if (register_specifier & 0xf) {
            return;
        }
        if (mode == 0x8) {
            address = mcs251_native_wr_address(env, code);
        } else if (!mcs251_native_dr_address(env, code, &address)) {
            return;
        }

        position = register_code * 2;
        if (opcode == 0x0b) {
            mcs251_cpu_set_reg(env, position, 2,
                               mcs251_load(env, address, 2));
        } else {
            mcs251_store(env, address, 2,
                         mcs251_cpu_get_reg(env, position, 2));
        }
        if (mode == 0xa && code == 14) {
            mcs251_dptr_finish(env, true);
        }
        return;
    }

    switch (mode & 0xc) {
    case 0x0:
        bytes = 1;
        position = code;
        break;
    case 0x4:
        bytes = 2;
        position = code * 2;
        break;
    case 0xc:
        bytes = 4;
        if (!mcs251_native_dr_position(code, &position)) {
            return;
        }
        break;
    default:
        return;
    }
    if ((mode & 3) == 3) {
        return;
    }

    value = mcs251_cpu_get_reg(env, position, bytes);
    if (opcode == 0x0b) {
        value += 1u << (mode & 3);
    } else {
        value -= 1u << (mode & 3);
    }
    mcs251_cpu_set_reg(env, position, bytes, value);
    mcs251_set_nz(env, value, bytes * 8);
}

static void mcs251_native_displacement_move(CPUMCS251State *env,
                                            uint8_t opcode, uint32_t *pc)
{
    uint8_t specifier = mcs251_fetch8(env, pc);
    unsigned data_code = specifier >> 4;
    unsigned pointer_code = specifier & 0xf;
    unsigned bytes = opcode & 0x40 ? 2 : 1;
    unsigned position = data_code * bytes;
    uint32_t displacement = mcs251_fetch16(env, pc);
    uint32_t address;

    if (opcode & 0x20) {
        if (!mcs251_native_dr_address(env, pointer_code, &address)) {
            return;
        }
        address = (address + displacement) & MCS251_ADDR_MASK;
    } else {
        address = (mcs251_native_wr_address(env, pointer_code) +
                   displacement) & 0xffff;
    }

    if (opcode & 0x10) {
        mcs251_store(env, address, bytes,
                     mcs251_cpu_get_reg(env, position, bytes));
    } else {
        mcs251_cpu_set_reg(env, position, bytes,
                           mcs251_load(env, address, bytes));
    }
}

static void mcs251_native_shift_execute(CPUMCS251State *env,
                                        uint8_t opcode, uint32_t *pc)
{
    uint8_t specifier = mcs251_fetch8(env, pc);
    unsigned code = specifier >> 4;
    unsigned mode = specifier & 0xf;
    unsigned bytes;
    unsigned position;
    uint32_t value;
    uint32_t result;

    if (mode == 0) {
        bytes = 1;
        position = code;
    } else if (mode == 4) {
        bytes = 2;
        position = code * 2;
    } else {
        return;
    }

    value = mcs251_cpu_get_reg(env, position, bytes);
    if (opcode == 0x3e) {
        env->flag_c = extract32(value, bytes * 8 - 1, 1);
        result = value << 1;
    } else {
        env->flag_c = value & 1;
        if (opcode == 0x0e) {
            if (bytes == 1) {
                result = (int8_t)value >> 1;
            } else {
                result = (int16_t)value >> 1;
            }
        } else {
            result = value >> 1;
        }
    }
    mcs251_cpu_set_reg(env, position, bytes, result);
    mcs251_set_nz(env, result, bytes * 8);
}

static void mcs251_native_move_store(CPUMCS251State *env, uint32_t *pc)
{
    uint8_t specifier = mcs251_fetch8(env, pc);
    unsigned code = specifier >> 4;
    unsigned mode = specifier & 0xf;
    unsigned position;
    unsigned bytes;
    uint32_t address;
    uint32_t value;

    if (mode == 0xc) {
        uint16_t immediate = mcs251_fetch16(env, pc);

        if (mcs251_native_dr_position(code, &position)) {
            mcs251_cpu_set_reg(env, position, 2, immediate);
            if (position == 56) {
                mcs251_dptr_finish(env, false);
            }
        }
        return;
    }

    if (mode == 0x9 || mode == 0xb) {
        uint8_t source = mcs251_fetch8(env, pc);

        if (source & 0xf) {
            return;
        }
        if (mode == 0x9) {
            address = mcs251_native_wr_address(env, code);
        } else if (!mcs251_native_dr_address(env, code, &address)) {
            return;
        }
        mcs251_store8(env, address, mcs251_cpu_get_reg8(env, source >> 4));
        if (mode == 0xb && code == 14) {
            mcs251_dptr_finish(env, true);
        }
        return;
    }

    switch (mode) {
    case 0x1:
    case 0x3:
        bytes = 1;
        position = code;
        break;
    case 0x5:
    case 0x7:
        bytes = 2;
        position = code * 2;
        break;
    case 0xd:
    case 0xf:
        bytes = 4;
        if (!mcs251_native_dr_position(code, &position)) {
            if (mode == 0xd) {
                mcs251_fetch8(env, pc);
            } else {
                mcs251_fetch16(env, pc);
            }
            return;
        }
        break;
    default:
        return;
    }

    value = mcs251_cpu_get_reg(env, position, bytes);
    if (mode == 0x1 || mode == 0x5 || mode == 0xd) {
        mcs251_direct_store(env, mcs251_fetch8(env, pc), bytes, value);
    } else {
        address = mcs251_fetch16(env, pc);
        mcs251_store(env, address, bytes, value);
    }
}

static void mcs251_native_bit_execute(CPUMCS251State *env, uint32_t *pc)
{
    uint8_t specifier = mcs251_fetch8(env, pc);
    uint8_t operation = specifier & 0xf8;
    uint8_t bit = specifier & 7;
    uint8_t direct = mcs251_fetch8(env, pc);
    bool branch_operation = operation == 0x10 || operation == 0x20 ||
                            operation == 0x30;
    bool rmw = operation == 0x10 || operation == 0x90 ||
               operation == 0xb0 || operation == 0xc0 ||
               operation == 0xd0;
    uint8_t byte;
    bool value;
    bool write = false;
    bool branch = false;

    switch (operation) {
    case 0x10:
    case 0x20:
    case 0x30:
    case 0x70:
    case 0x80:
    case 0x90:
    case 0xa0:
    case 0xb0:
    case 0xc0:
    case 0xd0:
    case 0xe0:
    case 0xf0:
        break;
    default:
        return;
    }
    if (direct < 0x20) {
        if (branch_operation) {
            mcs251_fetch8(env, pc);
        }
        return;
    }
    byte = rmw ? mcs251_cpu_direct_rmw_read(env, direct) :
                 mcs251_cpu_direct_read(env, direct);
    value = extract8(byte, bit, 1);

    switch (operation) {
    case 0x10:
        branch = value;
        if (value) {
            value = false;
            write = true;
        }
        break;
    case 0x20:
        branch = value;
        break;
    case 0x30:
        branch = !value;
        break;
    case 0x70:
        env->flag_c |= value;
        break;
    case 0x80:
        env->flag_c &= value;
        break;
    case 0x90:
        value = env->flag_c;
        write = true;
        break;
    case 0xa0:
        env->flag_c = value;
        break;
    case 0xb0:
        value = !value;
        write = true;
        break;
    case 0xc0:
        value = false;
        write = true;
        break;
    case 0xd0:
        value = true;
        write = true;
        break;
    case 0xe0:
        env->flag_c |= !value;
        break;
    case 0xf0:
        env->flag_c &= !value;
        break;
    default:
        return;
    }

    if (write) {
        byte = deposit32(byte, bit, 1, value);
        mcs251_cpu_direct_write(env, direct, byte);
    }
    if (branch_operation) {
        int8_t displacement = mcs251_fetch8(env, pc);

        if (branch) {
            mcs251_relative(pc, displacement);
        }
    }
}

static void mcs251_native_pushpop_execute(CPUMCS251State *env,
                                          uint8_t opcode, uint32_t *pc)
{
    uint8_t specifier = mcs251_fetch8(env, pc);
    unsigned code = specifier >> 4;
    unsigned mode = specifier & 0xf;
    unsigned position;
    unsigned bytes;
    uint32_t value;

    if (opcode == 0xca && specifier == 0x02) {
        mcs251_push(env, mcs251_fetch8(env, pc));
        return;
    }
    if (opcode == 0xca && specifier == 0x06) {
        mcs251_push_value(env, mcs251_fetch16(env, pc), 2);
        return;
    }

    switch (mode) {
    case 0x8:
        position = code;
        bytes = 1;
        break;
    case 0x9:
        position = code * 2;
        bytes = 2;
        break;
    case 0xb:
        bytes = 4;
        if (!mcs251_native_dr_position(code, &position)) {
            return;
        }
        break;
    default:
        return;
    }

    if (opcode == 0xca) {
        value = mcs251_cpu_get_reg(env, position, bytes);
        mcs251_push_value(env, value, bytes);
    } else {
        mcs251_cpu_set_reg(env, position, bytes,
                           mcs251_pop_value(env, bytes));
    }
    if (bytes == 4 && position == 56) {
        mcs251_dptr_finish(env, false);
    }
}

static void mcs251_source_execute(CPUMCS251State *env, uint8_t opcode,
                                  uint32_t *pc)
{
    uint8_t specifier;
    uint8_t rel;
    uint32_t address;

    if ((opcode & 0x0f) <= 5) {
        mcs251_classic_execute(env, opcode, pc);
        return;
    }

    switch (opcode) {
    case 0x08:
    case 0x18:
    case 0x28:
    case 0x38:
    case 0x48:
    case 0x58:
    case 0x68:
    case 0x78:
        rel = mcs251_fetch8(env, pc);
        if ((opcode == 0x08 &&
             (env->flag_z || env->flag_n != env->flag_ov)) ||
            (opcode == 0x18 &&
             (!env->flag_z && env->flag_n == env->flag_ov)) ||
            (opcode == 0x28 && (env->flag_c || env->flag_z)) ||
            (opcode == 0x38 && (!env->flag_c && !env->flag_z)) ||
            (opcode == 0x48 && env->flag_n != env->flag_ov) ||
            (opcode == 0x58 && env->flag_n == env->flag_ov) ||
            (opcode == 0x68 && env->flag_z) ||
            (opcode == 0x78 && !env->flag_z)) {
            mcs251_relative(pc, rel);
        }
        break;
    case 0x09:
    case 0x19:
    case 0x29:
    case 0x39:
    case 0x49:
    case 0x59:
    case 0x69:
    case 0x79:
        mcs251_native_displacement_move(env, opcode, pc);
        break;
    case 0x0a:
    case 0x1a:
        specifier = mcs251_fetch8(env, pc);
        if (opcode == 0x0a) {
            address = mcs251_cpu_get_reg8(env, specifier & 0xf);
        } else {
            address = (int8_t)mcs251_cpu_get_reg8(env, specifier & 0xf);
        }
        mcs251_cpu_set_reg(env, (specifier >> 4) * 2, 2, address);
        break;
    case 0x0b:
    case 0x1b:
        mcs251_native_incdec_execute(env, opcode, pc);
        break;
    case 0x0e:
    case 0x1e:
    case 0x3e:
        mcs251_native_shift_execute(env, opcode, pc);
        break;
    case 0x2c:
    case 0x2d:
    case 0x2f:
    case 0x4c:
    case 0x4d:
    case 0x5c:
    case 0x5d:
    case 0x6c:
    case 0x6d:
    case 0x7c:
    case 0x7d:
    case 0x7f:
    case 0x9c:
    case 0x9d:
    case 0x9f:
    case 0xbc:
    case 0xbd:
    case 0xbf:
        mcs251_native_register_execute(env, opcode,
                                       mcs251_fetch8(env, pc));
        break;
    case 0x2e:
    case 0x4e:
    case 0x5e:
    case 0x6e:
    case 0x7e:
    case 0x9e:
    case 0xbe:
        mcs251_native_generic_execute(env, opcode, pc);
        break;
    case 0x7a:
        mcs251_native_move_store(env, pc);
        break;
    case 0x89:
    case 0x99:
        specifier = mcs251_fetch8(env, pc);
        if ((specifier & 0xf) == 4) {
            address = (*pc & 0xff0000) |
                      mcs251_native_wr_address(env, specifier >> 4);
            if (opcode == 0x99) {
                mcs251_push_near_return(env, *pc);
            }
            *pc = address;
        } else if ((specifier & 0xf) == 8 &&
                   mcs251_native_dr_address(env, specifier >> 4,
                                            &address)) {
            if (opcode == 0x99) {
                mcs251_push_value(env, *pc, 3);
            }
            *pc = address;
        }
        break;
    case 0x8a:
        *pc = mcs251_fetch24(env, pc);
        break;
    case 0x8c:
    case 0x8d:
    case 0xac:
    case 0xad:
        mcs251_native_muldiv_execute(env, opcode,
                                     mcs251_fetch8(env, pc));
        break;
    case 0x9a:
        address = mcs251_fetch24(env, pc);
        mcs251_push_value(env, *pc, 3);
        *pc = address;
        break;
    case 0xa9:
        mcs251_native_bit_execute(env, pc);
        break;
    case 0xaa:
        mcs251_return_extended(env, pc);
        break;
    case 0xb9:
        /* TRAP is architecturally defined to execute as NOP. */
        break;
    case 0xca:
    case 0xda:
        mcs251_native_pushpop_execute(env, opcode, pc);
        break;
    default:
        /* All unused primary encodings are architecturally NOP. */
        break;
    }
}

static int mcs251_pick_interrupt(CPUMCS251State *env)
{
    uint32_t enabled = env->irq_pending & env->ie & 0x1f;
    bool timer0_nmi = env->timer0_mode3_armed &&
                      (env->irq_pending & (1u << MCS251_IRQ_TIMER0));
    int best = -1;
    int best_level = -1;
    int irq;

    if (timer0_nmi) {
        enabled |= 1u << MCS251_IRQ_TIMER0;
    }
    if (!(env->ie & 0x80) && !timer0_nmi) {
        return -1;
    }

    for (irq = 0; irq < MCS251_NUM_IRQS; irq++) {
        int level;

        if (!(enabled & (1u << irq))) {
            continue;
        }
        level = irq == MCS251_IRQ_TIMER0 && timer0_nmi ? 4 :
                ((env->iph >> irq) & 1) << 1 | ((env->ip >> irq) & 1);
        if (env->irq_level != UINT32_MAX && level <= env->irq_level) {
            continue;
        }
        if (level > best_level) {
            best = irq;
            best_level = level;
        }
    }
    return best;
}

bool mcs251_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    CPUMCS251State *env = cpu_env(cs);
    int irq;

    if (!(interrupt_request & CPU_INTERRUPT_HARD)) {
        return false;
    }

    irq = mcs251_pick_interrupt(env);
    if (irq < 0) {
        return false;
    }

    env->irq_ack = irq;
    mcs251_cpu_do_interrupt(cs);
    return true;
}

void mcs251_cpu_do_interrupt(CPUState *cs)
{
    static const uint32_t vectors[MCS251_NUM_IRQS] = {
        0xff0003, 0xff000b, 0xff0013, 0xff001b, 0xff0023,
    };
    CPUMCS251State *env = cpu_env(cs);
    uint32_t old_pc = env->pc;
    unsigned irq = env->irq_ack;
    unsigned level;

    if (irq >= MCS251_NUM_IRQS) {
        return;
    }

    env->ta_stage = 0;
    level = irq == MCS251_IRQ_TIMER0 && env->timer0_mode3_armed ? 4 :
            ((env->iph >> irq) & 1) << 1 | ((env->ip >> irq) & 1);
    if (env->irq_depth < MCS251_MAX_IRQ_DEPTH) {
        env->irq_level_stack[env->irq_depth++] = env->irq_level;
    }
    env->irq_level = level;

    /*
     * STC32G uses the MCS-251 four-byte interrupt frame. RETI pops,
     * in order, PC[15:8], PC[7:0], PC[23:16], and PSW1.
     */
    mcs251_push(env, mcs251_cpu_get_psw1(env));
    mcs251_push(env, old_pc >> 16);
    mcs251_push(env, old_pc);
    mcs251_push(env, old_pc >> 8);

    switch (irq) {
    case MCS251_IRQ_INT0:
        env->tcon &= ~0x02;
        break;
    case MCS251_IRQ_TIMER0:
        env->tcon &= ~0x20;
        break;
    case MCS251_IRQ_INT1:
        env->tcon &= ~0x08;
        break;
    case MCS251_IRQ_TIMER1:
        env->tcon &= ~0x80;
        break;
    default:
        break;
    }

    /*
     * The timer and external-interrupt flags are cleared on acknowledge.
     * UART RI/TI are software-cleared level sources and must remain pending.
     */
    if (irq != MCS251_IRQ_UART1) {
        env->irq_pending &= ~(1u << irq);
    }
    if (!env->irq_pending) {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
    env->pc = vectors[irq];
    env->irq_ack = UINT32_MAX;
    qemu_plugin_vcpu_interrupt_cb(cs, old_pc);
}

void HELPER(mcs251_execute)(CPUMCS251State *env)
{
    uint32_t pc = env->pc;
    bool source_mode = !(env->auxr2 & 0x40);
    uint8_t opcode = mcs251_fetch8(env, &pc);

    env->ta_touched = false;
    while (opcode == 0xa5) {
        source_mode = !source_mode;
        opcode = mcs251_fetch8(env, &pc);
    }

    if (source_mode) {
        mcs251_source_execute(env, opcode, &pc);
    } else {
        mcs251_classic_execute(env, opcode, &pc);
    }
    if (!env->ta_touched) {
        env->ta_stage = 0;
    }
    env->pc = pc & MCS251_ADDR_MASK;
}
